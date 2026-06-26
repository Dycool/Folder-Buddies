#include "ram_cache.h"

#include "client.h"
#include "common.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <list>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <cerrno>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach/mach.h>
#  include <sys/sysctl.h>
#  include <sys/types.h>
#  include <unistd.h>
#elif defined(__linux__)
#  include <sys/sysinfo.h>
#endif

namespace fb {
namespace {

using clock_t_ = std::chrono::steady_clock;
using ms = std::chrono::milliseconds;

constexpr uint64_t kBlock = kMaxIO;          // 1 MiB block granularity (matches protocol max IO)
constexpr int64_t kMetaTtlMs = 2000;         // metadata cache TTL
constexpr int64_t kDirTtlMs = 5000;          // directory listing cache TTL
constexpr int64_t kNegTtlMs = 1000;          // negative lookup cache TTL
constexpr size_t kMetaMax = 8192;            // bounded metadata entries
constexpr size_t kDirMax = 1024;             // bounded directory entries
constexpr size_t kNegMax = 4096;             // bounded negative entries
constexpr uint32_t kMaxReadAheadBlocks = 32; // up to 32 MiB read-ahead window
constexpr size_t kPrefetchQueueMax = 512;    // drop prefetch jobs past this depth

constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr uint64_t kGiB = 1024ull * kMiB;

int64_t now_ms() {
    return std::chrono::duration_cast<ms>(clock_t_::now().time_since_epoch()).count();
}

// ---- system RAM detection -------------------------------------------------

struct RamInfo { uint64_t total = 0; uint64_t avail = 0; };

RamInfo detect_ram() {
    RamInfo r;
#if defined(_WIN32)
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    if (GlobalMemoryStatusEx(&m)) { r.total = m.ullTotalPhys; r.avail = m.ullAvailPhys; }
#elif defined(__APPLE__)
    uint64_t mem = 0;
    size_t len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0) r.total = mem;
    mach_port_t host = mach_host_self();
    vm_size_t page = 0;
    host_page_size(host, &page);
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) ==
        KERN_SUCCESS) {
        r.avail = static_cast<uint64_t>(vm.free_count + vm.inactive_count) *
                  static_cast<uint64_t>(page);
    }
#elif defined(__linux__)
    struct sysinfo si{};
    if (sysinfo(&si) == 0) {
        uint64_t unit = si.mem_unit ? si.mem_unit : 1;
        r.total = static_cast<uint64_t>(si.totalram) * unit;
        // freeram excludes reclaimable page cache, so treat buffers as available too.
        r.avail = static_cast<uint64_t>(si.freeram + si.bufferram) * unit;
    }
    // Prefer the kernel's MemAvailable estimate (accounts for reclaimable page
    // cache and slab); fall back to freeram+bufferram above if it's unreadable.
    if (FILE* f = std::fopen("/proc/meminfo", "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            unsigned long long kb = 0;
            if (std::sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
                r.avail = static_cast<uint64_t>(kb) * 1024ull;
                break;
            }
        }
        std::fclose(f);
    }
#endif
    if (r.total == 0) r.total = 4 * kGiB; // conservative fallback
    if (r.avail == 0) r.avail = r.total / 4;
    if (r.avail > r.total) r.avail = r.total;
    return r;
}

// Block-cache byte budget from total/available RAM. Returns 0 when memory is so
// tight that only the tiny metadata/directory caches should remain.
uint64_t compute_block_budget(const RamInfo& r) {
    uint64_t cap;
    if (r.total <= 4 * kGiB) cap = 256 * kMiB;
    else if (r.total <= 8 * kGiB) cap = 512 * kMiB;
    else if (r.total <= 16 * kGiB) cap = 1 * kGiB;
    else if (r.total <= 32 * kGiB) cap = 2 * kGiB;
    else cap = 4 * kGiB;

    uint64_t budget = std::min({r.avail / 4, r.total / 10, cap});

    if (r.avail < 512 * kMiB) return 0;                       // disable block cache
    if (r.avail < 1 * kGiB) return std::min<uint64_t>(budget, 64 * kMiB);
    if (r.avail < 2 * kGiB) return std::min<uint64_t>(budget, 128 * kMiB);
    return budget;
}

