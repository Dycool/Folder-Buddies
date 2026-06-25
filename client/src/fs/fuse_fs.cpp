#define FUSE_USE_VERSION 31

#include "fuse_fs.h"

#include "remote_fs.h"
#include "common.h"
#include "fuse_backend.h"
#include "osflags.h"

#include <fuse.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>

// POSIX FUSE aliases. Windows uses src/projfs_mount.cpp instead.
#ifdef _WIN32
#  define FB_STAT struct fuse_stat
#  define FB_STATVFS struct fuse_statvfs
#  define FB_TIMESPEC struct fuse_timespec
using fb_mode_t = fuse_mode_t;
using fb_off_t = fuse_off_t;
#else
#  define FB_STAT struct stat
#  define FB_STATVFS struct statvfs
#  define FB_TIMESPEC struct timespec
using fb_mode_t = mode_t;
using fb_off_t = off_t;
#endif

namespace fb {
namespace {

RemoteFs* ctx() { return static_cast<RemoteFs*>(fuse_get_context()->private_data); }

void fill_stat(FB_STAT& st, const WireAttr& a) {
    std::memset(&st, 0, sizeof(st));
    st.st_mode = a.mode;
    st.st_nlink = a.nlink ? a.nlink : 1;
    st.st_size = static_cast<decltype(st.st_size)>(a.size);
    st.st_uid = a.uid;
    st.st_gid = a.gid;
    st.st_ino = a.ino;
    st.st_blocks = static_cast<decltype(st.st_blocks)>(a.blocks);
#if defined(__APPLE__)
    st.st_atimespec.tv_sec = a.atime;
    st.st_mtimespec.tv_sec = a.mtime;
    st.st_ctimespec.tv_sec = a.ctime;
#else
    st.st_atim.tv_sec = a.atime;
    st.st_mtim.tv_sec = a.mtime;
    st.st_ctim.tv_sec = a.ctime;
#endif
}

int fs_getattr(const char* path, FB_STAT* stbuf, struct fuse_file_info*) {
    Writer w;
    w.str(path);
    std::vector<uint8_t> resp;
    int st = ctx()->request(OP_GETATTR, w.b, resp);
    if (st) return -st;
    Reader r(resp.data(), resp.size());
    WireAttr a;
    if (!r.pod(a)) return -EIO;
    fill_stat(*stbuf, a);
    return 0;
}

int fs_readdir(const char* path, void* buf, fuse_fill_dir_t filler, fb_off_t,
               struct fuse_file_info*, enum fuse_readdir_flags rdflags) {
    Writer w;
    w.str(path);
    std::vector<uint8_t> resp;
    int st = ctx()->request(OP_READDIR, w.b, resp);
    if (st) return -st;

