#pragma once

#include "resource.h"
#include "scanner.h"
#include "image_preview.h"
#include "cleanup.h"
#include <commctrl.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 扫描线程通过消息发布进度和结果，清理与预览通过定时器刷新。
inline constexpr UINT WM_SCAN_PROGRESS = WM_APP;
inline constexpr UINT WM_SCAN_DONE = WM_APP + 2;
inline constexpr UINT_PTR CLEANUP_TIMER = 1;
inline constexpr UINT_PTR PREVIEW_TIMER = 2;

struct CleanupOutcome {
    size_t row;
    bool recycled;
    std::wstring recoveryPath;
};

// 工作线程只发布结果，由界面线程分批更新控件和扫描统计。
struct CleanupProgress {
    std::mutex mutex;
    std::vector<CleanupOutcome> outcomes;
    std::wstring file;
    bool finished = false;
    bool failed = false;
};

// 对话框及后台任务的共享状态。
struct DlgData {
    std::wstring    folder;
    ScanResult      result;
    std::thread     worker;
    // 完整结果索引保持稳定，搜索只改变可见行映射；每组第一个文件保留。
    struct ItemRef { int g; int f; bool removed = false; std::wstring status; };
    std::vector<ItemRef> items;
    std::vector<int> visibleRows;
    std::vector<int> itemRows;
    std::wstring searchQuery;
    bool            scanning  = false;
    std::wstring    scanError;
    bool            cleaning = false;
    CleanupProgress cleanup;
    CleanupOperations cleanupOperations;
    size_t          cleanupTotal = 0;
    size_t          cleanupApplied = 0;
    size_t          cleanupSucceeded = 0;
    size_t          cleanupFailed = 0;
    std::wstring    cleanupRecoveryPath;
    std::atomic_bool cancelled = false;
    std::mutex      progressMutex;
    std::wstring    currentFile;
    int             current = 0;
    int             total = 0;
    std::atomic_bool progressPending = false;
    std::chrono::steady_clock::time_point started;
    HFONT           bodyFont = nullptr;
    HFONT           titleFont = nullptr;
    HFONT           numberFont = nullptr;
    HBRUSH          background = CreateSolidBrush(RGB(245, 247, 251));
    HBRUSH          card = CreateSolidBrush(RGB(255, 255, 255));
    bool            closeAfterWork = false;
    ImagePreview    previewLoader;
    PreviewImage    preview;
    uint64_t        previewRequest = 0;
    int             previewRow = -1;
    std::wstring    previewPath;
    HIMAGELIST      rowHeightImages = nullptr;

    ~DlgData() {
        DeleteObject(bodyFont);
        DeleteObject(titleFont);
        DeleteObject(numberFont);
        DeleteObject(background);
        DeleteObject(card);
        if (rowHeightImages) ImageList_Destroy(rowHeightImages);
    }
};

// 界面模块的内部协作接口，状态仍由对话框统一持有。
int scaled(HWND window, int value);
std::wstring fmtSize(uint64_t bytes);
void updateFonts(HWND hDlg, DlgData& d);
void layoutDialog(HWND hDlg);
void paintDialog(HWND hDlg, DlgData& d, HDC printDC = nullptr);
void drawButton(const DRAWITEMSTRUCT& item, DlgData& d);
INT_PTR handleDialogAppearance(HWND hDlg, DlgData* d, UINT msg, WPARAM wp, LPARAM lp);
void setSummary(HWND hDlg, int files, int duplicates, uint64_t bytes);
void setProgressMarquee(HWND hDlg, bool enabled);
void drawPreview(const DRAWITEMSTRUCT& item, DlgData& d);
void selectPreview(HWND hDlg, DlgData& d, int row);
void refreshPreview(HWND hDlg, DlgData& d);
void revealSelectedFile(HWND hDlg, DlgData& d);
void initListView(HWND hList);
void filterListView(HWND hList, DlgData& d);
void populateListView(HWND hList, DlgData& d);
INT_PTR handleListNotification(HWND hDlg, DlgData* d, LPARAM lp);
void updateContextMenuState(HWND hDlg, bool busy);
void updateScanControls(HWND hDlg, DlgData* d);
void cancelScan(HWND hDlg, DlgData& d);
std::wstring chooseFolder(HWND owner, const std::wstring& initialFolder);
void startScan(HWND hDlg, DlgData& d, const std::wstring& folder);
INT_PTR handleScanProgress(HWND hDlg, DlgData* d);
INT_PTR handleScanDone(HWND hDlg, DlgData* d, WPARAM wp, LPARAM lp);
void cancelCleanup(HWND hDlg, DlgData& d);
void refreshCleanup(HWND hDlg, DlgData& d);
void startCleanup(HWND hDlg, DlgData& d);
void requestClose(HWND hDlg, DlgData* d);
INT_PTR handleDialogCommand(HWND hDlg, DlgData* d, WPARAM wp);
INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp);
