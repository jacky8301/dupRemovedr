#include "dialog.h"
#include "context_menu.h"
#include <filesystem>
#include <memory>
#include <shlobj.h>
#include <shobjidl.h>

static void scanThread(HWND hDlg, std::wstring folder) {
    auto* d = reinterpret_cast<DlgData*>(
        GetWindowLongPtrW(hDlg, GWLP_USERDATA));
    if (!d) return;

    try {
        DuplicateScanner scanner;
        auto lastUpdate = std::chrono::steady_clock::time_point{};
        ScanResult result = scanner.scan(folder,
            [hDlg, d, &lastUpdate](int cur, int total, const std::wstring& file) {
                if (d->cancelled.load()) return;
                const auto now = std::chrono::steady_clock::now();
                if (cur != total && now - lastUpdate < std::chrono::milliseconds(80)) return;
                lastUpdate = now;
                // 合并尚未处理的刷新，消息队列中最多保留一条进度消息。
                std::lock_guard<std::mutex> lock(d->progressMutex);
                d->current = cur;
                d->total = total;
                d->currentFile = file;
                if (!d->progressPending.exchange(true) &&
                    !PostMessageW(hDlg, WM_SCAN_PROGRESS, 0, 0))
                    d->progressPending.store(false);
            },
            [d]() { return d->cancelled.load(); });

        if (!d->cancelled.load()) {
            auto payload = std::make_unique<ScanResult>(std::move(result));
            if (PostMessageW(hDlg, WM_SCAN_DONE, 1, reinterpret_cast<LPARAM>(payload.get())))
                payload.release();
        } else {
            PostMessageW(hDlg, WM_SCAN_DONE, 0, 0);
        }
    } catch (const ScanError& error) {
        d->scanError = L"扫描失败 · 无法读取目录（错误 " + std::to_wstring(error.code) + L"）· " + error.path;
        PostMessageW(hDlg, WM_SCAN_DONE, 0, 0);
    } catch (...) {
        d->scanError = L"扫描失败，请重新选择目录后重试";
        PostMessageW(hDlg, WM_SCAN_DONE, 0, 0);
    }
}

void updateContextMenuState(HWND hDlg, bool scanning) {
    bool registered = isContextMenuRegistered();
    SetDlgItemTextW(hDlg, IDC_MENU_STATUS,
                    registered ? L"已添加资源管理器右键菜单"
                               : L"可添加资源管理器右键菜单，方便快速扫描文件夹");
    EnableWindow(GetDlgItem(hDlg, IDC_INSTALL_BTN), !scanning && !registered);
    EnableWindow(GetDlgItem(hDlg, IDC_UNINSTALL_BTN), !scanning && registered);
}

void updateScanControls(HWND hDlg, DlgData* d) {
    bool scanning = d && d->scanning;
    bool cleaning = d && d->cleaning;
    bool busy = scanning || cleaning;
    bool hasFolder = d && !d->folder.empty();
    bool hasDuplicates = d && d->result.totalDup > 0;

    EnableWindow(GetDlgItem(hDlg, IDC_BROWSE_BTN), !busy);
    EnableWindow(GetDlgItem(hDlg, IDC_SCAN_BTN), !cleaning && (scanning || hasFolder));
    SetDlgItemTextW(hDlg, IDC_SCAN_BTN, scanning ? L"取消扫描" : L"开始扫描");
    EnableWindow(GetDlgItem(hDlg, IDC_DELETE_BTN), cleaning ? !d->cancelled.load() : !scanning && hasDuplicates);
    SetDlgItemTextW(hDlg, IDC_DELETE_BTN, cleaning ? L"停止清理" : L"清理全部重复副本");
    EnableWindow(GetDlgItem(hDlg, IDC_SEARCH_EDIT), !busy && d && !d->result.groups.empty());
    EnableWindow(GetDlgItem(hDlg, IDC_SEARCH_CLEAR), !busy && d && !d->searchQuery.empty());
    EnableWindow(GetDlgItem(hDlg, IDC_REVEAL_BTN), !busy && d && d->previewRow >= 0 &&
        !d->items[d->visibleRows[d->previewRow]].removed);
    updateContextMenuState(hDlg, busy);
}

void cancelScan(HWND hDlg, DlgData& d) {
    if (!d.scanning || d.cancelled.load()) return;

    d.cancelled.store(true);
    SetDlgItemTextW(hDlg, IDC_STATUS, L"正在取消扫描…");
    EnableWindow(GetDlgItem(hDlg, IDC_SCAN_BTN), FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_BROWSE_BTN), FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_DELETE_BTN), FALSE);
}

