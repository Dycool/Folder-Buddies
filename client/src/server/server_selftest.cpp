// End-to-end protocol self-test: a real Server and Client talking over
// loopback TCP through the full encrypted framing. Covers the filesystem
// operations, path-traversal rejection, read-only enforcement, malformed
// payload handling, timestamp round-trips, invalidation broadcast, and
// authentication failure.
#include "server.h"
#include "client.h"
#include "common.h"
#include "osflags.h"
#include "token.h"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#ifndef EROFS
#  define EROFS EACCES
#endif

namespace fs = std::filesystem;
using namespace fb;

static int g_failures = 0;
#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

static std::vector<uint8_t> path_payload(const std::string& p) {
    Writer w;
    w.str(p);
    return w.b;
}

static std::vector<uint8_t> open_payload(const std::string& p, int32_t flags, uint32_t mode) {
    Writer w;
    w.str(p);
    w.pod(flags);
    w.pod(mode);
    return w.b;
}

static bool getattr(Client& c, const std::string& p, WireAttr& a, int& status) {
    std::vector<uint8_t> resp;
    status = c.request(OP_GETATTR, path_payload(p), resp);
    if (status != 0) return false;
    Reader r(resp.data(), resp.size());
    return r.pod(a);
}

int main() {
    net_startup();
    std::error_code ec;
    const fs::path base = fs::temp_directory_path() /
        ("fb-server-selftest-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path share = base / "share";
    fs::create_directories(share / "sub", ec);
    const std::string hello = "hello folder buddies";
    { std::ofstream f(share / "hello.txt", std::ios::binary); f << hello; }
    { std::ofstream f(base / "outside.txt", std::ios::binary); f << "secret"; }

    Server server;
    server.allowWrites = true;
    std::string err;
    if (!server.start(share.string(), 0, err)) {
        std::printf("server self-test FAIL (start: %s)\n", err.c_str());
        return 1;
    }

    Token tok;
    tok.ip = "127.0.0.1";
    tok.port = server.boundPort;
    tok.secret = server.secret;
    tok.folder = server.shareName;
    tok.allowWrites = true;

    Client client;
    CHECK(client.connect(tok, 2, err));

    std::vector<uint8_t> resp;

    // ---- attributes ----
    WireAttr a{};
    int status = 0;
    CHECK(getattr(client, "/hello.txt", a, status));
    CHECK(a.size == hello.size());
    CHECK(getattr(client, "/sub", a, status) && (a.mode & 0040000u));
    CHECK(!getattr(client, "/missing.txt", a, status) && status == ENOENT);

    // ---- path traversal must never escape the share ----
    CHECK(client.request(OP_GETATTR, path_payload("../outside.txt"), resp) != 0);
    CHECK(client.request(OP_GETATTR, path_payload("/../outside.txt"), resp) != 0);
    CHECK(client.request(OP_GETATTR, path_payload("/sub/../../outside.txt"), resp) != 0);
    CHECK(client.request(OP_UNLINK, path_payload("/../outside.txt"), resp) != 0);
    CHECK(fs::exists(base / "outside.txt", ec)); // still there

    // ---- readdir ----
    CHECK(client.request(OP_READDIR, path_payload("/"), resp) == 0);
    {
        Reader r(resp.data(), resp.size());
        uint32_t n = 0;
        CHECK(r.pod(n) && n == 2);
        bool sawFile = false, sawDir = false;
        for (uint32_t i = 0; i < n; ++i) {
            std::string name;
            WireAttr ea{};
            CHECK(r.str(name) && r.pod(ea));
            if (name == "hello.txt") sawFile = ea.size == hello.size();
            if (name == "sub") sawDir = (ea.mode & 0040000u) != 0;
        }
        CHECK(sawFile && sawDir);
    }

    // ---- open + positional read ----
    uint64_t fh = 0;
    CHECK(client.request(OP_OPEN, open_payload("/hello.txt", FB_O_RDONLY, 0), resp) == 0);
    { Reader r(resp.data(), resp.size()); CHECK(r.pod(fh)); }
    {
        Writer w;
        w.pod(fh);
        uint64_t off = 6;
        w.pod(off);
        uint32_t sz = 6;
        w.pod(sz);
        CHECK(client.request(OP_READ, w.b, resp) == 0);
        CHECK(std::string(resp.begin(), resp.end()) == "folder");
    }
    { // read past EOF -> empty
        Writer w;
        w.pod(fh);
        uint64_t off = 4096;
        w.pod(off);
        uint32_t sz = 16;
        w.pod(sz);
        CHECK(client.request(OP_READ, w.b, resp) == 0 && resp.empty());
    }
    {
        Writer w;
        w.pod(fh);
        CHECK(client.request(OP_RELEASE, w.b, resp) == 0);
        Writer rd;
        rd.pod(fh);
        uint64_t off = 0;
        rd.pod(off);
        uint32_t sz = 4;
        rd.pod(sz);
        CHECK(client.request(OP_READ, rd.b, resp) == EBADF);
    }

    // ---- malformed payloads are rejected, not interpreted ----
    CHECK(client.request(OP_READ, {1, 2, 3}, resp) == EINVAL);
    CHECK(client.request(OP_OPEN, path_payload("/hello.txt"), resp) == EINVAL);
    CHECK(client.request(OP_UTIMENS, path_payload("/hello.txt"), resp) == EINVAL);

    // ---- create + write + read back + on-disk verification ----
    CHECK(client.request(OP_CREATE,
                         open_payload("/new.bin", FB_O_WRONLY | FB_O_CREAT | FB_O_TRUNC, 0644),
                         resp) == 0);
    { Reader r(resp.data(), resp.size()); CHECK(r.pod(fh)); }
    {
        Writer w;
        w.pod(fh);
        uint64_t off = 0;
        w.pod(off);
        w.raw("abcdef", 6);
        CHECK(client.request(OP_WRITE, w.b, resp) == 0);
        Reader r(resp.data(), resp.size());
        uint32_t written = 0;
        CHECK(r.pod(written) && written == 6);
    }
    {
        Writer w;
        w.pod(fh);
        CHECK(client.request(OP_RELEASE, w.b, resp) == 0);
    }
    {
        std::ifstream f(share / "new.bin", std::ios::binary);
        std::string disk((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(disk == "abcdef");
    }

    // ---- truncate ----
    {
        Writer w;
        w.str("/new.bin");
        uint64_t size = 3;
        w.pod(size);
        CHECK(client.request(OP_TRUNCATE, w.b, resp) == 0);
        CHECK(getattr(client, "/new.bin", a, status) && a.size == 3);
    }

    // ---- utimens round-trip (regression: file_clock epoch conversion) ----
    {
        Writer w;
        w.str("/new.bin");
        int64_t t = 1600000000; // 2020-09-13T12:26:40Z
        w.pod(t);
        w.pod(t);
        CHECK(client.request(OP_UTIMENS, w.b, resp) == 0);
        CHECK(getattr(client, "/new.bin", a, status));
        CHECK(a.mtime >= 1600000000 - 2 && a.mtime <= 1600000000 + 2);
    }

    // ---- mkdir / rename / unlink / rmdir ----
    {
        Writer w;
        w.str("/d2");
        uint32_t mode = 0755;
        w.pod(mode);
        CHECK(client.request(OP_MKDIR, w.b, resp) == 0);
        CHECK(getattr(client, "/d2", a, status) && (a.mode & 0040000u));
        CHECK(client.request(OP_MKDIR, w.b, resp) == EEXIST);
    }
    {
        Writer w;
        w.str("/new.bin");
        w.str("/d2/moved.bin");
        CHECK(client.request(OP_RENAME, w.b, resp) == 0);
        CHECK(!getattr(client, "/new.bin", a, status) && status == ENOENT);
        CHECK(getattr(client, "/d2/moved.bin", a, status) && a.size == 3);
    }
    CHECK(client.request(OP_UNLINK, path_payload("/d2/moved.bin"), resp) == 0);
    CHECK(client.request(OP_RMDIR, path_payload("/d2"), resp) == 0);
    CHECK(!getattr(client, "/d2", a, status) && status == ENOENT);

    // ---- statfs / access ----
    CHECK(client.request(OP_STATFS, path_payload("/"), resp) == 0);
    {
        Reader r(resp.data(), resp.size());
        WireStatvfs sv{};
        CHECK(r.pod(sv) && sv.bsize == 4096 && sv.blocks > 0);
    }
    {
        Writer w;
        w.str("/hello.txt");
        uint32_t mode = 2; // W_OK, allowed on a writable share
        w.pod(mode);
        CHECK(client.request(OP_ACCESS, w.b, resp) == 0);
    }

    // ---- invalidation broadcast reaches other clients without blocking ----
    {
        Client watcher;
        CHECK(watcher.connect(tok, 1, err));
        std::mutex m;
        std::condition_variable cv;
        std::vector<std::string> seen;
        watcher.setInvalidateCallback([&](const std::string& p) {
            {
                std::lock_guard<std::mutex> lk(m);
                seen.push_back(p);
            }
            cv.notify_all();
        });
        CHECK(client.request(OP_CREATE,
                             open_payload("/ping.txt", FB_O_WRONLY | FB_O_CREAT | FB_O_TRUNC, 0644),
                             resp) == 0);
        { Reader r(resp.data(), resp.size()); CHECK(r.pod(fh)); }
        {
            std::unique_lock<std::mutex> lk(m);
            bool got = cv.wait_for(lk, std::chrono::seconds(5), [&] {
                for (const auto& p : seen)
                    if (p == "/ping.txt") return true;
                return false;
            });
            CHECK(got);
        }
        Writer rel;
        rel.pod(fh);
        client.request(OP_RELEASE, rel.b, resp);
        watcher.disconnect();
    }

    client.disconnect();
    server.stop();

    // ---- read-only enforcement ----
    {
        Server ro;
        ro.allowWrites = false;
        CHECK(ro.start(share.string(), 0, err));
        Token rtok = tok;
        rtok.port = ro.boundPort;
        rtok.secret = ro.secret;
        rtok.allowWrites = false;
        Client rc;
        CHECK(rc.connect(rtok, 1, err));
        CHECK(getattr(rc, "/hello.txt", a, status)); // reads still work
        CHECK(rc.request(OP_OPEN, open_payload("/hello.txt", FB_O_WRONLY, 0), resp) == EROFS);
        CHECK(rc.request(OP_UNLINK, path_payload("/hello.txt"), resp) == EROFS);
        {
            Writer w;
            w.str("/nd");
            uint32_t mode = 0755;
            w.pod(mode);
            CHECK(rc.request(OP_MKDIR, w.b, resp) == EROFS);
        }
        {
            Writer w;
            w.str("/hello.txt");
            uint32_t mode = 2;
            w.pod(mode);
            CHECK(rc.request(OP_ACCESS, w.b, resp) == EROFS);
        }
        CHECK(fs::exists(share / "hello.txt", ec)); // untouched
        rc.disconnect();
        ro.stop();
    }

    // ---- wrong secret must not authenticate ----
    {
        Server s2;
        s2.allowWrites = false;
        CHECK(s2.start(share.string(), 0, err));
        Token bad = tok;
        bad.port = s2.boundPort;
        bad.secret = s2.secret;
        bad.secret[0] ^= 0x01;
        Client bc;
        CHECK(!bc.connect(bad, 1, err));
        s2.stop();
    }

    fs::remove_all(base, ec);
    if (g_failures) {
        std::printf("server self-test FAIL (%d checks)\n", g_failures);
        return 1;
    }
    std::printf("server self-test PASS\n");
    return 0;
}
