// 重复文件清理工具：选择目录或从右键菜单启动，确认重复内容后清理副本。
// 构建：cmake -B build/optimized && cmake --build build/optimized --config Release

#include "resource.h"
#include "scanner.h"
#include "md5.h"
#include "image_preview.h"

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
#include <stdexcept>
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
static constexpr UINT_PTR CLEANUP_TIMER = 1;
static constexpr UINT_PTR PREVIEW_TIMER = 2;

struct CleanupTask {
    size_t row;
    std::wstring path;
    std::wstring keptPath;
    std::string expectedHash;
    uint64_t expectedSize;
};

struct CleanupOutcome {
    size_t row;
    bool recycled;
};

// 工作线程只发布结果，由界面线程分批更新控件和扫描统计。
struct CleanupProgress {
    std::mutex mutex;
    std::vector<CleanupOutcome> outcomes;
    std::wstring file;
    bool finished = false;
    bool failed = false;
};

// ---- Per-dialog state ---------------------------------------------------
struct DlgData {
    std::wstring    folder;
    ScanResult      result;
    std::thread     worker;
    // ListView item -> [groupIdx, fileIdx]; fileIdx==0 is "kept", >0 is dup
    struct ItemRef { int g; int f; bool removed = false; };
    std::vector<ItemRef> items;
    bool            scanning  = false;
    std::wstring    scanError;
    bool            cleaning = false;
    CleanupProgress cleanup;
    size_t          cleanupTotal = 0;
    size_t          cleanupApplied = 0;
    size_t          cleanupSucceeded = 0;
    size_t          cleanupFailed = 0;
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
    bool            closeAfterWork = false;
    ImagePreview    previewLoader;
    PreviewImage    preview;
    uint64_t        previewRequest = 0;
    int             previewRow = -1;
    std::wstring    previewPath;
    HIMAGELIST      rowHeightImages = nullptr;

    ~DlgData() {
        DeleteObject(bodyFont);
        DeleteObject(titleFont);
        DeleteObject(numberFont);
        DeleteObject(background);
        DeleteObject(card);
        if (rowHeightImages) ImageList_Destroy(rowHeightImages);
    }
};

static void scanThread(HWND hDlg, std::wstring folder);
static std::wstring fmtSize(uint64_t bytes);
static void selectPreview(HWND hDlg, DlgData& d, int row);
static void requestClose(HWND hDlg, DlgData* d);

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
    d.titleFont = makeFont(24, FW_SEMIBOLD);
    d.numberFont = makeFont(22, FW_SEMIBOLD);
    for (HWND child = GetWindow(hDlg, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(d.bodyFont), TRUE);
    SendDlgItemMessageW(hDlg, IDC_TITLE, WM_SETFONT, reinterpret_cast<WPARAM>(d.titleFont), TRUE);
    for (int id : {IDC_FILES_VALUE, IDC_DUP_VALUE, IDC_SPACE_VALUE})
        SendDlgItemMessageW(hDlg, id, WM_SETFONT, reinterpret_cast<WPARAM>(d.numberFont), TRUE);
    DeleteObject(oldBody);
    DeleteObject(oldTitle);
    DeleteObject(oldNumber);
    HIMAGELIST spacing = ImageList_Create(1, scaled(hDlg, 30), ILC_COLOR32, 1, 1);
    ListView_SetImageList(GetDlgItem(hDlg, IDC_LIST), spacing, LVSIL_SMALL);
    if (d.rowHeightImages) ImageList_Destroy(d.rowHeightImages);
    d.rowHeightImages = spacing;
}

static int previewWidth(int width) { return std::clamp(width / 3, 260, 360); }

