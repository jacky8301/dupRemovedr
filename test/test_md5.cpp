// Console test for MD5 and scanner
#include "../src/md5.h"
#include "../src/scanner.h"
#include <windows.h>
#include <cstdio>

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        wprintf(L"Usage: test_md5.exe <folder>\n");
        return 1;
    }

    // Test known MD5 vector
    MD5 md5;
    const char* testStr = "hello world\n";
    md5.update(testStr, strlen(testStr));
    std::string hex = MD5::toHex(md5.finalize());
    wprintf(L"MD5 of 'hello world\\n': %hs\n", hex.c_str());
    wprintf(L"Expected:              6f5902ac237024bdd0c176cb93063dc4\n\n");

    // Test scanner
    wprintf(L"--- Scanning: %s ---\n", argv[1]);
    DuplicateScanner scanner;
    auto result = scanner.scan(argv[1],
        [](int cur, int total, const std::wstring& f) {
            wprintf(L"[%d/%d] %s\n", cur, total, f.c_str());
        });

    wprintf(L"\nResult: %d files, %d groups, %d dups, %llu bytes wasted\n",
        result.totalFiles, (int)result.groups.size(),
        result.totalDup, result.wastedBytes);

    for (int g = 0; g < (int)result.groups.size(); ++g) {
        auto& grp = result.groups[g];
        wprintf(L"\nGroup %d MD5=%hs:\n", g+1, grp.md5.c_str());
        for (int f = 0; f < (int)grp.files.size(); ++f)
            wprintf(L"  [%s] %s\n", f==0 ? L"KEEP" : L"DEL ", grp.files[f].c_str());
    }
    return 0;
}