// ---- path helpers ---------------------------------------------------------

std::string parent_of(const std::string& p) {
    if (p.empty() || p == "/") return "/";
    size_t slash = p.find_last_of('/');
    if (slash == std::string::npos) return "/";
    if (slash == 0) return "/";
    return p.substr(0, slash);
}

std::string join_path(const std::string& dir, const std::string& name) {
    if (dir.empty() || dir == "/") return "/" + name;
    return dir + "/" + name;
}

// ---- a tiny bounded prefetch thread pool ----------------------------------

class PrefetchPool {
public:
    explicit PrefetchPool(unsigned workers) {
        running_ = true;
        for (unsigned i = 0; i < workers; ++i)
            threads_.emplace_back([this] { loop(); });
    }
    ~PrefetchPool() {
        {
            std::lock_guard<std::mutex> lk(m_);
            running_ = false;
        }
        cv_.notify_all();
        for (auto& t : threads_)
            if (t.joinable()) t.join();
    }
    // Drops the job (returns false) if the queue is already saturated, so a slow
    // network can never make read-ahead pile up unbounded memory.
    bool submit(std::function<void()> job) {
        std::lock_guard<std::mutex> lk(m_);
        if (!running_ || q_.size() >= kPrefetchQueueMax) return false;
        q_.push_back(std::move(job));
        cv_.notify_one();
        return true;
    }

private:
    void loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return !running_ || !q_.empty(); });
                if (!running_ && q_.empty()) return;
                job = std::move(q_.front());
                q_.pop_front();
            }
            job();
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    std::vector<std::thread> threads_;
    bool running_ = false;
};

} // namespace

// ===========================================================================

struct RamCache::Impl {
    RemoteFs* inner;

    // ---- metadata / directory / negative caches (path-keyed, short TTL) ----
    struct MetaEntry { WireAttr attr; int64_t at; };
    struct DirEntry { std::vector<uint8_t> resp; int64_t at; };
    std::mutex metaMtx;
    std::unordered_map<std::string, MetaEntry> meta;
    std::unordered_map<std::string, int64_t> neg; // path -> insertion time
    std::mutex dirMtx;
    std::unordered_map<std::string, DirEntry> dir;

    // ---- file-handle tracking (fh -> path + captured version + read pattern) ----
    struct FhInfo {
        std::string path;
        bool haveVer = false;
        int64_t verAt = 0;     // when size/mtime were last revalidated
        uint64_t size = 0;
        int64_t mtime = 0;
        uint64_t lastEnd = 0;  // byte offset just past the previous sequential read
        uint32_t seqRun = 0;   // consecutive contiguous reads
        uint32_t raWindow = 0; // current read-ahead window in blocks
        int activeReads = 0;   // host OP_READs in flight against this handle
        bool closing = false;  // RELEASE pending: refuse new reads, drain active ones
    };
    std::mutex fhMtx;
    std::condition_variable fhCv; // signalled when activeReads drops to 0
    std::unordered_map<uint64_t, FhInfo> fh;

    // ---- RAM block cache ---------------------------------------------------
    struct Block {
        std::vector<uint8_t> data;
        bool eof = false;
        std::list<std::pair<std::string, uint64_t>>::iterator lru;
    };
    struct FileBlocks {
        uint64_t size = 0;
        int64_t mtime = 0;
        std::unordered_map<uint64_t, Block> blocks;
    };
    std::mutex blockMtx;
    std::condition_variable blockCv;
    std::unordered_map<std::string, FileBlocks> files;
    std::list<std::pair<std::string, uint64_t>> lru; // front = most recently used
    std::set<std::pair<std::string, uint64_t>> inflight;
    uint64_t curBytes = 0;

    // ---- budget / memory pressure -----------------------------------------
    RamInfo ram;
    uint64_t blockBudget = 0;                 // 0 => block cache disabled (set once)
    std::atomic<uint64_t> effectiveBudget{0}; // shrinks under memory pressure
    std::atomic<int64_t> budgetCheckedAt{0};

