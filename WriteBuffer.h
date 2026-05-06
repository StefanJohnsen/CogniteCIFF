#pragma once
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "Constants.h"

inline constexpr size_t WRITE_BUFFER_SIZE = 100ULL * 1024 * 1024; // 100MB

struct WriteBuffer
{
	explicit WriteBuffer(const size_t size = WRITE_BUFFER_SIZE) : configured_size(size)
	{
		validateConfiguredSize();
	}

	explicit WriteBuffer(std::ofstream&& s, const size_t size = WRITE_BUFFER_SIZE) : WriteBuffer(size)
	{
		set(std::move(s));
	}

	explicit WriteBuffer(const std::string& path, const size_t size = WRITE_BUFFER_SIZE) : WriteBuffer(size)
	{
		set(path);
	}

	~WriteBuffer()
	{
		try
		{
			close();
		}
		catch (...)
		{
			// Suppress exceptions in destructor
		}
	}

	[[nodiscard]] bool good() const noexcept
	{
		return enabled && stream.is_open() && stream.good();
	}

	void set(const std::string& file)
	{
		close();
		path = file;

		if (!enabled)
			return;

		ensureBufferAllocated();
		stream = std::ofstream(path, std::ios::binary | std::ios::out);

		check();
	}

	void set(std::ofstream&& s)
	{
		close();
		path.clear();

		if (!enabled)
			return;

		ensureBufferAllocated();
		stream = std::move(s);

		check();
	}

	void close()
	{
		if (enabled)
			flush();
		else
			position = 0;

		// Only close streams we opened ourselves (via path)
		// Moved-in streams are caller's responsibility
		if (!path.empty())
		{
			if (stream.is_open())
				stream.close();
		}
	}

	WriteBuffer(const WriteBuffer&) = delete;
	WriteBuffer& operator=(const WriteBuffer&) = delete;
	WriteBuffer(WriteBuffer&&) = delete;
	WriteBuffer& operator=(WriteBuffer&&) = delete;

	void write(const std::string& str)
	{
		writeToBuffer(str.c_str(), str.length());
	}

	void write(const char* str, const size_t size)
	{
		writeToBuffer(str, size);
	}

	void write(const float f)
	{
		writeToBuffer(f);
	}

	void write(const double d)
	{
		writeToBuffer(d);
	}

	template <typename T>
	void write(const T& t)
	{
		writeToBuffer(t);
	}

	template <typename T>
	void write(const T* data, const size_t elementCount)
	{
		writeDataToBuffer<T>(data, elementCount);
	}

	template <typename T>
	size_t overwriteAt(const size_t pos, const T& t)
	{
		return overwriteAtPosition(pos, t);
	}

	void append(const std::string& file, bool deleteFile = false)
	{
		appendFile(file, deleteFile);
	}

	void append(const std::vector<std::string>& files, bool deleteFiles)
	{
		appendFile(files, deleteFiles);
	}

	void flush()
	{
		if (!enabled)
		{
			position = 0;
			return;
		}

		if (!stream.is_open() || position == 0)
			return;

		stream.write(buffer.data(), static_cast<std::streamsize>(position));

		if (!stream)
			throw std::ios_base::failure("Failed to write data to file: Stream error");

		position = 0;
	}

	[[nodiscard]] size_t tell()
	{
		if (!enabled)
			return 0;

		return stream_tell() + position;
	}

	[[nodiscard]] size_t stream_tell()
	{
		if (!enabled)
			return 0;

		const std::streampos pos = stream.tellp();

		if (pos == std::streampos(-1))
			throw std::ios_base::failure("Failed to get file position");

		return static_cast<size_t>(static_cast<std::streamoff>(pos));
	}

	[[nodiscard]] const std::string& getFile() const noexcept
	{
		return path;
	}

	[[nodiscard]] std::ofstream& getStream()
	{
		if (position != 0 && enabled)
			throw std::logic_error("Buffer must be flushed before accessing the stream");

		return stream;
	}

private:

