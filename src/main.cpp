// 重复文件清理工具：选择目录或从右键菜单启动，确认重复内容后清理副本。
// 构建：cmake -B build/optimized && cmake --build build/optimized --config Release

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
#include <chrono>
#include <algorithm>
#include <mutex>
#include <uxtheme.h>

#pragma comment(lib, "comctl32.lib")

// Enable visual styles
#pragma comment(linker, "\"/manifestdependency:type='win32' \
    name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
    processorArchitecture='*' publicKeyToken='6595b64144ccf1df' \
    language='*'\"")

// ---- Custom messages ----------------------------------------------------
#define WM_SCAN_PROGRESS   (WM_APP + 0)  // 从共享状态中读取最新进度
#define WM_SCAN_DONE       (WM_APP + 2)  // WPARAM=1 ok, LPARAM=ScanResult*

// ---- Per-dialog state ---------------------------------------------------
struct DlgData {
    std::wstring    folder;
    ScanResult      result;
    std::thread     worker;
    // ListView item -> [groupIdx, fileIdx]; fileIdx==0 is "kept", >0 is dup
    struct ItemRef { int g; int f; bool removed = false; };
    std::vector<ItemRef> items;
    bool            scanning  = false;
    std::atomic_bool cancelled = false;
    std::mutex      progressMutex;
    std::wstring    currentFile;
    int             current = 0;
    int             total = 0;
    std::atomic_bool progressPending = false;
    std::chrono::steady_clock::time_point started;
    HFONT           bodyFont = nullptr;
    HFONT           titleFont = nullptr;
    HFONT           numberFont = nullptr;
    HBRUSH          background = CreateSolidBrush(RGB(245, 247, 251));
    HBRUSH          card = CreateSolidBrush(RGB(255, 255, 255));
    bool            closeAfterScan = false;

    ~DlgData() {
        DeleteObject(bodyFont);
        DeleteObject(titleFont);
        DeleteObject(numberFont);
        DeleteObject(background);
        DeleteObject(card);
    }
};

static void scanThread(HWND hDlg, std::wstring folder);
static std::wstring fmtSize(uint64_t bytes);

static int scaled(HWND window, int value) {
    return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96);
}

