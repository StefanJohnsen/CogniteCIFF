#pragma once

#include "CmdArgs.h"
#include "CmdBar.h"
#include "Convert.h"
#include "WriteBuffer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace ciff
{
	struct ConvertRange final
	{
		explicit ConvertRange(Convert& convert) : convert_(convert)
		{
		}

		void run(const std::vector<Node>& list, const size_t from, const size_t to)
		{
			if (from > to || to > list.size())
				throw std::invalid_argument("ConvertRange: invalid range");

			for (size_t index = from; index < to; ++index)
				convert_.convertNode(index, list[index]);
		}

	private:
		Convert& convert_;
	};

	namespace async_detail
	{
		inline constexpr size_t worker_write_buffer_size = 1ULL * 1024ULL * 1024ULL;
		inline constexpr size_t min_nodes_per_thread = 100U;
		inline constexpr size_t max_worker_count = 16U;

		template <typename AsyncChunkState>
		struct WorkerResult final
		{
			size_t from = 0;
			size_t to = 0;
			bool succeeded = false;
			std::exception_ptr error;
			AsyncChunkState chunkState;
		};

		struct WorkerFailure final
		{
			size_t from = 0;
			size_t to = 0;
			std::exception_ptr error;
		};

		template <typename AsyncChunkState>
		struct PendingWorker final
		{
			size_t from = 0;
			size_t to = 0;
			std::future<WorkerResult<AsyncChunkState>> future;
		};

		class StagingOutput final
		{
		public:
			StagingOutput(const std::filesystem::path& target, const size_t shardCount)
			{
				if (target.empty())
					throw std::invalid_argument("ConvertAsync: target file is empty");

				std::error_code ec;
				target_ = std::filesystem::absolute(target, ec);

				if (ec)
					throw std::system_error(ec, "ConvertAsync: failed to resolve target path");

				if (target_.filename().empty())
					throw std::invalid_argument("ConvertAsync: target file name is empty");

				const auto parent = target_.parent_path();
				if (parent.empty())
					throw std::invalid_argument("ConvertAsync: target directory is empty");

				ec.clear();
				if (!std::filesystem::is_directory(parent, ec) || ec)
					throw std::runtime_error("ConvertAsync: target directory does not exist");

				createUniqueDirectory(parent);

				try
				{
					// Keep the final basename while staging. Sidecar-producing
					// converters use it in their headers and derive sibling files
					// such as model.mtl from this path.
					merged_ = directory_ / target_.filename();
					shards_.reserve(shardCount);
					const auto shardDirectory = directory_ / "shards";
					std::error_code shardError;
					if (!std::filesystem::create_directory(shardDirectory, shardError))
					{
						if (shardError)
							throw std::system_error(shardError, "ConvertAsync: failed to create shard directory");
						throw std::runtime_error("ConvertAsync: shard directory already exists");
					}

					const auto extension = target_.extension();
					for (size_t index = 0; index < shardCount; ++index)
						shards_.emplace_back(shardDirectory / ("chunk-" + std::to_string(index) + extension.string()));
				}
				catch (...)
				{
					cleanup();
					throw;
				}
			}

			~StagingOutput() noexcept
			{
				cleanup();
			}

			StagingOutput(const StagingOutput&) = delete;
			StagingOutput& operator=(const StagingOutput&) = delete;
			StagingOutput(StagingOutput&&) = delete;
			StagingOutput& operator=(StagingOutput&&) = delete;

			[[nodiscard]] const std::filesystem::path& shard(const size_t index) const
			{
				return shards_.at(index);
			}

			[[nodiscard]] const std::filesystem::path& merged() const noexcept
			{
				return merged_;
			}

			void publish(const std::vector<std::filesystem::path>& stagedSidecars = {})
			{
				std::error_code ec;
				if (!std::filesystem::is_regular_file(merged_, ec) || ec)
					throw std::runtime_error("ConvertAsync: staged output file does not exist");

				if (stagedSidecars.empty())
				{
					replaceFile(merged_, target_, "ConvertAsync: failed to publish target file");
					return;
				}

				auto sidecars = std::vector<SidecarPublication>{};
				sidecars.reserve(stagedSidecars.size());

				for (size_t index = 0; index < stagedSidecars.size(); ++index)
				{
					ec.clear();
					auto staged = std::filesystem::absolute(stagedSidecars[index], ec);
					if (ec)
						throw std::system_error(ec, "ConvertAsync: failed to resolve staged sidecar path");
					if (staged.parent_path() != directory_ || staged.filename().empty())
						throw std::invalid_argument("ConvertAsync: staged sidecar lies outside the staging directory");
					if (!std::filesystem::is_regular_file(staged, ec) || ec)
						throw std::runtime_error("ConvertAsync: staged sidecar file does not exist");

					auto target = target_.parent_path() / staged.filename();
					if (target == target_)
						throw std::invalid_argument("ConvertAsync: sidecar target overlaps the primary target");
					for (const auto& existing : sidecars)
					{
						if (existing.target == target)
							throw std::invalid_argument("ConvertAsync: duplicate sidecar target");
					}

					auto publication = SidecarPublication{
						.staged = std::move(staged),
						.target = std::move(target),
					};
					ec.clear();
					const auto targetExists = std::filesystem::exists(publication.target, ec);
					if (ec)
						throw std::system_error(ec, "ConvertAsync: failed to inspect sidecar target");
					if (targetExists)
					{
						if (!std::filesystem::is_regular_file(publication.target, ec) || ec)
							throw std::runtime_error("ConvertAsync: sidecar target is not a regular file");
						publication.backup = directory_ / ("sidecar-backup-" + std::to_string(index) + ".tmp");
						ec.clear();
						std::filesystem::copy_file(publication.target, publication.backup,
							std::filesystem::copy_options::none, ec);
						if (ec)
							throw std::system_error(ec, "ConvertAsync: failed to back up sidecar target");
					}
					sidecars.emplace_back(std::move(publication));
				}

				try
				{
					for (auto& sidecar : sidecars)
					{
						replaceFile(sidecar.staged, sidecar.target,
							"ConvertAsync: failed to publish sidecar file");
						sidecar.published = true;
					}
					replaceFile(merged_, target_, "ConvertAsync: failed to publish target file");
				}
				catch (...)
				{
					const auto publishError = std::current_exception();
					auto rollbackError = std::string{};
					for (auto sidecar = sidecars.rbegin(); sidecar != sidecars.rend(); ++sidecar)
					{
						if (!sidecar->published)
							continue;
						try
						{
							if (!sidecar->backup.empty())
							{
								replaceFile(sidecar->backup, sidecar->target,
									"ConvertAsync: failed to restore sidecar target");
							}
							else
							{
								ec.clear();
								std::filesystem::remove(sidecar->target, ec);
								if (ec)
									throw std::system_error(ec, "ConvertAsync: failed to remove published sidecar");
							}
						}
						catch (const std::exception& error)
						{
							if (rollbackError.empty())
								rollbackError = error.what();
						}
						catch (...)
						{
							if (rollbackError.empty())
								rollbackError = "unknown rollback error";
						}
					}

					if (!rollbackError.empty())
						throw std::runtime_error("ConvertAsync: publish failed and sidecar rollback failed: " + rollbackError);
					std::rethrow_exception(publishError);
				}
			}

		private:
			struct SidecarPublication final
			{
				std::filesystem::path staged;
				std::filesystem::path target;
				std::filesystem::path backup;
				bool published = false;
			};

			static void replaceFile(const std::filesystem::path& source,
				const std::filesystem::path& target, const char* message)
			{
#if defined(_WIN32)
				if (!MoveFileExW(source.c_str(), target.c_str(),
						MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
				{
					throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), message);
				}
#else
				auto ec = std::error_code{};
				std::filesystem::rename(source, target, ec);
				if (ec)
					throw std::system_error(ec, message);
#endif
			}

			void createUniqueDirectory(const std::filesystem::path& parent)
			{
				constexpr size_t maxAttempts = 128U;
				static std::atomic_uint64_t sequence = 0U;

				for (size_t attempt = 0; attempt < maxAttempts; ++attempt)
				{
					const auto clock = static_cast<uint64_t>(
						std::chrono::steady_clock::now().time_since_epoch().count());
					const auto ordinal = sequence.fetch_add(1U, std::memory_order_relaxed);

					std::ostringstream name;
					name << ".convert-async-" << std::hex << clock << '-' << ordinal;
					auto candidate = parent / name.str();

					std::error_code ec;
					if (std::filesystem::create_directory(candidate, ec))
					{
						directory_ = std::move(candidate);
						return;
					}

					if (ec && ec != std::errc::file_exists)
						throw std::system_error(ec, "ConvertAsync: failed to create staging directory");
				}

				throw std::runtime_error("ConvertAsync: failed to allocate a unique staging directory");
			}

			void cleanup() noexcept
			{
				if (directory_.empty())
					return;

				std::error_code ec;
				std::filesystem::remove_all(directory_, ec);
			}

			std::filesystem::path target_;
			std::filesystem::path directory_;
			std::filesystem::path merged_;
			std::vector<std::filesystem::path> shards_;
		};

		class WorkerProgress final
		{
		public:
			explicit WorkerProgress(const size_t totalNodeCount)
			{
				if (bar::isIdle() || totalNodeCount == 0)
					return;

				bar::start("Parse, tess and write geometry", totalNodeCount);
				started_ = true;
			}

			~WorkerProgress() noexcept
			{
				cancel();
			}

			void complete() noexcept
			{
				if (!started_)
					return;

				started_ = false;

				try
				{
					bar::stop();
				}
				catch (...)
				{
				}
			}

			void cancel() noexcept
			{
				if (!started_)
					return;

				started_ = false;

				try
				{
					bar::cancel();
				}
				catch (...)
				{
				}
			}

			WorkerProgress(const WorkerProgress&) = delete;
			WorkerProgress& operator=(const WorkerProgress&) = delete;

		private:
			bool started_ = false;
		};

		inline void stepWorkerProgress(const std::atomic_size_t& completedNodeCount, std::mutex& progressMutex)
		{
			if (bar::isIdle())
				return;

			std::lock_guard<std::mutex> lock(progressMutex);
			bar::step(completedNodeCount.load(std::memory_order_relaxed), false);
		}

		inline void reportWorkerFailure(const WorkerFailure& failure)
		{
			std::cerr << "ConvertAsync: exception in worker range [" << failure.from << ", " << failure.to << "]: ";

			try
			{
				if (failure.error)
					std::rethrow_exception(failure.error);
			}
			catch (const std::exception& e)
			{
				std::cerr << e.what() << std::endl;
				return;
			}
			catch (...)
			{
				std::cerr << "unknown error" << std::endl;
				return;
			}

			std::cerr << "unknown error" << std::endl;
		}
	} // namespace async_detail
} // namespace ciff

