#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct PreviewImage {
    uint64_t request = 0;
    unsigned width = 0, height = 0;
    unsigned originalWidth = 0, originalHeight = 0;
    std::vector<unsigned char> pixels;
    std::wstring message;
};

// 单个后台线程合并选择请求；只发布最新图片，不持有任何窗口或控件。
class ImagePreview {
public:
    ImagePreview();
    ~ImagePreview();
    uint64_t select(const std::wstring& path);
    std::unique_ptr<PreviewImage> takeReady();
    void stop();
    bool stopped() const { return exited_.load(); }

private:
    void run();
    std::mutex mutex_;
    std::condition_variable changed_;
    std::wstring path_;
    uint64_t request_ = 0;
    bool pending_ = false;
    bool stopping_ = false;
    std::unique_ptr<PreviewImage> ready_;
    std::atomic_bool exited_{false};
    std::thread worker_;
};