static void layoutDialog(HWND hDlg) {
    RECT client{};
    GetClientRect(hDlg, &client);
    const int w = MulDiv(client.right, 96, static_cast<int>(GetDpiForWindow(hDlg)));
    const int h = MulDiv(client.bottom, 96, static_cast<int>(GetDpiForWindow(hDlg)));
    const bool compact = h < 700;
    const int headerShift = compact ? 28 : 0;
    const int resultsShift = compact ? 52 : 0;
    auto place = [hDlg](int id, int x, int y, int width, int height) {
        MoveWindow(GetDlgItem(hDlg, id), scaled(hDlg, x), scaled(hDlg, y),
                   scaled(hDlg, width), scaled(hDlg, height), TRUE);
    };
    place(IDC_TITLE, 24, 16, w - 48, 34);
    place(IDC_SUBTITLE, 25, 54, w - 48, 22);
    ShowWindow(GetDlgItem(hDlg, IDC_SUBTITLE), compact ? SW_HIDE : SW_SHOW);
    place(IDC_FOLDER_PATH, 40, 98 - headerShift, w - 332, 24);
    place(IDC_BROWSE_BTN, w - 276, 90 - headerShift, 118, 36);
    place(IDC_SCAN_BTN, w - 146, 90 - headerShift, 106, 36);
    place(IDC_STATUS, 40, 137 - headerShift, w - 80, 22);
    place(IDC_PROGRESS, 40, 166 - headerShift, w - 80, 6);
    const int cardWidth = (w - 72) / 3;
    const int labels[] = {IDC_FILES_LABEL, IDC_DUP_LABEL, IDC_SPACE_LABEL};
    const int values[] = {IDC_FILES_VALUE, IDC_DUP_VALUE, IDC_SPACE_VALUE};
    for (int i = 0; i < 3; ++i) {
        const int x = 40 + i * (cardWidth + 12);
        place(labels[i], x, 195 - headerShift, compact ? cardWidth / 2 : cardWidth - 32, 22);
        place(values[i], x + (compact ? cardWidth / 2 : 0), compact ? 163 : 220,
            compact ? cardWidth / 2 - 32 : cardWidth - 32, 30);
    }
    const int sideWidth = previewWidth(w), sideX = w - 24 - sideWidth;
    const int listWidth = sideX - 40;
    place(IDC_RESULTS_LABEL, 24, 270 - resultsShift, 94, 22);
    place(IDC_HINT, 118, 270 - resultsShift, listWidth - 94, 22);
    place(IDC_LIST, 25, 301 - resultsShift, listWidth - 2, (std::max)(h - 402 + resultsShift, 80));
    place(IDC_PREVIEW_TITLE, sideX, 270 - resultsShift, sideWidth, 22);
    const int detailShift = compact ? 24 : 0;
    place(IDC_PREVIEW_IMAGE, sideX + 16, 316 - resultsShift, sideWidth - 32,
          (std::max)(h - 582 + resultsShift + detailShift, 54));
    place(IDC_PREVIEW_NAME, sideX + 16, h - 250 + detailShift, sideWidth - 32, 24);
    place(IDC_PREVIEW_INFO, sideX + 16, h - 221 + detailShift, sideWidth - 32, 22);
    place(IDC_PREVIEW_PATH, sideX + 16, h - 194 + detailShift, sideWidth - 32, 22);
    place(IDC_REVEAL_BTN, sideX + 16, h - 161 + detailShift, sideWidth - 32, 34);
    place(IDC_PREVIEW_NOTE, sideX + 16, h - 119, sideWidth - 32, 18);
    ShowWindow(GetDlgItem(hDlg, IDC_PREVIEW_NOTE), compact ? SW_HIDE : SW_SHOW);
    place(IDC_MENU_STATUS, 24, h - 82, w - 48, 20);
    place(IDC_INSTALL_BTN, 24, h - 54, 132, 34);
    place(IDC_UNINSTALL_BTN, 168, h - 54, 132, 34);
    place(IDC_DELETE_BTN, w - 282, h - 54, 154, 34);
    place(IDC_CLOSE_BTN, w - 116, h - 54, 92, 34);
    HWND list = GetDlgItem(hDlg, IDC_LIST);
    ListView_SetColumnWidth(list, 0, scaled(hDlg, 80));
    const int nameWidth = std::clamp(listWidth / 3, 140, 220);
    ListView_SetColumnWidth(list, 1, scaled(hDlg, nameWidth));
    ListView_SetColumnWidth(list, 2, (std::max)(scaled(hDlg, listWidth - nameWidth - 176), 80));
    ListView_SetColumnWidth(list, 3, scaled(hDlg, 76));
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
    const bool compact = h < 700;
    const int headerShift = compact ? 28 : 0;
    const int resultsShift = compact ? 52 : 0;
    panel(24, 82 - headerShift, w - 48, 98);
    const int cardWidth = (w - 72) / 3;
    for (int i = 0; i < 3; ++i)
        panel(24 + i * (cardWidth + 12), 186 - headerShift, cardWidth, compact ? 46 : 70);
    const int sideWidth = previewWidth(w), sideX = w - 24 - sideWidth;
    panel(24, 300 - resultsShift, sideX - 40, (std::max)(h - 400 + resultsShift, 82));
    panel(sideX, 300 - resultsShift, sideWidth, (std::max)(h - 400 + resultsShift, 82));
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
        item.CtlID == IDC_SCAN_BTN || item.CtlID == IDC_BROWSE_BTN || item.CtlID == IDC_REVEAL_BTN
            ? d.card : d.background);
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

