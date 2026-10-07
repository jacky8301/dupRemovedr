#include "md5.h"
#include <windows.h>
#include <cstring>
#include <memory>

// ---- MD5 core (RFC 1321) ------------------------------------------------

namespace {

constexpr uint32_t S11 = 7,  S12 = 12, S13 = 17, S14 = 22;
constexpr uint32_t S21 = 5,  S22 = 9,  S23 = 14, S24 = 20;
constexpr uint32_t S31 = 4,  S32 = 11, S33 = 16, S34 = 23;
constexpr uint32_t S41 = 6,  S42 = 10, S43 = 15, S44 = 21;

inline uint32_t F(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (~x & z); }
inline uint32_t G(uint32_t x, uint32_t y, uint32_t z) { return (x & z) | (y & ~z); }
inline uint32_t H(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
inline uint32_t I(uint32_t x, uint32_t y, uint32_t z) { return y ^ (x | ~z); }

inline uint32_t rotateLeft(uint32_t x, uint32_t n) {
    return (x << n) | (x >> (32 - n));
}

inline void FF(uint32_t& a, uint32_t b, uint32_t c, uint32_t d,
               uint32_t x, uint32_t s, uint32_t ac) {
    a += F(b, c, d) + x + ac;
    a = rotateLeft(a, s);
    a += b;
}

inline void GG(uint32_t& a, uint32_t b, uint32_t c, uint32_t d,
               uint32_t x, uint32_t s, uint32_t ac) {
    a += G(b, c, d) + x + ac;
    a = rotateLeft(a, s);
    a += b;
}

inline void HH(uint32_t& a, uint32_t b, uint32_t c, uint32_t d,
               uint32_t x, uint32_t s, uint32_t ac) {
    a += H(b, c, d) + x + ac;
    a = rotateLeft(a, s);
    a += b;
}

inline void II(uint32_t& a, uint32_t b, uint32_t c, uint32_t d,
               uint32_t x, uint32_t s, uint32_t ac) {
    a += I(b, c, d) + x + ac;
    a = rotateLeft(a, s);
    a += b;
}

} // anonymous namespace

void MD5::reset() {
    m_state[0] = 0x67452301;
    m_state[1] = 0xefcdab89;
    m_state[2] = 0x98badcfe;
    m_state[3] = 0x10325476;
    m_count = 0;
}

void MD5::update(const void* data, size_t length) {
    auto* input = static_cast<const uint8_t*>(data);
    size_t index = static_cast<size_t>(m_count / 8) % 64;
    m_count += static_cast<uint64_t>(length) * 8;

    size_t partLen = 64 - index;
    size_t i = 0;
    if (length >= partLen) {
        std::memcpy(m_buffer + index, input, partLen);
        transform(m_buffer);
        for (i = partLen; i + 63 < length; i += 64) {
            transform(input + i);
        }
        index = 0;
    }
    std::memcpy(m_buffer + index, input + i, length - i);
}

void MD5::transform(const uint8_t block[64]) {
    auto* x = reinterpret_cast<const uint32_t*>(block);
    uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];

    // Round 1
    FF(a, b, c, d, x[ 0], S11, 0xd76aa478); FF(d, a, b, c, x[ 1], S12, 0xe8c7b756);
    FF(c, d, a, b, x[ 2], S13, 0x242070db); FF(b, c, d, a, x[ 3], S14, 0xc1bdceee);
    FF(a, b, c, d, x[ 4], S11, 0xf57c0faf); FF(d, a, b, c, x[ 5], S12, 0x4787c62a);
    FF(c, d, a, b, x[ 6], S13, 0xa8304613); FF(b, c, d, a, x[ 7], S14, 0xfd469501);
    FF(a, b, c, d, x[ 8], S11, 0x698098d8); FF(d, a, b, c, x[ 9], S12, 0x8b44f7af);
    FF(c, d, a, b, x[10], S13, 0xffff5bb1); FF(b, c, d, a, x[11], S14, 0x895cd7be);
    FF(a, b, c, d, x[12], S11, 0x6b901122); FF(d, a, b, c, x[13], S12, 0xfd987193);
    FF(c, d, a, b, x[14], S13, 0xa679438e); FF(b, c, d, a, x[15], S14, 0x49b40821);

