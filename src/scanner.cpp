#include "scanner.h"
#include "md5.h"

#include <windows.h>
#include <winioctl.h>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

using ScanClock = std::chrono::steady_clock;

static double elapsedMilliseconds(ScanClock::time_point start) {
    return std::chrono::duration<double, std::milli>(ScanClock::now() - start).count();
}

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

// 只有明确没有寻道惩罚的本地磁盘才自动并发，查询失败时保守地串行读取。
static unsigned hashWorkerLimit(const std::wstring& folder, unsigned requested) {
    const unsigned cpuCount = (std::max)(1u, std::thread::hardware_concurrency());
    const unsigned limit = (std::min)(4u, cpuCount);
    if (requested != 0) return (std::min)(requested, limit);
    wchar_t volumePath[MAX_PATH]{};
    wchar_t volumeName[MAX_PATH]{};
    if (!GetVolumePathNameW(folder.c_str(), volumePath, MAX_PATH) ||
        GetDriveTypeW(volumePath) != DRIVE_FIXED ||
        !GetVolumeNameForVolumeMountPointW(volumePath, volumeName, MAX_PATH))
        return 1;
    const size_t length = wcslen(volumeName);
    if (length == 0) return 1;
    if (volumeName[length - 1] == L'\\') volumeName[length - 1] = L'\0';
    HANDLE volume = CreateFileW(volumeName, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (volume == INVALID_HANDLE_VALUE) return 1;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor{};
    DWORD returned = 0;
    const BOOL success = DeviceIoControl(volume, IOCTL_STORAGE_QUERY_PROPERTY,
        &query, sizeof(query), &descriptor, sizeof(descriptor), &returned, nullptr);
    CloseHandle(volume);
    return success && returned >= sizeof(descriptor) && !descriptor.IncursSeekPenalty
        ? limit : 1;
}

// 工作线程只读文件；回调、分组和统计均由调用线程完成。
static void hashCandidates(
    const std::vector<std::pair<std::wstring, uint64_t>>& files,
    const std::vector<size_t>& candidates, uint64_t size, unsigned workerLimit,
    const DuplicateScanner::CancelFn& isCancelled,
    const std::function<void(size_t, const std::string&)>& consume,
    ScanResult& stats) {
    unsigned workers = static_cast<unsigned>((std::min)(
        static_cast<size_t>(workerLimit), candidates.size()));
    // 小批次的线程创建开销可能超过收益，至少有 8 MiB 待校验内容才并行。
    constexpr uint64_t parallelBytes = 8 * 1024 * 1024;
    if (size < 256 * 1024 || size < (parallelBytes + candidates.size() - 1) / candidates.size())
        workers = 1;
    stats.hashWorkers = (std::max)(stats.hashWorkers, workers);
    if (workers <= 1) {
        for (size_t index : candidates) {
            if (shouldCancel(isCancelled)) return;
            ++stats.hashedFiles;
            consume(index, MD5::hashFile(files[index].first, isCancelled, size));
        }
        return;
    }

    std::vector<std::string> digests(candidates.size());
    std::vector<bool> ready(candidates.size(), false);
    std::vector<std::thread> threads;
    std::atomic<size_t> next{0};
    std::atomic<int> started{0};
    std::atomic<bool> stop{false};
    std::mutex mutex;
    std::condition_variable completed;
    std::exception_ptr failure;
    auto join = [&] {
        stop.store(true);
        for (auto& thread : threads) if (thread.joinable()) thread.join();
        stats.hashedFiles += started.load();
    };
    try {
        threads.reserve(workers);
        for (unsigned worker = 0; worker < workers; ++worker) {
            threads.emplace_back([&] {
                try {
                    while (!stop.load()) {
                        const size_t job = next.fetch_add(1);
                        if (job >= candidates.size()) break;
                        ++started;
                        auto digest = MD5::hashFile(files[candidates[job]].first,
                            [&] { return stop.load(); }, size);
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            digests[job] = std::move(digest);
                            ready[job] = true;
                        }
                        completed.notify_one();
                    }
                } catch (...) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (!failure) failure = std::current_exception();
                    }
                    stop.store(true);
                    completed.notify_one();
                }
            });
        }
        for (size_t job = 0; job < candidates.size(); ++job) {
            if (shouldCancel(isCancelled)) break;
            std::unique_lock<std::mutex> lock(mutex);
            while (!ready[job] && !failure) {
                completed.wait_for(lock, std::chrono::milliseconds(10));
                lock.unlock();
                if (shouldCancel(isCancelled)) { join(); return; }
                lock.lock();
            }
            if (failure) std::rethrow_exception(failure);
            lock.unlock();
            consume(candidates[job], digests[job]);
        }
    } catch (...) {
        // 创建线程或用户回调抛出异常时，也要先等待所有读取结束。
        join();
        throw;
    }
    join();
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
    ScanResult& stats, unsigned maxHashWorkers)
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
            const auto sampleStart = ScanClock::now();
            auto hash = MD5::hashFileSample(files[index].first, size, isCancelled);
            stats.sampleMilliseconds += elapsedMilliseconds(sampleStart);
            if (size <= MD5::sampleBytes) ++stats.hashedFiles;
            else ++stats.sampledFiles;
            if (hash.empty()) report(files[index].first);
            else samples[hash].push_back(index);
        }
        for (const auto& [sample, candidates] : samples) {
            if (shouldCancel(isCancelled)) return {};
            std::unordered_map<std::string, std::vector<std::wstring>> hashes;
            auto consume = [&](size_t index, const std::string& hash) {
                if (!hash.empty()) hashes[hash].push_back(files[index].first);
                report(files[index].first);
            };
            // 小文件已完整读取；只有采样相同的大文件才进入完整校验。
            if (size > MD5::sampleBytes && candidates.size() > 1) {
                const auto hashStart = ScanClock::now();
                hashCandidates(files, candidates, size, maxHashWorkers, isCancelled, consume, stats);
                stats.hashMilliseconds += elapsedMilliseconds(hashStart);
            } else {
                for (size_t index : candidates) {
                    if (shouldCancel(isCancelled)) return {};
                    consume(index, size <= MD5::sampleBytes ? sample : std::string{});
                }
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
                                   CancelFn isCancelled, unsigned maxHashWorkers) {
    ScanResult result;
    const auto enumerateStart = ScanClock::now();
    auto files = enumerateFiles(folder, isCancelled);
    result.enumerateMilliseconds = elapsedMilliseconds(enumerateStart);
    result.totalFiles = static_cast<int>(files.size());
    if (shouldCancel(isCancelled)) return result;

    result.groups = groupByHash(files, onProgress, isCancelled, result,
        files.empty() ? 1 : hashWorkerLimit(folder, maxHashWorkers));
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
