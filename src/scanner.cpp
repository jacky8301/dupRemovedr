#include "scanner.h"
#include "md5.h"

#include <windows.h>
#include <unordered_map>
#include <algorithm>

// ---- Helpers ------------------------------------------------------------

static std::wstring buildFindPattern(const std::wstring& folder) {
    if (folder.empty()) return L"*";
    if (folder.back() == L'\\' || folder.back() == L'/')
        return folder + L"*";
    return folder + L"\\*";
}

static bool shouldCancel(const DuplicateScanner::CancelFn& isCancelled) {
    return isCancelled && isCancelled();
}

// ---- File Enumeration ---------------------------------------------------

std::vector<std::pair<std::wstring, uint64_t>>
DuplicateScanner::enumerateFiles(const std::wstring& folder,
                                  CancelFn isCancelled) {
    std::vector<std::pair<std::wstring, uint64_t>> result;
    std::vector<std::wstring> dirs;
    dirs.push_back(folder);

    while (!dirs.empty()) {
        if (shouldCancel(isCancelled)) break;

        std::wstring currentDir = dirs.back();
        dirs.pop_back();

        std::wstring pattern = buildFindPattern(currentDir);
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (hFind == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER)
            hFind = FindFirstFileW(pattern.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) continue;

        do {
            if (shouldCancel(isCancelled)) break;

            if (wcscmp(fd.cFileName, L".")  == 0 ||
                wcscmp(fd.cFileName, L"..") == 0)
                continue;

            std::wstring fullPath = currentDir;
            if (!fullPath.empty() && fullPath.back() != L'\\' && fullPath.back() != L'/')
                fullPath += L'\\';
            fullPath += fd.cFileName;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // 不递归目录联接和符号链接，避免循环及重复遍历。
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    dirs.push_back(fullPath);
            } else {
                ULARGE_INTEGER size;
                size.LowPart  = fd.nFileSizeLow;
                size.HighPart = fd.nFileSizeHigh;
                if (size.QuadPart == 0) continue; // skip empty files
                result.emplace_back(fullPath, size.QuadPart);
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    return result;
}

// ---- Hash-based Grouping ------------------------------------------------

std::vector<DuplicateGroup>
DuplicateScanner::groupByHash(
    const std::vector<std::pair<std::wstring, uint64_t>>& files,
    ProgressFn onProgress,
    CancelFn isCancelled,
    ScanResult& stats)
{
    std::unordered_map<uint64_t, std::vector<size_t>> sizes;
    for (size_t i = 0; i < files.size(); ++i) {
        if (shouldCancel(isCancelled)) return {};
        sizes[files[i].second].push_back(i);
    }
    const int total = static_cast<int>(files.size());
    int completed = 0;
    auto report = [&](const std::wstring& path) {
        if (onProgress) onProgress(++completed, total, path);
        else ++completed;
    };
    std::vector<DuplicateGroup> result;
    for (const auto& [size, indices] : sizes) {
        if (shouldCancel(isCancelled)) return {};
        if (indices.size() == 1) {
            // 大小唯一的文件无需打开，也不需要读取内容。
            report(files[indices.front()].first);
            continue;
        }
        std::unordered_map<std::string, std::vector<size_t>> samples;
        for (size_t index : indices) {
            if (shouldCancel(isCancelled)) return {};
            auto hash = MD5::hashFileSample(files[index].first, size, isCancelled);
            if (size <= MD5::sampleBytes) ++stats.hashedFiles;
            else ++stats.sampledFiles;
            if (hash.empty()) report(files[index].first);
            else samples[hash].push_back(index);
        }
        for (const auto& [sample, candidates] : samples) {
            if (shouldCancel(isCancelled)) return {};
            std::unordered_map<std::string, std::vector<std::wstring>> hashes;
            for (size_t index : candidates) {
                if (shouldCancel(isCancelled)) return {};
                // 小文件的采样已覆盖全文；大文件必须用完整 MD5 二次确认。
                const bool needsHash = size > MD5::sampleBytes && candidates.size() > 1;
                std::string hash;
                if (size <= MD5::sampleBytes) hash = sample;
                else if (needsHash) {
                    ++stats.hashedFiles;
                    hash = MD5::hashFile(files[index].first, isCancelled, size);
                }
                if (!hash.empty()) hashes[hash].push_back(files[index].first);
                report(files[index].first);
            }
            for (auto& [hash, paths] : hashes) {
                if (paths.size() < 2) continue;
                std::sort(paths.begin(), paths.end());
                result.push_back({hash, std::move(paths), size});
            }
        }
    }
    // 固定分组与保留文件的顺序，结果不受哈希表遍历顺序影响。
    std::sort(result.begin(), result.end(), [](const DuplicateGroup& a, const DuplicateGroup& b) {
        return a.files.front() < b.files.front();
    });
    return result;
}

// ---- Public API ---------------------------------------------------------

ScanResult DuplicateScanner::scan(const std::wstring& folder,
                                   ProgressFn onProgress,
                                   CancelFn isCancelled) {
    ScanResult result;
    auto files = enumerateFiles(folder, isCancelled);
    result.totalFiles = static_cast<int>(files.size());
    if (shouldCancel(isCancelled)) return result;

    result.groups = groupByHash(files, onProgress, isCancelled, result);
    if (shouldCancel(isCancelled)) {
        result.groups.clear();
        result.totalDup = 0;
        result.wastedBytes = 0;
        return result;
    }

    for (const auto& g : result.groups) {
        int dupCount = static_cast<int>(g.files.size()) - 1;
        result.totalDup   += dupCount;
        result.wastedBytes += g.fileSize * dupCount;
    }
    return result;
}
