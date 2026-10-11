#include "dialog.h"
#include <algorithm>
#include <filesystem>
#include <shlobj.h>

void drawPreview(const DRAWITEMSTRUCT& item, DlgData& d) {
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

void selectPreview(HWND hDlg, DlgData& d, int row) {
    d.preview = {};
    d.previewRow = -1;
    d.previewPath.clear();
    d.preview.message = L"选择一张图片\n在此查看预览";
    SetDlgItemTextW(hDlg, IDC_PREVIEW_TITLE, L"文件预览");
    SetDlgItemTextW(hDlg, IDC_PREVIEW_NAME, L"尚未选择文件");
    SetDlgItemTextW(hDlg, IDC_PREVIEW_INFO, L"单击左侧结果查看详情");
    SetDlgItemTextW(hDlg, IDC_PREVIEW_PATH, L"");
    bool available = false;
    if (row >= 0 && row < static_cast<int>(d.visibleRows.size())) {
        const auto& ref = d.items[d.visibleRows[row]];
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

void refreshPreview(HWND hDlg, DlgData& d) {
    auto image = d.previewLoader.takeReady();
    if (!image || image->request != d.previewRequest) return;
    d.preview = std::move(*image);
    if (!d.preview.pixels.empty() && d.previewRow >= 0) {
        const auto& ref = d.items[d.visibleRows[d.previewRow]];
        SetDlgItemTextW(hDlg, IDC_PREVIEW_TITLE, L"图片预览");
        SetDlgItemTextW(hDlg, IDC_PREVIEW_INFO,
            ((ref.f == 0 ? L"保留 · " : L"副本 · ") + fmtSize(d.result.groups[ref.g].fileSize) + L" · " +
             std::to_wstring(d.preview.originalWidth) + L" × " + std::to_wstring(d.preview.originalHeight)).c_str());
    }
    SetDlgItemTextW(hDlg, IDC_PREVIEW_IMAGE,
        d.preview.pixels.empty() ? d.preview.message.c_str() : L"已加载所选图片预览");
    InvalidateRect(GetDlgItem(hDlg, IDC_PREVIEW_IMAGE), nullptr, FALSE);
}

void revealSelectedFile(HWND hDlg, DlgData& d) {
    if (d.scanning || d.cleaning || d.previewRow < 0 || d.items[d.visibleRows[d.previewRow]].removed) return;
    PIDLIST_ABSOLUTE item = nullptr;
    HRESULT hr = SHParseDisplayName(d.previewPath.c_str(), nullptr, &item, 0, nullptr);
    if (SUCCEEDED(hr)) hr = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
    CoTaskMemFree(item);
    if (FAILED(hr)) MessageBoxW(hDlg, L"无法定位文件，文件可能已移动或删除。", L"定位文件", MB_OK | MB_ICONINFORMATION);
}
