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
};

/// Recursively scans a folder, computes MD5 for each file,
/// and groups files by hash to identify duplicates.
class DuplicateScanner {
public:
    /// Called periodically: (current, total, filePath)
    using ProgressFn = std::function<void(int, int, const std::wstring&)>;
    using CancelFn = std::function<bool()>;

    /// Scan a folder for duplicate files.
    ScanResult scan(const std::wstring& folder, ProgressFn onProgress = {},
                    CancelFn isCancelled = {});

private:
    std::vector<std::pair<std::wstring, uint64_t>>
    enumerateFiles(const std::wstring& folder, CancelFn isCancelled);

    std::vector<DuplicateGroup> groupByHash(
        const std::vector<std::pair<std::wstring, uint64_t>>& files,
        ProgressFn onProgress,
        CancelFn isCancelled);
};