std::wstring chooseFolder(HWND owner, const std::wstring& initialFolder) {
    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) return {};

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS |
                           FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    dialog->SetTitle(L"选择需要扫描的文件夹");

    if (!initialFolder.empty() && std::filesystem::exists(initialFolder)) {
        IShellItem* initialItem = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                initialFolder.c_str(), nullptr, IID_PPV_ARGS(&initialItem)))) {
            dialog->SetFolder(initialItem);
            initialItem->Release();
        }
    }

    std::wstring folder;
    hr = dialog->Show(owner);
    if (SUCCEEDED(hr)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                folder = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }

    dialog->Release();
    return folder;
}

void startScan(HWND hDlg, DlgData& d, const std::wstring& folder) {
    if (d.scanning || d.cleaning || folder.empty()) return;

    if (d.worker.joinable()) d.worker.join();

    selectPreview(hDlg, d, -1);
    d.folder = folder;
    d.result = {};
    d.items.clear();
    d.visibleRows.clear();
    d.itemRows.clear();
    d.searchQuery.clear();
    d.cancelled.store(false);
    d.progressPending.store(false);
    d.current = 0;
    d.total = 0;
    d.started = std::chrono::steady_clock::now();
    d.scanning = true;
    d.scanError.clear();
    SetDlgItemTextW(hDlg, IDC_SEARCH_EDIT, L"");
    SetDlgItemTextW(hDlg, IDC_HINT, L"单击预览 · 双击定位 · 绿色保留");

    SetDlgItemTextW(hDlg, IDC_FOLDER_PATH, d.folder.c_str());
    SetDlgItemTextW(hDlg, IDC_STATUS, L"正在枚举目录并筛选候选文件…");
    setSummary(hDlg, 0, 0, 0);
    setProgressMarquee(hDlg, true);
    SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETRANGE32, 0, 1000);
    SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETPOS, 0, 0);
    ListView_DeleteAllItems(GetDlgItem(hDlg, IDC_LIST));
    updateScanControls(hDlg, &d);

    d.worker = std::thread(scanThread, hDlg, d.folder);
}

INT_PTR handleScanProgress(HWND hDlg, DlgData* d) {
    if (!d || !d->scanning) return TRUE;
    int current, total;
    std::wstring file;
    {
        std::lock_guard<std::mutex> lock(d->progressMutex);
        current = d->current;
        total = d->total;
        file = d->currentFile;
        d->progressPending.store(false);
    }
    if (d->cancelled.load()) return TRUE;
    setProgressMarquee(hDlg, false);
    const int position = total > 0 ? static_cast<int>(1000LL * current / total) : 0;
    SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETPOS, position, 0);
    SetDlgItemTextW(hDlg, IDC_FILES_VALUE, std::to_wstring(current).c_str());
    SetDlgItemTextW(hDlg, IDC_STATUS,
        (L"正在比对 " + std::to_wstring(current) + L" / " + std::to_wstring(total) + L" · " + file).c_str());
    return TRUE;
}

INT_PTR handleScanDone(HWND hDlg, DlgData* d, WPARAM wp, LPARAM lp) {
    std::unique_ptr<ScanResult> pRes(reinterpret_cast<ScanResult*>(lp));
    if (!d) return TRUE;
    d->scanning = false;
    if (d->worker.joinable()) d->worker.join();
    setProgressMarquee(hDlg, false);
    if (d->closeAfterWork) {
        requestClose(hDlg, d);
        return TRUE;
    }

    if (wp && pRes) {
        d->result = std::move(*pRes);

        HWND hList = GetDlgItem(hDlg, IDC_LIST);
        populateListView(hList, *d);

        wchar_t buf[256];
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - d->started).count();
        swprintf_s(buf, L"扫描完成 · %d 个文件 · %d 组重复 · 耗时 %.2f 秒",
            d->result.totalFiles, static_cast<int>(d->result.groups.size()), seconds);
        SetDlgItemTextW(hDlg, IDC_STATUS, buf);
        setSummary(hDlg, d->result.totalFiles, d->result.totalDup, d->result.wastedBytes);
        SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETPOS, 1000, 0);

        if (!d->result.groups.empty())
            EnableWindow(GetDlgItem(hDlg, IDC_DELETE_BTN), TRUE);
    } else {
        SetDlgItemTextW(hDlg, IDC_STATUS,
                        d->cancelled.load()
                            ? L"扫描已取消"
                            : d->scanError.c_str());
        SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETPOS, 0, 0);
    }
    updateScanControls(hDlg, d);
    return TRUE;
}
