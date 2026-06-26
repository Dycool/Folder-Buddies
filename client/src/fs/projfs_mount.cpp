#include "fuse_fs.h"

#ifdef _WIN32

#include "remote_fs.h"
#include "common.h"
#include "fuse_backend.h"
#include "osflags.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <rpc.h>
#include <shellapi.h>
#include <shlobj_core.h>
#include "projfs_loader.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <system_error>
#include <utility>
#include <vector>

namespace fb {
namespace {

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string narrow_raw(PCWSTR s) {
    if (!s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    for (char& c : out)
        if (c == '\\') c = '/';
    return out;
}

std::string make_rooted(std::string out) {
    if (out.empty()) return "/";
    if (out.front() != '/') out.insert(out.begin(), '/');
    return out;
}

std::string narrow(PCWSTR s) {
    return make_rooted(narrow_raw(s));
}

std::string narrow_optional(PCWSTR s) {
    std::string out = narrow_raw(s);
    if (out.empty()) return {};
    return make_rooted(std::move(out));
}

std::string sanitize(const std::string& name) {
    std::string s;
    for (char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|')
            s += '_';
        else
            s += c;
    }
    if (s.empty()) s = "share";
    return s;
}

std::string free_drive_letter() {
    DWORD mask = ::GetLogicalDrives();
    for (char c = 'Z'; c >= 'D'; --c) {
        if (!(mask & (1u << (c - 'A')))) return std::string(1, c) + ":";
    }
    return {};
}

std::string dedupe_path(const std::string& base, const std::string& name) {
    namespace fs = std::filesystem;
    for (int n = 1; n < 1000; ++n) {
        std::string cand = n == 1 ? name : name + "-" + std::to_string(n);
        std::error_code ec;
        fs::path p = fs::path(base) / cand;
        if (!fs::exists(p, ec)) return p.string();
        if (fs::is_directory(p, ec) && fs::is_empty(p, ec)) return p.string();
    }
    return (fs::path(base) / name).string();
}

std::string default_backing_base(const std::string& mountBase) {
    if (!mountBase.empty()) return mountBase;
    const char* local = std::getenv("LOCALAPPDATA");
    if (local && *local) return (std::filesystem::path(local) / "FolderBuddies" / "mounts").string();
    const char* profile = std::getenv("USERPROFILE");
    return (std::filesystem::path(profile && *profile ? profile : "C:\\") /
            "AppData" / "Local" / "FolderBuddies" / "mounts").string();
}

std::wstring drive_root(const std::wstring& driveName) {
    return driveName + L"\\";
}

std::wstring dos_device_target(const std::string& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    return L"\\??\\" + widen((ec ? std::filesystem::path(path) : absolute).string());
}

bool drive_mapping_exists(const std::wstring& driveName) {
    wchar_t target[32768];
    return ::QueryDosDeviceW(driveName.c_str(), target,
                             static_cast<DWORD>(sizeof(target) / sizeof(target[0]))) != 0;
}

std::string windows_error(const char* what) {
    return std::string(what) + " (Windows error " + std::to_string(::GetLastError()) + ")";
}

LARGE_INTEGER unix_to_filetime(int64_t unixSeconds) {
    LARGE_INTEGER li{};
    li.QuadPart = (unixSeconds + 11644473600LL) * 10000000LL;
    return li;
}

PRJ_FILE_BASIC_INFO to_basic_info(const WireAttr& a) {
    PRJ_FILE_BASIC_INFO bi{};
    bool isDir = (a.mode & 0040000u) == 0040000u;
    bi.IsDirectory = isDir ? TRUE : FALSE;
    bi.FileSize = static_cast<INT64>(a.size);
    bi.CreationTime = unix_to_filetime(a.ctime);
    bi.LastAccessTime = unix_to_filetime(a.atime);
    bi.LastWriteTime = unix_to_filetime(a.mtime);
    bi.ChangeTime = unix_to_filetime(a.ctime);
    bi.FileAttributes = isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    return bi;
}

struct DirEntry {
    std::wstring name;
    PRJ_FILE_BASIC_INFO info{};
};

struct ProjfsState {
    RemoteFs* client = nullptr;
    PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT ctx = nullptr;
    bool allowWrites = false;
    std::filesystem::path root;
    std::mutex enumMtx;
    std::unordered_map<std::string, std::vector<DirEntry>> enumerations;
};

HRESULT status_to_hresult(int st) {
    if (st == 0) return S_OK;
    switch (st) {
    case ENOENT: return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    case EACCES: return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    case ENOTDIR: return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    case EISDIR: return HRESULT_FROM_WIN32(ERROR_CANNOT_MAKE);
    default: return HRESULT_FROM_WIN32(ERROR_GEN_FAILURE);
    }
}

constexpr PRJ_NOTIFY_TYPES kWriteNotifyMask = static_cast<PRJ_NOTIFY_TYPES>(
    PRJ_NOTIFY_FILE_OPENED |
    PRJ_NOTIFY_NEW_FILE_CREATED |
    PRJ_NOTIFY_FILE_OVERWRITTEN |
    PRJ_NOTIFY_PRE_DELETE |
    PRJ_NOTIFY_PRE_RENAME |
    PRJ_NOTIFY_PRE_SET_HARDLINK |
    PRJ_NOTIFY_FILE_RENAMED |
    PRJ_NOTIFY_HARDLINK_CREATED |
    PRJ_NOTIFY_FILE_HANDLE_CLOSED_FILE_MODIFIED |
    PRJ_NOTIFY_FILE_HANDLE_CLOSED_FILE_DELETED |
    PRJ_NOTIFY_FILE_PRE_CONVERT_TO_FULL);

PRJ_FILE_BASIC_INFO apply_readonly(const PRJ_FILE_BASIC_INFO& in, bool allowWrites) {
    PRJ_FILE_BASIC_INFO out = in;
    if (!allowWrites && !out.IsDirectory) out.FileAttributes |= FILE_ATTRIBUTE_READONLY;
    return out;
}

std::filesystem::path local_path(ProjfsState* st, const std::string& rel) {
    std::string p = rel;
    while (!p.empty() && (p.front() == '/' || p.front() == '\\')) p.erase(p.begin());
    std::filesystem::path out = st->root;
    if (!p.empty()) out /= std::filesystem::u8path(p);
    return out;
}

int remote_path_only(ProjfsState* st, uint16_t op, const std::string& path) {
    Writer w;
    w.str(path);
    std::vector<uint8_t> resp;
    return st->client->request(op, w.b, resp);
}

int remote_mkdir(ProjfsState* st, const std::string& path) {
    Writer w;
    w.str(path);
    uint32_t mode = 0755;
    w.pod(mode);
    std::vector<uint8_t> resp;
    int rc = st->client->request(OP_MKDIR, w.b, resp);
    return rc == EEXIST ? 0 : rc;
}

int remote_rename(ProjfsState* st, const std::string& from, const std::string& to) {
    Writer w;
    w.str(from);
    w.str(to);
    std::vector<uint8_t> resp;
    return st->client->request(OP_RENAME, w.b, resp);
}

int remote_truncate(ProjfsState* st, const std::string& path, uint64_t size) {
    Writer w;
    w.str(path);
    w.pod(size);
    std::vector<uint8_t> resp;
    return st->client->request(OP_TRUNCATE, w.b, resp);
}

int remote_open_for_write(ProjfsState* st, const std::string& path, uint64_t& fh) {
    Writer w;
    w.str(path);
    int32_t flags = FB_O_WRONLY | FB_O_CREAT | FB_O_TRUNC;
    uint32_t mode = 0644;
    w.pod(flags);
    w.pod(mode);
    std::vector<uint8_t> resp;
    int rc = st->client->request(OP_CREATE, w.b, resp);
    if (rc) return rc;
    Reader r(resp.data(), resp.size());
    return r.pod(fh) ? 0 : EIO;
}

void remote_release(ProjfsState* st, uint64_t fh) {
    Writer w;
    w.pod(fh);
    std::vector<uint8_t> ignored;
    st->client->request(OP_RELEASE, w.b, ignored);
}

int remote_create_empty_file(ProjfsState* st, const std::string& path) {
    uint64_t fh = 0;
    int rc = remote_open_for_write(st, path, fh);
    if (rc) return rc;
    remote_release(st, fh);
    return remote_truncate(st, path, 0);
}

int remote_write(ProjfsState* st, uint64_t fh, uint64_t offset, const char* data, uint32_t n) {
    Writer w;
    w.pod(fh);
    w.pod(offset);
    w.raw(data, n);
    std::vector<uint8_t> resp;
    int rc = st->client->request(OP_WRITE, w.b, resp);
    if (rc) return rc;
    Reader r(resp.data(), resp.size());
    uint32_t written = 0;
    if (!r.pod(written)) return EIO;
    return written == n ? 0 : EIO;
}

int sync_local_file_to_remote(ProjfsState* st, const std::string& relPath) {
    std::filesystem::path lp = local_path(st, relPath);
    std::error_code ec;
    if (std::filesystem::is_directory(lp, ec)) return remote_mkdir(st, relPath);
    if (!std::filesystem::exists(lp, ec)) return ENOENT;

    uint64_t fh = 0;
    int rc = remote_open_for_write(st, relPath, fh);
    if (rc) return rc;

    std::ifstream in(lp, std::ios::binary);
    if (!in) { remote_release(st, fh); return EIO; }
    std::vector<char> buf(kMaxIO);
    uint64_t offset = 0;
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize got = in.gcount();
        if (got <= 0) break;
        rc = remote_write(st, fh, offset, buf.data(), static_cast<uint32_t>(got));
        if (rc) { remote_release(st, fh); return rc; }
        offset += static_cast<uint64_t>(got);
        st->client->bytesWritten += static_cast<uint64_t>(got);
    }
    remote_release(st, fh);
    return remote_truncate(st, relPath, offset);
}

HRESULT query_attr(ProjfsState* st, const std::string& path, WireAttr& attr) {
    Writer w;
    w.str(path);
    std::vector<uint8_t> resp;
    int rc = st->client->request(OP_GETATTR, w.b, resp);
    if (rc) return status_to_hresult(rc);
    Reader r(resp.data(), resp.size());
    if (!r.pod(attr)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    return S_OK;
}

std::vector<DirEntry> query_dir(ProjfsState* st, const std::string& path) {
    Writer w;
    w.str(path);
    std::vector<uint8_t> resp;
    int rc = st->client->request(OP_READDIR, w.b, resp);
    if (rc) return {};
    Reader r(resp.data(), resp.size());
    uint32_t n = 0;
    if (!r.pod(n)) return {};
    std::vector<DirEntry> entries;
    entries.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        std::string name;
        WireAttr a;
        if (!r.str(name) || !r.pod(a)) break;
        entries.push_back({widen(name), apply_readonly(to_basic_info(a), st->allowWrites)});
    }
    std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return entries;
}

std::string enum_key(const GUID& g) {
    RPC_CSTR s = nullptr;
    if (UuidToStringA(const_cast<GUID*>(&g), &s) != RPC_S_OK || !s) return {};
    std::string out(reinterpret_cast<char*>(s));
    RpcStringFreeA(&s);
    return out;
}

HRESULT CALLBACK start_enum_cb(const PRJ_CALLBACK_DATA* data, const GUID* enumId) {
    auto* st = static_cast<ProjfsState*>(data->InstanceContext);
    std::lock_guard<std::mutex> lk(st->enumMtx);
    st->enumerations[enum_key(*enumId)] = query_dir(st, narrow(data->FilePathName));
    return S_OK;
}

HRESULT CALLBACK end_enum_cb(const PRJ_CALLBACK_DATA* data, const GUID* enumId) {
    auto* st = static_cast<ProjfsState*>(data->InstanceContext);
    std::lock_guard<std::mutex> lk(st->enumMtx);
    st->enumerations.erase(enum_key(*enumId));
    return S_OK;
}

HRESULT CALLBACK get_enum_cb(const PRJ_CALLBACK_DATA* data, const GUID* enumId,
                             PCWSTR searchExpression, PRJ_DIR_ENTRY_BUFFER_HANDLE dirEntryBufferHandle) {
    auto* st = static_cast<ProjfsState*>(data->InstanceContext);
    std::vector<DirEntry> entries;
    {
        std::lock_guard<std::mutex> lk(st->enumMtx);
        auto it = st->enumerations.find(enum_key(*enumId));
        if (it == st->enumerations.end()) return HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER);
        entries = it->second;
    }
    for (const auto& e : entries) {
        if (searchExpression && !fb::projfs::FileNameMatch(e.name.c_str(), searchExpression)) continue;
        HRESULT hr = fb::projfs::FillDirEntryBuffer(e.name.c_str(), const_cast<PRJ_FILE_BASIC_INFO*>(&e.info),
                                           dirEntryBufferHandle);
        if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) return S_OK;
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

HRESULT CALLBACK placeholder_cb(const PRJ_CALLBACK_DATA* data) {
    auto* st = static_cast<ProjfsState*>(data->InstanceContext);
    WireAttr a{};
    HRESULT hr = query_attr(st, narrow(data->FilePathName), a);
    if (FAILED(hr)) return hr;
    PRJ_PLACEHOLDER_INFO info{};
    info.FileBasicInfo = apply_readonly(to_basic_info(a), st->allowWrites);
    return fb::projfs::WritePlaceholderInfo(st->ctx, data->FilePathName, &info, sizeof(info));
}

HRESULT CALLBACK file_data_cb(const PRJ_CALLBACK_DATA* data, UINT64 byteOffset, UINT32 length) {
    auto* st = static_cast<ProjfsState*>(data->InstanceContext);
    uint64_t offset = byteOffset;
    uint32_t remaining = length;
    while (remaining > 0) {
        uint32_t chunk = std::min<uint32_t>(remaining, kMaxIO);
        Writer w;
        uint64_t pseudoFh = 0;
        w.pod(pseudoFh); // server OP_READ expects a file handle, so open first below
        (void)w;

        // Open/read/release keeps ProjFS stateless and avoids leaking handles.
        Writer ow;
        ow.str(narrow(data->FilePathName));
        int32_t flags = 0; // read-only
        uint32_t mode = 0;
        ow.pod(flags);
        ow.pod(mode);
        std::vector<uint8_t> openResp;
        int rc = st->client->request(OP_OPEN, ow.b, openResp);
        if (rc) return status_to_hresult(rc);
        Reader ordr(openResp.data(), openResp.size());
        uint64_t fh = 0;
        if (!ordr.pod(fh)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);

        Writer rw;
        rw.pod(fh);
        rw.pod(offset);
        rw.pod(chunk);
        std::vector<uint8_t> readResp;
        rc = st->client->request(OP_READ, rw.b, readResp);

        Writer rel;
        rel.pod(fh);
        std::vector<uint8_t> ignored;
        st->client->request(OP_RELEASE, rel.b, ignored);

        if (rc) return status_to_hresult(rc);
        if (readResp.empty()) break;
        HRESULT hr = fb::projfs::WriteFileData(st->ctx, &data->DataStreamId, readResp.data(), offset,
                                      static_cast<UINT32>(readResp.size()));
        if (FAILED(hr)) return hr;
        offset += readResp.size();
        remaining -= static_cast<UINT32>(readResp.size());
        if (readResp.size() < chunk) break;
    }
    st->client->bytesRead += (length - remaining);
    return S_OK;
}

HRESULT CALLBACK notification_cb(const PRJ_CALLBACK_DATA* data, BOOLEAN isDirectory,
                                 PRJ_NOTIFICATION notification, PCWSTR destinationFileName,
                                 PRJ_NOTIFICATION_PARAMETERS* operationParameters) {
    auto* st = static_cast<ProjfsState*>(data->InstanceContext);
    const bool dir = isDirectory == TRUE;
    const std::string path = narrow_optional(data->FilePathName);
    const std::string dest = narrow_optional(destinationFileName);

    auto deny_write = [&]() -> HRESULT {
        return st->allowWrites ? S_OK : HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    };
    auto post_mask = [&]() {
        if (!operationParameters) return;
        operationParameters->PostCreate.NotificationMask = kWriteNotifyMask;
    };
    auto rename_mask = [&]() {
        if (!operationParameters) return;
        operationParameters->FileRenamed.NotificationMask = kWriteNotifyMask;
    };

    switch (notification) {
    case PRJ_NOTIFICATION_FILE_OPENED:
        post_mask();
        return S_OK;

    case PRJ_NOTIFICATION_NEW_FILE_CREATED:
        post_mask();
        if (!st->allowWrites) return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        if (dir) return status_to_hresult(remote_mkdir(st, path));
        // Create/truncate now so zero-byte files appear remotely even if no
        // modified-close notification follows. Final contents are uploaded on close.
        return status_to_hresult(remote_create_empty_file(st, path));

    case PRJ_NOTIFICATION_FILE_OVERWRITTEN:
        post_mask();
        if (!st->allowWrites) return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        return dir ? S_OK : status_to_hresult(remote_create_empty_file(st, path));

    case PRJ_NOTIFICATION_PRE_DELETE:
        if (FAILED(deny_write())) return deny_write();
        return status_to_hresult(remote_path_only(st, dir ? OP_RMDIR : OP_UNLINK, path));

    case PRJ_NOTIFICATION_PRE_RENAME:
        if (FAILED(deny_write())) return deny_write();
        if (path == "/") return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        if (dest.empty()) return status_to_hresult(remote_path_only(st, dir ? OP_RMDIR : OP_UNLINK, path));
        // Empty source means an item is moving in from outside the virtualization
        // root. It cannot be renamed remotely yet because no remote source exists.
        if (path.empty() || path == "/") return S_OK;
        return status_to_hresult(remote_rename(st, path, dest));

    case PRJ_NOTIFICATION_FILE_RENAMED:
        rename_mask();
        if (!st->allowWrites) return S_OK;
        // If an item moved in from outside the root, upload/create it now.
        if ((path.empty() || path == "/") && !dest.empty()) return status_to_hresult(sync_local_file_to_remote(st, dest));
        return S_OK;

    case PRJ_NOTIFICATION_PRE_SET_HARDLINK:
        // Hard links do not map safely to the remote cross-platform protocol.
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

    case PRJ_NOTIFICATION_HARDLINK_CREATED:
        return S_OK;

    case PRJ_NOTIFICATION_FILE_PRE_CONVERT_TO_FULL:
        return deny_write();

    case PRJ_NOTIFICATION_FILE_HANDLE_CLOSED_FILE_MODIFIED:
        if (!st->allowWrites) return S_OK;
        if (dir) return S_OK;
        return status_to_hresult(sync_local_file_to_remote(st, path));

    case PRJ_NOTIFICATION_FILE_HANDLE_CLOSED_FILE_DELETED:
        return S_OK; // PRE_DELETE already propagated and can veto failures.

    default:
        return S_OK;
    }
}

PRJ_CALLBACKS callbacks() {
    PRJ_CALLBACKS cb{};
    cb.StartDirectoryEnumerationCallback = start_enum_cb;
    cb.EndDirectoryEnumerationCallback = end_enum_cb;
    cb.GetDirectoryEnumerationCallback = get_enum_cb;
    cb.GetPlaceholderInfoCallback = placeholder_cb;
    cb.GetFileDataCallback = file_data_cb;
    cb.NotificationCallback = notification_cb;
    return cb;
}

} // namespace

bool Mount::start(RemoteFs* client, const std::string& mountBase, const std::string& volname,
                  bool allowWrites, std::string& err) {
    if (!ensure_fuse_backend(err)) return false;
    if (!fb::projfs::ensure_loaded()) { err = "ProjectedFSLib.dll could not be loaded"; return false; }
    if (backend_) { err = "a share is already mounted"; return false; }

    stopping_.store(false);
    active_.store(false);
    driveName_.clear();
    driveTarget_.clear();

    std::string base = default_backing_base(mountBase);
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    std::string backingRoot = dedupe_path(base, sanitize(volname));
    std::filesystem::create_directories(backingRoot, ec);
    if (ec) { err = "failed to create ProjFS root: " + backingRoot; return false; }

    // Insert the RAM-only cache between ProjFS and the transport. No persistence.
    cache_ = std::make_unique<RamCache>(client);

    auto* state = new ProjfsState;
    state->client = cache_.get();
    state->allowWrites = allowWrites;
    state->root = std::filesystem::path(backingRoot);
    PRJ_CALLBACKS cb = callbacks();
    HRESULT hr = fb::projfs::MarkDirectoryAsPlaceholder(widen(backingRoot).c_str(), nullptr, nullptr, nullptr);
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        delete state;
        cache_.reset();
        err = "fb::projfs::MarkDirectoryAsPlaceholder failed";
        return false;
    }
    PRJ_NOTIFICATION_MAPPING mapping{};
    mapping.NotificationRoot = L"";
    mapping.NotificationBitMask = kWriteNotifyMask;
    PRJ_STARTVIRTUALIZING_OPTIONS opts{};
    opts.NotificationMappings = &mapping;
    opts.NotificationMappingsCount = 1;
    hr = fb::projfs::StartVirtualizing(widen(backingRoot).c_str(), &cb, state, &opts, &state->ctx);
    if (FAILED(hr)) {
        delete state;
        cache_.reset();
        err = "fb::projfs::StartVirtualizing failed";
        return false;
    }

