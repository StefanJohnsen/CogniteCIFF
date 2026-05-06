#pragma once

#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

struct TempFile
{
	TempFile(const std::filesystem::path& directory, const std::filesystem::path& file_name)
		: temp(directory / file_name)
	{
		if (directory.empty())
			throw std::invalid_argument("TempFile requires a directory");

		if (file_name.empty())
			throw std::invalid_argument("TempFile requires a file name");

		if (file_name != file_name.filename())
			throw std::invalid_argument("TempFile file name must not contain directories");

		if (!file_name.has_extension())
			throw std::invalid_argument("TempFile file name must include an extension");

		std::error_code ec;
		const auto parent = directory.parent_path();

		if (!parent.empty())
		{
			if (!std::filesystem::exists(parent, ec))
				throw std::runtime_error("TempFile parent directory does not exist");

			ec.clear();

			if (!std::filesystem::is_directory(parent, ec))
				throw std::runtime_error("TempFile parent path is not a directory");
		}

		ec.clear();

		if (std::filesystem::exists(directory, ec))
		{
			ec.clear();

			if (!std::filesystem::is_directory(directory, ec))
				throw std::runtime_error("TempFile directory path is not a directory");
		}
		else
		{
			ec.clear();
			std::filesystem::create_directories(directory, ec);

			if (ec)
				throw std::runtime_error("Failed to create TempFile directory");
		}

		ec.clear();

		if (!std::filesystem::exists(temp, ec))
			return;

		ec.clear();

		if (!std::filesystem::remove(temp, ec) || ec)
			throw std::runtime_error("Failed to remove existing TempFile");
	}

	~TempFile() noexcept
	{
		cleanup();
	}

	TempFile(const TempFile&) = delete;
	TempFile& operator=(const TempFile&) = delete;

	TempFile(TempFile&& other) noexcept
	{
		moveFrom(other);
	}

	TempFile& operator=(TempFile&& other) noexcept
	{
		if (this == &other)
			return *this;

		cleanup();
		moveFrom(other);
		return *this;
	}

	const std::filesystem::path& path() const noexcept
	{
		return temp;
	}

	void release() noexcept
	{
		remove_on_destroy = false;
	}

private:
	void cleanup() noexcept
	{
		if (!remove_on_destroy)
			return;

		std::error_code ec;

		if (!temp.empty())
			std::filesystem::remove(temp, ec);

		ec.clear();

		const auto directory = temp.parent_path();

		if (directory.empty())
			return;

		if (!std::filesystem::exists(directory, ec))
			return;

		ec.clear();

		if (!std::filesystem::is_directory(directory, ec))
			return;

		ec.clear();

		if (!std::filesystem::is_empty(directory, ec))
			return;

		ec.clear();
		std::filesystem::remove(directory, ec);
	}

	void moveFrom(TempFile& other) noexcept
	{
		temp = std::move(other.temp);
		remove_on_destroy = other.remove_on_destroy;

		other.temp.clear();
		other.remove_on_destroy = false;
	}

	std::filesystem::path temp;
	bool remove_on_destroy = true;
};
