#pragma once

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "CmdBar.h"
#include "ReadCIFF.h"
#include "WriteBuffer.h"

namespace ciff
{
	struct Convert
	{
		struct AsyncSharedState
		{
		};

		struct AsyncChunkState
		{
		};

		explicit Convert(Read& data, const size_t writeBufferSize = WRITE_BUFFER_SIZE)
			: data(data), write(writeBufferSize)
		{
		}

		virtual ~Convert() = default;

		bool run()
		{
			SetFile();

			try
			{
				convert();
				return true;
			}
			catch (const std::exception& e)
			{
				std::cerr << e.what() << std::endl;
			}
			catch (...)
			{
				std::cerr << "Unknown error occurred." << std::endl;
			}

			return false;
		}

	protected:
		void convert()
		{
			if (data.nodes.empty())
				return;

			bar::start("Parse, tess and write geometry", data.nodes.size());
			WriteHeader();

			for (nodeIndex = 0; nodeIndex < data.nodes.size(); ++nodeIndex)
			{
				bar::step(nodeIndex);
				convertNode(nodeIndex, data.nodes[nodeIndex]);
			}

			WriteMaterial(true);
			WriteFooter();
			bar::stop();
		}

	public:
		void convertNode(const size_t index, const Node& node)
		{
			nodeIndex = index;

			if (index == 0)
				WriteHead(node);
			else
				WriteModel(node);

			for (const auto geometryIndex : node.geometries)
				WriteGeometry(node, geometryIndex);
		}

		virtual bool SetFile()
		{
			return SetFile(std::filesystem::path(data.target_cad));
		}

		bool SetFile(const std::filesystem::path& target)
		{
			if (write.good())
				return false;

			write.set(target.string());
			source_file = data.source_cad;
			target_file = target.string();
			return true;
		}

		virtual void WriteHeader() = 0;

		virtual void WriteHead(const Node& node)
		{
			WriteNode(node);
		}

		virtual void WriteModel(const Node& node)
		{
			WriteNode(node);
		}

		virtual void WriteNode(const Node& node) = 0;
		virtual void WriteGeometry(const Node& node, size_t geometryIndex) = 0;
		virtual void WriteMaterial(bool header) = 0;
		virtual void WriteFooter() = 0;

		[[nodiscard]] AsyncSharedState PrepareAsyncSharedState()
		{
			return {};
		}

		void AttachAsyncSharedState(const AsyncSharedState&)
		{
		}

		void PrepareAsyncChunk(AsyncChunkState&)
		{
		}

		void BeginAsyncMerge()
		{
		}

		void PrepareAsyncChunkForMerge(AsyncChunkState&)
		{
		}

		void MergeAsyncChunk(const std::filesystem::path& shardFile, const AsyncChunkState&,
			const bool outputEnabled)
		{
			if (outputEnabled)
				write.append(shardFile.string(), false);
		}

		void CommitAsyncMerge() noexcept
		{
		}

		[[nodiscard]] std::vector<std::filesystem::path> AsyncSidecarFiles() const
		{
			return {};
		}

		[[nodiscard]] std::vector<rgb> getColors() const
		{
			return data.colors;
		}

		std::string source_file;
		std::string target_file;
		Read& data;
		WriteBuffer write;
		size_t nodeIndex = 0;
	};
} // namespace ciff
