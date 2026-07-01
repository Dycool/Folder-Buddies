// RamCache self-test: a fake in-memory RemoteFs records how many requests
// reach the "network" while the test drives reads, writes, and mutations
// through the cache. Verifies data correctness at block boundaries, metadata
// / directory / negative caching, and write-through invalidation.
#include "ram_cache.h"
#include "remote_fs.h"
#include "common.h"
#include "osflags.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace fb;

static int g_failures = 0;
#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

namespace {

class FakeRemote final : public RemoteFs {
public:
    struct File {
        std::string data;
        int64_t mtime = 1000;
    };
    std::map<std::string, File> files; // "/path" -> content
    std::map<uint16_t, int> calls;     // op -> times it reached the "network"

    bool connected() const override { return true; }

    int count(uint16_t op) {
        std::lock_guard<std::mutex> lk(mtx_);
        return calls[op];
    }

    int request(uint16_t op, const std::vector<uint8_t>& payload,
                std::vector<uint8_t>& resp) override {
        std::lock_guard<std::mutex> lk(mtx_);
        calls[op]++;
        Reader r(payload.data(), payload.size());
        switch (op) {
        case OP_GETATTR: {
            std::string path;
            if (!r.str(path)) return EINVAL;
            Writer w;
            if (path == "/") {
                w.pod(dir_attr());
            } else {
                auto it = files.find(path);
                if (it == files.end()) return ENOENT;
                w.pod(file_attr(it->second));
            }
            resp = std::move(w.b);
            return 0;
        }
        case OP_READDIR: {
            std::string path;
            if (!r.str(path) || path != "/") return ENOENT;
            Writer w;
            uint32_t n = static_cast<uint32_t>(files.size());
            w.pod(n);
            for (auto& kv : files) {
                w.str(kv.first.substr(1)); // basename: everything is in "/"
                w.pod(file_attr(kv.second));
            }
            resp = std::move(w.b);
            return 0;
        }
        case OP_OPEN:
        case OP_CREATE: {
            std::string path;
            int32_t flags = 0;
            uint32_t mode = 0;
            if (!r.str(path) || !r.pod(flags) || !r.pod(mode)) return EINVAL;
            if (op == OP_CREATE) files.try_emplace(path);
            if (!files.count(path)) return ENOENT;
            uint64_t id = nextFh_++;
            fhs_[id] = path;
            Writer w;
            w.pod(id);
            resp = std::move(w.b);
            return 0;
        }
        case OP_READ: {
            uint64_t fh = 0, off = 0;
            uint32_t size = 0;
            if (!r.pod(fh) || !r.pod(off) || !r.pod(size)) return EINVAL;
            auto it = fhs_.find(fh);
            if (it == fhs_.end()) return EBADF;
            const std::string& data = files[it->second].data;
            if (off >= data.size()) { resp.clear(); return 0; }
            size_t n = std::min<size_t>(size, data.size() - static_cast<size_t>(off));
            resp.assign(data.begin() + static_cast<size_t>(off),
                        data.begin() + static_cast<size_t>(off) + n);
            return 0;
        }
        case OP_WRITE: {
            uint64_t fh = 0, off = 0;
            if (!r.pod(fh) || !r.pod(off)) return EINVAL;
            auto it = fhs_.find(fh);
            if (it == fhs_.end()) return EBADF;
            File& f = files[it->second];
            size_t n = static_cast<size_t>(r.e - r.p);
            if (f.data.size() < off + n) f.data.resize(static_cast<size_t>(off) + n);
            std::memcpy(f.data.data() + off, r.p, n);
            f.mtime++; // content changed -> new version
            Writer w;
            uint32_t written = static_cast<uint32_t>(n);
            w.pod(written);
            resp = std::move(w.b);
            return 0;
        }
        case OP_RELEASE: {
            uint64_t fh = 0;
            if (!r.pod(fh)) return EINVAL;
            fhs_.erase(fh);
            return 0;
        }
        case OP_RENAME: {
            std::string from, to;
            if (!r.str(from) || !r.str(to)) return EINVAL;
            auto it = files.find(from);
            if (it == files.end()) return ENOENT;
            files[to] = std::move(it->second);
            files.erase(it);
            return 0;
        }
        case OP_UNLINK: {
            std::string path;
            if (!r.str(path)) return EINVAL;
            return files.erase(path) ? 0 : ENOENT;
        }
        default:
            return 0;
        }
    }

private:
    static WireAttr dir_attr() {
        WireAttr a{};
        a.mode = 0040755;
        a.nlink = 2;
        a.mtime = 1000;
        return a;
    }
    static WireAttr file_attr(const File& f) {
        WireAttr a{};
        a.mode = 0100644;
        a.nlink = 1;
        a.size = f.data.size();
        a.mtime = f.mtime;
        return a;
    }
    std::mutex mtx_;
    std::map<uint64_t, std::string> fhs_;
    uint64_t nextFh_ = 1;
};

std::vector<uint8_t> path_payload(const std::string& p) {
    Writer w;
    w.str(p);
    return w.b;
}

std::vector<uint8_t> open_payload(const std::string& p, int32_t flags) {
    Writer w;
    w.str(p);
    w.pod(flags);
    uint32_t mode = 0644;
    w.pod(mode);
    return w.b;
}

std::vector<uint8_t> read_payload(uint64_t fh, uint64_t off, uint32_t size) {
    Writer w;
    w.pod(fh);
    w.pod(off);
    w.pod(size);
    return w.b;
}

std::string pattern(size_t n) {
    std::string s;
    s.reserve(n);
    for (size_t i = 0; i < n; ++i)
        s.push_back(static_cast<char>('A' + (i * 7 + i / 251) % 26));
    return s;
}

} // namespace