static void updateFonts(HWND hDlg, DlgData& d) {
    const auto makeFont = [hDlg](int size, int weight) {
        return CreateFontW(-scaled(hDlg, size), 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Microsoft YaHei UI");
    };
    HFONT oldBody = d.bodyFont, oldTitle = d.titleFont, oldNumber = d.numberFont;
    d.bodyFont = makeFont(14, FW_NORMAL);
    d.titleFont = makeFont(26, FW_SEMIBOLD);
    d.numberFont = makeFont(25, FW_SEMIBOLD);
    for (HWND child = GetWindow(hDlg, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(d.bodyFont), TRUE);
    SendDlgItemMessageW(hDlg, IDC_TITLE, WM_SETFONT, reinterpret_cast<WPARAM>(d.titleFont), TRUE);
    for (int id : {IDC_FILES_VALUE, IDC_DUP_VALUE, IDC_SPACE_VALUE})
        SendDlgItemMessageW(hDlg, id, WM_SETFONT, reinterpret_cast<WPARAM>(d.numberFont), TRUE);
    DeleteObject(oldBody);
    DeleteObject(oldTitle);
    DeleteObject(oldNumber);
}

static void layoutDialog(HWND hDlg) {
    RECT client{};
    GetClientRect(hDlg, &client);
    const int w = MulDiv(client.right, 96, static_cast<int>(GetDpiForWindow(hDlg)));
    const int h = MulDiv(client.bottom, 96, static_cast<int>(GetDpiForWindow(hDlg)));
    auto place = [hDlg](int id, int x, int y, int width, int height) {
        MoveWindow(GetDlgItem(hDlg, id), scaled(hDlg, x), scaled(hDlg, y),
                   scaled(hDlg, width), scaled(hDlg, height), TRUE);
    };
    place(IDC_TITLE, 24, 18, w - 48, 38);
    place(IDC_SUBTITLE, 25, 61, w - 48, 22);
    place(IDC_FOLDER_PATH, 42, 110, w - 338, 24);
    place(IDC_BROWSE_BTN, w - 278, 103, 118, 38);
    place(IDC_SCAN_BTN, w - 148, 103, 106, 38);
    place(IDC_STATUS, 42, 151, w - 84, 22);
    place(IDC_PROGRESS, 42, 183, w - 84, 7);
    const int cardWidth = (w - 72) / 3;
    const int labels[] = {IDC_FILES_LABEL, IDC_DUP_LABEL, IDC_SPACE_LABEL};
    const int values[] = {IDC_FILES_VALUE, IDC_DUP_VALUE, IDC_SPACE_VALUE};
    for (int i = 0; i < 3; ++i) {
        place(labels[i], 42 + i * (cardWidth + 12), 232, cardWidth - 36, 22);
        place(values[i], 42 + i * (cardWidth + 12), 260, cardWidth - 36, 38);
    }
    place(IDC_RESULTS_LABEL, 24, 329, 140, 22);
    place(IDC_HINT, 170, 329, w - 194, 22);
    place(IDC_LIST, 25, 366, w - 50, (std::max)(h - 488, 80));
    place(IDC_MENU_STATUS, 24, h - 98, w - 48, 22);
    place(IDC_INSTALL_BTN, 24, h - 64, 132, 38);
    place(IDC_UNINSTALL_BTN, 168, h - 64, 132, 38);
    place(IDC_DELETE_BTN, w - 282, h - 64, 154, 38);
    place(IDC_CLOSE_BTN, w - 116, h - 64, 92, 38);
    HWND list = GetDlgItem(hDlg, IDC_LIST);
    ListView_SetColumnWidth(list, 0, scaled(hDlg, 92));
    ListView_SetColumnWidth(list, 1, (std::max)(scaled(hDlg, w - 264), 100));
    ListView_SetColumnWidth(list, 2, scaled(hDlg, 94));
    InvalidateRect(hDlg, nullptr, TRUE);
}

static void paintDialog(HWND hDlg, DlgData& d, HDC printDC = nullptr) {
    PAINTSTRUCT ps{};
    HDC dc = printDC ? printDC : BeginPaint(hDlg, &ps);
    RECT client{};
    GetClientRect(hDlg, &client);
    FillRect(dc, &client, d.background);
    const int w = MulDiv(client.right, 96, static_cast<int>(GetDpiForWindow(hDlg)));
    const int h = MulDiv(client.bottom, 96, static_cast<int>(GetDpiForWindow(hDlg)));
    HGDIOBJ oldBrush = SelectObject(dc, d.card);
    HPEN border = CreatePen(PS_SOLID, 1, RGB(226, 232, 240));
    HGDIOBJ oldPen = SelectObject(dc, border);
    auto panel = [&](int x, int y, int width, int height) {
        RoundRect(dc, scaled(hDlg, x), scaled(hDlg, y), scaled(hDlg, x + width),
                  scaled(hDlg, y + height), scaled(hDlg, 16), scaled(hDlg, 16));
    };
    panel(24, 96, w - 48, 112);
    const int cardWidth = (w - 72) / 3;
    for (int i = 0; i < 3; ++i) panel(24 + i * (cardWidth + 12), 220, cardWidth, 92);
    panel(24, 365, w - 48, (std::max)(h - 486, 82));
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(border);
    if (!printDC) EndPaint(hDlg, &ps);
}

static void drawButton(const DRAWITEMSTRUCT& item, DlgData& d) {
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool primary = item.CtlID == IDC_SCAN_BTN;
    const bool danger = item.CtlID == IDC_DELETE_BTN;
    COLORREF fill = primary ? RGB(37, 99, 235) : danger ? RGB(255, 241, 242) : RGB(255, 255, 255);
    COLORREF textColor = primary ? RGB(255, 255, 255) : danger ? RGB(190, 40, 55) : RGB(51, 65, 85);
    if (pressed) fill = primary ? RGB(29, 78, 216) : RGB(226, 232, 240);
    if (disabled) { fill = RGB(235, 239, 245); textColor = RGB(148, 163, 184); }
    FillRect(item.hDC, &item.rcItem,
        item.CtlID == IDC_SCAN_BTN || item.CtlID == IDC_BROWSE_BTN ? d.card : d.background);
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, primary && !disabled ? fill : RGB(217, 225, 235));
    HGDIOBJ oldBrush = SelectObject(item.hDC, brush);
    HGDIOBJ oldPen = SelectObject(item.hDC, pen);
    HGDIOBJ oldFont = SelectObject(item.hDC, d.bodyFont);
    RoundRect(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right,
              item.rcItem.bottom, scaled(item.hwndItem, 10), scaled(item.hwndItem, 10));
    wchar_t text[80]{};
    GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, textColor);
    RECT textRect = item.rcItem;
    DrawTextW(item.hDC, text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -4, -4);
        DrawFocusRect(item.hDC, &focus);
    }
    SelectObject(item.hDC, oldBrush);
    SelectObject(item.hDC, oldPen);
    SelectObject(item.hDC, oldFont);
    DeleteObject(brush);
    DeleteObject(pen);
}

