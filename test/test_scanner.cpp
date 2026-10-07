#include "../src/scanner.h"
#include "../src/md5.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <atomic>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;

    TempDir() {
        wchar_t buffer[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, buffer);
        path = fs::path(buffer) /
               (L"dupRemoverScannerTest-" + std::to_wstring(GetCurrentProcessId()));
        fs::remove_all(path);
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void writeFile(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

bool expect(bool condition, const char* message) {
    if (condition) {
        std::printf("PASS: %s\n", message);
        return true;
    }

    std::printf("FAIL: %s\n", message);
    return false;
}

const DuplicateGroup* findGroupBySize(const ScanResult& result, uint64_t size,
                                      size_t fileCount) {
    auto it = std::find_if(result.groups.begin(), result.groups.end(),
        [size, fileCount](const DuplicateGroup& group) {
            return group.fileSize == size && group.files.size() == fileCount;
        });
    return it == result.groups.end() ? nullptr : &*it;
}

bool containsPath(const DuplicateGroup& group, const fs::path& path) {
    const std::wstring expected = fs::absolute(path).wstring();
    return std::any_of(group.files.begin(), group.files.end(),
        [&expected](const std::wstring& actual) {
            return fs::absolute(actual).wstring() == expected;
        });
}

bool testDetectsDuplicatesAndTotals() {
    TempDir temp;

    const fs::path copyA1 = temp.path / L"copy-a-1.bin";
    const fs::path copyA2 = temp.path / L"nested" / L"copy-a-2.bin";
    const fs::path copyB1 = temp.path / L"copy-b-1.bin";
    const fs::path copyB2 = temp.path / L"nested" / L"copy-b-2.bin";
    const fs::path copyB3 = temp.path / L"nested" / L"deeper" / L"copy-b-3.bin";

    writeFile(copyA1, "same-content");
    writeFile(copyA2, "same-content");
    writeFile(copyB1, "other-duplicate-content");
    writeFile(copyB2, "other-duplicate-content");
    writeFile(copyB3, "other-duplicate-content");
    writeFile(temp.path / L"unique.txt", "unique-content");
    writeFile(temp.path / L"empty-a.txt", "");
    writeFile(temp.path / L"nested" / L"empty-b.txt", "");

    int progressCalls = 0;
    DuplicateScanner scanner;
    ScanResult result = scanner.scan(temp.path.wstring(),
        [&progressCalls](int current, int total, const std::wstring&) {
            ++progressCalls;
            expect(current >= 1 && current <= total, "progress stays in range");
        });

    bool ok = true;
    ok &= expect(result.totalFiles == 6, "empty files are ignored in totalFiles");
    ok &= expect(result.groups.size() == 2, "two duplicate groups are found");
    ok &= expect(result.totalDup == 3, "totalDup counts all but one file per group");
    ok &= expect(result.wastedBytes ==
                 std::string("same-content").size() +
                 std::string("other-duplicate-content").size() * 2,
                 "wastedBytes sums duplicate payload sizes");
    ok &= expect(progressCalls == result.totalFiles,
                 "progress is reported for each scanned file");

    const DuplicateGroup* groupA =
        findGroupBySize(result, std::string("same-content").size(), 2);
    const DuplicateGroup* groupB =
        findGroupBySize(result, std::string("other-duplicate-content").size(), 3);

    ok &= expect(groupA != nullptr, "two-file duplicate group is present");
    ok &= expect(groupB != nullptr, "three-file duplicate group is present");

    if (groupA) {
        ok &= expect(containsPath(*groupA, copyA1), "group A contains first copy");
        ok &= expect(containsPath(*groupA, copyA2), "group A contains nested copy");
    }

    if (groupB) {
        ok &= expect(containsPath(*groupB, copyB1), "group B contains first copy");
        ok &= expect(containsPath(*groupB, copyB2), "group B contains nested copy");
        ok &= expect(containsPath(*groupB, copyB3), "group B contains deep copy");
    }

    return ok;
}

bool testCancellationBeforeHashing() {
    TempDir temp;
    writeFile(temp.path / L"a.bin", "same");
    writeFile(temp.path / L"b.bin", "same");

    DuplicateScanner scanner;
    ScanResult result = scanner.scan(temp.path.wstring(), {}, []() { return true; });

    bool ok = true;
    ok &= expect(result.totalFiles == 0, "immediate cancellation stops enumeration");
    ok &= expect(result.groups.empty(), "immediate cancellation returns no groups");
    ok &= expect(result.totalDup == 0, "immediate cancellation returns no duplicates");
    ok &= expect(result.wastedBytes == 0, "immediate cancellation returns no waste");
    return ok;
}

bool testSizeAndSampleFiltering() {
    TempDir temp;
    writeFile(temp.path / L"unique-size.bin", std::string(70001, 'U'));
    writeFile(temp.path / L"sample-a.bin", std::string(65536, 'A'));
    writeFile(temp.path / L"sample-b.bin", std::string(65536, 'B'));
    std::string contents(65536, 'C');
    writeFile(temp.path / L"original.bin", contents);
    writeFile(temp.path / L"中文目录" / L"副本.bin", contents);
    // 修改采样区以外的字节，必须通过全文 MD5 排除这个文件。
    contents[8192] = 'D';
    writeFile(temp.path / L"sample-collision.bin", contents);

    bool progressOk = true;
    int previous = 0;
    auto result = DuplicateScanner().scan(temp.path.wstring(),
        [&](int current, int total, const std::wstring&) {
            progressOk &= current == previous + 1 && total == 6;
            previous = current;
        });
    bool ok = expect(result.totalFiles == 6, "all nonempty files remain in the total");
    ok &= expect(result.groups.size() == 1 && result.totalDup == 1,
                 "matching samples alone cannot produce a duplicate");
    ok &= expect(result.sampledFiles == 5 && result.hashedFiles == 3,
                 "unique sizes and unique samples skip full file hashing");
    ok &= expect(progressOk && previous == 6, "filtered files have monotonic complete progress");
    if (!result.groups.empty()) {
        ok &= expect(containsPath(result.groups[0], temp.path / L"original.bin") &&
                     containsPath(result.groups[0], temp.path / L"中文目录" / L"副本.bin"),
                     "Unicode copies are found while the sample collision is excluded");
        ok &= expect(result.groups[0].md5 == MD5::hashFile((temp.path / L"original.bin").wstring()),
                     "reported hashes are full MD5 digests");
    }
    auto again = DuplicateScanner().scan(temp.path.wstring());
    ok &= expect(!result.groups.empty() && !again.groups.empty() &&
                 result.groups[0].files == again.groups[0].files,
                 "kept file order is stable across repeated scans");
    return ok;
}

bool testSampleBoundaries() {
    TempDir temp;
    const size_t sizes[] = {1, 4096, 12287, 12288, 12289, 1024 * 1024 + 17};
    for (size_t size : sizes) {
        const auto prefix = std::to_wstring(size);
        std::string payload(size, 'X');
        writeFile(temp.path / (prefix + L"-a.bin"), payload);
        writeFile(temp.path / (prefix + L"-b.bin"), payload);
        payload[size / 3] = 'Y';
        writeFile(temp.path / (prefix + L"-different.bin"), payload);
    }
    const auto result = DuplicateScanner().scan(temp.path.wstring());
    return expect(result.groups.size() == 6 && result.totalDup == 6,
                  "small-file and sampling-boundary duplicates are correct") &&
           expect(result.totalFiles == 18, "all boundary files are counted");
}

bool testReadFailuresAndCancellation() {
    TempDir temp;
    const auto a = temp.path / L"a.bin";
    const auto b = temp.path / L"b.bin";
    writeFile(a, "abc");
    writeFile(b, "abc");
    SetLastError(ERROR_ACCESS_DENIED);
    bool ok = expect(MD5::hashFile(a.wstring()) == "900150983cd24fb0d6963f7d28e17f72",
                     "stale Win32 errors do not reject successful reads");
    ok &= expect(MD5::hashFile(a.wstring(), {}, 4).empty(),
                 "changed file sizes are rejected");
    HANDLE locked = CreateFileW(b.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    ok &= expect(locked != INVALID_HANDLE_VALUE, "unreadable-file fixture can be locked");
    auto result = DuplicateScanner().scan(temp.path.wstring());
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    ok &= expect(result.totalFiles == 2 && result.groups.empty(),
                 "unreadable files do not create false duplicate groups");

    const auto large = temp.path / L"large.bin";
    writeFile(large, std::string(4 * 1024 * 1024, 'L'));
    int checks = 0;
    ok &= expect(MD5::hashFile(large.wstring(), [&] { return ++checks >= 3; }).empty(),
                 "cancellation interrupts a large file between read chunks");
    ok &= expect(checks == 3, "large-file cancellation stops without reading remaining chunks");
    bool cancelled = false;
    result = DuplicateScanner().scan(temp.path.wstring(),
        [&](int, int, const std::wstring&) { cancelled = true; },
        [&] { return cancelled; });
    ok &= expect(cancelled && result.groups.empty() && result.totalDup == 0 && result.wastedBytes == 0,
                 "cancellation during scanning discards incomplete results");
    return ok;
}

bool sameGroups(const ScanResult& left, const ScanResult& right) {
    if (left.totalFiles != right.totalFiles || left.totalDup != right.totalDup ||
        left.wastedBytes != right.wastedBytes || left.groups.size() != right.groups.size())
        return false;
    for (size_t i = 0; i < left.groups.size(); ++i) {
        if (left.groups[i].md5 != right.groups[i].md5 ||
            left.groups[i].fileSize != right.groups[i].fileSize ||
            left.groups[i].files != right.groups[i].files)
            return false;
    }
    return true;
}

bool filesAreClosed(const fs::path& folder) {
    for (const auto& entry : fs::directory_iterator(folder)) {
        HANDLE file = CreateFileW(entry.path().c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        CloseHandle(file);
    }
    return true;
}

bool testParallelHashingAndCallbacks() {
    TempDir temp;
    std::string contents(2 * 1024 * 1024, 'A');
    for (int i = 0; i < 8; ++i) {
        // 两组文件采样相同，只有完整校验才能把它们正确分开。
        contents[8192] = i < 6 ? 'A' : 'B';
        writeFile(temp.path / (std::to_wstring(i) + L".bin"), contents);
    }
    auto serial = DuplicateScanner().scan(temp.path.wstring(), {}, {}, 1);
    const auto caller = std::this_thread::get_id();
    std::atomic<bool> callbacksOnCaller{true};
    int previous = 0;
    bool progressOk = true;
    auto parallel = DuplicateScanner().scan(temp.path.wstring(),
        [&](int current, int total, const std::wstring&) {
            if (std::this_thread::get_id() != caller) {
                callbacksOnCaller.store(false);
                return;
            }
            progressOk &= current == previous + 1 && total == 8;
            previous = current;
        }, [&] {
            if (std::this_thread::get_id() != caller) callbacksOnCaller.store(false);
            return false;
        }, 64);
    bool ok = expect(sameGroups(serial, parallel) && parallel.groups.size() == 2 &&
                     parallel.totalDup == 6, "parallel hashes preserve groups, totals, MD5 and kept-file order");
    const unsigned expectedWorkers = (std::min)(4u,
        (std::max)(1u, std::thread::hardware_concurrency()));
    ok &= expect(serial.hashWorkers == 1 && parallel.hashWorkers == expectedWorkers,
                 "explicit serial mode and bounded parallel mode are honored");
    ok &= expect(parallel.hashedFiles == 8 && parallel.sampledFiles == 8,
                 "parallel statistics count each file exactly once");
    ok &= expect(callbacksOnCaller.load() && progressOk && previous == 8,
                 "callbacks stay on the caller thread with monotonic complete progress");
    ok &= expect(filesAreClosed(temp.path), "parallel success releases every file handle");

    bool cancelled = false;
    int progressCalls = 0;
    auto stopped = DuplicateScanner().scan(temp.path.wstring(),
        [&](int, int, const std::wstring&) { ++progressCalls; cancelled = true; },
        [&] { return cancelled; }, 4);
    ok &= expect(cancelled && progressCalls == 1 && stopped.groups.empty() &&
                 stopped.totalDup == 0 && stopped.wastedBytes == 0,
                 "parallel cancellation discards partial results and stops progress");
    ok &= expect(filesAreClosed(temp.path), "parallel cancellation joins workers and closes files");

    bool caught = false;
    try {
        DuplicateScanner().scan(temp.path.wstring(),
            [](int, int, const std::wstring&) { throw std::runtime_error("progress callback"); }, {}, 4);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    ok &= expect(caught && filesAreClosed(temp.path),
                 "throwing progress callbacks join workers before propagating the exception");

    bool throwOnCancel = false;
    caught = false;
    try {
        DuplicateScanner().scan(temp.path.wstring(),
            [&](int, int, const std::wstring&) { throwOnCancel = true; },
            [&]() -> bool {
                if (throwOnCancel) throw std::runtime_error("cancel callback");
                return false;
            }, 4);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    ok &= expect(caught && filesAreClosed(temp.path),
                 "throwing cancel callbacks also join workers and close files");
    return ok;
}

bool testSmallBatchStaysSerial() {
    TempDir temp;
    writeFile(temp.path / L"a.bin", std::string(65536, 'A'));
    writeFile(temp.path / L"b.bin", std::string(65536, 'A'));
    const auto result = DuplicateScanner().scan(temp.path.wstring(), {}, {}, 4);
    return expect(result.hashWorkers == 1 && result.totalDup == 1,
                  "small batches avoid parallel thread overhead");
}

} // namespace

int main() {
    bool ok = true;
    ok &= testDetectsDuplicatesAndTotals();
    ok &= testCancellationBeforeHashing();
    ok &= testSizeAndSampleFiltering();
    ok &= testSampleBoundaries();
    ok &= testReadFailuresAndCancellation();
    ok &= testParallelHashingAndCallbacks();
    ok &= testSmallBatchStaysSerial();

    std::printf("=== scanner tests %s ===\n", ok ? "passed" : "failed");
    return ok ? 0 : 1;
}
