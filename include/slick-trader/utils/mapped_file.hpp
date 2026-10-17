#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace slick::trader {

// A file mapped into memory, read-write: state that has to outlive the process.
//
// Writing to it is a store to memory - no system call, nothing to wait for - so the thread
// that owns the state keeps it on disk without ever blocking on the disk. The operating
// system writes the pages out in its own time, and still does when the process dies: what
// was stored before a crash or a kill is in the file. Only the machine itself going down
// (power, kernel) can lose the last moments, which flush() narrows.
//
// Not thread-safe, and not for two processes at once: one owner.
class MappedFile {
public:
    MappedFile() = default;

    // Maps `path`, creating it and any missing directories. A new file is `size` bytes of
    // zeros; an existing one is grown, with zeros, if it is smaller, and otherwise mapped
    // whole. Throws std::runtime_error when the file cannot be opened or mapped.
    MappedFile(const std::string& path, std::size_t size);
    ~MappedFile();

    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    std::uint8_t* data() noexcept { return data_; }
    const std::uint8_t* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool is_open() const noexcept { return data_ != nullptr; }

    // True when the constructor created the file: there was nothing to restore.
    bool created() const noexcept { return created_; }

    // Asks the operating system to write the mapped pages to disk now. Blocks on the disk:
    // not for a latency-sensitive thread.
    void flush() noexcept;

    void close() noexcept;

private:
    std::uint8_t* data_{nullptr};
    std::size_t size_{0};
    bool created_{false};
#if defined(_WIN32) || defined(_WIN64)
    void* file_{nullptr};       // HANDLE
    void* mapping_{nullptr};    // HANDLE
#else
    int fd_{-1};
#endif
};

} // namespace slick::trader