static void setSummary(HWND hDlg, int files, int duplicates, uint64_t bytes) {
    SetDlgItemTextW(hDlg, IDC_FILES_VALUE, std::to_wstring(files).c_str());
    SetDlgItemTextW(hDlg, IDC_DUP_VALUE, std::to_wstring(duplicates).c_str());
    SetDlgItemTextW(hDlg, IDC_SPACE_VALUE, fmtSize(bytes).c_str());
}

static void setProgressMarquee(HWND hDlg, bool enabled) {
    HWND progress = GetDlgItem(hDlg, IDC_PROGRESS);
    LONG_PTR style = GetWindowLongPtrW(progress, GWL_STYLE);
    SetWindowLongPtrW(progress, GWL_STYLE, enabled ? style | PBS_MARQUEE : style & ~PBS_MARQUEE);
    SendMessageW(progress, PBM_SETMARQUEE, enabled, 30);
}

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
        error = L"无法获取程序路径。";
        return false;
    }

    HKEY menuKey = nullptr;
    LSTATUS status = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Classes\\Directory\\shell\\FindDupFiles",
        0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE,
        nullptr, &menuKey, nullptr);
    if (status != ERROR_SUCCESS) {
        error = L"无法创建右键菜单注册表项。";
        return false;
    }

    std::wstring iconValue = exePath + L",0";
    bool ok = setRegistryString(menuKey, nullptr, L"查找重复文件") &&
              setRegistryString(menuKey, L"Icon", iconValue);
    RegCloseKey(menuKey);
    if (!ok) {
        error = L"无法写入右键菜单注册表值。";
        return false;
    }

    HKEY commandKey = nullptr;
    status = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Classes\\Directory\\shell\\FindDupFiles\\command",
        0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE,
        nullptr, &commandKey, nullptr);
    if (status != ERROR_SUCCESS) {
        error = L"无法创建右键菜单命令注册表项。";
        return false;
    }

    std::wstring command = L"\"" + exePath + L"\" \"%1\"";
    ok = setRegistryString(commandKey, nullptr, command);
    RegCloseKey(commandKey);
    if (!ok) {
        error = L"无法写入右键菜单命令。";
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
        error = L"已移除当前用户的注册；旧的系统级注册需要管理员权限才能移除。";
        return false;
    }

    if (userStatus == ERROR_SUCCESS || machineStatus == ERROR_SUCCESS ||
        (userStatus == ERROR_FILE_NOT_FOUND &&
         machineStatus == ERROR_FILE_NOT_FOUND)) {
        return true;
    } else {
        error = L"无法移除右键菜单。";
    }
    return false;
}

static void updateContextMenuState(HWND hDlg, bool scanning) {
    bool registered = isContextMenuRegistered();
    SetDlgItemTextW(hDlg, IDC_MENU_STATUS,
                    registered ? L"已添加资源管理器右键菜单"
                               : L"可添加资源管理器右键菜单，方便快速扫描文件夹");
    EnableWindow(GetDlgItem(hDlg, IDC_INSTALL_BTN), !scanning && !registered);
    EnableWindow(GetDlgItem(hDlg, IDC_UNINSTALL_BTN), !scanning && registered);
}

static void updateScanControls(HWND hDlg, DlgData* d) {
    bool scanning = d && d->scanning;
    bool hasFolder = d && !d->folder.empty();
    bool hasDuplicates = d && d->result.totalDup > 0;

    EnableWindow(GetDlgItem(hDlg, IDC_BROWSE_BTN), !scanning);
    EnableWindow(GetDlgItem(hDlg, IDC_SCAN_BTN), scanning || hasFolder);
    SetDlgItemTextW(hDlg, IDC_SCAN_BTN, scanning ? L"取消扫描" : L"开始扫描");
    EnableWindow(GetDlgItem(hDlg, IDC_DELETE_BTN), !scanning && hasDuplicates);
    updateContextMenuState(hDlg, scanning);
}

