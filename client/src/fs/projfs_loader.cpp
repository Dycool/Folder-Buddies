#include "projfs_loader.h"

#ifdef _WIN32

#include <mutex>

namespace fb {
namespace projfs {

namespace {

HMODULE g_dll = nullptr;
bool g_loaded = false;
std::once_flag g_once;

void do_load() {
    g_dll = LoadLibraryW(L"ProjectedFSLib.dll");
    if (!g_dll) return;

#define LOAD_SYM(name) \
    do { \
        auto p = GetProcAddress(g_dll, "Prj" #name); \
        if (!p) { FreeLibrary(g_dll); g_dll = nullptr; return; } \
        name = reinterpret_cast<decltype(name)>(p); \
    } while (0)

    LOAD_SYM(MarkDirectoryAsPlaceholder);
    LOAD_SYM(StartVirtualizing);
    LOAD_SYM(StopVirtualizing);
    LOAD_SYM(WritePlaceholderInfo);
    LOAD_SYM(WriteFileData);
    LOAD_SYM(FillDirEntryBuffer);
    LOAD_SYM(FileNameMatch);

#undef LOAD_SYM

    g_loaded = true;
}

} // namespace

MarkDirectoryAsPlaceholder_t MarkDirectoryAsPlaceholder = nullptr;
StartVirtualizing_t StartVirtualizing = nullptr;
StopVirtualizing_t StopVirtualizing = nullptr;
WritePlaceholderInfo_t WritePlaceholderInfo = nullptr;
WriteFileData_t WriteFileData = nullptr;
FillDirEntryBuffer_t FillDirEntryBuffer = nullptr;
FileNameMatch_t FileNameMatch = nullptr;

bool ensure_loaded() {
    std::call_once(g_once, do_load);
    return g_loaded;
}

} // namespace projfs
} // namespace fb

#endif // _WIN32
