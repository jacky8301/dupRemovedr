// 程序入口负责进程初始化与主对话框启动。
#include "dialog.h"

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
