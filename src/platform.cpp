#include "platform.hpp"
#include <cerrno>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace platform {
#ifdef _WIN32

std::filesystem::path executablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            throw std::runtime_error("cannot determine executable path");
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::canonical(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
}

unsigned long processId() { return GetCurrentProcessId(); }

std::tm utcTime(std::time_t time) {
    std::tm result{};
    gmtime_s(&result, &time);
    return result;
}

int runProcess(const std::vector<std::filesystem::path> &argv) {
    std::wstring commandLine;
    for (const auto &argument : argv) {
        if (!commandLine.empty())
            commandLine += L' ';
        commandLine += L'"' + argument.wstring() + L'"';
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &startup, &process))
        throw std::runtime_error("cannot launch process: " + argv.front().string());
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
}

FileLock::FileLock(const std::filesystem::path &path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot open lock file: " + path.string());
    OVERLAPPED region{};
    acquired_ = LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                           &region) != 0;
    handle_ = reinterpret_cast<std::intptr_t>(file);
}

FileLock::~FileLock() {
    if (handle_ != -1)
        CloseHandle(reinterpret_cast<HANDLE>(handle_));
}

#else

std::filesystem::path executablePath() { return std::filesystem::canonical("/proc/self/exe"); }

unsigned long processId() { return static_cast<unsigned long>(getpid()); }

std::tm utcTime(std::time_t time) {
    std::tm result{};
    gmtime_r(&time, &result);
    return result;
}

int runProcess(const std::vector<std::filesystem::path> &argv) {
    std::vector<char *> args;
    for (const auto &argument : argv)
        args.push_back(const_cast<char *>(argument.c_str()));
    args.push_back(nullptr);
    pid_t pid;
    if (posix_spawn(&pid, args[0], nullptr, nullptr, args.data(), environ) != 0)
        throw std::runtime_error("cannot launch process: " + argv.front().string());
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            throw std::runtime_error("cannot wait for process");
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

FileLock::FileLock(const std::filesystem::path &path) {
    int fd = open(path.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd < 0)
        throw std::runtime_error("cannot open lock file: " + path.string());
    acquired_ = flock(fd, LOCK_EX | LOCK_NB) == 0;
    handle_ = fd;
}

FileLock::~FileLock() {
    if (handle_ != -1)
        close(static_cast<int>(handle_));
}

#endif
} // namespace platform