    std::string drive = free_drive_letter();
    if (drive.empty()) {
        fb::projfs::StopVirtualizing(state->ctx);
        delete state;
        cache_.reset();
        err = "no free drive letter to mount the share";
        return false;
    }

    driveName_ = widen(drive);
    driveTarget_ = dos_device_target(backingRoot);
    if (!::DefineDosDeviceW(DDD_RAW_TARGET_PATH, driveName_.c_str(), driveTarget_.c_str())) {
        fb::projfs::StopVirtualizing(state->ctx);
        delete state;
        cache_.reset();
        driveName_.clear();
        driveTarget_.clear();
        err = windows_error("DefineDosDevice failed");
        return false;
    }

    mp_ = drive + "\\";
    backend_ = state;
    active_.store(true);
    ::SHChangeNotify(SHCNE_DRIVEADD, SHCNF_PATHW, drive_root(driveName_).c_str(), nullptr);

    thread_ = std::thread([this] {
        while (!stopping_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!driveName_.empty() && !drive_mapping_exists(driveName_)) {
                if (!stopping_.load() && active_.exchange(false)) notifyEjected();
                break;
            }
        }
    });
    return true;
}

void Mount::stop() {
    stopping_.store(true);
    active_.store(false);

    if (!driveName_.empty()) {
        std::wstring root = drive_root(driveName_);
        ::DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE | DDD_RAW_TARGET_PATH,
                           driveName_.c_str(), driveTarget_.empty() ? nullptr : driveTarget_.c_str());
        ::SHChangeNotify(SHCNE_DRIVEREMOVED, SHCNF_PATHW, root.c_str(), nullptr);
    }

    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) thread_.detach();
        else thread_.join();
    }

    if (backend_) {
        auto* state = static_cast<ProjfsState*>(backend_);
        if (state->ctx) fb::projfs::StopVirtualizing(state->ctx);
        delete state;
    }
    backend_ = nullptr;
    driveName_.clear();
    driveTarget_.clear();
    // Drop the cache (joins prefetch threads) while the transport is still alive.
    cache_.reset();
}

} // namespace fb

#endif // _WIN32
