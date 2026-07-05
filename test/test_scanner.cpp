#include "../src/scanner.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

} // namespace

int main() {
    bool ok = true;
    ok &= testDetectsDuplicatesAndTotals();
    ok &= testCancellationBeforeHashing();

    std::printf("=== scanner tests %s ===\n", ok ? "passed" : "failed");
    return ok ? 0 : 1;
}
