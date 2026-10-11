#include "dialog.h"
#include <algorithm>

int scaled(HWND window, int value) {
    return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96);
}

void updateFonts(HWND hDlg, DlgData& d) {
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

void layoutDialog(HWND hDlg) {
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
    place(IDC_SEARCH_EDIT, 25, 300 - resultsShift, listWidth - 76, 30);
    place(IDC_SEARCH_CLEAR, 25 + listWidth - 68, 300 - resultsShift, 66, 30);
    place(IDC_LIST, 25, 341 - resultsShift, listWidth - 2, (std::max)(h - 442 + resultsShift, 80));
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

void paintDialog(HWND hDlg, DlgData& d, HDC printDC) {
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
    panel(24, 340 - resultsShift, sideX - 40, (std::max)(h - 440 + resultsShift, 82));
    panel(sideX, 300 - resultsShift, sideWidth, (std::max)(h - 400 + resultsShift, 82));
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(border);
    if (!printDC) EndPaint(hDlg, &ps);
}

void drawButton(const DRAWITEMSTRUCT& item, DlgData& d) {
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

void setSummary(HWND hDlg, int files, int duplicates, uint64_t bytes) {
    SetDlgItemTextW(hDlg, IDC_FILES_VALUE, std::to_wstring(files).c_str());
    SetDlgItemTextW(hDlg, IDC_DUP_VALUE, std::to_wstring(duplicates).c_str());
    SetDlgItemTextW(hDlg, IDC_SPACE_VALUE, fmtSize(bytes).c_str());
}

void setProgressMarquee(HWND hDlg, bool enabled) {
    HWND progress = GetDlgItem(hDlg, IDC_PROGRESS);
    LONG_PTR style = GetWindowLongPtrW(progress, GWL_STYLE);
    SetWindowLongPtrW(progress, GWL_STYLE, enabled ? style | PBS_MARQUEE : style & ~PBS_MARQUEE);
    SendMessageW(progress, PBM_SETMARQUEE, enabled, 30);
}

// 文件大小的统一显示格式。

std::wstring fmtSize(uint64_t bytes) {
    wchar_t buf[64];
    if      (bytes < 1024ULL)           swprintf_s(buf, L"%llu B",   bytes);
    else if (bytes < 1024ULL * 1024)    swprintf_s(buf, L"%.1f KB", bytes / 1024.0);
    else if (bytes < 1024ULL * 1024*1024) swprintf_s(buf, L"%.1f MB", bytes / (1024.0*1024.0));
    else                                 swprintf_s(buf, L"%.2f GB", bytes / (1024.0*1024.0*1024.0));
    return buf;
}

static INT_PTR colorDialogControl(DlgData* d, WPARAM wp, LPARAM lp) {
    if (!d) return FALSE;
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

INT_PTR handleDialogAppearance(HWND hDlg, DlgData* d, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
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

    case WM_CTLCOLORSTATIC:
        return colorDialogControl(d, wp, lp);

    case WM_DRAWITEM:
        if (d && wp == IDC_PREVIEW_IMAGE) { drawPreview(*reinterpret_cast<DRAWITEMSTRUCT*>(lp), *d); return TRUE; }
        if (d && wp) { drawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lp), *d); return TRUE; }
        break;
    }
    return FALSE;
}