static void cancelScan(HWND hDlg, DlgData& d) {
    if (!d.scanning || d.cancelled.load()) return;

    d.cancelled.store(true);
    SetDlgItemTextW(hDlg, IDC_STATUS, L"正在取消扫描…");
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

static void startScan(HWND hDlg, DlgData& d, const std::wstring& folder) {
    if (d.scanning || folder.empty()) return;

    if (d.worker.joinable()) d.worker.join();

    d.folder = folder;
    d.result = {};
    d.items.clear();
    d.cancelled.store(false);
    d.progressPending.store(false);
    d.current = 0;
    d.total = 0;
    d.started = std::chrono::steady_clock::now();
    d.scanning = true;

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

// ---- ListView helpers ---------------------------------------------------

static void initListView(HWND hList) {
    ListView_SetExtendedListViewStyle(hList,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    SetWindowTheme(hList, L"Explorer", nullptr);
    ListView_SetBkColor(hList, RGB(255, 255, 255));
    ListView_SetTextColor(hList, RGB(51, 65, 85));

    const struct { const wchar_t* t; int w; } cols[] = {
        { L"处理方式", 92 },
        { L"文件路径", 390 },
        { L"文件大小", 94 },
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
    SendMessageW(hList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(hList);
    d.items.clear();

    LVITEMW item{};
    for (int g = 0; g < (int)d.result.groups.size(); ++g) {
        const auto& grp = d.result.groups[g];
        for (int f = 0; f < (int)grp.files.size(); ++f) {
            item.iItem    = (int)d.items.size();
            item.iSubItem = 0;
            item.mask     = LVIF_TEXT | LVIF_PARAM;
            item.pszText  = (f == 0) ? const_cast<wchar_t*>(L"保留")
                                     : const_cast<wchar_t*>(L"重复副本");
            item.lParam   = item.iItem;
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
    SendMessageW(hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hList, nullptr, TRUE);
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

    if (ref.removed) {
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
    return SHFileOperationW(&f) == 0 && !f.fAnyOperationsAborted;
}

static int deleteDuplicates(DlgData& d, HWND hList) {
    int n = 0;
    for (size_t row = 0; row < d.items.size(); ++row) {
        auto& ref = d.items[row];
        if (ref.g < 0) continue;
        if (ref.f == 0) continue;  // keep first
        if (ref.removed) continue;
        if (recycleFile(d.result.groups[ref.g].files[ref.f])) {
            ++n;
            ref.removed = true;
            --d.result.totalDup;
            d.result.wastedBytes -= d.result.groups[ref.g].fileSize;
            ListView_SetItemText(hList, static_cast<int>(row), 0,
                const_cast<wchar_t*>(L"已回收"));
        }
    }
    return n;
}

// ---- Worker thread ------------------------------------------------------

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
    } catch (...) {
        PostMessageW(hDlg, WM_SCAN_DONE, 0, 0);
    }
}

static void requestClose(HWND hDlg, DlgData* d) {
    if (d && d->scanning) {
        d->closeAfterScan = true;
        cancelScan(hDlg, *d);
        EnableWindow(GetDlgItem(hDlg, IDC_CLOSE_BTN), FALSE);
        return;
    }
    EndDialog(hDlg, 0);
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
        updateFonts(hDlg, *d);
        SetWindowTheme(GetDlgItem(hDlg, IDC_PROGRESS), L"", L"");
        SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETBARCOLOR, 0, RGB(37, 99, 235));
        SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETBKCOLOR, 0, RGB(231, 237, 247));
        RECT initial{};
        GetWindowRect(hDlg, &initial);
        const int width = scaled(hDlg, 960), height = scaled(hDlg, 740);
        SetWindowPos(hDlg, nullptr,
            initial.left + (initial.right - initial.left - width) / 2,
            initial.top + (initial.bottom - initial.top - height) / 2,
            width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        layoutDialog(hDlg);
        setProgressMarquee(hDlg, false);

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
            SetDlgItemTextW(hDlg, IDC_FOLDER_PATH, L"请选择需要扫描的文件夹");
            SetDlgItemTextW(hDlg, IDC_STATUS,
                            L"选择文件夹开始扫描，也可从资源管理器右键菜单启动");
        } else {
            startScan(hDlg, *d, d->folder);
        }
        updateScanControls(hDlg, d);
        return TRUE;
    }

    case WM_SCAN_PROGRESS: {
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

    case WM_SCAN_DONE: {
        std::unique_ptr<ScanResult> pRes(reinterpret_cast<ScanResult*>(lp));
        if (!d) return TRUE;
        d->scanning = false;
        if (d->worker.joinable()) d->worker.join();
        setProgressMarquee(hDlg, false);
        if (d->closeAfterScan) {
            EndDialog(hDlg, 0);
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
                                : L"扫描失败，请重新选择目录后重试");
            SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETPOS, 0, 0);
        }
        updateScanControls(hDlg, d);
        return TRUE;
    }

    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE)
            updateContextMenuState(hDlg, d && d->scanning);
        return FALSE;

    case WM_SIZE:
        if (d && wp != SIZE_MINIMIZED) layoutDialog(hDlg);
        return TRUE;

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize = {scaled(hDlg, 860), scaled(hDlg, 680)};
        return TRUE;
    }

    case WM_DPICHANGED: {
        auto* bounds = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hDlg, nullptr, bounds->left, bounds->top,
            bounds->right - bounds->left, bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (d) { updateFonts(hDlg, *d); layoutDialog(hDlg); }
        return TRUE;
    }

    case WM_PAINT:
        if (d) { paintDialog(hDlg, *d); return TRUE; }
        break;

    case WM_PRINTCLIENT:
        if (d) { paintDialog(hDlg, *d, reinterpret_cast<HDC>(wp)); return TRUE; }
        break;

    case WM_CTLCOLORDLG:
        if (d) return reinterpret_cast<INT_PTR>(d->background);
        break;

    case WM_CTLCOLORSTATIC: {
        if (!d) break;
        HDC dc = reinterpret_cast<HDC>(wp);
        int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        bool card = id == IDC_FOLDER_PATH || id == IDC_STATUS ||
            (id >= IDC_FILES_LABEL && id <= IDC_SPACE_VALUE);
        COLORREF color = RGB(100, 116, 139);
        if (id == IDC_TITLE || id == IDC_FOLDER_PATH || id == IDC_RESULTS_LABEL)
            color = RGB(30, 41, 59);
        if (id == IDC_FILES_VALUE) color = RGB(37, 99, 235);
        if (id == IDC_DUP_VALUE) color = RGB(190, 40, 55);
        if (id == IDC_SPACE_VALUE) color = RGB(24, 125, 88);
        SetTextColor(dc, color);
        SetBkColor(dc, card ? RGB(255, 255, 255) : RGB(245, 247, 251));
        return reinterpret_cast<INT_PTR>(card ? d->card : d->background);
    }

    case WM_DRAWITEM:
        if (d && wp) { drawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lp), *d); return TRUE; }
        break;

    case WM_COMMAND: {
        switch (LOWORD(wp)) {
        case IDC_CLOSE_BTN:
        case IDCANCEL:
            requestClose(hDlg, d);
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
            if (!d || d->scanning || d->result.totalDup == 0) break;
            {
                HWND hList = GetDlgItem(hDlg, IDC_LIST);
                int ret = MessageBoxW(hDlg,
                    L"将所有重复副本移入回收站？\n"
                    L"每组保留一份文件，其余副本移入回收站。",
                    L"确认清理", MB_OKCANCEL | MB_ICONWARNING);
                if (ret != IDOK) break;

                int n = deleteDuplicates(*d, hList);
                wchar_t buf[128];
                swprintf_s(buf, L"已将 %d 个重复副本移入回收站。", n);
                MessageBoxW(hDlg, buf, L"清理完成", MB_OK | MB_ICONINFORMATION);
                setSummary(hDlg, d->result.totalFiles, d->result.totalDup, d->result.wastedBytes);
                updateScanControls(hDlg, d);
            }
            return TRUE;

        case IDC_INSTALL_BTN:
            {
                std::wstring error;
                if (registerContextMenu(error)) {
                    updateScanControls(hDlg, d);
                    SetDlgItemTextW(hDlg, IDC_MENU_STATUS,
                        L"已为当前用户添加右键菜单");
                } else {
                    MessageBoxW(hDlg, error.c_str(), L"添加右键菜单失败",
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
                        L"右键菜单已移除");
                } else {
                    updateScanControls(hDlg, d);
                    MessageBoxW(hDlg, error.c_str(), L"移除右键菜单失败",
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
            SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, colorListViewRow(
                reinterpret_cast<NMLVCUSTOMDRAW*>(lp), d));
            return TRUE;
        }
        if (hdr && hdr->idFrom == IDC_LIST && hdr->code == LVN_GETEMPTYMARKUP) {
            auto* empty = reinterpret_cast<NMLVEMPTYMARKUP*>(lp);
            empty->dwFlags = EMF_CENTERED;
            wcscpy_s(empty->szMarkup, d && d->scanning ? L"正在扫描，请稍候…" :
                d && d->cancelled.load() ? L"扫描已取消，点击开始扫描重试" :
                d && !d->folder.empty() ? L"未发现重复文件" : L"选择文件夹后，重复文件会显示在这里");
            SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, TRUE);
            return TRUE;
        }
        break;
    }

    case WM_CLOSE:
        requestClose(hDlg, d);
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
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
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
