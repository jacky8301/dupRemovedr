#include "dialog.h"
#include <shellapi.h>
#include <uxtheme.h>

#pragma comment(lib, "comctl32.lib")

// 为应用程序和使用对话框的测试启用系统视觉样式。
#pragma comment(linker, "\"/manifestdependency:type='win32' \
    name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
    processorArchitecture='*' publicKeyToken='6595b64144ccf1df' \
    language='*'\"")

void requestClose(HWND hDlg, DlgData* d) {
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

static INT_PTR initializeDialog(HWND hDlg) {
    auto* d = new DlgData();
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
    SendDlgItemMessageW(hDlg, IDC_SEARCH_EDIT, EM_SETCUEBANNER, TRUE,
        reinterpret_cast<LPARAM>(L"搜索文件名，显示匹配的完整分组"));
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

    // 从命令行读取初始扫描目录。
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

INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = reinterpret_cast<DlgData*>(
        GetWindowLongPtrW(hDlg, GWLP_USERDATA));

    switch (msg) {

    case WM_INITDIALOG:
        return initializeDialog(hDlg);

    case WM_SCAN_PROGRESS:
        return handleScanProgress(hDlg, d);

    case WM_SCAN_DONE:
        return handleScanDone(hDlg, d, wp, lp);

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

    case WM_COMMAND:
        return handleDialogCommand(hDlg, d, wp);

    case WM_NOTIFY:
        return handleListNotification(hDlg, d, lp);

    case WM_CLOSE:
        requestClose(hDlg, d);
        return TRUE;

    case WM_DESTROY:
        KillTimer(hDlg, PREVIEW_TIMER);
        delete d;
        SetWindowLongPtrW(hDlg, GWLP_USERDATA, 0);
        return TRUE;
    }
    return handleDialogAppearance(hDlg, d, msg, wp, lp);
}