    // Round 2
    GG(a, b, c, d, x[ 1], S21, 0xf61e2562); GG(d, a, b, c, x[ 6], S22, 0xc040b340);
    GG(c, d, a, b, x[11], S23, 0x265e5a51); GG(b, c, d, a, x[ 0], S24, 0xe9b6c7aa);
    GG(a, b, c, d, x[ 5], S21, 0xd62f105d); GG(d, a, b, c, x[10], S22, 0x02441453);
    GG(c, d, a, b, x[15], S23, 0xd8a1e681); GG(b, c, d, a, x[ 4], S24, 0xe7d3fbc8);
    GG(a, b, c, d, x[ 9], S21, 0x21e1cde6); GG(d, a, b, c, x[14], S22, 0xc33707d6);
    GG(c, d, a, b, x[ 3], S23, 0xf4d50d87); GG(b, c, d, a, x[ 8], S24, 0x455a14ed);
    GG(a, b, c, d, x[13], S21, 0xa9e3e905); GG(d, a, b, c, x[ 2], S22, 0xfcefa3f8);
    GG(c, d, a, b, x[ 7], S23, 0x676f02d9); GG(b, c, d, a, x[12], S24, 0x8d2a4c8a);

    // Round 3
    HH(a, b, c, d, x[ 5], S31, 0xfffa3942); HH(d, a, b, c, x[ 8], S32, 0x8771f681);
    HH(c, d, a, b, x[11], S33, 0x6d9d6122); HH(b, c, d, a, x[14], S34, 0xfde5380c);
    HH(a, b, c, d, x[ 1], S31, 0xa4beea44); HH(d, a, b, c, x[ 4], S32, 0x4bdecfa9);
    HH(c, d, a, b, x[ 7], S33, 0xf6bb4b60); HH(b, c, d, a, x[10], S34, 0xbebfbc70);
    HH(a, b, c, d, x[13], S31, 0x289b7ec6); HH(d, a, b, c, x[ 0], S32, 0xeaa127fa);
    HH(c, d, a, b, x[ 3], S33, 0xd4ef3085); HH(b, c, d, a, x[ 6], S34, 0x04881d05);
    HH(a, b, c, d, x[ 9], S31, 0xd9d4d039); HH(d, a, b, c, x[12], S32, 0xe6db99e5);
    HH(c, d, a, b, x[15], S33, 0x1fa27cf8); HH(b, c, d, a, x[ 2], S34, 0xc4ac5665);

    // Round 4
    II(a, b, c, d, x[ 0], S41, 0xf4292244); II(d, a, b, c, x[ 7], S42, 0x432aff97);
    II(c, d, a, b, x[14], S43, 0xab9423a7); II(b, c, d, a, x[ 5], S44, 0xfc93a039);
    II(a, b, c, d, x[12], S41, 0x655b59c3); II(d, a, b, c, x[ 3], S42, 0x8f0ccc92);
    II(c, d, a, b, x[10], S43, 0xffeff47d); II(b, c, d, a, x[ 1], S44, 0x85845dd1);
    II(a, b, c, d, x[ 8], S41, 0x6fa87e4f); II(d, a, b, c, x[15], S42, 0xfe2ce6e0);
    II(c, d, a, b, x[ 6], S43, 0xa3014314); II(b, c, d, a, x[13], S44, 0x4e0811a1);
    II(a, b, c, d, x[ 4], S41, 0xf7537e82); II(d, a, b, c, x[11], S42, 0xbd3af235);
    II(c, d, a, b, x[ 2], S43, 0x2ad7d2bb); II(b, c, d, a, x[ 9], S44, 0xeb86d391);

    m_state[0] += a; m_state[1] += b;
    m_state[2] += c; m_state[3] += d;
}

MD5::Digest MD5::finalize() {
    // Save original message length (before padding corrupts m_count)
    uint64_t originalBitCount = m_count;

    size_t index = static_cast<size_t>(m_count / 8) % 64;
    size_t padLen = (index < 56) ? (56 - index) : (120 - index);

    uint8_t pad[64];
    pad[0] = 0x80;
    for (size_t i = 1; i < padLen; i++) pad[i] = 0;
    update(pad, padLen);

    // Append original message length (64-bit, little-endian)
    uint8_t lenBytes[8];
    for (int i = 0; i < 8; i++) {
        lenBytes[i] = static_cast<uint8_t>(originalBitCount >> (i * 8));
    }
    update(lenBytes, 8);

    Digest digest;
    for (int i = 0; i < 4; i++) {
        digest[i * 4 + 0] = static_cast<uint8_t>(m_state[i] & 0xFF);
        digest[i * 4 + 1] = static_cast<uint8_t>((m_state[i] >> 8) & 0xFF);
        digest[i * 4 + 2] = static_cast<uint8_t>((m_state[i] >> 16) & 0xFF);
        digest[i * 4 + 3] = static_cast<uint8_t>((m_state[i] >> 24) & 0xFF);
    }
    return digest;
}

