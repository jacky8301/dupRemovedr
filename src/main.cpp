// Duplicate File Cleaner
// Right-click a folder, select "Find Duplicate Files", this tool scans
// for MD5-identical files and helps delete duplicates.
//
// Build: cmake -B build && cmake --build build --config Release

#include "resource.h"
#include "scanner.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <string>
#include <vector>
#include <thread>
#include <memory>
#include <filesystem>
#include <atomic>

#pragma comment(lib, "comctl32.lib")

// Enable visual styles
#pragma comment(linker, "\"/manifestdependency:type='win32' \
    name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
    processorArchitecture='*' publicKeyToken='6595b64144ccf1df' \
    language='*'\"")

// ---- Custom messages ----------------------------------------------------
#define WM_SCAN_PROGRESS   (WM_APP + 0)  // WPARAM=cur, LPARAM=total
#define WM_SCAN_FILE       (WM_APP + 1)  // LPARAM=wstring* (callee owns, delete)
#define WM_SCAN_DONE       (WM_APP + 2)  // WPARAM=1 ok, LPARAM=ScanResult*

// ---- Per-dialog state ---------------------------------------------------
struct DlgData {
    std::wstring    folder;
    ScanResult      result;
    std::thread     worker;
    // ListView item -> [groupIdx, fileIdx]; fileIdx==0 is "kept", >0 is dup
    struct ItemRef { int g; int f; };
    std::vector<ItemRef> items;
    bool            scanning  = false;
    std::atomic_bool cancelled = false;
};

static void scanThread(HWND hDlg, std::wstring folder);

// ---- Utility ------------------------------------------------------------

static std::wstring fmtSize(uint64_t bytes) {
    wchar_t buf[64];
    if      (bytes < 1024ULL)           swprintf_s(buf, L"%llu B",   bytes);
    else if (bytes < 1024ULL * 1024)    swprintf_s(buf, L"%.1f KB", bytes / 1024.0);
    else if (bytes < 1024ULL * 1024*1024) swprintf_s(buf, L"%.1f MB", bytes / (1024.0*1024.0));
    else                                 swprintf_s(buf, L"%.2f GB", bytes / (1024.0*1024.0*1024.0));
    return buf;
}

static bool isContextMenuRegistered() {
    HKEY key = nullptr;
    LSTATUS status = RegOpenKeyExW(
        HKEY_CLASSES_ROOT, L"Directory\\shell\\FindDupFiles",
        0, KEY_READ, &key);
    if (status == ERROR_SUCCESS) {
        RegCloseKey(key);
        return true;
    }
    return false;
}

static void updateContextMenuState(HWND hDlg, bool scanning) {
    bool registered = isContextMenuRegistered();
    SetDlgItemTextW(hDlg, IDC_MENU_STATUS,
                    registered ? L"Registered in Explorer."
                               : L"Not registered.");
    EnableWindow(GetDlgItem(hDlg, IDC_INSTALL_BTN), !scanning && !registered);
    EnableWindow(GetDlgItem(hDlg, IDC_UNINSTALL_BTN), !scanning && registered);
}

static void updateScanControls(HWND hDlg, DlgData* d) {
    bool scanning = d && d->scanning;
    bool hasFolder = d && !d->folder.empty();
    bool hasDuplicates = d && !d->result.groups.empty();

    EnableWindow(GetDlgItem(hDlg, IDC_BROWSE_BTN), !scanning);
    EnableWindow(GetDlgItem(hDlg, IDC_SCAN_BTN), scanning || hasFolder);
    SetDlgItemTextW(hDlg, IDC_SCAN_BTN, scanning ? L"&Cancel" : L"&Scan");
    EnableWindow(GetDlgItem(hDlg, IDC_DELETE_BTN), !scanning && hasDuplicates);
    updateContextMenuState(hDlg, scanning);
}

static void cancelScan(HWND hDlg, DlgData& d) {
    if (!d.scanning || d.cancelled.load()) return;

    d.cancelled.store(true);
    SetDlgItemTextW(hDlg, IDC_STATUS, L"Cancelling scan...");
    EnableWindow(GetDlgItem(hDlg, IDC_SCAN_BTN), FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_BROWSE_BTN), FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_DELETE_BTN), FALSE);
}

