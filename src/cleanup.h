#pragma once

#include <windows.h>
#include <shellapi.h>
#include <atomic>
#include <cstdint>
#include <string>

struct CleanupTask {
    size_t row;
    std::wstring path;
    std::wstring keptPath;
    std::string expectedHash;
    uint64_t expectedSize;
};

// 默认使用真实系统调用；测试可为单次清理或对话框提供替身。
struct CleanupOperations {
    decltype(&SHFileOperationW) shellOperation = SHFileOperationW;
    decltype(&SetFileInformationByHandle) setInformation = SetFileInformationByHandle;
    decltype(&ReOpenFile) reopen = ReOpenFile;
};

bool recycleFile(const std::wstring& path, const CleanupOperations& operations = {});
bool recycleVerifiedDuplicate(const CleanupTask& task, const std::atomic_bool& cancelled,
    std::wstring* recoveryPath = nullptr, const CleanupOperations& operations = {});
