#pragma once

#include <cstdint>
#include <string>
#include <array>
#include <functional>
#include <limits>

/// Self-contained MD5 implementation (RFC 1321).
class MD5 {
public:
    using Digest = std::array<uint8_t, 16>;

    MD5() { reset(); }

    void reset();
    void update(const void* data, size_t length);
    Digest finalize();

    /// Convert digest to hex string (lowercase, 32 chars)
    static std::string toHex(const Digest& digest);

    /// 分块计算完整 MD5；读取失败、文件变化或取消时返回空字符串。
    static std::string hashFile(const std::wstring& filepath,
        const std::function<bool()>& isCancelled = {},
        uint64_t expectedSize = (std::numeric_limits<uint64_t>::max)());

    /// 采样头部、中部和尾部，仅用于筛选；小文件直接读取完整内容。
    static constexpr uint64_t sampleBytes = 12 * 1024;
    static std::string hashFileSample(const std::wstring& filepath,
        uint64_t expectedSize, const std::function<bool()>& isCancelled = {});

private:
    void transform(const uint8_t block[64]);

    uint32_t m_state[4];
    uint64_t m_count;
    uint8_t  m_buffer[64];
};