    std::unique_ptr<PrefetchPool> pool;

    // ---- debug stats (internal; dumped only if FOLDERBUDDIES_CACHE_DEBUG) --
    bool debug = false;
    std::atomic<uint64_t> metaHit{0}, metaMiss{0};
    std::atomic<uint64_t> dirHit{0}, dirMiss{0};
    std::atomic<uint64_t> negHit{0}, negMiss{0};
    std::atomic<uint64_t> blockHit{0}, blockMiss{0};
    std::atomic<uint64_t> bytesFromCache{0}, bytesFromNet{0};
    std::atomic<uint64_t> evictions{0}, pressureShrinks{0};

    explicit Impl(RemoteFs* in) : inner(in) {
        ram = detect_ram();
        blockBudget = compute_block_budget(ram);
        effectiveBudget = blockBudget;
        budgetCheckedAt = now_ms();
        debug = std::getenv("FOLDERBUDDIES_CACHE_DEBUG") != nullptr;
        unsigned hw = std::thread::hardware_concurrency();
        unsigned workers = std::clamp<unsigned>(hw ? hw : 4, 2, 8);
        pool = std::make_unique<PrefetchPool>(workers);
        if (auto* cl = dynamic_cast<Client*>(inner)) {
            cl->onInvalidate = [this](const std::string& path) {
                invalidateFile(path);
                dirErase(path);
                dirErase(parent_of(path));
            };
        }
    }

    ~Impl() {
        pool.reset(); // join prefetch threads before any cache state is destroyed
        if (debug) dump_stats();
    }

    // ---- forwarding --------------------------------------------------------
    int forward(uint16_t op, const std::vector<uint8_t>& payload, std::vector<uint8_t>& resp) {
        return inner->request(op, payload, resp);
    }

    // ---- metadata cache ----------------------------------------------------
    bool metaLookup(const std::string& path, WireAttr& out) {
        std::lock_guard<std::mutex> lk(metaMtx);
        auto it = meta.find(path);
        if (it == meta.end()) return false;
        if (now_ms() - it->second.at > kMetaTtlMs) { meta.erase(it); return false; }
        out = it->second.attr;
        return true;
    }
    void metaStore(const std::string& path, const WireAttr& a) {
        std::lock_guard<std::mutex> lk(metaMtx);
        if (meta.size() >= kMetaMax) trim(meta, kMetaMax / 2);
        meta[path] = {a, now_ms()};
        neg.erase(path);
    }
    void metaErase(const std::string& path) {
        std::lock_guard<std::mutex> lk(metaMtx);
        meta.erase(path);
        neg.erase(path);
    }
    bool negLookup(const std::string& path) {
        std::lock_guard<std::mutex> lk(metaMtx);
        auto it = neg.find(path);
        if (it == neg.end()) return false;
        if (now_ms() - it->second > kNegTtlMs) { neg.erase(it); return false; }
        return true;
    }
    void negStore(const std::string& path) {
        std::lock_guard<std::mutex> lk(metaMtx);
        if (neg.size() >= kNegMax) trim(neg, kNegMax / 2);
        neg[path] = now_ms();
    }

    // ---- directory cache ---------------------------------------------------
    bool dirLookup(const std::string& path, std::vector<uint8_t>& out) {
        std::lock_guard<std::mutex> lk(dirMtx);
        auto it = dir.find(path);
        if (it == dir.end()) return false;
        if (now_ms() - it->second.at > kDirTtlMs) { dir.erase(it); return false; }
        out = it->second.resp;
        return true;
    }
    void dirStore(const std::string& path, const std::vector<uint8_t>& resp) {
        std::lock_guard<std::mutex> lk(dirMtx);
        if (dir.size() >= kDirMax) trim(dir, kDirMax / 2);
        dir[path] = {resp, now_ms()};
    }
    void dirErase(const std::string& path) {
        std::lock_guard<std::mutex> lk(dirMtx);
        dir.erase(path);
    }

