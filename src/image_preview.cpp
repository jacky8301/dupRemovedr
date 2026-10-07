#include "image_preview.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>

using Microsoft::WRL::ComPtr;

static PreviewImage decodePreview(const std::wstring& path) {
    PreviewImage result;
    auto extension = std::filesystem::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    const wchar_t* formats[] = {L".jpg", L".jpeg", L".jfif", L".png", L".bmp", L".dib",
        L".gif", L".tif", L".tiff", L".ico", L".webp", L".heic", L".heif", L".avif"};
    if (std::find(std::begin(formats), std::end(formats), extension) == std::end(formats)) {
        result.message = L"此文件不支持图片预览\n可点击“定位文件”查看";
        return result;
    }
    result.message = L"无法加载图片\n文件可能已移动、损坏或缺少解码器";
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(frame->GetSize(&result.originalWidth, &result.originalHeight))) return result;

    // 限制解码输入和显示位图尺寸，避免超大图片耗尽内存。
    if (!result.originalWidth || !result.originalHeight ||
        uint64_t(result.originalWidth) * result.originalHeight > 100000000ULL) {
        result.message = L"图片尺寸过大，暂不预览\n可点击“定位文件”查看";
        return result;
    }
    const double ratio = (std::min)(1.0, 1200.0 / (std::max)(result.originalWidth, result.originalHeight));
    const UINT width = (std::max)(1U, static_cast<UINT>(result.originalWidth * ratio));
    const UINT height = (std::max)(1U, static_cast<UINT>(result.originalHeight * ratio));
    if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeFant)) ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return result;
    std::vector<unsigned char> pixels(width * height * 4);
    if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data())))
        return result;
    // 将透明像素合成到预览背景，避免透明 PNG 显示为黑底。
    for (size_t i = 0; i < pixels.size(); i += 4) {
        const unsigned inverseAlpha = 255 - pixels[i + 3];
        pixels[i] = static_cast<unsigned char>(pixels[i] + 249 * inverseAlpha / 255);
        pixels[i + 1] = static_cast<unsigned char>(pixels[i + 1] + 245 * inverseAlpha / 255);
        pixels[i + 2] = static_cast<unsigned char>(pixels[i + 2] + 241 * inverseAlpha / 255);
        pixels[i + 3] = 255;
    }
    result.width = width;
    result.height = height;
    result.pixels = std::move(pixels);
    result.message.clear();
    return result;
}

ImagePreview::ImagePreview() : worker_(&ImagePreview::run, this) {}

ImagePreview::~ImagePreview() {
    stop();
    if (worker_.joinable()) worker_.join();
}

uint64_t ImagePreview::select(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    path_ = path;
    ++request_;
    ready_.reset();
    pending_ = !path.empty();
    changed_.notify_one();
    return request_;
}

std::unique_ptr<PreviewImage> ImagePreview::takeReady() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::move(ready_);
}

void ImagePreview::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    changed_.notify_one();
}

void ImagePreview::run() {
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        std::wstring path;
        uint64_t request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [this] { return stopping_ || pending_; });
            if (stopping_) break;
            path = path_;
            request = request_;
            pending_ = false;
        }
        auto result = std::make_unique<PreviewImage>();
        try {
            if (SUCCEEDED(comHr)) *result = decodePreview(path);
            else result->message = L"图片预览暂不可用";
        } catch (...) {
            result->message = L"图片加载失败";
        }
        result->request = request;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!stopping_ && request == request_) ready_ = std::move(result);
        }
    }
    if (SUCCEEDED(comHr)) CoUninitialize();
    exited_.store(true);
}
