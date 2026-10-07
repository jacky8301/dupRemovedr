// 仅操作本次创建的独立样本，使用真实 Shell 验证回收边界。
#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <cstdio>
#include <atomic>
#include <stdexcept>

namespace fs = std::filesystem;
static fs::path sourcePath;
static fs::path replacementPath;
static fs::path shellPath;
static int behavior = 0;
static bool replacementInstalled = false;
static bool sourceLocked = false;
static int renameCalls = 0;
static int shellCalls = 0;
static std::atomic_bool* cancellationTarget = nullptr;

static BOOL WINAPI checkedSetInformation(HANDLE file, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID info, DWORD size) {
    if (kind == FileRenameInfo && ++renameCalls == 1) {
        // 在提交暂存前尝试替换源文件，必须被共享锁拒绝。
        sourceLocked = !MoveFileExW(sourcePath.c_str(),
            (sourcePath.parent_path() / L"displaced.bin").c_str(), 0);
        if (behavior == 7) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
        const BOOL result = SetFileInformationByHandle(file, kind, info, size);
        if (behavior == 5 && result) cancellationTarget->store(true);
        return result;
    }
    return SetFileInformationByHandle(file, kind, info, size);
}

static HANDLE WINAPI checkedReopen(HANDLE file, DWORD access, DWORD sharing, DWORD flags) {
    if (behavior == 8 && access == GENERIC_READ) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    return ReOpenFile(file, access, sharing, flags);
}

static int WINAPI checkedShellOperation(LPSHFILEOPSTRUCTW operation) {
    shellPath = operation->pFrom;
    ++shellCalls;
    if (behavior == 1 || behavior == 3) {
        // 在最后一次校验之后创建不同的新文件，模拟同步软件替换原路径。
        if (fs::exists(sourcePath))
            MoveFileExW(sourcePath.c_str(), (sourcePath.parent_path() / L"displaced.bin").c_str(), 0);
        replacementInstalled = MoveFileExW(replacementPath.c_str(), sourcePath.c_str(), 0) != 0;
    }
    if (behavior == 2 || behavior == 3) return ERROR_ACCESS_DENIED;
    if (behavior == 4) throw std::runtime_error("模拟回收异常");
    return SHFileOperationW(operation);
}

#define SHFileOperationW checkedShellOperation
#define SetFileInformationByHandle checkedSetInformation
#define ReOpenFile checkedReopen
#define wWinMain applicationEntry
#include "../src/main.cpp"
#undef wWinMain
#undef SHFileOperationW
#undef SetFileInformationByHandle
#undef ReOpenFile

static void write(const fs::path& path, const std::string& contents) {
    std::ofstream file(path, std::ios::binary);
    file << contents;
    if (!file) throw std::runtime_error("无法写入测试样本");
}

static std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

static bool expect(bool condition, const char* message) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
    return condition;
}

int main() {
    const auto root = fs::current_path() /
        (L"cleanup-safety-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!fs::create_directory(root)) return 2;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = true;
    std::atomic_bool cancelled{false};
    cancellationTarget = &cancelled;
    for (int scenario = 0; scenario < 10; ++scenario) {
        const auto folder = root / std::to_wstring(scenario);
        fs::create_directory(folder);
        const auto kept = folder / L"a.htm";
        sourcePath = folder / L"b.htm";
        replacementPath = folder / L"replacement.bin";
        write(kept, "<html>same</html>");
        write(sourcePath, "<html>same</html>");
        write(replacementPath, "unique replacement");
        const auto assets = folder / L"b_files";
        fs::create_directory(assets);
        write(assets / L"unique.txt", "unique asset");
        behavior = scenario;
        cancelled.store(scenario == 9);
        renameCalls = shellCalls = 0;
        sourceLocked = false;
        shellPath.clear();
        replacementInstalled = false;
        const CleanupTask task{0, sourcePath.wstring(), kept.wstring(), MD5::hashFile(kept.wstring()), 17};
        bool recycled = false;
        std::wstring recoveryPath;
        try { recycled = scenario == 6 ? recycleFile(sourcePath.wstring())
                : recycleVerifiedDuplicate(task, cancelled, &recoveryPath); }
        catch (const std::runtime_error&) {}
        if (scenario == 0 || scenario == 6) {
            ok &= expect(recycled && !fs::exists(sourcePath) && read(assets / L"unique.txt") == "unique asset",
                         "回收 HTML 时保留非重复的关联资源");
        } else if (scenario == 1) {
            ok &= expect(replacementInstalled && recycled && read(sourcePath) == "unique replacement",
                         "校验后原路径上的新文件不被回收");
        } else if (scenario != 3) {
            ok &= expect(!recycled && read(sourcePath) == "<html>same</html>",
                         "回收失败或抛出异常后恢复原文件");
        } else {
            ok &= expect(replacementInstalled && !recycled && read(sourcePath) == "unique replacement" &&
                         shellPath != sourcePath && read(shellPath) == "<html>same</html>" && recoveryPath == shellPath,
                         "恢复时不覆盖新文件，已验证副本仍保留在暂存目录");
            ok &= expect(DuplicateScanner().scan(folder.wstring()).totalDup == 0,
                         "重新扫描原目录时跳过尚未恢复的暂存副本");
        }
        if (scenario != 6 && scenario != 9)
            ok &= expect(sourceLocked, "通过句柄提交暂存之前，源文件不能被重命名替换");
        if (scenario == 5 || scenario >= 7)
            ok &= expect(shellCalls == 0, "取消或暂存失败时不调用 Shell");
        if (scenario != 3) {
            bool hasStaging = false;
            for (const auto& entry : fs::directory_iterator(folder))
                hasStaging |= entry.path().filename().wstring().find(L".dupRemover-staging-") == 0;
            ok &= expect(recoveryPath.empty() && !hasStaging, "成功或恢复原位后不遗留暂存目录");
        }
        ok &= expect(read(kept) == "<html>same</html>", "保留文件始终完整");
    }
    // 保留这次创建的样本，便于检查真实回收后的磁盘状态。
    std::printf("测试样本目录：%s\n", root.u8string().c_str());
    CoUninitialize();
    return ok ? 0 : 1;
}