std::string MD5::toHex(const Digest& digest) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result(32, '0');
    for (size_t i = 0; i < digest.size(); ++i) {
        result[i * 2] = hex[digest[i] >> 4];
        result[i * 2 + 1] = hex[digest[i] & 15];
    }
    return result;
}

namespace {

// 文件句柄由作用域管理，提前取消和读取失败时也会关闭。
struct ReadHandle {
    HANDLE value;
    ~ReadHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

bool cancelled(const std::function<bool()>& check) {
    return check && check();
}

bool unchanged(HANDLE file, const BY_HANDLE_FILE_INFORMATION& before) {
    BY_HANDLE_FILE_INFORMATION after{};
    return GetFileInformationByHandle(file, &after) &&
           before.nFileSizeHigh == after.nFileSizeHigh &&
           before.nFileSizeLow == after.nFileSizeLow &&
           CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) == 0;
}

bool fileInfo(HANDLE file, uint64_t expectedSize, BY_HANDLE_FILE_INFORMATION& info) {
    if (!GetFileInformationByHandle(file, &info)) return false;
    const uint64_t size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) |
                          info.nFileSizeLow;
    return expectedSize == (std::numeric_limits<uint64_t>::max)() || size == expectedSize;
}

} // 匿名命名空间

std::string MD5::hashFile(const std::wstring& filepath,
                         const std::function<bool()>& isCancelled,
                         uint64_t expectedSize) {
    if (cancelled(isCancelled)) return {};
    ReadHandle file{CreateFileW(filepath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return {};
    return hashOpenFile(file.value, isCancelled, expectedSize);
}

std::string MD5::hashOpenFile(void* fileHandle,
                             const std::function<bool()>& isCancelled,
                             uint64_t expectedSize) {
    if (cancelled(isCancelled)) return {};
    HANDLE file = static_cast<HANDLE>(fileHandle);
    LARGE_INTEGER start{};
    if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) return {};
    BY_HANDLE_FILE_INFORMATION before{};
    if (!fileInfo(file, expectedSize, before)) return {};

    MD5 md5;
    // 每个扫描线程复用缓冲区，避免为每个文件重新分配和清零。
    constexpr DWORD bufferSize = 1024 * 1024;
    thread_local auto buffer = std::make_unique<uint8_t[]>(bufferSize);
    for (;;) {
        if (cancelled(isCancelled)) return {};
        DWORD bytesRead = 0;
        // 直接检查 ReadFile 的返回值，成功读取不依赖残留错误码。
        if (!ReadFile(file, buffer.get(), bufferSize, &bytesRead, nullptr)) return {};
        if (bytesRead == 0) break;
        md5.update(buffer.get(), bytesRead);
    }
    if (cancelled(isCancelled) || !unchanged(file, before)) return {};
    return toHex(md5.finalize());
}

std::string MD5::hashFileSample(const std::wstring& filepath, uint64_t expectedSize,
                               const std::function<bool()>& isCancelled) {
    if (expectedSize <= sampleBytes)
        return hashFile(filepath, isCancelled, expectedSize);
    if (cancelled(isCancelled)) return {};
    ReadHandle file{CreateFileW(filepath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_RANDOM_ACCESS, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION before{};
    if (!fileInfo(file.value, expectedSize, before)) return {};

    MD5 md5;
    constexpr DWORD blockSize = static_cast<DWORD>(sampleBytes / 3);
    uint8_t buffer[blockSize];
    const uint64_t offsets[] = {0, (expectedSize - blockSize) / 2, expectedSize - blockSize};
    for (uint64_t offset : offsets) {
        if (cancelled(isCancelled)) return {};
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        DWORD bytesRead = 0;
        if (!SetFilePointerEx(file.value, position, nullptr, FILE_BEGIN) ||
            !ReadFile(file.value, buffer, blockSize, &bytesRead, nullptr) ||
            bytesRead != blockSize) return {};
        md5.update(buffer, bytesRead);
    }
    if (cancelled(isCancelled) || !unchanged(file.value, before)) return {};
    return toHex(md5.finalize());
}
