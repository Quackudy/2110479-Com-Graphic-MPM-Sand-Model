#pragma once
// The few OS-specific operations the project needs, so no other file includes
// POSIX or Windows headers.
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <vector>

namespace platform {
std::filesystem::path executablePath();
unsigned long processId();
// Thread-safe UTC broken-down time.
std::tm utcTime(std::time_t time);
// Runs argv[0] with the remaining arguments, waits, and returns its exit code
// (1 if it did not exit normally). Throws if the process cannot be started.
// Arguments must not contain double quotes (always true for file paths on Windows).
int runProcess(const std::vector<std::filesystem::path> &argv);

// Exclusive advisory lock on a file, released on destruction or process exit.
// The constructor throws if the file cannot be opened; acquired() is false when
// another process already holds the lock.
class FileLock {
  public:
    explicit FileLock(const std::filesystem::path &path);
    ~FileLock();
    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;
    bool acquired() const { return acquired_; }

  private:
    std::intptr_t handle_ = -1;
    bool acquired_ = false;
};
} // namespace platform
