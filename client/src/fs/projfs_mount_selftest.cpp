#ifdef _WIN32

#include "common.h"
#include "fuse_backend.h"
#include "fuse_fs.h"
#include "remote_fs.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cerrno>
#include <filesystem>
#include <functional>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

class EmptyRemoteFs final : public fb::RemoteFs {
public:
    bool connected() const override { return true; }

    int request(uint16_t op, const std::vector<uint8_t>&,
                std::vector<uint8_t>& resp) override {
        fb::Writer w;
        if (op == fb::OP_READDIR) {
            uint32_t count = 0;
            w.pod(count);
        } else if (op == fb::OP_GETATTR) {
            fb::WireAttr attr{};
            attr.ino = 1;
            attr.mode = 0040755;
            attr.nlink = 2;
            w.pod(attr);
        } else if (op == fb::OP_STATFS) {
            fb::WireStatvfs stat{};
            stat.bsize = stat.frsize = 4096;
            stat.blocks = stat.bfree = stat.bavail = 1024;
            stat.files = stat.ffree = 1024;
            stat.namemax = 255;
            w.pod(stat);
        } else {
            return ENOENT;
        }
        resp = std::move(w.b);
        return 0;
    }
};

std::vector<std::string> first_free_letters(size_t count) {
    std::vector<std::string> out;
    DWORD mask = ::GetLogicalDrives();
    for (char c = 'D'; c <= 'Z' && out.size() < count; ++c) {
        if (mask & (1u << (c - 'A'))) continue;
        std::wstring name{static_cast<wchar_t>(c), L':'};
        wchar_t target[32768];
        if (::QueryDosDeviceW(name.c_str(), target,
                              static_cast<DWORD>(std::size(target))) != 0)
            continue;
        out.push_back(std::string(1, c) + ":\\");
    }
    return out;
}

}

int main() {
    if (!fb::projfs_available()) {
        std::cout << "SKIP: ProjFS is not enabled\n";
        return 77;
    }

    std::vector<std::string> expected = first_free_letters(2);
    if (expected.size() != 2) {
        std::cout << "SKIP: fewer than two drive letters are available\n";
        return 77;
    }

    std::filesystem::path base = std::filesystem::temp_directory_path() /
        ("FolderBuddiesDriveAllocationSelftest-" + std::to_string(::GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    if (ec) {
        std::cerr << "failed to create self-test directory: " << ec.message() << '\n';
        return 1;
    }

    EmptyRemoteFs remoteA;
    EmptyRemoteFs remoteB;
    fb::Mount mountA;
    fb::Mount mountB;
    std::atomic_bool go{false};
    bool okA = false;
    bool okB = false;
    std::string errA;
    std::string errB;

    auto start = [&](fb::Mount& mount, EmptyRemoteFs& remote, const char* label,
                     bool& ok, std::string& err) {
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        ok = mount.start(&remote, base.string(), label, false, err);
    };

    std::thread a(start, std::ref(mountA), std::ref(remoteA), "Allocation A",
                  std::ref(okA), std::ref(errA));
    std::thread b(start, std::ref(mountB), std::ref(remoteB), "Allocation B",
                  std::ref(okB), std::ref(errB));
    go.store(true, std::memory_order_release);
    a.join();
    b.join();

    std::set<std::string> actual;
    if (okA) actual.insert(mountA.mountpoint());
    if (okB) actual.insert(mountB.mountpoint());
    std::set<std::string> wanted(expected.begin(), expected.end());

    bool passed = okA && okB && actual == wanted;
    if (!passed) {
        std::cerr << "mount A: " << (okA ? mountA.mountpoint() : errA) << '\n'
                  << "mount B: " << (okB ? mountB.mountpoint() : errB) << '\n'
                  << "expected: " << expected[0] << ", " << expected[1] << '\n';
    } else {
        std::cout << "PASS: simultaneous mounts used " << expected[0]
                  << " and " << expected[1] << '\n';
    }

    if (okA) mountA.stop();
    if (okB) mountB.stop();
    std::filesystem::remove_all(base, ec);
    return passed ? 0 : 1;
}

#endif
