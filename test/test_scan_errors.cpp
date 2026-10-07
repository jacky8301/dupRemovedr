// 注入目录 API 失败，避免依赖管理员权限或修改真实目录的 ACL。
#include <windows.h>
#include <string>
#include <filesystem>
#include <fstream>
#include <cstdio>

static std::wstring deniedPattern;
static bool failNext = false;
static int openFindHandles = 0;

static HANDLE WINAPI checkedFindFirst(LPCWSTR path, FINDEX_INFO_LEVELS level, LPVOID data,
    FINDEX_SEARCH_OPS operation, LPVOID filter, DWORD flags) {
    if (path == deniedPattern) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    const HANDLE handle = FindFirstFileExW(path, level, data, operation, filter, flags);
    if (handle != INVALID_HANDLE_VALUE) ++openFindHandles;
    return handle;
}

static BOOL WINAPI checkedFindNext(HANDLE handle, LPWIN32_FIND_DATAW data) {
    if (failNext) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return FindNextFileW(handle, data);
}

static BOOL WINAPI checkedFindClose(HANDLE handle) {
    --openFindHandles;
    return FindClose(handle);
}

#define FindFirstFileExW checkedFindFirst
#define FindNextFileW checkedFindNext
#define FindClose checkedFindClose
#include "../src/scanner.cpp"
#undef FindFirstFileExW
#undef FindNextFileW
#undef FindClose

static bool verifyFailure(const std::filesystem::path& root, const std::filesystem::path& failedPath) {
    bool caught = false;
    int progress = 0;
    try {
        DuplicateScanner().scan(root.wstring(), [&](int, int, const std::wstring&) { ++progress; });
    } catch (const ScanError& error) {
        caught = error.path == failedPath.wstring() && error.code == ERROR_ACCESS_DENIED;
    }
    const bool ok = caught && progress == 0 && openFindHandles == 0;
    std::printf("%s: Directory errors discard partial results and release enumeration handles\n", ok ? "PASS" : "FAIL");
    return ok;
}

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::current_path() / (L"scan-error-fixtures-" + std::to_wstring(GetCurrentProcessId()));
    if (!fs::create_directory(root)) return 2;
    fs::create_directory(root / L"child");
    std::ofstream(root / L"a.bin") << "same";
    std::ofstream(root / L"b.bin") << "same";

    deniedPattern = root.wstring() + L"\\*";
    bool ok = verifyFailure(root, root);
    deniedPattern = (root / L"child").wstring() + L"\\*";
    ok &= verifyFailure(root, root / L"child");
    deniedPattern.clear();
    failNext = true;
    ok &= verifyFailure(root, root);

    fs::remove(root / L"a.bin");
    fs::remove(root / L"b.bin");
    fs::remove(root / L"child");
    fs::remove(root);
    return ok ? 0 : 1;
}
