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
#include <shlobj.h>
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
    const HKEY roots[] = { HKEY_CURRENT_USER, HKEY_CLASSES_ROOT };
    const wchar_t* paths[] = {
        L"Software\\Classes\\Directory\\shell\\FindDupFiles",
        L"Directory\\shell\\FindDupFiles",
    };

    for (int i = 0; i < 2; ++i) {
        LSTATUS status = RegOpenKeyExW(roots[i], paths[i], 0, KEY_READ, &key);
        if (status == ERROR_SUCCESS) {
            RegCloseKey(key);
            return true;
        }
    }
    return false;
}

static std::wstring getModulePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = GetModuleFileNameW(
            nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (len == 0) return {};
        if (len < path.size()) {
            path.resize(len);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

static bool setRegistryString(HKEY key, const wchar_t* valueName,
                              const std::wstring& value) {
    DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(
        key, valueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()), bytes) == ERROR_SUCCESS;
}

static bool registerContextMenu(std::wstring& error) {
    std::wstring exePath = getModulePath();
    if (exePath.empty()) {
        error = L"Cannot determine the application path.";
        return false;
    }

    HKEY menuKey = nullptr;
    LSTATUS status = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Classes\\Directory\\shell\\FindDupFiles",
        0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE,
        nullptr, &menuKey, nullptr);
    if (status != ERROR_SUCCESS) {
        error = L"Failed to create the context menu registry key.";
        return false;
    }

    std::wstring iconValue = exePath + L",0";
    bool ok = setRegistryString(menuKey, nullptr, L"Find Duplicate Files") &&
              setRegistryString(menuKey, L"Icon", iconValue);
    RegCloseKey(menuKey);
    if (!ok) {
        error = L"Failed to write the context menu registry values.";
        return false;
    }

    HKEY commandKey = nullptr;
    status = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Classes\\Directory\\shell\\FindDupFiles\\command",
        0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE,
        nullptr, &commandKey, nullptr);
    if (status != ERROR_SUCCESS) {
        error = L"Failed to create the context menu command registry key.";
        return false;
    }

    std::wstring command = L"\"" + exePath + L"\" \"%1\"";
    ok = setRegistryString(commandKey, nullptr, command);
    RegCloseKey(commandKey);
    if (!ok) {
        error = L"Failed to write the context menu command.";
        return false;
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

static bool unregisterContextMenu(std::wstring& error) {
    LSTATUS userStatus = RegDeleteTreeW(
        HKEY_CURRENT_USER,
        L"Software\\Classes\\Directory\\shell\\FindDupFiles");
    LSTATUS machineStatus = RegDeleteTreeW(
        HKEY_CLASSES_ROOT,
        L"Directory\\shell\\FindDupFiles");

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    if (machineStatus == ERROR_ACCESS_DENIED) {
        error = L"Removed the current-user registration if present, but an "
                L"old machine-wide registration needs Administrator rights.";
        return false;
    }

    if (userStatus == ERROR_SUCCESS || machineStatus == ERROR_SUCCESS ||
        (userStatus == ERROR_FILE_NOT_FOUND &&
         machineStatus == ERROR_FILE_NOT_FOUND)) {
        return true;
    } else {
        error = L"Failed to remove the context menu registration.";
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

static LRESULT colorListViewRow(NMLVCUSTOMDRAW* draw, DlgData* d) {
    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) {
        return CDRF_NOTIFYITEMDRAW;
    }

    if (draw->nmcd.dwDrawStage != CDDS_ITEMPREPAINT || !d) {
        return CDRF_DODEFAULT;
    }

    int row = static_cast<int>(draw->nmcd.dwItemSpec);
    if (row < 0 || row >= static_cast<int>(d->items.size())) {
        return CDRF_DODEFAULT;
    }

    const auto& ref = d->items[row];
    if (ref.g < 0 || ref.f < 0) {
        return CDRF_DODEFAULT;
    }

    wchar_t status[32] = {};
    ListView_GetItemText(draw->nmcd.hdr.hwndFrom, row, 0, status,
                         static_cast<int>(std::size(status)));
    if (wcscmp(status, L"Deleted") == 0) {
        draw->clrText = RGB(96, 96, 96);
        draw->clrTextBk = RGB(245, 245, 245);
        return CDRF_NEWFONT;
    }

    if (ref.f == 0) {
        draw->clrText = RGB(24, 105, 56);
        draw->clrTextBk = RGB(236, 249, 241);
    } else {
        draw->clrText = RGB(170, 38, 38);
        draw->clrTextBk = RGB(255, 239, 239);
    }
    return CDRF_NEWFONT;
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
            {
                std::wstring error;
                if (registerContextMenu(error)) {
                    updateScanControls(hDlg, d);
                    SetDlgItemTextW(hDlg, IDC_MENU_STATUS,
                        L"Registered for the current user.");
                } else {
                    MessageBoxW(hDlg, error.c_str(), L"Registration failed",
                                MB_OK | MB_ICONERROR);
                }
            }
            return TRUE;

        case IDC_UNINSTALL_BTN:
            {
                std::wstring error;
                if (unregisterContextMenu(error)) {
                    updateScanControls(hDlg, d);
                    SetDlgItemTextW(hDlg, IDC_MENU_STATUS,
                        L"Context menu removed.");
                } else {
                    updateScanControls(hDlg, d);
                    MessageBoxW(hDlg, error.c_str(), L"Unregistration failed",
                                MB_OK | MB_ICONWARNING);
                }
            }
            return TRUE;
        }
        break;
    }

    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lp);
        if (hdr && hdr->idFrom == IDC_LIST && hdr->code == NM_CUSTOMDRAW) {
            return colorListViewRow(
                reinterpret_cast<NMLVCUSTOMDRAW*>(lp), d);
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
