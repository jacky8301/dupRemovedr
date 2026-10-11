#include "dialog.h"
#include <filesystem>
#include <uxtheme.h>

void initListView(HWND hList) {
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

void filterListView(HWND hList, DlgData& d) {
    HWND dialog = GetParent(hList);
    const int selected = d.previewRow >= 0 ? d.visibleRows[d.previewRow] : -1;
    selectPreview(dialog, d, -1);
    SendMessageW(hList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(hList);
    d.visibleRows.clear();
    d.itemRows.assign(d.items.size(), -1);

    std::vector<bool> matches(d.result.groups.size(), d.searchQuery.empty());
    size_t matchedGroups = 0;
    for (size_t g = 0; g < d.result.groups.size(); ++g) {
        if (!matches[g]) {
            for (const auto& file : d.result.groups[g].files) {
                const auto name = std::filesystem::path(file).filename().wstring();
                if (FindStringOrdinal(FIND_FROMSTART, name.c_str(), static_cast<int>(name.size()),
                    d.searchQuery.c_str(), static_cast<int>(d.searchQuery.size()), TRUE) >= 0) {
                    matches[g] = true;
                    break;
                }
            }
        }
        if (matches[g]) ++matchedGroups;
    }

    LVITEMW item{};
    int previousGroup = -1;
    for (int index = 0; index < static_cast<int>(d.items.size()); ++index) {
        const auto& ref = d.items[index];
        if (ref.g < 0 || !matches[ref.g]) continue;
        const auto& grp = d.result.groups[ref.g];
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iSubItem = 0;
        // 只在可见组之间插入分隔行，并沿用该组前的完整结果索引。
        if (previousGroup >= 0 && previousGroup != ref.g) {
            item.iItem = static_cast<int>(d.visibleRows.size());
            item.pszText = const_cast<wchar_t*>(L"");
            item.lParam = index - 1;
            ListView_InsertItem(hList, &item);
            d.itemRows[index - 1] = item.iItem;
            d.visibleRows.push_back(index - 1);
        }
        previousGroup = ref.g;
        item.iItem = static_cast<int>(d.visibleRows.size());
        item.pszText = const_cast<wchar_t*>(!ref.status.empty() ? ref.status.c_str() :
            ref.removed ? L"已回收" : ref.f == 0 ? L"保留" : L"重复副本");
        item.lParam = index;
        d.itemRows[index] = item.iItem;
        d.visibleRows.push_back(index);
        ListView_InsertItem(hList, &item);
        const std::filesystem::path path(grp.files[ref.f]);
        auto name = path.filename().wstring();
        auto folder = path.parent_path().wstring();
        ListView_SetItemText(hList, item.iItem, 1, name.data());
        ListView_SetItemText(hList, item.iItem, 2, folder.data());
        auto size = fmtSize(grp.fileSize);
        ListView_SetItemText(hList, item.iItem, 3, size.data());
    }
    SetDlgItemTextW(dialog, IDC_HINT, (L"显示 " + std::to_wstring(matchedGroups) + L" / " +
        std::to_wstring(d.result.groups.size()) + L" 组 · 单击预览 · 双击定位").c_str());
    if (selected >= 0 && d.itemRows[selected] >= 0) {
        const int row = d.itemRows[selected];
        ListView_SetItemState(hList, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(hList, row, FALSE);
    }
    SendMessageW(hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hList, nullptr, TRUE);
}

void populateListView(HWND hList, DlgData& d) {
    selectPreview(GetParent(hList), d, -1);
    d.items.clear();
    for (int g = 0; g < static_cast<int>(d.result.groups.size()); ++g) {
        if (g > 0) d.items.push_back({-1, -1, false, {}});
        for (int f = 0; f < static_cast<int>(d.result.groups[g].files.size()); ++f)
            d.items.push_back({g, f, false, {}});
    }
    filterListView(hList, d);
}

static LRESULT colorListViewRow(NMLVCUSTOMDRAW* draw, DlgData* d) {
    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) {
        return CDRF_NOTIFYITEMDRAW;
    }

    if (draw->nmcd.dwDrawStage != CDDS_ITEMPREPAINT || !d) {
        return CDRF_DODEFAULT;
    }

    int row = static_cast<int>(draw->nmcd.dwItemSpec);
    if (row < 0 || row >= static_cast<int>(d->visibleRows.size())) {
        return CDRF_DODEFAULT;
    }

    const auto& ref = d->items[d->visibleRows[row]];
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

INT_PTR handleListNotification(HWND hDlg, DlgData* d, LPARAM lp) {
    auto* hdr = reinterpret_cast<NMHDR*>(lp);
    if (hdr && d && hdr->idFrom == IDC_LIST && hdr->code == LVN_GETINFOTIP) {
        auto* tip = reinterpret_cast<NMLVGETINFOTIPW*>(lp);
        if (tip->iItem >= 0 && tip->iItem < static_cast<int>(d->visibleRows.size())) {
            const auto& ref = d->items[d->visibleRows[tip->iItem]];
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
            d && !d->searchQuery.empty() ? L"没有匹配的分组，请尝试其他文件名或清空搜索" :
            d && d->cancelled.load() ? L"扫描已取消，点击开始扫描重试" :
            d && !d->scanError.empty() ? L"扫描失败，请检查目录后重试" :
            d && !d->folder.empty() ? L"未发现重复文件" : L"选择文件夹后，重复文件会显示在这里");
        SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, TRUE);
        return TRUE;
    }
    return FALSE;
}
