#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
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
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace conversion
{
    namespace detail
    {
        inline std::filesystem::path resolveTarget(const std::filesystem::path& target)
        {
            if (target.empty())
                throw std::invalid_argument("Conversion output target is empty");

            auto error = std::error_code{};
            auto absolute = std::filesystem::absolute(target, error);
            if (error)
                throw std::system_error(error, "Failed to resolve conversion output target");

            absolute = absolute.lexically_normal();
            if (absolute.filename().empty())
                throw std::invalid_argument("Conversion output target has no file name");

            const auto parent = absolute.parent_path();
            if (parent.empty())
                throw std::invalid_argument("Conversion output target has no parent directory");

            error.clear();
            if (!std::filesystem::is_directory(parent, error) || error)
                throw std::runtime_error("Conversion output directory does not exist");

            error.clear();
            const auto targetExists = std::filesystem::exists(absolute, error);
            if (error)
                throw std::system_error(error, "Failed to inspect conversion output target");
            if (targetExists)
            {
                error.clear();
                const auto regularTarget = std::filesystem::is_regular_file(absolute, error);
                if (error)
                    throw std::system_error(error, "Failed to inspect conversion output target");
                if (!regularTarget)
                    throw std::invalid_argument("Conversion output target is not a regular file");
            }

            return absolute;
        }

        inline void replaceFile(const std::filesystem::path& source, const std::filesystem::path& target,
                                const char* message)
        {
#if defined(_WIN32)
            if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), message);
            }
#else
            auto error = std::error_code{};
            std::filesystem::rename(source, target, error);
            if (error)
                throw std::system_error(error, message);
#endif
        }

        inline void flushFile(const std::filesystem::path& path)
        {
#if defined(_WIN32)
            const auto file =
                CreateFileW(path.c_str(), GENERIC_WRITE, 0U, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                        "Failed to reopen atomic conversion output");
            }
            if (!FlushFileBuffers(file))
            {
                const auto error = GetLastError();
                CloseHandle(file);
                throw std::system_error(static_cast<int>(error), std::system_category(),
                                        "Failed to flush atomic conversion output");
            }
            if (!CloseHandle(file))
            {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                        "Failed to close atomic conversion output");
            }
#else
            const auto file = ::open(path.c_str(), O_WRONLY);
            if (file < 0)
                throw std::system_error(errno, std::generic_category(), "Failed to reopen atomic conversion output");
            if (::fsync(file) != 0)
            {
                const auto error = errno;
                ::close(file);
                throw std::system_error(error, std::generic_category(), "Failed to flush atomic conversion output");
            }
            if (::close(file) != 0)
                throw std::system_error(errno, std::generic_category(), "Failed to close atomic conversion output");
#endif
        }

        inline bool sameTarget(const std::filesystem::path& left, const std::filesystem::path& right) noexcept
        {
#if defined(_WIN32)
            return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
            return left == right;
#endif
        }
    } // namespace detail

    class AtomicFile final
    {
      public:
        explicit AtomicFile(const std::filesystem::path& target) : target_(detail::resolveTarget(target))
        {
            create();
        }

        [[nodiscard]] static AtomicFile create(const std::filesystem::path& target)
        {
            return AtomicFile(target);
        }

        AtomicFile(const AtomicFile&) = delete;
        AtomicFile& operator=(const AtomicFile&) = delete;

        AtomicFile(AtomicFile&& other) noexcept
            : target_(std::move(other.target_)), path_(std::move(other.path_)),
              remove_(std::exchange(other.remove_, false)), flushed_(std::exchange(other.flushed_, false)),
              handle_(std::exchange(other.handle_, InvalidHandle))
        {
        }

        AtomicFile& operator=(AtomicFile&& other) noexcept
        {
            if (this == &other)
                return *this;
            cleanup();
            target_ = std::move(other.target_);
            path_ = std::move(other.path_);
            remove_ = std::exchange(other.remove_, false);
            flushed_ = std::exchange(other.flushed_, false);
            handle_ = std::exchange(other.handle_, InvalidHandle);
            return *this;
        }

        ~AtomicFile() noexcept
        {
            cleanup();
        }

        [[nodiscard]] const std::filesystem::path& path() const noexcept
        {
            return path_;
        }

        [[nodiscard]] const std::filesystem::path& target() const noexcept
        {
            return target_;
        }

        void write(const std::span<const uint8_t> bytes)
        {
            if (handle_ == InvalidHandle)
                throw std::logic_error("Atomic conversion output is closed");

            auto offset = size_t{};
            while (offset < bytes.size())
            {
#if defined(_WIN32)
                const auto request =
                    static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, (std::numeric_limits<DWORD>::max)()));
                auto written = DWORD{};
                if (!WriteFile(handle_, bytes.data() + offset, request, &written, nullptr))
                {
                    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                            "Failed to write atomic conversion output");
                }
                if (written == 0U)
                    throw std::runtime_error("Failed to write complete atomic conversion output");
                offset += written;
