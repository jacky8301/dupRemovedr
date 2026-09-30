#include "../src/scanner.h"

#include <windows.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

// 使用固定数据集比较扫描实现；此程序只在显式运行时生成测试文件。
int main(int argc, char* argv[]) {
    if (argc != 2) return 1;
    const auto root = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(root);
    for (int i = 0; i < 80; ++i) {
        const size_t size = i < 40 ? (2 * 1024 * 1024 + i * 1031)
                                  : 4 * 1024 * 1024;
        std::string data(size, static_cast<char>('A' + i % 23));
        if (i < 72) {
            data[0] = static_cast<char>(i);
            data[size / 2] = static_cast<char>(i);
        }
        // 最后八份内容相同，用于核对优化前后的查重结果。
        if (i >= 72) data.assign(size, 'Z');
        std::ofstream file(root / (std::to_string(i) + ".bin"), std::ios::binary);
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    for (int run = 0; run < 3; ++run) {
        DuplicateScanner scanner;
        const auto start = std::chrono::steady_clock::now();
        const auto result = scanner.scan(root.wstring());
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        std::printf("run=%d files=%d groups=%zu duplicates=%d sampled=%d full_hashes=%d milliseconds=%.2f\n",
                    run + 1, result.totalFiles, result.groups.size(),
                    result.totalDup, result.sampledFiles, result.hashedFiles, elapsed);
        if (result.totalFiles != 80 || result.groups.size() != 1 || result.totalDup != 7)
            return 2;
    }
    return 0;
}