static std::wstring chooseFolder(HWND owner, const std::wstring& initialFolder) {
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
    dialog->SetTitle(L"Choose a folder to scan");

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

static void startScan(HWND hDlg, DlgData& d, const std::wstring& folder) {
    if (d.scanning || folder.empty()) return;

    if (d.worker.joinable()) d.worker.join();

    d.folder = folder;
    d.result = {};
    d.items.clear();
    d.cancelled.store(false);
    d.scanning = true;

    SetDlgItemTextW(hDlg, IDC_FOLDER_PATH, d.folder.c_str());
    SetDlgItemTextW(hDlg, IDC_STATUS, L"Scanning...");
    SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETPOS, 0, 0);
    ListView_DeleteAllItems(GetDlgItem(hDlg, IDC_LIST));
    updateScanControls(hDlg, &d);

    d.worker = std::thread(scanThread, hDlg, d.folder);
}

// ---- ListView helpers ---------------------------------------------------

static void initListView(HWND hList) {
    ListView_SetExtendedListViewStyle(hList,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    const struct { const wchar_t* t; int w; } cols[] = {
        { L"Status",    60 },
        { L"File Path", 390 },
        { L"Size",      80 },
    };
    LVCOLUMNW c = { LVCF_TEXT | LVCF_WIDTH | LVCF_FMT };
    for (int i = 0; i < 3; ++i) {
        c.pszText = const_cast<wchar_t*>(cols[i].t);
        c.cx      = cols[i].w;
        c.fmt     = (i == 2) ? LVCFMT_RIGHT : LVCFMT_LEFT;
        ListView_InsertColumn(hList, i, &c);
    }
}

static void populateListView(HWND hList, DlgData& d) {
    ListView_DeleteAllItems(hList);
    d.items.clear();

    LVITEMW item;
    for (int g = 0; g < (int)d.result.groups.size(); ++g) {
        const auto& grp = d.result.groups[g];
        for (int f = 0; f < (int)grp.files.size(); ++f) {
            item.iItem    = (int)d.items.size();
            item.iSubItem = 0;
            item.mask     = LVIF_TEXT | LVIF_PARAM;
            item.pszText  = (f == 0) ? const_cast<wchar_t*>(L"Keep")
                                     : const_cast<wchar_t*>(L"Del");
            // pack group/f into lParam for later lookup
            item.lParam   = (LPARAM)((INT_PTR)g * 1000000 + f);
            ListView_InsertItem(hList, &item);

            ListView_SetItemText(hList, item.iItem, 1,
                const_cast<wchar_t*>(grp.files[f].c_str()));
            std::wstring sz = fmtSize(grp.fileSize);
            ListView_SetItemText(hList, item.iItem, 2,
                const_cast<wchar_t*>(sz.c_str()));

            d.items.push_back({ g, f });
        }
        // blank separator between groups
        if (g + 1 < (int)d.result.groups.size()) {
            item.iItem   = (int)d.items.size();
            item.pszText = const_cast<wchar_t*>(L"");
            item.lParam  = -1;
            ListView_InsertItem(hList, &item);
            d.items.push_back({ -1, -1 });
        }
    }
}

// ---- Delete logic -------------------------------------------------------

static bool recycleFile(const std::wstring& path) {
    // SHFileOperation wants double-null terminated
    std::wstring buf = path + L'\0';
    SHFILEOPSTRUCTW f = {};
    f.wFunc  = FO_DELETE;
    f.pFrom  = buf.c_str();
    f.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION
             | FOF_NOERRORUI  | FOF_SILENT;
    return SHFileOperationW(&f) == 0;
}

static std::filesystem::path findSiblingScript(const wchar_t* scriptName) {
    wchar_t modulePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return {};

    std::filesystem::path exeDir =
        std::filesystem::path(modulePath).parent_path();

    std::filesystem::path candidates[] = {
        exeDir / scriptName,
        exeDir.parent_path() / scriptName
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate))
            return candidate;
    }
    return {};
}

static bool launchContextMenuScript(HWND owner, const wchar_t* scriptName) {
    std::filesystem::path scriptPath = findSiblingScript(scriptName);
    if (scriptPath.empty()) {
        std::wstring message = L"Cannot find ";
        message += scriptName;
        message += L".\nExpected it next to dupRemover.exe or its parent folder.";
        MessageBoxW(owner, message.c_str(), L"Script not found",
                    MB_OK | MB_ICONERROR);
        return false;
    }

    std::wstring parameters = L"/c \"\"";
    parameters += scriptPath.wstring();
    parameters += L"\"\"";

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = parameters.c_str();
    std::wstring workingDir = scriptPath.parent_path().wstring();
    sei.lpDirectory = workingDir.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        if (err != ERROR_CANCELLED) {
            wchar_t buf[256];
            swprintf_s(buf, L"Failed to launch %s.\nError code: %lu",
                       scriptName, err);
            MessageBoxW(owner, buf, L"Launch failed", MB_OK | MB_ICONERROR);
        }
        return false;
    }
    return true;
}

