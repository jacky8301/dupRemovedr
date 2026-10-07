// 用可控的回收站替身验证清理线程，避免测试触碰用户文件。
#include <windows.h>
#include <shellapi.h>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include "../src/md5.h"

static std::filesystem::path fixtureRoot;
static void writeFixture(const std::filesystem::path& path, const char* content) {
    std::ofstream(path, std::ios::binary) << content;
}

static DWORD uiThread;
static std::atomic_bool recycledOnUi{false};
static std::atomic_int calls{0};
static HANDLE recycleGate;
static DWORD delayMs = 0;
static bool failSome = false;
static bool throwError = false;
static std::vector<std::wstring> recycledPaths;
static bool checkProtection = false;
static bool protectedDuringRecycle = false;

static int WINAPI fakeShellOperation(LPSHFILEOPSTRUCTW operation) {
    if (GetCurrentThreadId() == uiThread) recycledOnUi.store(true);
    ++calls;
    if (checkProtection) {
        const auto kept = fixtureRoot / L"kept.bin";
        HANDLE changeKept = CreateFileW(kept.c_str(), GENERIC_WRITE | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        HANDLE changeCopy = CreateFileW(operation->pFrom, GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        protectedDuringRecycle = changeKept == INVALID_HANDLE_VALUE && changeCopy == INVALID_HANDLE_VALUE;
        if (changeKept != INVALID_HANDLE_VALUE) CloseHandle(changeKept);
        if (changeCopy != INVALID_HANDLE_VALUE) CloseHandle(changeCopy);
    }
    WaitForSingleObject(recycleGate, 5000);
    if (delayMs) Sleep(delayMs);
    if (throwError) throw std::runtime_error("模拟回收站异常");
    recycledPaths.push_back(operation->pFrom);
    operation->fAnyOperationsAborted = FALSE;
    return failSome && recycledPaths.size() % 3 == 0 ? 1 : 0;
}

#define SHFileOperationW fakeShellOperation
#define wWinMain applicationEntry
#include "../src/main.cpp"
#undef wWinMain
#undef SHFileOperationW

static bool expect(bool condition, const char* message) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
    return condition;
}

static void pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

static std::wstring label(HWND window, int id) {
    wchar_t buffer[512]{};
    GetDlgItemTextW(window, id, buffer, static_cast<int>(std::size(buffer)));
    return buffer;
}

static void seed(HWND window, DlgData& state, int count) {
    state.folder = L"测试目录";
    state.result = {};
    const auto kept = fixtureRoot / L"kept.bin";
    const auto duplicate = fixtureRoot / L"duplicate.bin";
    writeFixture(kept, "0123456789");
    writeFixture(duplicate, "0123456789");
    DuplicateGroup group{MD5::hashFile(kept.wstring()), {kept.wstring()}, 10};
    // 多行复用同一测试副本，回收站替身不删除文件，用于验证大批量界面更新。
    for (int i = 0; i < count; ++i)
        group.files.push_back(duplicate.wstring());
    state.result.groups.push_back(std::move(group));
    state.result.totalFiles = count + 1;
    state.result.totalDup = count;
    state.result.wastedBytes = count * 10ULL;
    populateListView(GetDlgItem(window, IDC_LIST), state);
    calls.store(0);
    recycledPaths.clear();
}

static bool finish(DlgData& state) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (state.cleaning && std::chrono::steady_clock::now() < deadline) {
        pump();
        Sleep(1);
    }
    return !state.cleaning;
}

// 用真正的模态对话框验证关闭时先停止后台任务，再销毁窗口。
static INT_PTR CALLBACK closeTestProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    const INT_PTR handled = DlgProc(window, msg, wp, lp);
    if (msg == WM_INITDIALOG) {
        auto* state = reinterpret_cast<DlgData*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        seed(window, *state, 300);
        startCleanup(window, *state);
        requestClose(window, state);
    }
    return handled;
}