	void validateConfiguredSize() const
	{
		if (configured_size == 0)
			throw std::invalid_argument("Buffer size cannot be zero");
	}

	void ensureBufferAllocated()
	{
		if (!enabled || !buffer.empty())
			return;

		buffer.resize(configured_size);
	}

	void appendFile(const std::string& file, bool deleteFile)
	{
		if (!enabled)
		{
			if (deleteFile)
				std::remove(file.c_str());

			return;
		}

		ensureBufferAllocated();

		if (stream.is_open())
		{
			flush();
			stream.seekp(0, std::ios::end);
			if (!stream)
				throw std::runtime_error("Failed to seek to end of target stream");
		}
		else
		{
			if (path.empty())
				throw std::runtime_error("Target stream is not open and has no file path");

			stream = std::ofstream(path, std::ios::binary | std::ios::out | std::ios::app);
			check();
		}

		std::ifstream input(file, std::ios::binary);
		if (!input.is_open())
			throw std::runtime_error("Failed to open source file for reading");

		while (input)
		{
			input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			const std::streamsize bytesRead = input.gcount();

			if (bytesRead <= 0)
				break;

			stream.write(buffer.data(), bytesRead);
			if (!stream)
				throw std::ios_base::failure("Failed to write chunk to target stream");
		}

		if (deleteFile)
		{
			input.close();
			std::remove(file.c_str());
		}
	}

	void appendFile(const std::vector<std::string>& files, bool deleteFiles)
	{
		if (!enabled)
		{
			if (deleteFiles)
			{
				for (const auto& file : files)
					std::remove(file.c_str());
			}

			return;
		}

		ensureBufferAllocated();

		if (stream.is_open())
		{
			flush();
			stream.seekp(0, std::ios::end);
			if (!stream)
				throw std::runtime_error("Failed to seek to end of target stream");
		}
		else
		{
			if (path.empty())
				throw std::runtime_error("Target stream is not open and has no file path");

			stream = std::ofstream(path, std::ios::binary | std::ios::out | std::ios::app);
			check();
		}

		for (const auto& file : files)
		{
			std::ifstream input(file, std::ios::binary);
			if (!input.is_open())
				throw std::runtime_error("Failed to open source file for reading");

			while (input)
			{
				input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
				const std::streamsize bytesRead = input.gcount();

				if (bytesRead <= 0)
					break;

				stream.write(buffer.data(), bytesRead);
				if (!stream)
					throw std::ios_base::failure("Failed to write chunk to target stream");
			}

			if (deleteFiles)
			{
				input.close();
				std::remove(file.c_str());
			}
		}
	}

	void check() const
	{
		if (!enabled)
			return;

		if (!stream.is_open())
			throw std::invalid_argument("Stream is not open");
		if (!stream.good())
			throw std::ios_base::failure("Stream has an error");
	}

	template <typename T>
	void writeToBuffer(const T& t)
	{
		writeDataToBuffer(&t, 1);
	}

	void writeToBuffer(const char* data, const size_t byteCount)
	{
		writeDataToBuffer(data, byteCount);
	}

	template <typename T>
	void writeDataToBuffer(const T* data, const size_t elementCount)
	{
		if (!enabled || !stream.is_open() || data == nullptr || elementCount == 0)
			return;

		if (elementCount > max_size / sizeof(T))
			throw std::overflow_error("Overflow");

		const size_t byteCount = elementCount * sizeof(T);

		if (byteCount > buffer.size())
		{
			flush();

			const char* raw = reinterpret_cast<const char*>(data);
			size_t written = 0;
			while (written < byteCount)
			{
				const size_t chunkSize = std::min(buffer.size(), byteCount - written);
				stream.write(raw + written, static_cast<std::streamsize>(chunkSize));

				if (!stream)
					throw std::ios_base::failure("Failed to write large data directly to file");

				written += chunkSize;
			}

			return;
		}

		if (position + byteCount > buffer.size())
			flush();

		std::memcpy(buffer.data() + position, data, byteCount);
		position += byteCount;
	}

