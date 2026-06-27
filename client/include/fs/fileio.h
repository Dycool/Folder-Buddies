#pragma once

#include <cstdint>
#include <fcntl.h>
#include <string>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace fb {

class FileHandle {
public:
    FileHandle() = default;
    ~FileHandle() { close(); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    bool open(const std::string& path, int flags, uint32_t mode) {
#ifdef _WIN32
        int acc = flags & 3;
        DWORD access = (acc == 0) ? GENERIC_READ
                     : (acc == 1) ? GENERIC_WRITE
                                  : (GENERIC_READ | GENERIC_WRITE);
        bool creat = flags & O_CREAT;
        bool excl = flags & O_EXCL;
        bool trunc = flags & O_TRUNC;
        DWORD disp;
        if (creat) disp = excl ? CREATE_NEW : (trunc ? CREATE_ALWAYS : OPEN_ALWAYS);
        else disp = trunc ? TRUNCATE_EXISTING : OPEN_EXISTING;

        std::wstring wp = widen(path);
        h_ = ::CreateFileW(wp.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           disp, FILE_ATTRIBUTE_NORMAL, nullptr);
        (void)mode;
        return h_ != INVALID_HANDLE_VALUE;
#else
        fd_ = ::open(path.c_str(), flags, static_cast<mode_t>(mode));
        return fd_ >= 0;
#endif
    }

    // Returns bytes transferred, 0 at EOF, -1 on error.
    int64_t pread(void* buf, uint32_t n, uint64_t off) {
#ifdef _WIN32
        OVERLAPPED o{};
        o.Offset = static_cast<DWORD>(off);
        o.OffsetHigh = static_cast<DWORD>(off >> 32);
        DWORD got = 0;
        if (!::ReadFile(h_, buf, n, &got, &o)) {
            if (::GetLastError() == ERROR_HANDLE_EOF) return 0;
            return -1;
        }
        return got;
#else
        return ::pread(fd_, buf, n, static_cast<off_t>(off));
#endif
    }

    int64_t pwrite(const void* buf, uint32_t n, uint64_t off) {
#ifdef _WIN32
        OVERLAPPED o{};
        o.Offset = static_cast<DWORD>(off);
        o.OffsetHigh = static_cast<DWORD>(off >> 32);
        DWORD put = 0;
        if (!::WriteFile(h_, buf, n, &put, &o)) return -1;
        return put;
#else
        return ::pwrite(fd_, buf, n, static_cast<off_t>(off));
#endif
    }

    bool fsync_data() {
#ifdef _WIN32
        return ::FlushFileBuffers(h_) != 0;
#else
        return ::fsync(fd_) == 0;
#endif
    }

    bool valid() const {
#ifdef _WIN32
        return h_ != INVALID_HANDLE_VALUE;
#else
        return fd_ >= 0;
#endif
    }

    void close() {
#ifdef _WIN32
        if (h_ != INVALID_HANDLE_VALUE) { ::CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; }
#else
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
#endif
    }

private:
#ifdef _WIN32
    static std::wstring widen(const std::string& s) {
        int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n ? n - 1 : 0, L'\0');
        if (n) ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    }
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

} // namespace fb
