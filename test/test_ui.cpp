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
        if (!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) continue;
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

static bool waitForPreview(DlgData& state) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (state.preview.request != state.previewRequest && std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(1);
    }
    return state.preview.request == state.previewRequest;
}

static bool previewScenario(HWND window, DlgData& state) {
    // 生成几何测试图，便于肉眼核对宽高比、颜色和预览位置。
    const auto path = std::filesystem::current_path() / L"ui-preview-sample.png";
    const CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
    {
        Gdiplus::Bitmap bitmap(960, 640, PixelFormat32bppARGB);
        Gdiplus::Graphics graphics(&bitmap);
        graphics.Clear(Gdiplus::Color(255, 223, 237, 250));
        Gdiplus::SolidBrush sun(Gdiplus::Color(255, 252, 191, 73));
        graphics.FillEllipse(&sun, 650, 80, 160, 160);
        Gdiplus::SolidBrush back(Gdiplus::Color(255, 101, 153, 164));
        Gdiplus::Point hills[] = {{0, 480}, {310, 140}, {650, 510}, {800, 330}, {960, 470}, {960, 640}, {0, 640}};
        graphics.FillPolygon(&back, hills, static_cast<INT>(std::size(hills)));
        Gdiplus::SolidBrush front(Gdiplus::Color(255, 41, 94, 106));
        Gdiplus::Point foreground[] = {{0, 600}, {430, 330}, {790, 640}, {0, 640}};
        graphics.FillPolygon(&front, foreground, static_cast<INT>(std::size(foreground)));
        if (bitmap.Save(path.c_str(), &png, nullptr) != Gdiplus::Ok) return false;
    }
    selectPreview(window, state, -1);
    state.result = {};
    const auto bytes = std::filesystem::file_size(path);
    state.result.groups = {{"sample", {path.wstring(), (path.parent_path() / L"副本.png").wstring()}, bytes},
                           {"text", {L"说明.txt", L"说明-副本.txt"}, 1024}};
    state.result.totalFiles = 4;
    state.result.totalDup = 2;
    state.result.wastedBytes = bytes + 1024;
    populateListView(GetDlgItem(window, IDC_LIST), state);
    setSummary(window, 4, 2, bytes + 1024);
    SetDlgItemTextW(window, IDC_STATUS, L"扫描完成 · 4 个文件 · 2 组重复 · 选择文件查看预览");
    HWND list = GetDlgItem(window, IDC_LIST);
    ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    bool ok = waitForPreview(state) && state.preview.width == 960 && state.preview.height == 640;
    ok &= IsWindowEnabled(GetDlgItem(window, IDC_REVEAL_BTN)) != FALSE;
    ok &= snapshot(window, L"ui-preview.png");
    SetWindowPos(window, nullptr, 0, 0, scaled(window, 860), scaled(window, 680), SWP_NOMOVE | SWP_NOZORDER);
    ok &= snapshot(window, L"ui-preview-minimum.png");
    ListView_SetItemState(list, 1, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ok &= waitForPreview(state) && state.preview.pixels.empty() && !state.preview.message.empty();
    ListView_SetItemState(list, 3, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ok &= waitForPreview(state) && state.preview.message.find(L"不支持") != std::wstring::npos;
    state.items[3].removed = true;
    selectPreview(window, state, 3);
    ok &= state.preview.pixels.empty() && !IsWindowEnabled(GetDlgItem(window, IDC_REVEAL_BTN)) &&
          state.preview.message.find(L"回收站") != std::wstring::npos;
    selectPreview(window, state, -1);
    ok &= DeleteFileW(path.c_str()) != FALSE;
    return ok;
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
    ok &= previewScenario(window, *state);
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
