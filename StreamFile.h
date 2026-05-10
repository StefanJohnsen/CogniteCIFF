#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#include <io.h>
#define seek_file _fseeki64
#define tell_file _ftelli64
#else
#include <endian.h>
#define seek_file fseeko
#define tell_file ftello
#endif

namespace ciff
{
	class StreamFile
	{
	public:
		StreamFile(const std::string& filename, const std::size_t chunkSize)
			: filename(filename), chunkSize(chunkSize), buffer(chunkSize)
		{
			file = std::fopen(filename.c_str(), "rb");

			if (!file)
				throw std::runtime_error("Failed to open file: " + filename);

			readNextChunk();
		}

		~StreamFile()
		{
			closeFile();
		}

		void closeFile()
		{
			if (file)
			{
				std::fclose(file);
				file = nullptr;
			}

			buffer.clear();
			bufferPos = 0;
			bytesRead = 0;
			currentPos = 0;
			chunkSize = 0;
			filename.clear();
		}

		void read(void* destination, const size_t size, const size_t count)
		{
			const auto totalBytes = size * count;

			if (totalBytes == 0)
				return;

			auto* dst = static_cast<char*>(destination);
			size_t copied = 0;

			while (copied < totalBytes)
			{
				if (bufferPos >= bytesRead && !readNextChunk())
					throw std::runtime_error("Unexpected end of file");

				const auto available = bytesRead - bufferPos;
				const auto remaining = totalBytes - copied;
				const auto toCopy = std::min(available, remaining);

				std::memcpy(dst + copied, buffer.data() + bufferPos, toCopy);

				bufferPos += toCopy;
				copied += toCopy;
				currentPos += static_cast<int64_t>(toCopy);
			}
		}

		[[nodiscard]] std::vector<uint8_t> readBytes(const size_t byteCount)
		{
			std::vector<uint8_t> bytes(byteCount);
			read(bytes.data(), sizeof(uint8_t), byteCount);
			return bytes;
		}

		void seek(const int64_t pos)
		{
			if (pos < 0)
				throw std::runtime_error("Negative stream position");

			const auto bufferStart = currentPos - static_cast<int64_t>(bufferPos);
			const auto bufferEnd = bufferStart + static_cast<int64_t>(bytesRead);

			if (pos >= bufferStart && pos < bufferEnd)
			{
				bufferPos = static_cast<size_t>(pos - bufferStart);
				currentPos = pos;
				return;
			}

			std::clearerr(file);

			if (seek_file(file, pos, SEEK_SET) != 0)
				throw std::runtime_error("Error while seeking");

			currentPos = pos;
			bufferPos = 0;
			bytesRead = 0;
			readNextChunk();
		}

		[[nodiscard]] int64_t tell() const
		{
			return currentPos;
		}

		[[nodiscard]] bool good() const
		{
			return file != nullptr && ferror(file) == 0;
		}

		[[nodiscard]] uintmax_t size() const
		{
			return std::filesystem::file_size(std::filesystem::path(filename));
		}

	private:
		bool readNextChunk()
		{
			if (!file || std::feof(file))
				return false;

			bytesRead = std::fread(buffer.data(), 1, chunkSize, file);
			bufferPos = 0;
			return bytesRead > 0;
		}

		FILE* file = nullptr;
		std::string filename;
		std::size_t chunkSize = 0;
		std::vector<char> buffer;
		std::size_t bytesRead = 0;
		std::size_t bufferPos = 0;
		int64_t currentPos = 0;
	};
}