    Reader r(resp.data(), resp.size());
    uint32_t n;
    if (!r.pod(n)) return -EIO;
    auto none = static_cast<enum fuse_fill_dir_flags>(0);
    filler(buf, ".", nullptr, 0, none);
    filler(buf, "..", nullptr, 0, none);
    for (uint32_t i = 0; i < n; ++i) {
        std::string name;
        WireAttr a;
        if (!r.str(name) || !r.pod(a)) break;
        FB_STAT stbuf;
        fill_stat(stbuf, a);
        auto fl = (rdflags & FUSE_READDIR_PLUS) ? FUSE_FILL_DIR_PLUS : none;
        filler(buf, name.c_str(), &stbuf, 0, fl);
    }
    return 0;
}

int do_open(const char* path, struct fuse_file_info* fi, uint16_t op, fb_mode_t mode) {
    Writer w;
    w.str(path);
    int32_t pflags = to_portable_flags(fi->flags);
    w.pod(pflags);
    uint32_t m = mode;
    w.pod(m);
    std::vector<uint8_t> resp;
    int st = ctx()->request(op, w.b, resp);
    if (st) return -st;
    Reader r(resp.data(), resp.size());
    uint64_t fh;
    if (!r.pod(fh)) return -EIO;
    fi->fh = fh;
    return 0;
}

int fs_open(const char* path, struct fuse_file_info* fi) { return do_open(path, fi, OP_OPEN, 0); }

int fs_create(const char* path, fb_mode_t mode, struct fuse_file_info* fi) {
    fi->flags |= O_CREAT;
    return do_open(path, fi, OP_CREATE, mode);
}

int fs_read(const char*, char* buf, size_t size, fb_off_t off, struct fuse_file_info* fi) {
    size_t done = 0;
    while (done < size) {
        uint32_t chunk = static_cast<uint32_t>(std::min<size_t>(size - done, kMaxIO));
        Writer w;
        w.pod(fi->fh);
        uint64_t o = static_cast<uint64_t>(off) + done;
        w.pod(o);
        w.pod(chunk);
        std::vector<uint8_t> resp;
        int st = ctx()->request(OP_READ, w.b, resp);
        if (st) return -st;
        if (resp.empty()) break; // EOF
        std::memcpy(buf + done, resp.data(), resp.size());
        done += resp.size();
        ctx()->bytesRead += resp.size();
        if (resp.size() < chunk) break;
    }
    return static_cast<int>(done);
}

int fs_write(const char*, const char* buf, size_t size, fb_off_t off, struct fuse_file_info* fi) {
    size_t done = 0;
    while (done < size) {
        uint32_t chunk = static_cast<uint32_t>(std::min<size_t>(size - done, kMaxIO));
        Writer w;
        w.pod(fi->fh);
        uint64_t o = static_cast<uint64_t>(off) + done;
        w.pod(o);
        w.raw(buf + done, chunk);
        std::vector<uint8_t> resp;
        int st = ctx()->request(OP_WRITE, w.b, resp);
        if (st) return -st;
        Reader r(resp.data(), resp.size());
        uint32_t written;
        if (!r.pod(written)) return -EIO;
        done += written;
        ctx()->bytesWritten += written;
        if (written < chunk) break;
    }
    return static_cast<int>(done);
}

template <uint16_t Op>
int fs_fh_only(const char*, struct fuse_file_info* fi) {
    Writer w;
    w.pod(fi->fh);
    std::vector<uint8_t> resp;
    int st = ctx()->request(Op, w.b, resp);
    return st ? -st : 0;
}

int fs_release(const char* p, struct fuse_file_info* fi) { return fs_fh_only<OP_RELEASE>(p, fi); }
int fs_flush(const char* p, struct fuse_file_info* fi) { return fs_fh_only<OP_FLUSH>(p, fi); }

int fs_fsync(const char*, int, struct fuse_file_info* fi) {
    Writer w;
    w.pod(fi->fh);
    std::vector<uint8_t> resp;
    int st = ctx()->request(OP_FSYNC, w.b, resp);
    return st ? -st : 0;
}

template <uint16_t Op>
int fs_path_only(const char* path) {
    Writer w;
    w.str(path);
    std::vector<uint8_t> resp;
    int st = ctx()->request(Op, w.b, resp);
    return st ? -st : 0;
}

int fs_unlink(const char* p) { return fs_path_only<OP_UNLINK>(p); }
int fs_rmdir(const char* p) { return fs_path_only<OP_RMDIR>(p); }

int fs_mkdir(const char* path, fb_mode_t mode) {
    Writer w;
    w.str(path);
    uint32_t m = mode;
    w.pod(m);
    std::vector<uint8_t> resp;
    int st = ctx()->request(OP_MKDIR, w.b, resp);
    return st ? -st : 0;
}

int fs_rename(const char* from, const char* to, unsigned int) {
    Writer w;
    w.str(from);
    w.str(to);
    std::vector<uint8_t> resp;
    int st = ctx()->request(OP_RENAME, w.b, resp);
    return st ? -st : 0;
}

int fs_truncate(const char* path, fb_off_t size, struct fuse_file_info*) {
    Writer w;
    w.str(path);
    uint64_t s = static_cast<uint64_t>(size);
    w.pod(s);
    std::vector<uint8_t> resp;
    int st = ctx()->request(OP_TRUNCATE, w.b, resp);
    return st ? -st : 0;
}

int fs_statfs(const char* path, FB_STATVFS* sv) {
    Writer w;
    w.str(path);
    std::vector<uint8_t> resp;
    int st = ctx()->request(OP_STATFS, w.b, resp);
    if (st) return -st;
    Reader r(resp.data(), resp.size());
    WireStatvfs s;
    if (!r.pod(s)) return -EIO;
    std::memset(sv, 0, sizeof(*sv));
    sv->f_bsize = s.bsize;
    sv->f_frsize = s.frsize;
    sv->f_blocks = s.blocks;
    sv->f_bfree = s.bfree;
    sv->f_bavail = s.bavail;
    sv->f_files = s.files;
    sv->f_ffree = s.ffree;
    sv->f_namemax = s.namemax;
    return 0;
}

int fs_access(const char* path, int mode) {
    Writer w;
    w.str(path);
    uint32_t m = static_cast<uint32_t>(mode);
    w.pod(m);
    std::vector<uint8_t> resp;
    int st = ctx()->request(OP_ACCESS, w.b, resp);
    return st ? -st : 0;
}

int fs_chmod(const char* path, fb_mode_t mode, struct fuse_file_info*) {
    Writer w;
    w.str(path);
    uint32_t m = mode;
    w.pod(m);
    std::vector<uint8_t> resp;
    int st = ctx()->request(OP_CHMOD, w.b, resp);
    return st ? -st : 0;
}

int fs_utimens(const char* path, const FB_TIMESPEC tv[2], struct fuse_file_info*) {
    Writer w;
    w.str(path);
    int64_t atime = tv ? tv[0].tv_sec : 0;
    int64_t mtime = tv ? tv[1].tv_sec : 0;
    w.pod(atime);
    w.pod(mtime);
    std::vector<uint8_t> resp;
    int st = ctx()->request(OP_UTIMENS, w.b, resp);
    return st ? -st : 0;
}

void* fs_init(struct fuse_conn_info* conn, struct fuse_config* cfg) {
    // Cache aggressively so most ops never hit the network.
    cfg->kernel_cache = 1;
    cfg->entry_timeout = 1.0;
    cfg->attr_timeout = 1.0;
    cfg->negative_timeout = 1.0;
    cfg->use_ino = 0;
    conn->max_write = kMaxIO;
#ifdef FUSE_CAP_WRITEBACK_CACHE
    if (conn->capable & FUSE_CAP_WRITEBACK_CACHE) conn->want |= FUSE_CAP_WRITEBACK_CACHE;
#endif
#ifdef FUSE_CAP_PARALLEL_DIROPS
    if (conn->capable & FUSE_CAP_PARALLEL_DIROPS) conn->want |= FUSE_CAP_PARALLEL_DIROPS;
#endif
    return fuse_get_context()->private_data;
}

struct fuse_operations make_ops() {
    struct fuse_operations o;
    std::memset(&o, 0, sizeof(o));
    o.init = fs_init;
    o.getattr = fs_getattr;
    o.readdir = fs_readdir;
    o.open = fs_open;
    o.create = fs_create;
    o.read = fs_read;
    o.write = fs_write;
    o.release = fs_release;
    o.flush = fs_flush;
    o.fsync = fs_fsync;
    o.unlink = fs_unlink;
    o.rmdir = fs_rmdir;
    o.mkdir = fs_mkdir;
    o.rename = fs_rename;
    o.truncate = fs_truncate;
    o.statfs = fs_statfs;
    o.access = fs_access;
    o.chmod = fs_chmod;
    o.utimens = fs_utimens;
    return o;
}

struct fuse_operations g_ops = make_ops();

} // namespace

namespace {

std::string sanitize(const std::string& name) {
    std::string s;
    for (char c : name) {
        // keep it filesystem/volume-label safe across all three OSes
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|')
            s += '_';
        else
            s += c;
    }
    if (s.empty()) s = "share";
    return s;
}

#ifdef _WIN32
// A free drive letter; high letters first to avoid C:/system drives.
std::string free_drive_letter() {
    DWORD mask = ::GetLogicalDrives();
    for (char c = 'Z'; c >= 'D'; --c)
        if (!(mask & (1u << (c - 'A')))) return std::string(1, c) + ":";
    return {};
}

bool label_in_use(const std::string& label) {
    DWORD mask = ::GetLogicalDrives();
    for (char c = 'A'; c <= 'Z'; ++c) {
        if (!(mask & (1u << (c - 'A')))) continue;
        std::string root = std::string(1, c) + ":\\";
        char vol[MAX_PATH] = {0};
        if (::GetVolumeInformationA(root.c_str(), vol, MAX_PATH, nullptr, nullptr, nullptr,
                                    nullptr, 0))
            if (label == vol) return true;
    }
    return false;
}

std::string dedupe_label(const std::string& name) {
    for (int n = 1; n < 1000; ++n) {
        std::string c = n == 1 ? name : name + "-" + std::to_string(n);
        if (!label_in_use(c)) return c;
    }
    return name;
}
#else
// Reuse the name if the path is free (or just a leftover empty dir); otherwise
// append -2, -3, … so a second "cool-folder" mounts as "cool-folder-2".
std::string dedupe_path(const std::string& base, const std::string& name) {
    namespace fs = std::filesystem;
    for (int n = 1; n < 1000; ++n) {
        std::string cand = n == 1 ? name : name + "-" + std::to_string(n);
        std::error_code ec;
        fs::path p = fs::path(base) / cand;
        if (!fs::exists(p, ec)) return cand;
        if (fs::is_directory(p, ec) && fs::is_empty(p, ec)) return cand;
    }
    return name;
}
#endif

} // namespace

bool Mount::start(RemoteFs* client, const std::string& mountBase, const std::string& volname,
                  bool allowWrites, std::string& err) {
    if (!ensure_fuse_backend(err)) return false;

    std::string name = sanitize(volname);
    std::string label; // volume label / displayed disk name
#if defined(_WIN32) || defined(__APPLE__)
    (void)mountBase; // the mountpoint is a drive letter / /Volumes, not under mountBase
#endif

#ifdef _WIN32
    label = dedupe_label(name);
    mp_ = free_drive_letter();
    if (mp_.empty()) { err = "no free drive letter to mount the share"; return false; }
#elif defined(__APPLE__)
    label = dedupe_path("/Volumes", name); // /Volumes is the place disks show up
    mp_ = "/Volumes/" + label;
    std::error_code ec;
    std::filesystem::create_directories(mp_, ec);
#else
    label = dedupe_path(mountBase, name);
    mp_ = mountBase + "/" + label;
    std::error_code ec;
    std::filesystem::create_directories(mp_, ec);
#endif

    struct fuse_args args = FUSE_ARGS_INIT(0, nullptr);
    fuse_opt_add_arg(&args, "folderbuddies");
    fuse_opt_add_arg(&args, "-o");
    fuse_opt_add_arg(&args, "max_read=1048576");
    if (!allowWrites) {
        fuse_opt_add_arg(&args, "-o");
        fuse_opt_add_arg(&args, "ro");
    }
#if defined(__APPLE__)
    fuse_opt_add_arg(&args, "-o");
    std::string vn = "volname=" + label;
    fuse_opt_add_arg(&args, vn.c_str());
    fuse_opt_add_arg(&args, "-o");
    fuse_opt_add_arg(&args, "local"); // show on the desktop / Finder as a disk
    fuse_opt_add_arg(&args, "-o");
    fuse_opt_add_arg(&args, "noappledouble");
#elif defined(_WIN32)
    fuse_opt_add_arg(&args, "-o");
    std::string vn = "volname=" + label;
    fuse_opt_add_arg(&args, vn.c_str());
#else
    (void)label;
#endif

    // Insert the RAM-only cache between the kernel and the transport. The mount
    // layer talks to the cache; the cache talks to `client`. No persistence.
    cache_ = std::make_unique<RamCache>(client);

    fuse_ = fuse_new(&args, &g_ops, sizeof(g_ops), cache_.get());
    fuse_opt_free_args(&args);
    if (!fuse_) { err = "fuse_new failed (is the FUSE driver installed?)"; cache_.reset(); return false; }

    if (fuse_mount(fuse_, mp_.c_str()) != 0) {
        err = "fuse_mount failed for " + mp_;
        fuse_destroy(fuse_);
        fuse_ = nullptr;
        return false;
    }

    thread_ = std::thread([this] { fuse_loop_mt(fuse_, 0); });
    return true;
}

void Mount::stop() {
    if (!fuse_) return;
    fuse_exit(fuse_);
    fuse_unmount(fuse_);
    if (thread_.joinable()) thread_.join();
    fuse_destroy(fuse_);
    fuse_ = nullptr;
    // Drop the cache (joins prefetch threads) while the transport is still alive.
    cache_.reset();
}

} // namespace fb
