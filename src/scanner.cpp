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
#include <filesystem>

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

using HashFiles = std::vector<std::pair<std::wstring, uint64_t>>;
using HashConsumer = std::function<void(size_t, const std::string&)>;

static void hashSerial(const HashFiles& files, const std::vector<size_t>& candidates,
                       uint64_t size, const DuplicateScanner::CancelFn& isCancelled,
                       const HashConsumer& consume, ScanResult& stats) {
    for (size_t index : candidates) {
        if (shouldCancel(isCancelled)) return;
        ++stats.hashedFiles;
        consume(index, MD5::hashFile(files[index].first, isCancelled, size));
    }
}

// 工作线程只发布摘要；取消检查、用户回调和统计仍在调用线程执行。
class ParallelHashBatch {
public:
    ParallelHashBatch(const HashFiles& files, const std::vector<size_t>& candidates, uint64_t size)
        : files_(files), candidates_(candidates), size_(size),
          digests_(candidates.size()), ready_(candidates.size(), false) {}

    ~ParallelHashBatch() { join(); }

    void start(unsigned workers) {
        threads_.reserve(workers);
        for (unsigned worker = 0; worker < workers; ++worker)
            threads_.emplace_back([this] { hashNext(); });
    }

    void consume(const DuplicateScanner::CancelFn& isCancelled, const HashConsumer& consumer) {
        for (size_t job = 0; job < candidates_.size(); ++job) {
            if (shouldCancel(isCancelled)) return;
            std::unique_lock<std::mutex> lock(mutex_);
            while (!ready_[job] && !failure_) {
                completed_.wait_for(lock, std::chrono::milliseconds(10));
                lock.unlock();
                if (shouldCancel(isCancelled)) return;
                lock.lock();
            }
            if (failure_) std::rethrow_exception(failure_);
            lock.unlock();
            consumer(candidates_[job], digests_[job]);
        }
    }

    int join() {
        stop_.store(true);
        for (auto& thread : threads_) if (thread.joinable()) thread.join();
        return started_.load();
    }

private:
    void hashNext() {
        try {
            while (!stop_.load()) {
                const size_t job = next_.fetch_add(1);
                if (job >= candidates_.size()) break;
                ++started_;
                auto digest = MD5::hashFile(files_[candidates_[job]].first,
                    [this] { return stop_.load(); }, size_);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    digests_[job] = std::move(digest);
                    ready_[job] = true;
                }
                completed_.notify_one();
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!failure_) failure_ = std::current_exception();
            }
            stop_.store(true);
            completed_.notify_one();
        }
    }

    const HashFiles& files_;
    const std::vector<size_t>& candidates_;
    uint64_t size_;
    std::vector<std::string> digests_;
    std::vector<bool> ready_;
    std::vector<std::thread> threads_;
    std::atomic<size_t> next_{0};
    std::atomic<int> started_{0};
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::condition_variable completed_;
    std::exception_ptr failure_;
};

static void hashCandidates(const HashFiles& files, const std::vector<size_t>& candidates,
    uint64_t size, unsigned workerLimit, const DuplicateScanner::CancelFn& isCancelled,
    const HashConsumer& consume, ScanResult& stats) {
    unsigned workers = static_cast<unsigned>((std::min)(
        static_cast<size_t>(workerLimit), candidates.size()));
    // 小批次的线程创建开销可能超过收益，至少有 8 MiB 待校验内容才并行。
    constexpr uint64_t parallelBytes = 8 * 1024 * 1024;
    if (size < 256 * 1024 || size < (parallelBytes + candidates.size() - 1) / candidates.size())
        workers = 1;
    stats.hashWorkers = (std::max)(stats.hashWorkers, workers);
    if (workers <= 1) {
        hashSerial(files, candidates, size, isCancelled, consume, stats);
        return;
    }
    ParallelHashBatch batch(files, candidates, size);
    try {
        batch.start(workers);
        batch.consume(isCancelled, consume);
    } catch (...) {
        // 创建线程或用户回调抛出异常时，也要先等待所有读取结束并记录统计。
        stats.hashedFiles += batch.join();
        throw;
    }
    stats.hashedFiles += batch.join();
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

        const DWORD attributes = GetFileAttributesW(currentDir.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) throw ScanError(currentDir, GetLastError());
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) throw ScanError(currentDir, ERROR_DIRECTORY);

        std::wstring pattern = buildFindPattern(currentDir);
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (hFind == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER)
            hFind = FindFirstFileW(pattern.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            // 有效的空目录可能没有任何匹配项，其他错误必须向调用方报告。
            if (error == ERROR_FILE_NOT_FOUND) continue;
            throw ScanError(currentDir, error);
        }
        struct FindHandle {
            HANDLE value;
            ~FindHandle() { FindClose(value); }
        } findHandle{hFind};

        do {
            if (shouldCancel(isCancelled)) return result;

            if (wcscmp(fd.cFileName, L".")  == 0 ||
                wcscmp(fd.cFileName, L"..") == 0)
                continue;

            std::wstring fullPath = currentDir;
            if (!fullPath.empty() && fullPath.back() != L'\\' && fullPath.back() != L'/')
                fullPath += L'\\';
            fullPath += fd.cFileName;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // 失败或中断后留下的暂存副本需要人工恢复，不能再次当作重复文件清理。
                if (std::wstring(fd.cFileName).find(L".dupRemover-staging-") == 0) continue;
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
        const DWORD error = GetLastError();
        if (error != ERROR_NO_MORE_FILES) throw ScanError(currentDir, error);
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
    if (shouldCancel(isCancelled)) return result;
    if (folder.empty()) throw ScanError(folder, ERROR_INVALID_NAME);
    std::error_code error;
    const auto absoluteFolder = std::filesystem::absolute(folder, error).lexically_normal().wstring();
    if (error) throw ScanError(folder, static_cast<uint32_t>(error.value()));
    const auto enumerateStart = ScanClock::now();
    auto files = enumerateFiles(absoluteFolder, isCancelled);
    result.enumerateMilliseconds = elapsedMilliseconds(enumerateStart);
    result.totalFiles = static_cast<int>(files.size());
    if (shouldCancel(isCancelled)) return result;

    result.groups = groupByHash(files, onProgress, isCancelled, result,
        files.empty() ? 1 : hashWorkerLimit(absoluteFolder, maxHashWorkers));
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
