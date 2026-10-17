#include <slick-trader/utils/mapped_file.hpp>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace slick::trader {

namespace {

[[noreturn]] void fail(const std::string& path, const char* what, long long code) {
    throw std::runtime_error("MappedFile: cannot " + std::string(what) + " " + path + " (error " +
                             std::to_string(code) + ")");
}

}   // namespace

MappedFile::MappedFile(const std::string& path, std::size_t size) {
    if (size == 0) {
        throw std::runtime_error("MappedFile: " + path + " needs a size");
    }
    const std::filesystem::path file_path(path);
    if (file_path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(file_path.parent_path(), ec);   // open() reports a failure
    }

#if defined(_WIN32) || defined(_WIN64)
    HANDLE file = CreateFileW(file_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        fail(path, "open", GetLastError());
    }
    created_ = GetLastError() != ERROR_ALREADY_EXISTS;

    LARGE_INTEGER existing{};
    if (!GetFileSizeEx(file, &existing)) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        fail(path, "size", error);
    }
    size_ = static_cast<std::uint64_t>(existing.QuadPart) > size ? static_cast<std::size_t>(existing.QuadPart) : size;

    // A mapping larger than the file extends it, with zeros.
    const auto wide = static_cast<std::uint64_t>(size_);
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, static_cast<DWORD>(wide >> 32),
                                        static_cast<DWORD>(wide & 0xFFFFFFFFu), nullptr);
    if (!mapping) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        fail(path, "map", error);
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size_);
    if (!view) {
        const DWORD error = GetLastError();
        CloseHandle(mapping);
        CloseHandle(file);
        fail(path, "map", error);
    }
    file_ = file;
    mapping_ = mapping;
    data_ = static_cast<std::uint8_t*>(view);
#else
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
    created_ = fd >= 0;
    if (fd < 0 && errno == EEXIST) {
        fd = ::open(path.c_str(), O_RDWR);
    }
    if (fd < 0) {
        fail(path, "open", errno);
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        const int error = errno;
        ::close(fd);
        fail(path, "size", error);
    }
    size_ = static_cast<std::size_t>(info.st_size) > size ? static_cast<std::size_t>(info.st_size) : size;
    if (static_cast<std::size_t>(info.st_size) < size_ && ::ftruncate(fd, static_cast<off_t>(size_)) != 0) {
        const int error = errno;
        ::close(fd);
        fail(path, "grow", error);
    }
    void* view = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED) {
        const int error = errno;
        ::close(fd);
        fail(path, "map", error);
    }
    fd_ = fd;
    data_ = static_cast<std::uint8_t*>(view);
#endif
}

MappedFile::~MappedFile() {
    close();
}

MappedFile::MappedFile(MappedFile&& other) noexcept {
    *this = std::move(other);
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        close();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        created_ = std::exchange(other.created_, false);
#if defined(_WIN32) || defined(_WIN64)
        file_ = std::exchange(other.file_, nullptr);
        mapping_ = std::exchange(other.mapping_, nullptr);
#else
        fd_ = std::exchange(other.fd_, -1);
#endif
    }
    return *this;
}

void MappedFile::flush() noexcept {
    if (!data_) {
        return;
    }
#if defined(_WIN32) || defined(_WIN64)
    FlushViewOfFile(data_, size_);
    FlushFileBuffers(static_cast<HANDLE>(file_));
#else
    ::msync(data_, size_, MS_SYNC);
#endif
}

void MappedFile::close() noexcept {
    if (!data_) {
        return;
    }
#if defined(_WIN32) || defined(_WIN64)
    UnmapViewOfFile(data_);
    CloseHandle(static_cast<HANDLE>(mapping_));
    CloseHandle(static_cast<HANDLE>(file_));
    mapping_ = nullptr;
    file_ = nullptr;
#else
    ::munmap(data_, size_);
    ::close(fd_);
    fd_ = -1;
#endif
    data_ = nullptr;
    size_ = 0;
}

} // namespace slick::trader