static int deleteDuplicates(DlgData& d, HWND hList) {
    int n = 0;
    for (auto& ref : d.items) {
        if (ref.g < 0) continue;
        if (ref.f == 0) continue;  // keep first
        if (recycleFile(d.result.groups[ref.g].files[ref.f])) {
            ++n;
            // find the list item and mark it
            LVFINDINFOW fi = { LVFI_PARAM };
            fi.lParam = (LPARAM)((INT_PTR)ref.g * 1000000 + ref.f);
            int idx = ListView_FindItem(hList, -1, &fi);
            if (idx >= 0)
                ListView_SetItemText(hList, idx, 0,
                    const_cast<wchar_t*>(L"Deleted"));
        }
    }
    return n;
}

// ---- Worker thread ------------------------------------------------------

static void scanThread(HWND hDlg, std::wstring folder) {
    auto* d = reinterpret_cast<DlgData*>(
        GetWindowLongPtrW(hDlg, GWLP_USERDATA));
    if (!d) return;

    DuplicateScanner scanner;
    ScanResult result = scanner.scan(folder,
        [hDlg, d](int cur, int total, const std::wstring& file) {
            if (d->cancelled.load()) return;
            PostMessageW(hDlg, WM_SCAN_FILE, 0,
                reinterpret_cast<LPARAM>(new std::wstring(file)));
            PostMessageW(hDlg, WM_SCAN_PROGRESS, (WPARAM)cur, (LPARAM)total);
        },
        [d]() {
            return d->cancelled.load();
        });

    if (!d->cancelled.load()) {
        PostMessageW(hDlg, WM_SCAN_DONE, 1,
            reinterpret_cast<LPARAM>(new ScanResult(std::move(result))));
    } else {
        PostMessageW(hDlg, WM_SCAN_DONE, 0, 0);
    }
}

// ---- Dialog procedure ---------------------------------------------------