#else
                const auto result = ::write(handle_, bytes.data() + offset, bytes.size() - offset);
                if (result < 0)
                {
                    if (errno == EINTR)
                        continue;
                    throw std::system_error(errno, std::generic_category(), "Failed to write atomic conversion output");
                }
                if (result == 0)
                    throw std::runtime_error("Failed to write complete atomic conversion output");
                offset += static_cast<size_t>(result);
#endif
            }
            flushed_ = false;
        }

        void flush()
        {
            if (handle_ == InvalidHandle)
                throw std::logic_error("Atomic conversion output is closed");
#if defined(_WIN32)
            if (!FlushFileBuffers(handle_))
            {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                        "Failed to flush atomic conversion output");
            }
#else
            if (::fsync(handle_) != 0)
                throw std::system_error(errno, std::generic_category(), "Failed to flush atomic conversion output");
#endif
            flushed_ = true;
        }

        void close()
        {
            if (handle_ == InvalidHandle)
                return;
            const auto file = std::exchange(handle_, InvalidHandle);
#if defined(_WIN32)
            if (!CloseHandle(file))
            {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                        "Failed to close atomic conversion output");
            }
#else
            if (::close(file) != 0)
                throw std::system_error(errno, std::generic_category(), "Failed to close atomic conversion output");
#endif
        }

        void publish()
        {
            if (handle_ != InvalidHandle)
            {
                if (!flushed_)
                    flush();
                close();
            }

            auto error = std::error_code{};
            if (!std::filesystem::is_regular_file(path_, error) || error)
                throw std::runtime_error("Atomic conversion output file does not exist");

            if (!flushed_)
            {
                detail::flushFile(path_);
                flushed_ = true;
            }

            detail::replaceFile(path_, target_, "Failed to publish atomic conversion output");
            remove_ = false;
        }

      private:
#if defined(_WIN32)
        using NativeHandle = HANDLE;
        static inline const NativeHandle InvalidHandle = INVALID_HANDLE_VALUE;
#else
        using NativeHandle = int;
        static constexpr NativeHandle InvalidHandle = -1;
