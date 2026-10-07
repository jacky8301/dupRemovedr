// 使用测试生成的图片验证实际解码、透明度、缩放和过期请求处理。
#include "image_preview.h"
#include <windows.h>
#include <gdiplus.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

static bool expect(bool condition, const char* message) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
    return condition;
}

static std::unique_ptr<PreviewImage> waitFor(ImagePreview& loader) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = loader.takeReady()) return result;
        Sleep(1);
    }
    return {};
}

int main() {
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok) return 2;
    const auto root = std::filesystem::current_path() /
        (L"preview-fixtures-" + std::to_wstring(GetCurrentProcessId()));
    if (!std::filesystem::create_directory(root)) return 2;
    bool ok = true;
    {
        UINT count = 0, size = 0;
        Gdiplus::GetImageEncodersSize(&count, &size);
        std::vector<unsigned char> buffer(size);
        auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
        Gdiplus::GetImageEncoders(count, size, encoders);
        ImagePreview loader;
        const struct { const wchar_t* mime; const wchar_t* name; } formats[] = {
            {L"image/png", L"透明图片.PNG"}, {L"image/jpeg", L"photo.jpg"},
            {L"image/bmp", L"photo.bmp"}, {L"image/gif", L"photo.gif"}, {L"image/tiff", L"photo.tiff"}
        };
        std::vector<std::filesystem::path> files;
        for (const auto& format : formats) {
            const CLSID* encoder = nullptr;
            for (UINT i = 0; i < count; ++i)
                if (wcscmp(encoders[i].MimeType, format.mime) == 0) encoder = &encoders[i].Clsid;
            if (!encoder) { ok = false; continue; }
            const auto path = root / format.name;
            {
                Gdiplus::Bitmap bitmap(3200, 1600, PixelFormat32bppARGB);
                Gdiplus::Graphics graphics(&bitmap);
                graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
                Gdiplus::SolidBrush brush(Gdiplus::Color(255, 30, 90, 160));
                graphics.FillRectangle(&brush, 0, 0, 1600, 1600);
                ok &= bitmap.Save(path.c_str(), encoder, nullptr) == Gdiplus::Ok;
            }
            files.push_back(path);
            const auto request = loader.select(path.wstring());
            auto image = waitFor(loader);
            ok &= expect(image && image->request == request && image->width == 1200 && image->height == 600 &&
                image->originalWidth == 3200 && image->originalHeight == 1600 && image->pixels.size() == 1200 * 600 * 4,
                "Real image decodes and scales to a bounded preview with its aspect ratio preserved");
            if (wcscmp(format.mime, L"image/png") == 0 && image && !image->pixels.empty()) {
                const auto index = (1190 + 300 * 1200) * 4;
                ok &= expect(image->pixels[index] == 249 && image->pixels[index + 1] == 245 &&
                             image->pixels[index + 2] == 241, "Transparent PNG is composited onto the preview background");
            }
        }
        const auto corrupt = root / L"broken.png";
        std::ofstream(corrupt) << "not an image";
        files.push_back(corrupt);
        loader.select(corrupt.wstring());
        auto broken = waitFor(loader);
        ok &= expect(broken && broken->pixels.empty() && !broken->message.empty(), "Corrupt images return an explanatory placeholder");
        loader.select((root / L"missing.jpg").wstring());
        auto missing = waitFor(loader);
        ok &= expect(missing && missing->pixels.empty() && !missing->message.empty(), "Missing images do not crash the worker");
        for (int i = 0; i < 100; ++i) loader.select(files.front().wstring());
        const auto latest = loader.select((root / L"document.txt").wstring());
        auto last = waitFor(loader);
        ok &= expect(last && last->request == latest && last->pixels.empty() &&
            last->message.find(L"不支持") != std::wstring::npos, "Rapid selection publishes only the latest request");
        loader.select(files.front().wstring());
        loader.select(L"");
        loader.stop();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!loader.stopped() && std::chrono::steady_clock::now() < deadline) Sleep(1);
        ok &= expect(loader.stopped() && !loader.takeReady(), "Clearing selection and closing discard outstanding previews");
        // 仅删除本测试在唯一目录中创建的文件，同时检查解码器已释放文件句柄。
        for (const auto& path : files) ok &= expect(DeleteFileW(path.c_str()) != FALSE, "Decoder releases its source file");
    }
    std::filesystem::remove(root);
    Gdiplus::GdiplusShutdown(token);
    return ok ? 0 : 1;
}