	void writeDataToBuffer(const char* data, const size_t byteCount)
	{
		if (!enabled || !stream.is_open() || data == nullptr || byteCount == 0)
			return;

		if (byteCount > buffer.size())
		{
			flush();

			size_t written = 0;
			while (written < byteCount)
			{
				const size_t chunkSize = std::min(buffer.size(), byteCount - written);
				stream.write(data + written, static_cast<std::streamsize>(chunkSize));

				if (!stream)
					throw std::ios_base::failure("Failed to write large data directly to file");

				written += chunkSize;
			}

			return;
		}

		if (position + byteCount > buffer.size())
			flush();

		std::memcpy(buffer.data() + position, data, byteCount);
		position += byteCount;
	}

	size_t overwriteAtPosition(const size_t pos, const char* data, const size_t byteCount)
	{
		if (!enabled)
			return pos;

		if (!stream.is_open())
			throw std::runtime_error("Stream is not set");

		if (data == nullptr || byteCount == 0)
			return pos;

		if (pos > max_size - byteCount)
			throw std::overflow_error("pos + byteCount overflow");

		const auto stream_end = stream_tell();

		// Disallow writing beyond logical end (stream_end + position)
		if (pos + byteCount > stream_end + position)
			throw std::out_of_range("writeAtPosition beyond logical end");

		// Case A: fully inside buffer -> memcpy
		if (pos >= stream_end && pos + byteCount <= stream_end + position)
		{
			const size_t offset = pos - stream_end;
			std::memcpy(buffer.data() + offset, data, byteCount);
			return pos + byteCount;
		}

		// Case B: fully inside stream
		if (pos + byteCount <= stream_end)
		{
			const size_t streamEndPosition = stream_end;

			stream.seekp(static_cast<std::streamoff>(pos), std::ios_base::beg);
			if (!stream)
				throw std::ios_base::failure("seekp to patch position failed");

			stream.write(data, static_cast<std::streamsize>(byteCount));
			if (!stream)
				throw std::ios_base::failure("patch write failed");

			// Restore the file cursor to the flushed stream end.
			// Buffered bytes still belong after stream_end and must not create a hole.
			stream.seekp(static_cast<std::streamoff>(streamEndPosition), std::ios_base::beg);
			if (!stream)
				throw std::ios_base::failure("seekp back to end failed");

			return pos + byteCount;
		}

		// Case C: overlaps -> split: left part to file, right part to buffer
		{
			const auto streamPartBytes = stream_end - pos;            // left part (fully in stream)
			const auto bufferPartBytes = byteCount - streamPartBytes; // right part (fully in buffer)

			const char* streamPart = data;
			const char* bufferPart = data + streamPartBytes;

			// Left part (inline Case B)
			if (streamPartBytes > 0)
			{
				const size_t streamEndPosition = stream_end;

				stream.seekp(static_cast<std::streamoff>(pos), std::ios_base::beg);
				if (!stream)
					throw std::ios_base::failure("seekp to patch position failed");

				stream.write(streamPart, static_cast<std::streamsize>(streamPartBytes));
				if (!stream)
					throw std::ios_base::failure("patch write failed");

				// Restore the file cursor to the flushed stream end.
				// Buffered bytes still belong after stream_end and must not create a hole.
				stream.seekp(static_cast<std::streamoff>(streamEndPosition), std::ios_base::beg);
				if (!stream)
					throw std::ios_base::failure("seekp back to end failed");
			}

			// Right part (inline Case A) - buffer window starts at stream_end
			if (bufferPartBytes > 0)
				std::memcpy(buffer.data(), bufferPart, bufferPartBytes);

			return pos + byteCount;
		}
	}

	template <typename T>
	size_t overwriteAtPosition(const size_t pos, const T& value)
	{
		static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
		return overwriteAtPosition(pos, reinterpret_cast<const char*>(&value), sizeof(T));
	}

	std::string path;
	std::ofstream stream;
	const size_t configured_size;
	std::vector<char> buffer;
	size_t position = 0;

public:

	inline static bool enabled = true;
};
