#include "dialog.h"
#include "context_menu.h"

static INT_PTR confirmCleanup(HWND hDlg, DlgData* d) {
    if (d && d->cleaning) { cancelCleanup(hDlg, *d); return TRUE; }
    if (!d || d->scanning || d->result.totalDup == 0) return FALSE;
    const int result = MessageBoxW(hDlg,
        L"将所有重复副本移入回收站？\n"
        L"每组保留一份文件，其余副本移入回收站。\n\n"
        L"清理范围为全部扫描结果，包含搜索未显示的分组。\n\n"
        L"若从回收站还原，文件将位于原目录下的 .dupRemover-staging-* 子目录，"
        L"请再将文件移回原目录。",
        L"确认清理", MB_OKCANCEL | MB_ICONWARNING);
    if (result != IDOK) return FALSE;
    startCleanup(hDlg, *d);
    return TRUE;
}

static void changeContextMenu(HWND hDlg, DlgData* d, bool install) {
    if (!d || d->scanning || d->cleaning) return;
    std::wstring error;
    const bool success = install ? registerContextMenu(error) : unregisterContextMenu(error);
    // 卸载可能只完成用户级移除，失败时也需要刷新注册状态。
    if (success || !install) updateScanControls(hDlg, d);
    if (success) {
        SetDlgItemTextW(hDlg, IDC_MENU_STATUS,
            install ? L"已为当前用户添加右键菜单" : L"右键菜单已移除");
    } else {
        MessageBoxW(hDlg, error.c_str(),
            install ? L"添加右键菜单失败" : L"移除右键菜单失败",
            MB_OK | (install ? MB_ICONERROR : MB_ICONWARNING));
    }
}

INT_PTR handleDialogCommand(HWND hDlg, DlgData* d, WPARAM wp) {
    const int command = LOWORD(wp);
    if (d && d->closeAfterWork && command != IDC_CLOSE_BTN && command != IDCANCEL) return TRUE;
    switch (command) {
    case IDC_SEARCH_EDIT:
        if (HIWORD(wp) == EN_CHANGE && d && !d->scanning && !d->cleaning) {
            HWND edit = GetDlgItem(hDlg, IDC_SEARCH_EDIT);
            std::wstring query(GetWindowTextLengthW(edit) + 1, L'\0');
            query.resize(GetWindowTextW(edit, query.data(), static_cast<int>(query.size())));
            d->searchQuery = std::move(query);
            filterListView(GetDlgItem(hDlg, IDC_LIST), *d);
            updateScanControls(hDlg, d);
        }
        return TRUE;
    case IDC_SEARCH_CLEAR:
        if (d && !d->scanning && !d->cleaning) {
            SetDlgItemTextW(hDlg, IDC_SEARCH_EDIT, L"");
            SetFocus(GetDlgItem(hDlg, IDC_SEARCH_EDIT));
        }
        return TRUE;
    case IDC_REVEAL_BTN:
        if (d) revealSelectedFile(hDlg, *d);
        return TRUE;
    case IDC_CLOSE_BTN:
    case IDCANCEL:
        requestClose(hDlg, d);
        return TRUE;
    case IDC_BROWSE_BTN:
        if (d && !d->scanning && !d->cleaning) {
            const auto folder = chooseFolder(hDlg, d->folder);
            if (!folder.empty()) startScan(hDlg, *d, folder);
        }
        return TRUE;
    case IDC_SCAN_BTN:
        if (d && d->scanning) cancelScan(hDlg, *d);
        else if (d && !d->folder.empty()) startScan(hDlg, *d, d->folder);
        return TRUE;
    case IDC_DELETE_BTN:
        return confirmCleanup(hDlg, d);
    case IDC_INSTALL_BTN:
    case IDC_UNINSTALL_BTN:
        changeContextMenu(hDlg, d, command == IDC_INSTALL_BTN);
        return TRUE;
    }
    return FALSE;
}