    template <class Map>
    static void trim(Map& m, size_t target) {
        // Short-TTL caches: cheap to shed the oldest-inserted half on overflow.
        while (m.size() > target && !m.empty()) m.erase(m.begin());
    }

    // ---- fh tracking -------------------------------------------------------
    void fhAdd(uint64_t id, const std::string& path) {
        std::lock_guard<std::mutex> lk(fhMtx);
        FhInfo info;
        info.path = path;
        fh[id] = std::move(info);
    }
    void fhRemove(uint64_t id) {
        std::lock_guard<std::mutex> lk(fhMtx);
        fh.erase(id);
    }
    bool fhPath(uint64_t id, std::string& path) {
        std::lock_guard<std::mutex> lk(fhMtx);
        auto it = fh.find(id);
        if (it == fh.end()) return false;
        path = it->second.path;
        return true;
    }
    // Claim the handle for one host read. Returns false if it's gone or closing,
    // which keeps RELEASE from racing a background read-ahead onto a dead fh.
    bool fhBeginRead(uint64_t id) {
        std::lock_guard<std::mutex> lk(fhMtx);
        auto it = fh.find(id);
        if (it == fh.end() || it->second.closing) return false;
        it->second.activeReads++;
        return true;
    }
    void fhEndRead(uint64_t id) {
        std::lock_guard<std::mutex> lk(fhMtx);
        auto it = fh.find(id);
        if (it != fh.end() && --it->second.activeReads <= 0) fhCv.notify_all();
    }
    // Mark closing and block until every in-flight host read on this handle has
    // finished, so OP_RELEASE is only forwarded once no read can still use the fh.
    void fhBeginClose(uint64_t id) {
        std::unique_lock<std::mutex> lk(fhMtx);
        auto it = fh.find(id);
        if (it == fh.end()) return;
        it->second.closing = true;
        fhCv.wait(lk, [&] {
            auto i = fh.find(id);
            return i == fh.end() || i->second.activeReads == 0;
        });
    }
    // Invalidate the cached file version for every handle pointing at `path` so
    // the next read re-derives size/mtime after a local mutation.
    void clearFhVersion(const std::string& path) {
        std::lock_guard<std::mutex> lk(fhMtx);
        for (auto& kv : fh)
            if (kv.second.path == path) kv.second.haveVer = false;
    }
    void renameFhPaths(const std::string& from, const std::string& to) {
        std::lock_guard<std::mutex> lk(fhMtx);
        for (auto& kv : fh) {
            if (kv.second.path == from) { kv.second.path = to; kv.second.haveVer = false; }
        }
    }

    // ---- budget / pressure -------------------------------------------------
    uint64_t budgetNow() {
        int64_t t = now_ms();
        if (t - budgetCheckedAt.load() > 2000) { // re-sample available RAM at most every 2 s
            budgetCheckedAt.store(t);
            RamInfo cur = detect_ram();
            uint64_t b = compute_block_budget({ram.total, cur.avail});
            if (b < effectiveBudget.load()) pressureShrinks++;
            effectiveBudget.store(b);
        }
        return effectiveBudget.load();
    }