template <typename Converter>
struct ConvertAsync final
{
	explicit ConvertAsync(ciff::Read& data, const size_t threadCount = std::thread::hardware_concurrency())
		: data_(data), threadCount_(threadCount)
	{
	}

	bool run()
	{
		try
		{
			return asyncConvert();
		}
		catch (const std::exception& e)
		{
			std::cerr << e.what() << std::endl;
		}
		catch (...)
		{
			std::cerr << "ConvertAsync: unknown error occurred." << std::endl;
		}

		return false;
	}

private:
	bool asyncConvert()
	{
		using AsyncChunkState = typename Converter::AsyncChunkState;
		using WorkerResult = ciff::async_detail::WorkerResult<AsyncChunkState>;
		using PendingWorker = ciff::async_detail::PendingWorker<AsyncChunkState>;
		static_assert(noexcept(std::declval<Converter&>().CommitAsyncMerge()),
			"ConvertAsync requires a non-throwing post-publish commit");

		const auto& list = data_.nodes;
		const auto total = list.size();

		if (total == 0)
			return Converter(data_).run();

		auto workerCount = threadCount_ == 0 ? 1U : threadCount_;
		workerCount = std::min(workerCount, ciff::async_detail::max_worker_count);
		workerCount = std::min(workerCount,
			(total + ciff::async_detail::min_nodes_per_thread - 1U) /
				ciff::async_detail::min_nodes_per_thread);

		if (workerCount <= 1)
			return Converter(data_).run();

		Converter convert(data_);
		const auto sharedState = convert.PrepareAsyncSharedState();

		const auto chunkSize = (total + workerCount - 1U) / workerCount;
		const bool outputEnabled = WriteBuffer::enabled;
		auto staging = outputEnabled
			? std::make_unique<ciff::async_detail::StagingOutput>(std::filesystem::path(data_.target_cad), workerCount)
			: nullptr;

		std::atomic_size_t completedCount = 0;
		std::mutex progressMutex;
		ciff::async_detail::WorkerProgress progress(total);

		std::vector<PendingWorker> pending;
		pending.reserve(workerCount);

		std::vector<WorkerResult> results;
		results.reserve(workerCount);

		std::optional<ciff::async_detail::WorkerFailure> firstFailure;

		for (size_t index = 0; index < workerCount; ++index)
		{
			const auto from = index * chunkSize;
			const auto to = std::min(from + chunkSize, total);

			if (from >= to)
				break;

			const auto workerTarget = outputEnabled ? staging->shard(index) : std::filesystem::path(data_.target_cad);

			try
			{
				auto future = std::async(std::launch::async,
					[this, &list, &completedCount, &progressMutex, from, to, workerTarget, sharedState]() {
						WorkerResult result;
						result.from = from;
						result.to = to;

						try
						{
							Converter convert(data_, ciff::async_detail::worker_write_buffer_size);
							convert.AttachAsyncSharedState(sharedState);

							if (!convert.SetFile(workerTarget))
								throw std::runtime_error("worker output is already open");

							convert.PrepareAsyncChunk(result.chunkState);
							ciff::ConvertRange(convert).run(list, from, to);
							convert.write.close();

							result.succeeded = true;

							completedCount.fetch_add(to - from, std::memory_order_relaxed);
							ciff::async_detail::stepWorkerProgress(completedCount, progressMutex);
						}
						catch (...)
						{
							result.error = std::current_exception();
						}

						return result;
					});

				pending.push_back({ from, to, std::move(future) });
			}
			catch (...)
			{
				firstFailure.emplace(ciff::async_detail::WorkerFailure{ from, to, std::current_exception() });
				break;
			}
		}

		for (auto& worker : pending)
		{
			try
			{
				auto result = worker.future.get();

				if (!result.succeeded && !firstFailure)
				{
					firstFailure.emplace(
						ciff::async_detail::WorkerFailure{ result.from, result.to, result.error });
				}

				results.emplace_back(std::move(result));
			}
			catch (...)
			{
				if (!firstFailure)
				{
					firstFailure.emplace(ciff::async_detail::WorkerFailure{
						worker.from, worker.to, std::current_exception() });
				}
			}
		}

		if (firstFailure)
		{
			progress.cancel();
			ciff::async_detail::reportWorkerFailure(*firstFailure);
			return false;
		}

		const auto mergedTarget = outputEnabled ? staging->merged() : std::filesystem::path(data_.target_cad);

		if (!convert.SetFile(mergedTarget))
			throw std::runtime_error("merge output is already open");

		convert.BeginAsyncMerge();
		for (auto& result : results)
			convert.PrepareAsyncChunkForMerge(result.chunkState);

		convert.WriteHeader();

		for (size_t index = 0; index < results.size(); ++index)
		{
			const auto shardFile = outputEnabled ? staging->shard(index) : std::filesystem::path{};
			convert.MergeAsyncChunk(shardFile, results[index].chunkState, outputEnabled);
		}

		convert.WriteMaterial(true);
		convert.WriteFooter();
		convert.write.close();

		if (outputEnabled)
			staging->publish(convert.AsyncSidecarFiles());

		convert.CommitAsyncMerge();

		progress.complete();

		return true;
	}

	ciff::Read& data_;
	size_t threadCount_;
};
