#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "StreamFile.h"

namespace ciff::binary
{
	// Default chunk size for buffered reads (4 MB).
	inline constexpr std::size_t StreamChunkSize = 4ULL * 1024 * 1024;

	// Thin little-endian binary reader on top of ciff::StreamFile.
	// CIFF is a little-endian format so the same memory layout is used directly.
	class Stream
	{
	public:
		explicit Stream(const std::string& filename, const std::size_t chunkSize = StreamChunkSize)
			: stream(filename, chunkSize)
		{
		}

		template <typename T>
		T read()
		{
			static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
			T value{};
			stream.read(&value, sizeof(T), 1);
			return value;
		}

		template <typename T>
		std::vector<T> readArray(const std::size_t count)
		{
			static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
			std::vector<T> values(count);

			if (count > 0)
				stream.read(values.data(), sizeof(T), count);

			return values;
		}

		// Reads a CIFF string: uint32 length prefix followed by raw bytes.
		std::string readString()
		{
			const auto length = read<uint32_t>();
			std::string value;

			if (length == 0)
				return value;

			value.resize(length);
			stream.read(value.data(), sizeof(char), length);
			return value;
		}

		void skip(const std::size_t bytes)
		{
			if (bytes == 0)
				return;

			stream.seek(stream.tell() + static_cast<int64_t>(bytes));
		}

		[[nodiscard]] int64_t tell() const
		{
			return stream.tell();
		}

		void seek(const int64_t pos)
		{
			stream.seek(pos);
		}

		[[nodiscard]] uintmax_t size() const
		{
			return stream.size();
		}

	private:
		ciff::StreamFile stream;
	};
}
