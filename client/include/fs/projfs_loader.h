#pragma once
#ifdef _WIN32

#include <projectedfslib.h>
#include <windows.h>

namespace fb {
namespace projfs {

// Loads ProjectedFSLib.dll and resolves all function pointers.
// Must be called before any ProjFs* function. Safe to call multiple times.
bool ensure_loaded();

// Function pointer typedefs matching the SDK signatures
using MarkDirectoryAsPlaceholder_t = HRESULT(WINAPI*)(PCWSTR rootPath, PCWSTR targetPath, PCWSTR virtualizationInstanceID, PRJ_CALLBACK_DATA* callbackData);
using StartVirtualizing_t = HRESULT(WINAPI*)(PCWSTR virtualizationRootPath, const PRJ_CALLBACKS* callbacks, void* instanceContext, const PRJ_STARTVIRTUALIZING_OPTIONS* options, PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT* namespaceVirtualizationContext);
using StopVirtualizing_t = void(WINAPI*)(PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT namespaceVirtualizationContext);
using WritePlaceholderInfo_t = HRESULT(WINAPI*)(PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT namespaceVirtualizationContext, PCWSTR destinationFileName, const PRJ_PLACEHOLDER_INFO* placeholderInfo, UINT32 placeholderInfoSize);
using WriteFileData_t = HRESULT(WINAPI*)(PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT namespaceVirtualizationContext, const PRJ_DATA_STREAM_ID* dataStreamId, const void* buffer, UINT64 byteOffset, UINT32 length);
using FillDirEntryBuffer_t = HRESULT(WINAPI*)(PCWSTR fileName, const PRJ_FILE_BASIC_INFO* basicInfo, PRJ_DIR_ENTRY_BUFFER_HANDLE dirEntryBufferHandle);
using FileNameMatch_t = BOOLEAN(WINAPI*)(PCWSTR fileNameToMatch, PCWSTR pattern);

// Resolved function pointers (nullptr if DLL not loaded or symbol missing)
extern MarkDirectoryAsPlaceholder_t MarkDirectoryAsPlaceholder;
extern StartVirtualizing_t StartVirtualizing;
extern StopVirtualizing_t StopVirtualizing;
extern WritePlaceholderInfo_t WritePlaceholderInfo;
extern WriteFileData_t WriteFileData;
extern FillDirEntryBuffer_t FillDirEntryBuffer;
extern FileNameMatch_t FileNameMatch;

} // namespace projfs
} // namespace fb

#endif // _WIN32
