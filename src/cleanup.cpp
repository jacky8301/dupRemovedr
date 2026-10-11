#include "cleanup.h"
#include "md5.h"
#include <filesystem>
#include <vector>
#include <cstring>
#include <cstddef>
#include <exception>

bool recycleFile(const std::wstring& path, const CleanupOperations& operations) {
    // 相对路径会使 FOF_ALLOWUNDO 失效；不能依赖清理时的工作目录解析路径。
    if (!std::filesystem::path(path).is_absolute()) return false;
    // SHFileOperation 要求路径以两个空字符结尾。
    std::wstring buf = path + L'\0';
    SHFILEOPSTRUCTW f = {};
    f.wFunc  = FO_DELETE;
    f.pFrom  = buf.c_str();
    f.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NO_CONNECTED_ELEMENTS
             | FOF_NOERRORUI  | FOF_SILENT;
    return operations.shellOperation(&f) == 0 && !f.fAnyOperationsAborted;
}

struct CleanupFileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit CleanupFileHandle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
    CleanupFileHandle(const CleanupFileHandle&) = delete;
    CleanupFileHandle& operator=(const CleanupFileHandle&) = delete;
    void reset(HANDLE handle = INVALID_HANDLE_VALUE) {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = handle;
    }
    ~CleanupFileHandle() { reset(); }
};

static std::vector<BYTE> renameInformation(const std::wstring& destination) {
    const DWORD bytes = static_cast<DWORD>(destination.size() * sizeof(wchar_t));
    std::vector<BYTE> buffer(offsetof(FILE_RENAME_INFO, FileName) + bytes + sizeof(wchar_t), 0);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    // 不允许覆盖目标；恢复原位时，新出现的文件必须保留。
    info->ReplaceIfExists = FALSE;
    info->FileNameLength = bytes;
    std::memcpy(info->FileName, destination.c_str(), bytes);
    return buffer;
}

static bool renameOpenFile(HANDLE file, std::vector<BYTE>& information,
                           const CleanupOperations& operations) {
    return operations.setInformation(file, FileRenameInfo, information.data(),
        static_cast<DWORD>(information.size())) != FALSE;
}

static std::wstring cleanupFilePath(HANDLE file) {
    const DWORD length = GetFinalPathNameByHandleW(file, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length) return {};
    std::wstring path(length, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(file, path.data(), length,
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!written || written >= length) return {};
    path.resize(written);
    if (path.compare(0, 8, L"\\\\?\\UNC\\") == 0) path = L"\\\\" + path.substr(8);
    else if (path.compare(0, 4, L"\\\\?\\") == 0) path.erase(0, 4);
    return std::filesystem::path(path).is_absolute() ? path : std::wstring{};
}

// 暂存目录与源文件同盘，保留原文件名。退出时只移除空目录，绝不递归清理。
struct CleanupStage {
    std::wstring directory;
    std::wstring path;
    CleanupFileHandle parentLock;
    CleanupFileHandle directoryLock;

    bool prepare(const std::wstring& original) {
        const auto parent = std::filesystem::path(original).parent_path();
        parentLock.reset(CreateFileW(parent.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (parentLock.value == INVALID_HANDLE_VALUE) return false;
        GUID id{};
        wchar_t text[40]{};
        if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, text, static_cast<int>(std::size(text)))) return false;
        const auto candidate = parent / (std::wstring(L".dupRemover-staging-") + text);
        path = (candidate / std::filesystem::path(original).filename()).wstring();
        // 旧 Shell API 不支持扩展长度路径，不能移动后才发现无法回收。
        if (path.size() >= MAX_PATH) return false;
        const auto candidateName = candidate.wstring();
        if (!CreateDirectoryW(candidateName.c_str(), nullptr)) return false;
        directory = candidateName;
        directoryLock.reset(CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (directoryLock.value == INVALID_HANDLE_VALUE) return false;
        BY_HANDLE_FILE_INFORMATION info{};
        return GetFileInformationByHandle(directoryLock.value, &info) &&
            !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    }

    ~CleanupStage() {
        directoryLock.reset();
        if (!directory.empty()) RemoveDirectoryW(directory.c_str());
    }
};

bool recycleVerifiedDuplicate(const CleanupTask& task, const std::atomic_bool& cancelled,
                                     std::wstring* recoveryPath, const CleanupOperations& operations) {
    if (recoveryPath) recoveryPath->clear();
    if (cancelled.load() || !std::filesystem::path(task.path).is_absolute() ||
        !std::filesystem::path(task.keptPath).is_absolute()) return false;
    // 直到副本进入独立暂存目录，两个文件都不允许其他进程写入或替换。
    CleanupFileHandle kept{CreateFileW(task.keptPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)},
      duplicate{CreateFileW(task.path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (kept.value == INVALID_HANDLE_VALUE || duplicate.value == INVALID_HANDLE_VALUE) return false;
    for (HANDLE file : {kept.value, duplicate.value}) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
    }
    const auto checkCancelled = [&] { return cancelled.load(); };
    if (task.expectedHash.empty() ||
        MD5::hashOpenFile(kept.value, checkCancelled, task.expectedSize) != task.expectedHash ||
        MD5::hashOpenFile(duplicate.value, checkCancelled, task.expectedSize) != task.expectedHash ||
        cancelled.load()) return false;
    const auto original = cleanupFilePath(duplicate.value);
    if (original.empty()) return false;
    CleanupStage stage;
    if (!stage.prepare(original)) return false;
    auto toStage = renameInformation(stage.path);
    auto toOriginal = renameInformation(original);
    // 所有可能分配内存的准备工作都在移动之前完成。
    if (recoveryPath) *recoveryPath = stage.path;
    if (cancelled.load() || !renameOpenFile(duplicate.value, toStage, operations)) {
        if (recoveryPath) recoveryPath->clear();
        return false;
    }
    bool recycled = false;
    std::exception_ptr failure;
    try {
        // ReOpenFile 绑定同一文件对象；仅在隔离原路径后才允许 Shell 重命名它。
        HANDLE shared = operations.reopen(duplicate.value, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_DELETE, FILE_FLAG_SEQUENTIAL_SCAN);
        if (shared != INVALID_HANDLE_VALUE) {
            duplicate.reset(shared);
            if (!cancelled.load()) recycled = recycleFile(stage.path, operations);
        }
    } catch (...) { failure = std::current_exception(); }
    if (recycled) {
        if (recoveryPath) recoveryPath->clear();
    } else {
        // 按仍持有的对象句柄恢复，不能按可能已变化的暂存路径重新找文件。
        CleanupFileHandle restore{operations.reopen(duplicate.value, GENERIC_READ | DELETE,
            FILE_SHARE_READ | FILE_SHARE_DELETE, 0)};
        const bool restored = restore.value != INVALID_HANDLE_VALUE
            ? renameOpenFile(restore.value, toOriginal, operations) : renameOpenFile(duplicate.value, toOriginal, operations);
        if (restored && recoveryPath) recoveryPath->clear();
    }
    // 先关闭文件句柄，再由 stage 析构尝试移除空目录。
    duplicate.reset();
    if (failure) std::rethrow_exception(failure);
    return recycled;
}
