#pragma once

#include <cstdint>
#include <string>
#include <array>

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

    /// Compute MD5 hex digest of a file. Returns empty string on failure.
    static std::string hashFile(const std::wstring& filepath);

private:
    void transform(const uint8_t block[64]);

    uint32_t m_state[4];
    uint64_t m_count;
    uint8_t  m_buffer[64];
};
