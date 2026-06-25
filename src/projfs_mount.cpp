#include "fuse_fs.h"

#ifdef _WIN32

#include "client.h"
#include "common.h"
#include "fuse_backend.h"
#include "osflags.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <rpc.h>
#include <projectedfslib.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fb {
namespace {

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string narrow(PCWSTR s) {
    if (!s) return "/";
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    for (char& c : out)
        if (c == '\\') c = '/';
    if (out.empty() || out.front() != '/') out.insert(out.begin(), '/');
    return out;
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
    Client* client = nullptr;
    PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT ctx = nullptr;
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
        entries.push_back({widen(name), to_basic_info(a)});
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
        if (searchExpression && !PrjFileNameMatch(e.name.c_str(), searchExpression)) continue;
        HRESULT hr = PrjFillDirEntryBuffer(e.name.c_str(), const_cast<PRJ_FILE_BASIC_INFO*>(&e.info),
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
    info.FileBasicInfo = to_basic_info(a);
    return PrjWritePlaceholderInfo(st->ctx, data->FilePathName, &info, sizeof(info));
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

        // Open/read/release keeps ProjFS stateless. This costs one metadata op
        // on first hydration, but avoids leaking handles if Explorer abandons a read.
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
        HRESULT hr = PrjWriteFileData(st->ctx, &data->DataStreamId, readResp.data(), offset,
                                      static_cast<UINT32>(readResp.size()));
        if (FAILED(hr)) return hr;
        offset += readResp.size();
        remaining -= static_cast<UINT32>(readResp.size());
        if (readResp.size() < chunk) break;
    }
    st->client->bytesRead += (length - remaining);
    return S_OK;
}

PRJ_CALLBACKS callbacks() {
    PRJ_CALLBACKS cb{};
    cb.StartDirectoryEnumerationCallback = start_enum_cb;
    cb.EndDirectoryEnumerationCallback = end_enum_cb;
    cb.GetDirectoryEnumerationCallback = get_enum_cb;
    cb.GetPlaceholderInfoCallback = placeholder_cb;
    cb.GetFileDataCallback = file_data_cb;
    return cb;
}

} // namespace

bool Mount::start(Client* client, const std::string& mountBase, const std::string& volname,
                  std::string& err) {
    if (!ensure_fuse_backend(err)) return false;

    std::string base = mountBase.empty() ? (std::string(std::getenv("USERPROFILE") ? std::getenv("USERPROFILE") : "C:\\") + "\\FolderBuddies") : mountBase;
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    mp_ = dedupe_path(base, sanitize(volname));
    std::filesystem::create_directories(mp_, ec);
    if (ec) { err = "failed to create ProjFS root: " + mp_; return false; }

    auto* state = new ProjfsState;
    state->client = client;
    PRJ_CALLBACKS cb = callbacks();
    HRESULT hr = PrjMarkDirectoryAsPlaceholder(widen(mp_).c_str(), nullptr, nullptr, nullptr);
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        delete state;
        err = "PrjMarkDirectoryAsPlaceholder failed";
        return false;
    }
    hr = PrjStartVirtualizing(widen(mp_).c_str(), &cb, state, nullptr, &state->ctx);
    if (FAILED(hr)) {
        delete state;
        err = "PrjStartVirtualizing failed";
        return false;
    }
    backend_ = state;
    return true;
}

void Mount::stop() {
    if (!backend_) return;
    auto* state = static_cast<ProjfsState*>(backend_);
    if (state->ctx) PrjStopVirtualizing(state->ctx);
    delete state;
    backend_ = nullptr;
}

} // namespace fb

#endif // _WIN32