static void drawPreview(const DRAWITEMSTRUCT& item, DlgData& d) {
    HBRUSH background = CreateSolidBrush(RGB(241, 245, 249));
    FillRect(item.hDC, &item.rcItem, background);
    DeleteObject(background);
    const int saved = SaveDC(item.hDC);
    RECT rect = item.rcItem;
    InflateRect(&rect, -scaled(item.hwndItem, 12), -scaled(item.hwndItem, 12));
    if (!d.preview.pixels.empty()) {
        const double ratio = (std::min)(double(rect.right - rect.left) / d.preview.width,
                                       double(rect.bottom - rect.top) / d.preview.height);
        const int width = (std::max)(1, static_cast<int>(d.preview.width * ratio));
        const int height = (std::max)(1, static_cast<int>(d.preview.height * ratio));
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = d.preview.width;
        info.bmiHeader.biHeight = -static_cast<LONG>(d.preview.height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(item.hDC, HALFTONE);
        SetBrushOrgEx(item.hDC, 0, 0, nullptr);
        StretchDIBits(item.hDC, rect.left + (rect.right - rect.left - width) / 2,
            rect.top + (rect.bottom - rect.top - height) / 2, width, height,
            0, 0, d.preview.width, d.preview.height, d.preview.pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    } else {
        SelectObject(item.hDC, d.bodyFont);
        SetTextColor(item.hDC, RGB(100, 116, 139));
        SetBkMode(item.hDC, TRANSPARENT);
        const auto& text = d.preview.message;
        RECT measured = rect;
        DrawTextW(item.hDC, text.c_str(), -1, &measured, DT_CENTER | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        rect.top += (std::max)(0L, (rect.bottom - rect.top - (measured.bottom - measured.top)) / 2);
        DrawTextW(item.hDC, text.c_str(), -1, &rect, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    }
    RestoreDC(item.hDC, saved);
}

static void selectPreview(HWND hDlg, DlgData& d, int row) {
    d.preview = {};
    d.previewRow = -1;
    d.previewPath.clear();
    d.preview.message = L"选择一张图片\n在此查看预览";
    SetDlgItemTextW(hDlg, IDC_PREVIEW_TITLE, L"文件预览");
    SetDlgItemTextW(hDlg, IDC_PREVIEW_NAME, L"尚未选择文件");
    SetDlgItemTextW(hDlg, IDC_PREVIEW_INFO, L"单击左侧结果查看详情");
    SetDlgItemTextW(hDlg, IDC_PREVIEW_PATH, L"");
    bool available = false;
    if (row >= 0 && row < static_cast<int>(d.items.size())) {
        const auto& ref = d.items[row];
        if (ref.g >= 0 && ref.f >= 0) {
            d.previewRow = row;
            const auto& group = d.result.groups[ref.g];
            const auto& path = group.files[ref.f];
            d.previewPath = path;
            available = !ref.removed;
            SetDlgItemTextW(hDlg, IDC_PREVIEW_NAME, std::filesystem::path(path).filename().c_str());
            SetDlgItemTextW(hDlg, IDC_PREVIEW_PATH, path.c_str());
            SetDlgItemTextW(hDlg, IDC_PREVIEW_INFO,
                ((ref.removed ? L"已回收 · " : ref.f == 0 ? L"保留 · " : L"重复副本 · ") + fmtSize(group.fileSize)).c_str());
            d.preview.message = available ? L"正在加载预览…" : L"此副本已移入回收站\n选择同组保留文件可继续预览";
        }
    }
    d.previewRequest = d.previewLoader.select(available ? d.previewPath : L"");
    EnableWindow(GetDlgItem(hDlg, IDC_REVEAL_BTN), available && !d.cleaning && !d.scanning);
    SetDlgItemTextW(hDlg, IDC_PREVIEW_IMAGE, d.preview.message.c_str());
    InvalidateRect(GetDlgItem(hDlg, IDC_PREVIEW_IMAGE), nullptr, FALSE);
}

static void refreshPreview(HWND hDlg, DlgData& d) {
    auto image = d.previewLoader.takeReady();
    if (!image || image->request != d.previewRequest) return;
    d.preview = std::move(*image);
    if (!d.preview.pixels.empty() && d.previewRow >= 0) {
        const auto& ref = d.items[d.previewRow];
        SetDlgItemTextW(hDlg, IDC_PREVIEW_TITLE, L"图片预览");
        SetDlgItemTextW(hDlg, IDC_PREVIEW_INFO,
            ((ref.f == 0 ? L"保留 · " : L"副本 · ") + fmtSize(d.result.groups[ref.g].fileSize) + L" · " +
             std::to_wstring(d.preview.originalWidth) + L" × " + std::to_wstring(d.preview.originalHeight)).c_str());
    }
    SetDlgItemTextW(hDlg, IDC_PREVIEW_IMAGE,
        d.preview.pixels.empty() ? d.preview.message.c_str() : L"已加载所选图片预览");
    InvalidateRect(GetDlgItem(hDlg, IDC_PREVIEW_IMAGE), nullptr, FALSE);
}

static void revealSelectedFile(HWND hDlg, DlgData& d) {
    if (d.scanning || d.cleaning || d.previewRow < 0 || d.items[d.previewRow].removed) return;
    PIDLIST_ABSOLUTE item = nullptr;
    HRESULT hr = SHParseDisplayName(d.previewPath.c_str(), nullptr, &item, 0, nullptr);
    if (SUCCEEDED(hr)) hr = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
    CoTaskMemFree(item);
    if (FAILED(hr)) MessageBoxW(hDlg, L"无法定位文件，文件可能已移动或删除。", L"定位文件", MB_OK | MB_ICONINFORMATION);
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
    bool cleaning = d && d->cleaning;
    bool busy = scanning || cleaning;
    bool hasFolder = d && !d->folder.empty();
    bool hasDuplicates = d && d->result.totalDup > 0;

    EnableWindow(GetDlgItem(hDlg, IDC_BROWSE_BTN), !busy);
    EnableWindow(GetDlgItem(hDlg, IDC_SCAN_BTN), !cleaning && (scanning || hasFolder));
    SetDlgItemTextW(hDlg, IDC_SCAN_BTN, scanning ? L"取消扫描" : L"开始扫描");
    EnableWindow(GetDlgItem(hDlg, IDC_DELETE_BTN), cleaning ? !d->cancelled.load() : !scanning && hasDuplicates);
    SetDlgItemTextW(hDlg, IDC_DELETE_BTN, cleaning ? L"停止清理" : L"清理重复副本");
    EnableWindow(GetDlgItem(hDlg, IDC_REVEAL_BTN), !busy && d && d->previewRow >= 0 &&
        !d->items[d->previewRow].removed);
    updateContextMenuState(hDlg, busy);
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
    if (d.scanning || d.cleaning || folder.empty()) return;

    if (d.worker.joinable()) d.worker.join();

    selectPreview(hDlg, d, -1);
    d.folder = folder;
    d.result = {};
    d.items.clear();
    d.cancelled.store(false);
    d.progressPending.store(false);
    d.current = 0;
    d.total = 0;
    d.started = std::chrono::steady_clock::now();
    d.scanning = true;
    d.scanError.clear();

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
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_INFOTIP);
    SetWindowTheme(hList, L"Explorer", nullptr);
    ListView_SetBkColor(hList, RGB(255, 255, 255));
    ListView_SetTextColor(hList, RGB(51, 65, 85));

    const struct { const wchar_t* t; int w; } cols[] = {
        { L"处理方式", 92 },
        { L"文件名", 180 },
        { L"所在文件夹", 240 },
        { L"文件大小", 94 },
    };
    LVCOLUMNW c = { LVCF_TEXT | LVCF_WIDTH | LVCF_FMT };
    for (int i = 0; i < 4; ++i) {
        c.pszText = const_cast<wchar_t*>(cols[i].t);
        c.cx      = cols[i].w;
        c.fmt     = (i == 3) ? LVCFMT_RIGHT : LVCFMT_LEFT;
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

            const std::filesystem::path path(grp.files[f]);
            auto name = path.filename().wstring();
            auto folder = path.parent_path().wstring();
            ListView_SetItemText(hList, item.iItem, 1, name.data());
            ListView_SetItemText(hList, item.iItem, 2, folder.data());
            std::wstring sz = fmtSize(grp.fileSize);
            ListView_SetItemText(hList, item.iItem, 3,
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
    if (ListView_GetItemState(draw->nmcd.hdr.hwndFrom, row, LVIS_SELECTED) & LVIS_SELECTED) {
        draw->clrText = RGB(30, 64, 175);
        draw->clrTextBk = RGB(219, 234, 254);
        return CDRF_NEWFONT;
    }
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

// ---- 后台清理与进度刷新 ---------------------------------------------------

static bool recycleFile(const std::wstring& path) {
    // 相对路径会使 FOF_ALLOWUNDO 失效；不能依赖清理时的工作目录解析路径。
    if (!std::filesystem::path(path).is_absolute()) return false;
    // SHFileOperation 要求路径以两个空字符结尾。
    std::wstring buf = path + L'\0';
    SHFILEOPSTRUCTW f = {};
    f.wFunc  = FO_DELETE;
    f.pFrom  = buf.c_str();
    f.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION
             | FOF_NOERRORUI  | FOF_SILENT;
    return SHFileOperationW(&f) == 0 && !f.fAnyOperationsAborted;
}

static bool recycleVerifiedDuplicate(const CleanupTask& task, const std::atomic_bool& cancelled) {
    if (cancelled.load()) return false;
    // 复核及回收期间锁住保留文件，禁止写入、重命名和删除。
    // 副本允许删除共享以供 Shell 回收，但禁止其他进程写入内容。
    struct FileHandle {
        HANDLE value;
        ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } kept{CreateFileW(task.keptPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr)},
      duplicate{CreateFileW(task.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr)};
    if (kept.value == INVALID_HANDLE_VALUE || duplicate.value == INVALID_HANDLE_VALUE) return false;
    const auto checkCancelled = [&] { return cancelled.load(); };
    if (task.expectedHash.empty() ||
        MD5::hashFile(task.keptPath, checkCancelled, task.expectedSize) != task.expectedHash ||
        MD5::hashFile(task.path, checkCancelled, task.expectedSize) != task.expectedHash ||
        cancelled.load()) return false;
    return recycleFile(task.path);
}

static void cleanupThread(DlgData* d, std::vector<CleanupTask> tasks) {
    const HRESULT comHr = CoInitializeEx(nullptr,
        COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool failed = FAILED(comHr);
    try {
        if (!failed) for (const auto& task : tasks) {
            if (d->cancelled.load()) break;
            {
                std::lock_guard<std::mutex> lock(d->cleanup.mutex);
                d->cleanup.file = task.path;
            }
            const bool recycled = recycleVerifiedDuplicate(task, d->cancelled);
            if (!recycled && d->cancelled.load()) break;
            std::lock_guard<std::mutex> lock(d->cleanup.mutex);
            d->cleanup.outcomes.push_back({task.row, recycled});
        }
    } catch (...) {
        failed = true;
    }
    if (SUCCEEDED(comHr)) CoUninitialize();
    std::lock_guard<std::mutex> lock(d->cleanup.mutex);
    d->cleanup.failed = failed;
    d->cleanup.finished = true;
}

static void cancelCleanup(HWND hDlg, DlgData& d) {
    if (!d.cleaning || d.cancelled.exchange(true)) return;
    SetDlgItemTextW(hDlg, IDC_STATUS, L"正在停止清理，等待当前文件处理完成…");
    EnableWindow(GetDlgItem(hDlg, IDC_DELETE_BTN), FALSE);
}

static void refreshCleanup(HWND hDlg, DlgData& d) {
    if (!d.cleaning) return;
    // 每次最多更新 512 行，避免积压结果长时间占用界面线程。
    CleanupOutcome batch[512];
    size_t count = 0;
    bool finished, failed;
    std::wstring file;
    {
        std::lock_guard<std::mutex> lock(d.cleanup.mutex);
        count = (std::min)(std::size(batch), d.cleanup.outcomes.size() - d.cleanupApplied);
        std::copy_n(d.cleanup.outcomes.begin() + d.cleanupApplied, count, batch);
        finished = d.cleanup.finished && d.cleanupApplied + count == d.cleanup.outcomes.size();
        failed = d.cleanup.failed;
        file = d.cleanup.file;
    }
    HWND list = GetDlgItem(hDlg, IDC_LIST);
    if (count) SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    for (size_t i = 0; i < count; ++i) {
        auto& ref = d.items[batch[i].row];
        if (batch[i].recycled) {
            ref.removed = true;
            ++d.cleanupSucceeded;
            --d.result.totalDup;
            d.result.wastedBytes -= d.result.groups[ref.g].fileSize;
        } else {
            ++d.cleanupFailed;
        }
        ListView_SetItemText(list, static_cast<int>(batch[i].row), 0,
            const_cast<wchar_t*>(batch[i].recycled ? L"已回收" : L"清理失败"));
        if (batch[i].recycled && static_cast<int>(batch[i].row) == d.previewRow)
            selectPreview(hDlg, d, d.previewRow);
    }
    if (count) {
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, FALSE);
    }
    d.cleanupApplied += count;
    setSummary(hDlg, d.result.totalFiles, d.result.totalDup, d.result.wastedBytes);
    const int position = d.cleanupTotal ? static_cast<int>(1000ULL * d.cleanupApplied / d.cleanupTotal) : 0;
    SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETPOS, position, 0);
    const std::wstring counts = std::to_wstring(d.cleanupApplied) + L" / " + std::to_wstring(d.cleanupTotal) +
        L" · 已回收 " + std::to_wstring(d.cleanupSucceeded) + L" · 失败 " + std::to_wstring(d.cleanupFailed);
    if (!finished) {
        SetDlgItemTextW(hDlg, IDC_STATUS, ((d.cancelled.load()
            ? L"正在停止，等待当前文件完成 · " : L"正在清理 ") + counts + L" · " + file).c_str());
        return;
    }
    KillTimer(hDlg, CLEANUP_TIMER);
    if (d.worker.joinable()) d.worker.join();
    d.cleaning = false;
    const wchar_t* status = failed ? L"清理中断 · " :
        d.cleanupApplied < d.cleanupTotal ? L"清理已停止 · " : L"清理完成 · ";
    SetDlgItemTextW(hDlg, IDC_STATUS, (status + counts).c_str());
    updateScanControls(hDlg, &d);
    if (d.closeAfterWork) requestClose(hDlg, &d);
}

static void startCleanup(HWND hDlg, DlgData& d) {
    if (d.scanning || d.cleaning || d.result.totalDup == 0) return;
    if (d.worker.joinable()) d.worker.join();
    try {
        std::vector<CleanupTask> tasks;
        tasks.reserve(d.result.totalDup);
        for (size_t row = 0; row < d.items.size(); ++row) {
            const auto& ref = d.items[row];
            if (ref.g >= 0 && ref.f > 0 && !ref.removed) {
                const auto& group = d.result.groups[ref.g];
                tasks.push_back({row, group.files[ref.f], group.files.front(), group.md5, group.fileSize});
            }
        }
        if (tasks.empty()) return;
        d.cleanup.outcomes.clear();
        d.cleanup.outcomes.reserve(tasks.size());
        d.cleanup.file.clear();
        d.cleanup.finished = false;
        d.cleanup.failed = false;
        d.cleanupTotal = tasks.size();
        d.cleanupApplied = d.cleanupSucceeded = d.cleanupFailed = 0;
        d.cancelled.store(false);
        d.cleaning = true;
        setProgressMarquee(hDlg, false);
        SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETRANGE32, 0, 1000);
        SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETPOS, 0, 0);
        SetDlgItemTextW(hDlg, IDC_STATUS,
            (L"正在清理 0 / " + std::to_wstring(d.cleanupTotal) + L" · 正在准备回收文件…").c_str());
        updateScanControls(hDlg, &d);
        if (!SetTimer(hDlg, CLEANUP_TIMER, 80, nullptr))
            throw std::runtime_error("cleanup timer");
        d.worker = std::thread(cleanupThread, &d, std::move(tasks));
    } catch (...) {
        KillTimer(hDlg, CLEANUP_TIMER);
        d.cleaning = false;
        SetDlgItemTextW(hDlg, IDC_STATUS, L"无法启动清理，请稍后重试");
        updateScanControls(hDlg, &d);
    }
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
    } catch (const ScanError& error) {
        d->scanError = L"扫描失败 · 无法读取目录（错误 " + std::to_wstring(error.code) + L"）· " + error.path;
        PostMessageW(hDlg, WM_SCAN_DONE, 0, 0);
    } catch (...) {
        d->scanError = L"扫描失败，请重新选择目录后重试";
        PostMessageW(hDlg, WM_SCAN_DONE, 0, 0);
    }
}

static void requestClose(HWND hDlg, DlgData* d) {
    if (d) {
        d->closeAfterWork = true;
        d->previewLoader.stop();
    }
    if (d && d->cleaning) {
        d->closeAfterWork = true;
        cancelCleanup(hDlg, *d);
        EnableWindow(GetDlgItem(hDlg, IDC_CLOSE_BTN), FALSE);
        return;
    }
    if (d && d->scanning) {
        d->closeAfterWork = true;
        cancelScan(hDlg, *d);
        EnableWindow(GetDlgItem(hDlg, IDC_CLOSE_BTN), FALSE);
        return;
    }
    if (d && !d->previewLoader.stopped()) {
        SetDlgItemTextW(hDlg, IDC_STATUS, L"正在结束图片预览…");
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
        selectPreview(hDlg, *d, -1);
        if (!SetTimer(hDlg, PREVIEW_TIMER, 80, nullptr)) {
            EndDialog(hDlg, 1);
            return TRUE;
        }
        SetWindowTheme(GetDlgItem(hDlg, IDC_PROGRESS), L"", L"");
        SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETBARCOLOR, 0, RGB(37, 99, 235));
        SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETBKCOLOR, 0, RGB(231, 237, 247));
        RECT initial{};
        GetWindowRect(hDlg, &initial);
        const int width = scaled(hDlg, 1160), height = scaled(hDlg, 820);
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

    case WM_TIMER:
        if (wp == CLEANUP_TIMER && d) { refreshCleanup(hDlg, *d); return TRUE; }
        if (wp == PREVIEW_TIMER && d) {
            refreshPreview(hDlg, *d);
            if (d->closeAfterWork && !d->scanning && !d->cleaning) requestClose(hDlg, d);
            return TRUE;
        }
        break;

    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE)
            updateContextMenuState(hDlg, d && (d->scanning || d->cleaning));
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
            (id >= IDC_PREVIEW_NAME && id <= IDC_PREVIEW_PATH) || id == IDC_PREVIEW_NOTE ||
            (id >= IDC_FILES_LABEL && id <= IDC_SPACE_VALUE);
        COLORREF color = RGB(100, 116, 139);
        if (id == IDC_TITLE || id == IDC_FOLDER_PATH || id == IDC_RESULTS_LABEL ||
            id == IDC_PREVIEW_TITLE || id == IDC_PREVIEW_NAME)
            color = RGB(30, 41, 59);
        if (id == IDC_FILES_VALUE) color = RGB(37, 99, 235);
        if (id == IDC_DUP_VALUE) color = RGB(190, 40, 55);
        if (id == IDC_SPACE_VALUE) color = RGB(24, 125, 88);
        SetTextColor(dc, color);
        SetBkColor(dc, card ? RGB(255, 255, 255) : RGB(245, 247, 251));
        return reinterpret_cast<INT_PTR>(card ? d->card : d->background);
    }

    case WM_DRAWITEM:
        if (d && wp == IDC_PREVIEW_IMAGE) { drawPreview(*reinterpret_cast<DRAWITEMSTRUCT*>(lp), *d); return TRUE; }
        if (d && wp) { drawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lp), *d); return TRUE; }
        break;

    case WM_COMMAND: {
        if (d && d->closeAfterWork && LOWORD(wp) != IDC_CLOSE_BTN && LOWORD(wp) != IDCANCEL) return TRUE;
        switch (LOWORD(wp)) {
        case IDC_REVEAL_BTN:
            if (d) revealSelectedFile(hDlg, *d);
            return TRUE;
        case IDC_CLOSE_BTN:
        case IDCANCEL:
            requestClose(hDlg, d);
            return TRUE;

        case IDC_BROWSE_BTN:
            if (d && !d->scanning && !d->cleaning) {
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
            if (d && d->cleaning) { cancelCleanup(hDlg, *d); return TRUE; }
            if (!d || d->scanning || d->result.totalDup == 0) break;
            {
                int ret = MessageBoxW(hDlg,
                    L"将所有重复副本移入回收站？\n"
                    L"每组保留一份文件，其余副本移入回收站。",
                    L"确认清理", MB_OKCANCEL | MB_ICONWARNING);
                if (ret != IDOK) break;

                startCleanup(hDlg, *d);
            }
            return TRUE;

        case IDC_INSTALL_BTN:
            if (!d || d->scanning || d->cleaning) return TRUE;
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
            if (!d || d->scanning || d->cleaning) return TRUE;
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
        if (hdr && d && hdr->idFrom == IDC_LIST && hdr->code == LVN_GETINFOTIP) {
            auto* tip = reinterpret_cast<NMLVGETINFOTIPW*>(lp);
            if (tip->iItem >= 0 && tip->iItem < static_cast<int>(d->items.size())) {
                const auto& ref = d->items[tip->iItem];
                if (ref.g >= 0 && ref.f >= 0)
                    wcsncpy_s(tip->pszText, tip->cchTextMax, d->result.groups[ref.g].files[ref.f].c_str(), _TRUNCATE);
            }
            return TRUE;
        }
        if (hdr && d && hdr->idFrom == IDC_LIST && hdr->code == LVN_ITEMCHANGED) {
            auto* change = reinterpret_cast<NMLISTVIEW*>(lp);
            if ((change->uChanged & LVIF_STATE) && ((change->uOldState ^ change->uNewState) & LVIS_SELECTED))
                selectPreview(hDlg, *d, ListView_GetNextItem(hdr->hwndFrom, -1, LVNI_SELECTED));
            return TRUE;
        }
        if (hdr && d && hdr->idFrom == IDC_LIST && hdr->code == NM_DBLCLK) {
            if (reinterpret_cast<NMITEMACTIVATE*>(lp)->iItem >= 0) revealSelectedFile(hDlg, *d);
            return TRUE;
        }
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
                d && !d->scanError.empty() ? L"扫描失败，请检查目录后重试" :
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
        KillTimer(hDlg, PREVIEW_TIMER);
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
