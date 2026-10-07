#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

/// A group of files that share the same MD5 hash.
struct DuplicateGroup {
    std::string               md5;
    std::vector<std::wstring> files;
    uint64_t                  fileSize = 0;
};

/// Complete result of a duplicate scan.
struct ScanResult {
    std::vector<DuplicateGroup> groups;
    int      totalFiles  = 0;
    int      totalDup    = 0;   // number of duplicate files (excl. 1 kept per group)
    uint64_t wastedBytes = 0;   // space taken by duplicates
    int      sampledFiles = 0; // 执行采样的文件数量
    int      hashedFiles = 0;  // 执行完整 MD5 的文件数量（包括直接完整读取的小文件）
    double   enumerateMilliseconds = 0; // 目录枚举的耗时
    double   sampleMilliseconds = 0;    // 采样及小文件完整读取的耗时
    double   hashMilliseconds = 0;      // 大文件完整校验的耗时
    unsigned hashWorkers = 1;           // 本次完整校验实际使用的最大并发数
};

/// 递归枚举目录，按大小与采样筛选，再用完整 MD5 确认重复文件。
class DuplicateScanner {
public:
    /// 在调用 scan 的线程上依次报告进度：(已完成数、总数、文件路径)。
    using ProgressFn = std::function<void(int, int, const std::wstring&)>;
    using CancelFn = std::function<bool()>;

    /// maxHashWorkers 为 0 时自动检测磁盘，为 1 时串行，其他值最多使用 4 个线程。
    /// 进度与取消回调均在调用线程执行，不要求回调自身支持并发。
    ScanResult scan(const std::wstring& folder, ProgressFn onProgress = {},
                    CancelFn isCancelled = {}, unsigned maxHashWorkers = 0);

private:
    std::vector<std::pair<std::wstring, uint64_t>>
    enumerateFiles(const std::wstring& folder, CancelFn isCancelled);

    std::vector<DuplicateGroup> groupByHash(
        const std::vector<std::pair<std::wstring, uint64_t>>& files,
        ProgressFn onProgress,
        CancelFn isCancelled,
        ScanResult& stats, unsigned maxHashWorkers);
};
