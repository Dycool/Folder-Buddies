#pragma once
#ifdef _WIN32

#include <windows.h>
#include <projectedfslib.h>

namespace fb {
namespace projfs {

bool ensure_loaded();

// Function pointer typedefs matching the SDK signatures
using MarkDirectoryAsPlaceholder_t = HRESULT(WINAPI*)(PCWSTR rootPathName, PCWSTR targetPathName, const PRJ_PLACEHOLDER_VERSION_INFO* versionInfo, const GUID* virtualizationInstanceID);
using StartVirtualizing_t = HRESULT(WINAPI*)(PCWSTR virtualizationRootPath, const PRJ_CALLBACKS* callbacks, const void* instanceContext, const PRJ_STARTVIRTUALIZING_OPTIONS* options, PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT* namespaceVirtualizationContext);
using StopVirtualizing_t = void(WINAPI*)(PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT namespaceVirtualizationContext);
using WritePlaceholderInfo_t = HRESULT(WINAPI*)(PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT namespaceVirtualizationContext, PCWSTR destinationFileName, const PRJ_PLACEHOLDER_INFO* placeholderInfo, UINT32 placeholderInfoSize);
using WriteFileData_t = HRESULT(WINAPI*)(PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT namespaceVirtualizationContext, const GUID* dataStreamId, void* buffer, UINT64 byteOffset, UINT32 length);
using FillDirEntryBuffer_t = HRESULT(WINAPI*)(PCWSTR fileName, PRJ_FILE_BASIC_INFO* fileBasicInfo, PRJ_DIR_ENTRY_BUFFER_HANDLE dirEntryBufferHandle);
using FileNameMatch_t = BOOLEAN(WINAPI*)(PCWSTR fileNameToCheck, PCWSTR pattern);

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