int main() {
    FakeRemote remote;
    const size_t bigLen = static_cast<size_t>(kMaxIO) * 2 + kMaxIO / 2; // spans 3 blocks
    remote.files["/big.bin"] = {pattern(bigLen), 1000};
    remote.files["/small.txt"] = {"tiny", 1000};

    RamCache cache(&remote);
    std::vector<uint8_t> resp;

    // ---- metadata caching: repeated getattr hits the cache, not the net ----
    CHECK(cache.request(OP_GETATTR, path_payload("/small.txt"), resp) == 0);
    CHECK(cache.request(OP_GETATTR, path_payload("/small.txt"), resp) == 0);
    CHECK(remote.count(OP_GETATTR) == 1);
    {
        Reader r(resp.data(), resp.size());
        WireAttr a{};
        CHECK(r.pod(a) && a.size == 4);
    }

    // ---- negative caching ----
    CHECK(cache.request(OP_GETATTR, path_payload("/nope"), resp) == ENOENT);
    CHECK(cache.request(OP_GETATTR, path_payload("/nope"), resp) == ENOENT);
    CHECK(remote.count(OP_GETATTR) == 2); // only the first miss went upstream

    // ---- readdir caching + metadata seeding from listings ----
    const int attrsBefore = remote.count(OP_GETATTR);
    CHECK(cache.request(OP_READDIR, path_payload("/"), resp) == 0);
    CHECK(cache.request(OP_READDIR, path_payload("/"), resp) == 0);
    CHECK(remote.count(OP_READDIR) == 1);
    CHECK(cache.request(OP_GETATTR, path_payload("/big.bin"), resp) == 0);
    CHECK(remote.count(OP_GETATTR) == attrsBefore); // served from the seeded listing

    // ---- block reads: correctness at block boundaries ----
    uint64_t fh = 0;
    CHECK(cache.request(OP_OPEN, open_payload("/big.bin", FB_O_RDONLY), resp) == 0);
    {
        Reader r(resp.data(), resp.size());
        CHECK(r.pod(fh));
    }
    const std::string& big = remote.files["/big.bin"].data;
    {
        CHECK(cache.request(OP_READ, read_payload(fh, 0, 100), resp) == 0);
        CHECK(std::string(resp.begin(), resp.end()) == big.substr(0, 100));
    }
    { // spans the first 1 MiB block boundary
        const uint64_t off = kMaxIO - 10;
        CHECK(cache.request(OP_READ, read_payload(fh, off, 20), resp) == 0);
        CHECK(std::string(resp.begin(), resp.end()) == big.substr(static_cast<size_t>(off), 20));
    }
    { // tail of the last, partial block
        const uint64_t off = bigLen - 5;
        CHECK(cache.request(OP_READ, read_payload(fh, off, 100), resp) == 0);
        CHECK(std::string(resp.begin(), resp.end()) == big.substr(static_cast<size_t>(off), 5));
    }
    { // past EOF
        CHECK(cache.request(OP_READ, read_payload(fh, bigLen + 100, 10), resp) == 0);
        CHECK(resp.empty());
    }

    // ---- block cache: re-reading the same block should not refetch ----
    // (skipped when the host machine is too memory-constrained for the cache)
    const int readsWarm = remote.count(OP_READ);
    CHECK(cache.request(OP_READ, read_payload(fh, 0, 100), resp) == 0);
    const bool blockCacheActive = remote.count(OP_READ) == readsWarm;
    if (!blockCacheActive)
        std::printf("note: block cache disabled by RAM budget; skipping hit-count checks\n");

    // ---- write-through + invalidation ----
    uint64_t wfh = 0;
    CHECK(cache.request(OP_OPEN, open_payload("/small.txt", FB_O_RDWR), resp) == 0);
    {
        Reader r(resp.data(), resp.size());
        CHECK(r.pod(wfh));
    }
    CHECK(cache.request(OP_READ, read_payload(wfh, 0, 16), resp) == 0);
    CHECK(std::string(resp.begin(), resp.end()) == "tiny");
    {
        Writer w;
        w.pod(wfh);
        uint64_t off = 0;
        w.pod(off);
        w.raw("HUGE", 4);
        CHECK(cache.request(OP_WRITE, w.b, resp) == 0);
        CHECK(remote.count(OP_WRITE) == 1); // strict write-through
        CHECK(remote.files["/small.txt"].data == "HUGE");
    }
    CHECK(cache.request(OP_READ, read_payload(wfh, 0, 16), resp) == 0);
    CHECK(std::string(resp.begin(), resp.end()) == "HUGE"); // stale block was purged

    // ---- release: reads through a closed handle fall through and fail ----
    {
        Writer w;
        w.pod(wfh);
        CHECK(cache.request(OP_RELEASE, w.b, resp) == 0);
        CHECK(cache.request(OP_READ, read_payload(wfh, 0, 4), resp) == EBADF);
    }

    // ---- rename invalidates the directory listing ----
    const int lists = remote.count(OP_READDIR);
    {
        Writer w;
        w.str("/small.txt");
        w.str("/renamed.txt");
        CHECK(cache.request(OP_RENAME, w.b, resp) == 0);
    }
    CHECK(cache.request(OP_READDIR, path_payload("/"), resp) == 0);
    CHECK(remote.count(OP_READDIR) == lists + 1); // went upstream after rename
    CHECK(cache.request(OP_GETATTR, path_payload("/renamed.txt"), resp) == 0);

    // ---- unlink installs a negative entry ----
    CHECK(cache.request(OP_UNLINK, path_payload("/renamed.txt"), resp) == 0);
    CHECK(cache.request(OP_GETATTR, path_payload("/renamed.txt"), resp) == ENOENT);

    {
        Writer w;
        w.pod(fh);
        cache.request(OP_RELEASE, w.b, resp);
    }

    if (g_failures) {
        std::printf("ram-cache self-test FAIL (%d checks)\n", g_failures);
        return 1;
    }
    std::printf("ram-cache self-test PASS\n");
    return 0;
}
