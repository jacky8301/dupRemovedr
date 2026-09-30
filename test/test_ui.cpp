// 在隐藏窗口中验证真实控件布局，并导出界面供视觉检查。
#define wWinMain applicationEntry
#include "../src/main.cpp"
#undef wWinMain

#include <gdiplus.h>
#include <cstdio>

// 先运行 benchmark_scanner 准备数据，再在构建目录运行本测试。

static bool snapshot(HWND window, const wchar_t* filename) {
    auto* state = reinterpret_cast<DlgData*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    RECT client{};
    GetClientRect(window, &client);
    HDC screen = GetDC(window);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, client.right, client.bottom);
    HGDIOBJ old = SelectObject(memory, bitmap);
    paintDialog(window, *state, memory);
    bool fits = true;
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        RECT rect{};
        GetWindowRect(child, &rect);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&rect), 2);
        fits &= rect.left >= 0 && rect.top >= 0 && rect.right <= client.right &&
                rect.bottom <= client.bottom && rect.right > rect.left && rect.bottom > rect.top;
        int saved = SaveDC(memory);
        SetViewportOrgEx(memory, rect.left, rect.top, nullptr);
        IntersectClipRect(memory, 0, 0, rect.right - rect.left, rect.bottom - rect.top);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(memory),
                     PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
        RestoreDC(memory, saved);
    }
    GdiFlush();
    SelectObject(memory, old);
    Gdiplus::Bitmap image(bitmap, nullptr);
    const CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
    bool saved = image.Save(filename, &png, nullptr) == Gdiplus::Ok;
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(window, screen);
    return fits && saved;
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok) return 1;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    HWND window = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_MAIN_DLG),
                                    nullptr, DlgProc, 0);
    if (!window) return 2;
    bool ok = snapshot(window, L"ui-ready.png");
    auto* state = reinterpret_cast<DlgData*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    const auto root = std::filesystem::current_path() / L"benchmark-data";
    startScan(window, *state, root.wstring());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (state->scanning && std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(1);
    }
    ok &= !state->scanning && state->result.totalFiles == 80 && state->result.totalDup == 7;
    ok &= SendDlgItemMessageW(window, IDC_PROGRESS, PBM_GETPOS, 0, 0) == 1000;
    ok &= snapshot(window, L"ui-completed.png");
    SetWindowPos(window, nullptr, 0, 0, scaled(window, 860), scaled(window, 680), SWP_NOMOVE | SWP_NOZORDER);
    ok &= snapshot(window, L"ui-minimum.png");
    state->cancelled.store(true);
    if (state->worker.joinable()) state->worker.join();
    DestroyWindow(window);
    Gdiplus::GdiplusShutdown(token);
    CoUninitialize();
    std::printf("UI layout and scan summary: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
