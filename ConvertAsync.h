#pragma once

#include "CmdArgs.h"
#include "CmdBar.h"
#include "Convert.h"
#include "ConversionOutput.h"
#include "WriteBuffer.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

		const auto chunkSize = (total + workerCount - 1U) / workerCount;
		const bool outputEnabled = WriteBuffer::enabled;
		auto staging = outputEnabled
			? std::make_unique<conversion::Workspace>(std::filesystem::path(data_.target_cad), workerCount)
			: nullptr;

		Converter convert(data_);
		const auto sharedState = convert.PrepareAsyncSharedState();

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

			const auto workerTarget = outputEnabled ? staging->part(index) : std::filesystem::path(data_.target_cad);

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

		const auto mergedTarget = outputEnabled ? staging->result() : std::filesystem::path(data_.target_cad);

		if (!convert.SetFile(mergedTarget))
			throw std::runtime_error("merge output is already open");

		convert.BeginAsyncMerge();
		for (auto& result : results)
			convert.PrepareAsyncChunkForMerge(result.chunkState);

		convert.WriteHeader();

		for (size_t index = 0; index < results.size(); ++index)
		{
			const auto shardFile = outputEnabled ? staging->part(index) : std::filesystem::path{};
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