int main() {
    fixtureRoot = std::filesystem::current_path() /
        (L"cleanup-fixtures-" + std::to_wstring(GetCurrentProcessId()));
    if (!std::filesystem::create_directory(fixtureRoot)) return 2;
    uiThread = GetCurrentThreadId();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    recycleGate = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    HWND window = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_MAIN_DLG), nullptr, DlgProc, 0);
    if (!window || !recycleGate) return 2;
    auto* state = reinterpret_cast<DlgData*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    bool ok = true;

    seed(window, *state, 300);
    ListView_SetItemState(GetDlgItem(window, IDC_LIST), 1, LVIS_SELECTED, LVIS_SELECTED);
    delayMs = 2;
    failSome = true;
    startCleanup(window, *state);
    ok &= expect(state->cleaning && label(window, IDC_STATUS).find(L"0 / 300") != std::wstring::npos,
                 "Cleanup starts immediately with a visible total");
    bool sawProgress = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (state->cleaning && std::chrono::steady_clock::now() < deadline) {
        pump();
        const auto pos = SendDlgItemMessageW(window, IDC_PROGRESS, PBM_GETPOS, 0, 0);
        sawProgress |= state->cleaning && pos > 0 && pos < 1000;
        Sleep(1);
    }
    ok &= expect(!state->cleaning && sawProgress && !recycledOnUi,
                 "UI processes timer progress while recycling runs off the UI thread");
    ok &= expect(state->cleanupSucceeded == 200 && state->cleanupFailed == 100 &&
        state->result.totalDup == 100 && state->result.wastedBytes == 1000 && !state->items[0].removed,
        "Failures stay retryable; retained files and totals are correct");
    ok &= expect(label(window, IDC_STATUS).find(L"失败 100") != std::wstring::npos,
                 "Completion reports failures");
    ok &= expect(state->preview.pixels.empty() && state->preview.message.find(L"回收站") != std::wstring::npos &&
        !IsWindowEnabled(GetDlgItem(window, IDC_REVEAL_BTN)), "Recycling the selected file clears its preview and disables reveal");
    failSome = false;
    delayMs = 0;
    calls.store(0);
    startCleanup(window, *state);
    ok &= expect(finish(*state) && calls == 100 && state->result.totalDup == 0 && state->result.wastedBytes == 0,
                 "Retry only recycles the previously failed files");
    ok &= expect(!IsWindowEnabled(GetDlgItem(window, IDC_DELETE_BTN)) &&
        SendDlgItemMessageW(window, IDC_PROGRESS, PBM_GETPOS, 0, 0) == 1000,
        "Successful cleanup reaches full progress and disables cleanup");

    seed(window, *state, 300);
    ResetEvent(recycleGate);
    startCleanup(window, *state);
    const auto started = std::chrono::steady_clock::now();
    while (calls == 0 && std::chrono::steady_clock::now() - started < std::chrono::seconds(2)) {
        pump();
        Sleep(1);
    }
    SendMessageW(window, WM_COMMAND, IDC_SCAN_BTN, 0);
    ok &= expect(state->cleaning && !state->scanning &&
        !IsWindowEnabled(GetDlgItem(window, IDC_BROWSE_BTN)) &&
        !IsWindowEnabled(GetDlgItem(window, IDC_SCAN_BTN)), "Conflicting actions are blocked during cleanup");
    SendMessageW(window, WM_COMMAND, IDC_DELETE_BTN, 0);
    ok &= expect(state->cancelled && state->cleaning &&
        label(window, IDC_STATUS).find(L"等待当前文件") != std::wstring::npos,
        "Stop responds while an individual recycle operation is blocked");
    SetEvent(recycleGate);
    ok &= expect(finish(*state) && calls == 1 && state->result.totalDup == 299 &&
        label(window, IDC_STATUS).find(L"清理已停止") != std::wstring::npos,
        "Stop preserves the completed result and leaves remaining files untouched");

    seed(window, *state, 5000);
    startCleanup(window, *state);
    ok &= expect(finish(*state) && state->cleanupSucceeded == 5000 && state->result.totalDup == 0,
                 "Large result batches drain without losing outcomes");
    ok &= expect(std::find(recycledPaths.begin(), recycledPaths.end(), (fixtureRoot / L"kept.bin").wstring()) == recycledPaths.end(),
                 "Retained file is never sent to the recycle operation");

    seed(window, *state, 3);
    throwError = true;
    startCleanup(window, *state);
    ok &= expect(finish(*state) && state->result.totalDup == 3 &&
        label(window, IDC_STATUS).find(L"清理中断") != std::wstring::npos &&
        IsWindowEnabled(GetDlgItem(window, IDC_DELETE_BTN)), "Worker exceptions restore the UI and allow retry");
    throwError = false;

    seed(window, *state, 1);
    checkProtection = true;
    startCleanup(window, *state);
    ok &= expect(finish(*state) && calls == 1 && protectedDuringRecycle,
                 "Retained file and duplicate content remain protected during recycling");
    checkProtection = false;
    for (const auto* lockedName : {L"kept.bin", L"duplicate.bin"}) {
        seed(window, *state, 1);
        HANDLE locked = CreateFileW((fixtureRoot / lockedName).c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        startCleanup(window, *state);
        ok &= expect(locked != INVALID_HANDLE_VALUE && finish(*state) && calls == 0,
                     "Files open for writing cannot be cleaned up");
        if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    }

    for (const auto* changed : {L"duplicate.bin", L"kept.bin"}) {
        seed(window, *state, 1);
        writeFixture(fixtureRoot / changed, "9876543210");
        startCleanup(window, *state);
        ok &= expect(finish(*state) && calls == 0 && state->result.totalDup == 1,
                     "Changed copies and changed retained files are never sent to deletion");
    }
    for (const auto* missing : {L"duplicate.bin", L"kept.bin"}) {
        seed(window, *state, 1);
        std::filesystem::remove(fixtureRoot / missing);
        startCleanup(window, *state);
        ok &= expect(finish(*state) && calls == 0 && state->result.totalDup == 1,
                     "Missing copies and missing retained files prevent deletion");
    }
    calls.store(0);
    ok &= expect(!recycleFile(L"relative.bin") && calls == 0,
                 "Recycle entry rejects relative paths before calling the shell");

    startScan(window, *state, (fixtureRoot / L"missing-directory").wstring());
    const auto scanDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (state->scanning && std::chrono::steady_clock::now() < scanDeadline) { pump(); Sleep(1); }
    ok &= expect(!state->scanning && label(window, IDC_STATUS).find(L"扫描失败") != std::wstring::npos &&
        label(window, IDC_STATUS).find(L"missing-directory") != std::wstring::npos &&
        !IsWindowEnabled(GetDlgItem(window, IDC_DELETE_BTN)), "Invalid scan roots show failure and disable cleanup");
    if (state->worker.joinable()) { state->cancelled.store(true); state->worker.join(); }
    DestroyWindow(window);

    delayMs = 2;
    const auto closeResult = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_MAIN_DLG),
                                           nullptr, closeTestProc, 0);
    ok &= expect(closeResult == 0 && calls <= 1, "Closing cancels cleanup and joins the worker before destruction");
    CloseHandle(recycleGate);
    // 仅删除本测试创建的两个固定文件及独立目录。
    std::filesystem::remove(fixtureRoot / L"kept.bin");
    std::filesystem::remove(fixtureRoot / L"duplicate.bin");
    std::filesystem::remove(fixtureRoot);
    CoUninitialize();
    return ok ? 0 : 1;
}