    // ---- block cache (must hold blockMtx) ----------------------------------
    void purgeFileLocked(const std::string& path) {
        auto it = files.find(path);
        if (it == files.end()) return;
        for (auto& kv : it->second.blocks) {
            curBytes -= kv.second.data.size();
            lru.erase(kv.second.lru);
        }
        files.erase(it);
    }
    void evictToBudgetLocked(uint64_t budget) {
        while (curBytes > budget && !lru.empty()) {
            auto key = lru.back();
            auto fit = files.find(key.first);
            if (fit != files.end()) {
                auto bit = fit->second.blocks.find(key.second);
                if (bit != fit->second.blocks.end()) {
                    curBytes -= bit->second.data.size();
                    fit->second.blocks.erase(bit);
                    if (fit->second.blocks.empty()) files.erase(fit);
                }
            }
            lru.pop_back();
            evictions++;
        }
    }
    // Returns true and fills out on cache hit (file present, version matches).
    bool blockGetLocked(const std::string& path, uint64_t size, int64_t mtime, uint64_t idx,
                        std::vector<uint8_t>& out, bool& eof) {
        auto it = files.find(path);
        if (it == files.end()) return false;
        if (it->second.size != size || it->second.mtime != mtime) {
            purgeFileLocked(path); // stale version: drop everything for this file
            return false;
        }
        auto bit = it->second.blocks.find(idx);
        if (bit == it->second.blocks.end()) return false;
        lru.splice(lru.begin(), lru, bit->second.lru); // touch -> most recently used
        out = bit->second.data;
        eof = bit->second.eof;
        return true;
    }
    void blockPutLocked(const std::string& path, uint64_t size, int64_t mtime, uint64_t idx,
                        std::vector<uint8_t>&& data, bool eof, uint64_t budget) {
        auto it = files.find(path);
        if (it != files.end() && (it->second.size != size || it->second.mtime != mtime)) {
            purgeFileLocked(path); // stale version: drop it (this erases the map entry)
            it = files.end();
        }
        // Re-acquire a valid reference: never reuse one across an erase.
        FileBlocks& fe = (it == files.end()) ? files[path] : it->second;
        fe.size = size;
        fe.mtime = mtime;
        if (fe.blocks.count(idx)) return; // someone won the race already
        lru.push_front({path, idx});
        Block b;
        b.data = std::move(data);
        b.eof = eof;
        b.lru = lru.begin();
        curBytes += b.data.size();
        fe.blocks.emplace(idx, std::move(b));
        evictToBudgetLocked(budget);
    }

    // Fetch one block from the host (no lock held during network I/O), with
    // in-flight de-duplication so concurrent readers never double-fetch.
    int blockFetch(const std::string& path, uint64_t fhId, uint64_t size, int64_t mtime,
                   uint64_t idx, std::vector<uint8_t>& out, bool& eof, bool* fromCache = nullptr) {
        std::unique_lock<std::mutex> lk(blockMtx);
        for (;;) {
            if (blockGetLocked(path, size, mtime, idx, out, eof)) {
                blockHit++;
                if (fromCache) *fromCache = true;
                return 0;
            }
            auto key = std::make_pair(path, idx);
            if (inflight.count(key)) {
                blockCv.wait(lk);
                continue; // re-check: it may now be cached or its fetch may have failed
            }
            inflight.insert(key);
            break;
        }
        lk.unlock();

        // Honor an open handle only — never read against a released/closing fh.
        // The refcount makes RELEASE wait for this read instead of pulling the fh
        // out from under it.
        std::vector<uint8_t> resp;
        int status;
        if (!fhBeginRead(fhId)) {
            status = EBADF;
        } else {
            Writer w;
            w.pod(fhId);
            uint64_t off = idx * kBlock;
            w.pod(off);
            uint32_t len = static_cast<uint32_t>(kBlock);
            w.pod(len);
            status = forward(OP_READ, w.b, resp);
            fhEndRead(fhId);
        }

        lk.lock();
        inflight.erase({path, idx});
        blockCv.notify_all();
        if (status != 0) return status;
        blockMiss++;
        bytesFromNet += resp.size();
        if (fromCache) *fromCache = false; // came over the wire, not from RAM
        bool blockEof = resp.size() < kBlock;
        out = resp;
        eof = blockEof;
        try {
            blockPutLocked(path, size, mtime, idx, std::move(resp), blockEof, budgetNow());
        } catch (const std::bad_alloc&) {
            // Memory pressure mid-insert: shed aggressively and disable growth once.
            evictToBudgetLocked(0);
            effectiveBudget = 0;
            pressureShrinks++;
        }
        return 0;
    }