#endif

        void create()
        {
            static auto serial = std::atomic_uint64_t{0U};
            const auto allocation = serial.fetch_add(1U, std::memory_order_relaxed);
#if defined(_WIN32)
            const auto process = static_cast<uint64_t>(GetCurrentProcessId());
#else
            const auto process = static_cast<uint64_t>(::getpid());
#endif
            for (size_t suffix = 0U; suffix < 10000U; ++suffix)
            {
                auto candidate = target_;
                candidate +=
                    ".tmp." + std::to_string(process) + "." + std::to_string(allocation) + "." + std::to_string(suffix);
#if defined(_WIN32)
                const auto file = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_NEW,
                                              FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE)
                {
                    path_ = std::move(candidate);
                    handle_ = file;
                    return;
                }
                const auto error = GetLastError();
                if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
                    continue;
                throw std::system_error(static_cast<int>(error), std::system_category(),
                                        "Failed to create atomic conversion output");
#else
                const auto file = ::open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
                if (file >= 0)
                {
                    path_ = std::move(candidate);
                    handle_ = file;
                    return;
                }
                if (errno == EEXIST)
                    continue;
                throw std::system_error(errno, std::generic_category(), "Failed to create atomic conversion output");
#endif
            }
            throw std::runtime_error("Failed to allocate atomic conversion output");
        }

        void closeNoThrow() noexcept
        {
            if (handle_ == InvalidHandle)
                return;
            const auto file = std::exchange(handle_, InvalidHandle);
#if defined(_WIN32)
            CloseHandle(file);
#else
            ::close(file);
#endif
        }

        void cleanup() noexcept
        {
            closeNoThrow();
            if (!remove_ || path_.empty())
                return;
            auto error = std::error_code{};
            std::filesystem::remove(path_, error);
        }

        std::filesystem::path target_;
        std::filesystem::path path_;
        bool remove_ = true;
        bool flushed_ = false;
        NativeHandle handle_ = InvalidHandle;
    };

    class Workspace final
    {
      public:
        explicit Workspace(const std::filesystem::path& target) : target_(detail::resolveTarget(target))
        {
            createUniqueDirectory();
            try
            {
                result_ = directory_ / target_.filename();
            }
            catch (...)
            {
                cleanup();
                throw;
            }
        }

        Workspace(const std::filesystem::path& target, const size_t partCount) : Workspace(target)
        {
            parts_.reserve(partCount);
            const auto partDirectory = directory_ / "parts";
            auto error = std::error_code{};
            if (!std::filesystem::create_directory(partDirectory, error))
            {
                if (error)
                    throw std::system_error(error, "Failed to create conversion parts directory");
                throw std::runtime_error("Conversion parts directory already exists");
            }

            const auto extension = target_.extension();
            for (size_t index = 0U; index < partCount; ++index)
            {
                auto partName = std::filesystem::path{"part-" + std::to_string(index)};
                partName += extension;
                parts_.emplace_back(partDirectory / partName);
            }
        }

        Workspace(const Workspace&) = delete;
        Workspace& operator=(const Workspace&) = delete;

        Workspace(Workspace&& other) noexcept
            : target_(std::move(other.target_)), directory_(std::move(other.directory_)),
              result_(std::move(other.result_)), parts_(std::move(other.parts_)),
              remove_(std::exchange(other.remove_, false))
        {
            other.directory_.clear();
        }

        Workspace& operator=(Workspace&& other) noexcept
        {
            if (this == &other)
                return *this;
            cleanup();
            target_ = std::move(other.target_);
            directory_ = std::move(other.directory_);
            result_ = std::move(other.result_);
            parts_ = std::move(other.parts_);
            remove_ = std::exchange(other.remove_, false);
            other.directory_.clear();
            return *this;
        }

        ~Workspace() noexcept
        {
            cleanup();
        }

        [[nodiscard]] const std::filesystem::path& target() const noexcept
        {
            return target_;
        }

        [[nodiscard]] const std::filesystem::path& directory() const noexcept
        {
            return directory_;
        }

        [[nodiscard]] const std::filesystem::path& result() const noexcept
        {
            return result_;
        }

        [[nodiscard]] const std::filesystem::path& part(const size_t index) const
        {
            return parts_.at(index);
        }

        [[nodiscard]] std::filesystem::path file(const std::filesystem::path& relative) const
        {
            if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
                throw std::invalid_argument("Conversion workspace file must be relative");

            const auto normalized = relative.lexically_normal();
            if (normalized.empty() || normalized == ".")
                throw std::invalid_argument("Conversion workspace file name is empty");
            for (const auto& component : normalized)
            {
                if (component == "..")
                    throw std::invalid_argument("Conversion workspace file lies outside the workspace");
            }

            const auto path = (directory_ / normalized).lexically_normal();
            const auto workspaceRelative = path.lexically_relative(directory_);
            if (workspaceRelative.empty() || workspaceRelative.is_absolute())
                throw std::invalid_argument("Conversion workspace file lies outside the workspace");
            for (const auto& component : workspaceRelative)
            {
                if (component == "..")
                    throw std::invalid_argument("Conversion workspace file lies outside the workspace");
            }
            const auto parent = path.parent_path();
            if (parent != directory_)
            {
                auto error = std::error_code{};
                std::filesystem::create_directories(parent, error);
                if (error)
                    throw std::system_error(error, "Failed to create conversion workspace directory");
            }
            return path;
        }

        void publish(const std::vector<std::filesystem::path>& stagedSidecars = {})
        {
            auto error = std::error_code{};
            if (!std::filesystem::is_regular_file(result_, error) || error)
                throw std::runtime_error("Conversion workspace result does not exist");

            if (stagedSidecars.empty())
            {
                detail::replaceFile(result_, target_, "Failed to publish conversion output");
                return;
            }

            auto sidecars = std::vector<SidecarPublication>{};
            sidecars.reserve(stagedSidecars.size());
            auto backupDirectory = std::filesystem::path{};
            for (size_t index = 0U; index < stagedSidecars.size(); ++index)
            {
                error.clear();
                auto staged = std::filesystem::absolute(stagedSidecars[index], error).lexically_normal();
                if (error)
                    throw std::system_error(error, "Failed to resolve conversion sidecar path");
                if (staged.parent_path() != directory_ || staged.filename().empty())
                    throw std::invalid_argument("Conversion sidecar lies outside the workspace");
                if (!std::filesystem::is_regular_file(staged, error) || error)
                    throw std::runtime_error("Conversion sidecar does not exist");

                auto sidecarTarget = target_.parent_path() / staged.filename();
                if (detail::sameTarget(sidecarTarget, target_))
                    throw std::invalid_argument("Conversion sidecar overlaps the primary target");
                for (const auto& existing : sidecars)
                {
                    if (detail::sameTarget(existing.target, sidecarTarget))
                        throw std::invalid_argument("Conversion sidecar target is duplicated");
                }

                auto publication = SidecarPublication{
                    .staged = std::move(staged),
                    .target = std::move(sidecarTarget),
                };
                error.clear();
                const auto targetStatus = std::filesystem::symlink_status(publication.target, error);
                if (error)
                {
                    if (error != std::errc::no_such_file_or_directory)
                        throw std::system_error(error, "Failed to inspect conversion sidecar target");
                    error.clear();
                }
                const auto targetExists = !error && std::filesystem::exists(targetStatus);
                if (targetExists)
                {
                    error.clear();
                    const auto regularTarget = std::filesystem::is_regular_file(publication.target, error);
                    if (error)
                        throw std::system_error(error, "Failed to inspect conversion sidecar target");
                    if (!regularTarget)
                        throw std::runtime_error("Conversion sidecar target is not a regular file");
                    if (backupDirectory.empty())
                    {
                        backupDirectory = directory_ / "publish-backups";
                        error.clear();
                        if (!std::filesystem::create_directory(backupDirectory, error))
                        {
                            if (error)
                                throw std::system_error(error, "Failed to create conversion backup directory");
                            throw std::runtime_error("Conversion backup directory already exists");
                        }
                    }
                    publication.backup = backupDirectory / ("sidecar-" + std::to_string(index) + ".tmp");
                }
                sidecars.emplace_back(std::move(publication));
            }

            try
            {
                for (auto& sidecar : sidecars)
                {
                    if (!sidecar.backup.empty())
                    {
                        detail::replaceFile(sidecar.target, sidecar.backup,
                                            "Failed to back up conversion sidecar target");
                        sidecar.backedUp = true;
                    }
                    detail::replaceFile(sidecar.staged, sidecar.target, "Failed to publish conversion sidecar");
                    sidecar.published = true;
                }
                detail::replaceFile(result_, target_, "Failed to publish conversion output");
            }
            catch (...)
            {
                const auto publishError = std::current_exception();
                auto rollbackError = std::string{};
                for (auto sidecar = sidecars.rbegin(); sidecar != sidecars.rend(); ++sidecar)
                {
                    if (!sidecar->published && !sidecar->backedUp)
                        continue;
                    try
                    {
                        if (sidecar->backedUp)
                        {
                            detail::replaceFile(sidecar->backup, sidecar->target,
                                                "Failed to restore conversion sidecar target");
                        }
                        else
                        {
                            error.clear();
                            std::filesystem::remove(sidecar->target, error);
                            if (error)
                                throw std::system_error(error, "Failed to remove published conversion sidecar");
                        }
                    }
                    catch (const std::exception& exception)
                    {
                        if (rollbackError.empty())
                            rollbackError = exception.what();
                    }
                    catch (...)
                    {
                        if (rollbackError.empty())
                            rollbackError = "unknown rollback error";
                    }
                }

                if (!rollbackError.empty())
                {
                    remove_ = false;
                    throw std::runtime_error("Conversion output publish and sidecar rollback failed; "
                                             "workspace retained for recovery: " +
                                             rollbackError);
                }
                std::rethrow_exception(publishError);
            }
        }

      private:
        struct SidecarPublication final
        {
            std::filesystem::path staged;
            std::filesystem::path target;
            std::filesystem::path backup;
            bool backedUp = false;
            bool published = false;
        };

        void createUniqueDirectory()
        {
            constexpr auto MaxAttempts = size_t{128U};
            const auto parent = target_.parent_path();
            for (size_t attempt = 0U; attempt < MaxAttempts; ++attempt)
            {
                const auto clock = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
                auto suffix = std::ostringstream{};
                suffix << std::hex << clock;

                auto stem = target_.stem().empty() ? std::filesystem::path{"output"}.native() : target_.stem().native();
                constexpr auto MaximumStemLength = size_t{80U};
                if (stem.size() > MaximumStemLength)
                    stem.resize(MaximumStemLength);

                auto name = std::filesystem::path{".conversion-"};
                name += stem;
                name += std::filesystem::path{"-" + suffix.str()}.native();
                if (attempt != 0U)
                    name += std::filesystem::path{"-" + std::to_string(attempt)}.native();
                auto candidate = parent / name;

                auto error = std::error_code{};
                if (std::filesystem::create_directory(candidate, error))
                {
                    directory_ = std::move(candidate);
                    return;
                }
                if (error && error != std::errc::file_exists)
                    throw std::system_error(error, "Failed to create conversion workspace");
            }
            throw std::runtime_error("Failed to allocate a unique conversion workspace");
        }

        void cleanup() noexcept
        {
            if (!remove_ || directory_.empty())
                return;
            auto error = std::error_code{};
            std::filesystem::remove_all(directory_, error);
        }

        std::filesystem::path target_;
        std::filesystem::path directory_;
        std::filesystem::path result_;
        std::vector<std::filesystem::path> parts_;
        bool remove_ = true;
    };
} // namespace conversion