static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = reinterpret_cast<DlgData*>(
        GetWindowLongPtrW(hDlg, GWLP_USERDATA));

    switch (msg) {

    case WM_INITDIALOG: {
        d = new DlgData();
        SetWindowLongPtrW(hDlg, GWLP_USERDATA, (LONG_PTR)d);

        HINSTANCE hInst = GetModuleHandleW(nullptr);
        HICON hIconBig = static_cast<HICON>(LoadImageW(
            hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
        HICON hIconSmall = static_cast<HICON>(LoadImageW(
            hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        if (hIconBig)
            SendMessageW(hDlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIconBig));
        if (hIconSmall)
            SendMessageW(hDlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIconSmall));

        HWND hList = GetDlgItem(hDlg, IDC_LIST);
        initListView(hList);

        // Parse command line for folder path
        int argc; LPWSTR* argv;
        argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argc >= 2) {
            d->folder = argv[1];
            if (d->folder.size() >= 2 &&
                d->folder.front() == L'"' && d->folder.back() == L'"')
                d->folder = d->folder.substr(1, d->folder.size() - 2);
        }
        LocalFree(argv);

        if (d->folder.empty()) {
            SetDlgItemTextW(hDlg, IDC_FOLDER_PATH, L"");
            SetDlgItemTextW(hDlg, IDC_STATUS,
                            L"Choose a folder, or use Explorer's context menu.");
        } else {
            startScan(hDlg, *d, d->folder);
        }
        updateScanControls(hDlg, d);
        return TRUE;
    }

    case WM_SCAN_FILE: {
        auto* p = reinterpret_cast<std::wstring*>(lp);
        if (!d || !d->scanning) {
            delete p;
            return TRUE;
        }
        if (p) {
            SetDlgItemTextW(hDlg, IDC_STATUS,
                (L"Scanning: " + *p).c_str());
            delete p;
        }
        return TRUE;
    }

    case WM_SCAN_PROGRESS: {
        if (!d || !d->scanning) return TRUE;
        SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETRANGE, 0,
                            MAKELPARAM(0, (int)lp));
        SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETPOS, (int)wp, 0);
        return TRUE;
    }

    case WM_SCAN_DONE: {
        if (!d) return TRUE;
        d->scanning = false;
        if (d->worker.joinable()) d->worker.join();

        auto* pRes = reinterpret_cast<ScanResult*>(lp);
        if (wp && pRes) {
            d->result = std::move(*pRes);
            delete pRes;

            HWND hList = GetDlgItem(hDlg, IDC_LIST);
            populateListView(hList, *d);

            wchar_t buf[256];
            swprintf_s(buf, L"Done: %d files, %d dup groups, "
                       L"%d duplicates (%s wasted).",
                d->result.totalFiles,
                (int)d->result.groups.size(),
                d->result.totalDup,
                fmtSize(d->result.wastedBytes).c_str());
            SetDlgItemTextW(hDlg, IDC_STATUS, buf);
            SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETPOS, 0, 0);

            if (!d->result.groups.empty())
                EnableWindow(GetDlgItem(hDlg, IDC_DELETE_BTN), TRUE);
        } else {
            SetDlgItemTextW(hDlg, IDC_STATUS,
                            d->cancelled.load()
                                ? L"Scan cancelled."
                                : L"Scan failed.");
            SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETPOS, 0, 0);
        }
        updateScanControls(hDlg, d);
        return TRUE;
    }

    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE)
            updateContextMenuState(hDlg, d && d->scanning);
        return FALSE;

    case WM_COMMAND: {
        switch (LOWORD(wp)) {
        case IDC_CLOSE_BTN:
            if (d) {
                d->cancelled.store(true);
                if (d->worker.joinable()) d->worker.join();
            }
            EndDialog(hDlg, 0);
            return TRUE;

        case IDC_BROWSE_BTN:
            if (d && !d->scanning) {
                std::wstring folder = chooseFolder(hDlg, d->folder);
                if (!folder.empty())
                    startScan(hDlg, *d, folder);
            }
            return TRUE;

        case IDC_SCAN_BTN:
            if (d && d->scanning) {
                cancelScan(hDlg, *d);
            } else if (d && !d->folder.empty()) {
                startScan(hDlg, *d, d->folder);
            }
            return TRUE;

        case IDC_DELETE_BTN:
            if (!d || d->result.groups.empty()) break;
            {
                HWND hList = GetDlgItem(hDlg, IDC_LIST);
                int ret = MessageBoxW(hDlg,
                    L"Move all duplicate files to Recycle Bin?\n"
                    L"(One copy of each group will be kept.)",
                    L"Confirm", MB_OKCANCEL | MB_ICONWARNING);
                if (ret != IDOK) break;

                int n = deleteDuplicates(*d, hList);
                wchar_t buf[128];
                swprintf_s(buf, L"%d file(s) moved to Recycle Bin.", n);
                MessageBoxW(hDlg, buf, L"Done", MB_OK | MB_ICONINFORMATION);
                EnableWindow(GetDlgItem(hDlg, IDC_DELETE_BTN), FALSE);
            }
            return TRUE;

        case IDC_INSTALL_BTN:
            if (launchContextMenuScript(hDlg, L"install.bat")) {
                SetDlgItemTextW(hDlg, IDC_MENU_STATUS,
                    L"Registration command started. Close its window to finish.");
            }
            return TRUE;

        case IDC_UNINSTALL_BTN:
            if (launchContextMenuScript(hDlg, L"uninstall.bat")) {
                SetDlgItemTextW(hDlg, IDC_MENU_STATUS,
                    L"Unregister command started. Close its window to finish.");
            }
            return TRUE;
        }
        break;
    }

    case WM_CLOSE:
        if (d) {
            d->cancelled.store(true);
            if (d->worker.joinable()) d->worker.join();
        }
        EndDialog(hDlg, 0);
        return TRUE;

    case WM_DESTROY:
        delete d;
        SetWindowLongPtrW(hDlg, GWLP_USERDATA, 0);
        return TRUE;
    }
    return FALSE;
}

// ---- Entry point --------------------------------------------------------

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    HRESULT comHr = CoInitializeEx(nullptr,
        COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INITCOMMONCONTROLSEX icc = {
        sizeof(icc),
        ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES
    };
    InitCommonControlsEx(&icc);

    DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_MAIN_DLG),
                    nullptr, DlgProc, 0);

    if (SUCCEEDED(comHr))
        CoUninitialize();
    return 0;
}