    // ---- read path ---------------------------------------------------------
    bool ensureVersion(uint64_t fhId, const std::string& path, uint64_t& size, int64_t& mtime) {
        {
            std::lock_guard<std::mutex> lk(fhMtx);
            auto it = fh.find(fhId);
            // Trust the captured version only briefly: past the TTL we re-derive
            // size/mtime so an external writer's change can't be served stale
            // from this handle's old blocks indefinitely.
            if (it != fh.end() && it->second.haveVer &&
                now_ms() - it->second.verAt <= kMetaTtlMs) {
                size = it->second.size;
                mtime = it->second.mtime;
                return true;
            }
        }
        WireAttr a;
        if (!metaLookup(path, a)) {
            Writer w;
            w.str(path);
            std::vector<uint8_t> resp;
            if (forward(OP_GETATTR, w.b, resp) != 0) return false;
            Reader r(resp.data(), resp.size());
            if (!r.pod(a)) return false;
            metaStore(path, a);
        }
        size = a.size;
        mtime = a.mtime;
        std::lock_guard<std::mutex> lk(fhMtx);
        auto it = fh.find(fhId);
        if (it != fh.end()) {
            it->second.haveVer = true;
            it->second.verAt = now_ms();
            it->second.size = size;
            it->second.mtime = mtime;
        }
        return true;
    }

    void scheduleReadAhead(uint64_t fhId, const std::string& path, uint64_t size, int64_t mtime,
                           uint64_t newEnd, uint64_t off) {
        uint64_t budget = budgetNow();
        if (budget == 0) return;
        uint32_t window;
        {
            std::lock_guard<std::mutex> lk(fhMtx);
            auto it = fh.find(fhId);
            if (it == fh.end()) return;
            FhInfo& f = it->second;
            bool sequential = (off == f.lastEnd) || (f.lastEnd == 0 && off == 0);
            if (sequential) {
                f.seqRun++;
                f.raWindow = f.raWindow == 0 ? 1 : std::min(f.raWindow * 2, kMaxReadAheadBlocks);
            } else {
                f.seqRun = 0;
                f.raWindow = 0; // random access: stop prefetching for this handle
            }
            f.lastEnd = newEnd;
            window = (f.seqRun >= 2) ? f.raWindow : 0;
        }
        if (window == 0) return;
        // Never let read-ahead claim more than half the budget.
        uint32_t cap = static_cast<uint32_t>(std::max<uint64_t>(1, budget / kBlock / 2));
        window = std::min(window, std::min(cap, kMaxReadAheadBlocks));

        uint64_t startIdx = newEnd / kBlock;
        for (uint32_t i = 0; i < window; ++i) {
            uint64_t idx = startIdx + i;
            pool->submit([this, path, fhId, size, mtime, idx] {
                std::vector<uint8_t> tmp;
                bool eof = false;
                blockFetch(path, fhId, size, mtime, idx, tmp, eof);
            });
        }
    }

    int doRead(const std::vector<uint8_t>& payload, std::vector<uint8_t>& resp) {
        Reader r(payload.data(), payload.size());
        uint64_t fhId, off;
        uint32_t size;
        if (!r.pod(fhId) || !r.pod(off) || !r.pod(size)) return forward(OP_READ, payload, resp);

        if (budgetNow() == 0) return forward(OP_READ, payload, resp); // block cache disabled

        std::string path;
        if (!fhPath(fhId, path)) return forward(OP_READ, payload, resp);

        uint64_t fsize;
        int64_t fmtime;
        if (!ensureVersion(fhId, path, fsize, fmtime)) return forward(OP_READ, payload, resp);

        std::vector<uint8_t> out;
        out.reserve(size);
        uint64_t cur = off;
        uint64_t end = off + size;
        while (cur < end) {
            uint64_t idx = cur / kBlock;
            uint64_t boff = cur % kBlock;
            std::vector<uint8_t> blk;
            bool eof = false;
            bool hit = false;
            int st = blockFetch(path, fhId, fsize, fmtime, idx, blk, eof, &hit); // cache or host
            if (st != 0) return st;
            if (boff >= blk.size()) break; // reached EOF within this block
            uint64_t take = std::min<uint64_t>(end - cur, blk.size() - boff);
            out.insert(out.end(), blk.begin() + boff, blk.begin() + boff + take);
            if (hit) bytesFromCache += take; // only bytes actually served from RAM
            cur += take;
            if (boff + take >= blk.size() && (eof || blk.size() < kBlock)) break; // end of file
        }

        scheduleReadAhead(fhId, path, fsize, fmtime, off + out.size(), off);
        resp = std::move(out);
        return 0;
    }

