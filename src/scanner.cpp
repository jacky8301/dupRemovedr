#include "scanner.h"
#include "md5.h"

#include <windows.h>
#include <unordered_map>

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
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) continue;

        do {
            if (shouldCancel(isCancelled)) break;

            if (wcscmp(fd.cFileName, L".")  == 0 ||
                wcscmp(fd.cFileName, L"..") == 0)
                continue;

            std::wstring fullPath = currentDir;
            if (fullPath.back() != L'\\') fullPath += L'\\';
            fullPath += fd.cFileName;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
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
    CancelFn isCancelled)
{
    struct GroupInfo {
        std::vector<std::wstring> paths;
        uint64_t size = 0;
    };
    std::unordered_map<std::string, GroupInfo> map;

    int total = static_cast<int>(files.size());
    for (int i = 0; i < total; ++i) {
        if (shouldCancel(isCancelled)) break;

        const auto& [path, size] = files[i];

        if (onProgress) onProgress(i + 1, total, path);
        if (shouldCancel(isCancelled)) break;

        std::string hash = MD5::hashFile(path);
        if (hash.empty()) continue; // skip unreadable files

        auto& info = map[hash];
        info.paths.push_back(path);
        if (info.size == 0) info.size = size;
    }

    std::vector<DuplicateGroup> result;
    for (auto& [hash, info] : map) {
        if (info.paths.size() < 2) continue;
        DuplicateGroup g;
        g.md5      = hash;
        g.files    = std::move(info.paths);
        g.fileSize = info.size;
        result.push_back(std::move(g));
    }
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

    result.groups = groupByHash(files, onProgress, isCancelled);
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