    // ---- handlers for cacheable read ops -----------------------------------
    int doGetattr(const std::vector<uint8_t>& payload, std::vector<uint8_t>& resp) {
        Reader r(payload.data(), payload.size());
        std::string path;
        if (!r.str(path)) return forward(OP_GETATTR, payload, resp);

        WireAttr a;
        if (metaLookup(path, a)) {
            metaHit++;
            Writer w; w.pod(a); resp = std::move(w.b); return 0;
        }
        if (negLookup(path)) { negHit++; return ENOENT; }
        metaMiss++; negMiss++;

        int status = forward(OP_GETATTR, payload, resp);
        if (status == 0) {
            Reader rr(resp.data(), resp.size());
            WireAttr got;
            if (rr.pod(got)) metaStore(path, got);
        } else if (status == ENOENT) {
            negStore(path);
        }
        return status;
    }

    int doReaddir(const std::vector<uint8_t>& payload, std::vector<uint8_t>& resp) {
        Reader r(payload.data(), payload.size());
        std::string path;
        if (!r.str(path)) return forward(OP_READDIR, payload, resp);

        if (dirLookup(path, resp)) { dirHit++; return 0; }
        dirMiss++;

        int status = forward(OP_READDIR, payload, resp);
        if (status != 0) return status;
        dirStore(path, resp);
        // Seed the metadata cache from the listing so subsequent getattr on each
        // child is a hit — the host already returns full attrs per entry.
        Reader rr(resp.data(), resp.size());
        uint32_t n = 0;
        if (rr.pod(n)) {
            for (uint32_t i = 0; i < n; ++i) {
                std::string name;
                WireAttr a;
                if (!rr.str(name) || !rr.pod(a)) break;
                metaStore(join_path(path, name), a);
            }
        }
        return status;
    }

    // ---- mutation invalidation helpers ------------------------------------
    void invalidateFile(const std::string& path) {
        metaErase(path);
        clearFhVersion(path);
        std::lock_guard<std::mutex> lk(blockMtx);
        purgeFileLocked(path);
    }

    void dump_stats() {
        auto pct = [](uint64_t h, uint64_t m) -> double {
            uint64_t t = h + m; return t ? 100.0 * static_cast<double>(h) / static_cast<double>(t) : 0.0;
        };
        std::fprintf(stderr,
            "[fb-cache] RAM total=%lluMiB avail=%lluMiB budget=%lluMiB curr=%lluMiB\n"
            "[fb-cache] meta %llu/%llu (%.0f%%) dir %llu/%llu (%.0f%%) neg %llu/%llu\n"
            "[fb-cache] block %llu/%llu (%.0f%%) evictions=%llu pressure=%llu\n"
            "[fb-cache] bytes served-from-RAM=%lluMiB fetched-from-net=%lluMiB\n",
            (unsigned long long)(ram.total / kMiB), (unsigned long long)(ram.avail / kMiB),
            (unsigned long long)(blockBudget / kMiB), (unsigned long long)(curBytes / kMiB),
            (unsigned long long)metaHit.load(), (unsigned long long)metaMiss.load(), pct(metaHit, metaMiss),
            (unsigned long long)dirHit.load(), (unsigned long long)dirMiss.load(), pct(dirHit, dirMiss),
            (unsigned long long)negHit.load(), (unsigned long long)negMiss.load(),
            (unsigned long long)blockHit.load(), (unsigned long long)blockMiss.load(), pct(blockHit, blockMiss),
            (unsigned long long)evictions.load(),
            (unsigned long long)pressureShrinks.load(),
            (unsigned long long)(bytesFromCache.load() / kMiB),
            (unsigned long long)(bytesFromNet.load() / kMiB));
    }
};

// ===========================================================================

RamCache::RamCache(RemoteFs* inner) : impl_(std::make_unique<Impl>(inner)) {}
RamCache::~RamCache() = default;

bool RamCache::connected() const { return impl_->inner->connected(); }

int RamCache::request(uint16_t op, const std::vector<uint8_t>& payload, std::vector<uint8_t>& resp) {
    Impl& d = *impl_;
    switch (op) {
    case OP_GETATTR:
        return d.doGetattr(payload, resp);
    case OP_READDIR:
        return d.doReaddir(payload, resp);
    case OP_READ:
        return d.doRead(payload, resp);

    case OP_OPEN:
    case OP_CREATE: {
        Reader r(payload.data(), payload.size());
        std::string path;
        r.str(path);
        int status = d.forward(op, payload, resp);
        if (status == 0) {
            Reader rr(resp.data(), resp.size());
            uint64_t fhId = 0;
            if (rr.pod(fhId)) d.fhAdd(fhId, path);
            if (op == OP_CREATE) {
                d.invalidateFile(path);
                d.dirErase(parent_of(path));
            }
        }
        return status;
    }

    case OP_WRITE: {
        Reader r(payload.data(), payload.size());
        uint64_t fhId = 0, off = 0;
        r.pod(fhId);
        r.pod(off);
        int status = d.forward(op, payload, resp); // strict write-through
        if (status == 0) {
            std::string path;
            if (d.fhPath(fhId, path)) d.invalidateFile(path);
        }
        return status;
    }

    case OP_RELEASE: {
        Reader r(payload.data(), payload.size());
        uint64_t fhId = 0;
        r.pod(fhId);
        // Block new prefetches and wait for in-flight reads to drain before we let
        // the host close the handle, then drop our tracking once it's released.
        d.fhBeginClose(fhId);
        int status = d.forward(op, payload, resp);
        d.fhRemove(fhId);
        return status;
    }

    case OP_TRUNCATE: {
        Reader r(payload.data(), payload.size());
        std::string path;
        r.str(path);
        int status = d.forward(op, payload, resp);
        if (status == 0) d.invalidateFile(path);
        return status;
    }

    case OP_UNLINK: {
        Reader r(payload.data(), payload.size());
        std::string path;
        r.str(path);
        int status = d.forward(op, payload, resp);
        if (status == 0) {
            d.invalidateFile(path);
            d.dirErase(parent_of(path));
        }
        return status;
    }

    case OP_RMDIR: {
        Reader r(payload.data(), payload.size());
        std::string path;
        r.str(path);
        int status = d.forward(op, payload, resp);
        if (status == 0) {
            d.metaErase(path);
            d.dirErase(path);
            d.dirErase(parent_of(path));
        }
        return status;
    }

    case OP_MKDIR: {
        Reader r(payload.data(), payload.size());
        std::string path;
        r.str(path);
        int status = d.forward(op, payload, resp);
        if (status == 0) {
            d.metaErase(path);
            d.dirErase(parent_of(path));
        }
        return status;
    }

    case OP_RENAME: {
        Reader r(payload.data(), payload.size());
        std::string from, to;
        r.str(from);
        r.str(to);
        int status = d.forward(op, payload, resp);
        if (status == 0) {
            d.invalidateFile(from);
            d.invalidateFile(to);
            d.dirErase(from);
            d.dirErase(to);
            d.dirErase(parent_of(from));
            d.dirErase(parent_of(to));
            d.renameFhPaths(from, to);
        }
        return status;
    }

    case OP_CHMOD:
    case OP_UTIMENS: {
        Reader r(payload.data(), payload.size());
        std::string path;
        r.str(path);
        int status = d.forward(op, payload, resp);
        if (status == 0) d.metaErase(path);
        return status;
    }

    // FSYNC/FLUSH stay honest (forwarded, host must ack); STATFS/ACCESS/others
    // are forwarded untouched.
    default:
        return d.forward(op, payload, resp);
    }
}

} // namespace fb
