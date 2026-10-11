#include "dialog.h"
#include <algorithm>
#include <filesystem>
#include <stdexcept>

static void cleanupThread(DlgData* d, std::vector<CleanupTask> tasks) {
    const HRESULT comHr = CoInitializeEx(nullptr,
        COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool failed = FAILED(comHr);
    try {
        if (!failed) for (const auto& task : tasks) {
            if (d->cancelled.load()) break;
            {
                std::lock_guard<std::mutex> lock(d->cleanup.mutex);
                d->cleanup.file = task.path;
            }
            std::wstring recoveryPath;
            bool recycled = false;
            try { recycled = recycleVerifiedDuplicate(task, d->cancelled, &recoveryPath, d->cleanupOperations); }
            catch (...) { failed = true; }
            if (!recycled && d->cancelled.load() && recoveryPath.empty()) break;
            std::lock_guard<std::mutex> lock(d->cleanup.mutex);
            d->cleanup.outcomes.push_back({task.row, recycled, std::move(recoveryPath)});
            if (failed) break;
        }
    } catch (...) {
        failed = true;
    }
    if (SUCCEEDED(comHr)) CoUninitialize();
    std::lock_guard<std::mutex> lock(d->cleanup.mutex);
    d->cleanup.failed = failed;
    d->cleanup.finished = true;
}

void cancelCleanup(HWND hDlg, DlgData& d) {
    if (!d.cleaning || d.cancelled.exchange(true)) return;
    SetDlgItemTextW(hDlg, IDC_STATUS, L"正在停止清理，等待当前文件处理完成…");
    EnableWindow(GetDlgItem(hDlg, IDC_DELETE_BTN), FALSE);
}

void refreshCleanup(HWND hDlg, DlgData& d) {
    if (!d.cleaning) return;
    // 每次最多更新 512 行，避免积压结果长时间占用界面线程。
    CleanupOutcome batch[512];
    size_t count = 0;
    bool finished, failed;
    std::wstring file;
    {
        std::lock_guard<std::mutex> lock(d.cleanup.mutex);
        count = (std::min)(std::size(batch), d.cleanup.outcomes.size() - d.cleanupApplied);
        std::copy_n(d.cleanup.outcomes.begin() + d.cleanupApplied, count, batch);
        finished = d.cleanup.finished && d.cleanupApplied + count == d.cleanup.outcomes.size();
        failed = d.cleanup.failed;
        file = d.cleanup.file;
    }
    HWND list = GetDlgItem(hDlg, IDC_LIST);
    if (count) SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    for (size_t i = 0; i < count; ++i) {
        auto& ref = d.items[batch[i].row];
        const int row = d.itemRows[batch[i].row];
        if (batch[i].recycled) {
            ref.removed = true;
            ++d.cleanupSucceeded;
            --d.result.totalDup;
            d.result.wastedBytes -= d.result.groups[ref.g].fileSize;
        } else {
            ++d.cleanupFailed;
            if (!batch[i].recoveryPath.empty()) {
                d.cleanupRecoveryPath = batch[i].recoveryPath;
                // 更新实际位置，用户可以从结果列表定位未能恢复原位的副本。
                d.result.groups[ref.g].files[ref.f] = batch[i].recoveryPath;
                auto folder = std::filesystem::path(batch[i].recoveryPath).parent_path().wstring();
                if (row >= 0) ListView_SetItemText(list, row, 2, folder.data());
            }
        }
        ref.status = batch[i].recycled ? L"已回收" :
            batch[i].recoveryPath.empty() ? L"清理失败" : L"已暂存";
        if (row >= 0) ListView_SetItemText(list, row, 0, ref.status.data());
        if ((batch[i].recycled || !batch[i].recoveryPath.empty()) &&
            row >= 0 && row == d.previewRow)
            selectPreview(hDlg, d, d.previewRow);
    }
    if (count) {
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, FALSE);
    }
    d.cleanupApplied += count;
    setSummary(hDlg, d.result.totalFiles, d.result.totalDup, d.result.wastedBytes);
    const int position = d.cleanupTotal ? static_cast<int>(1000ULL * d.cleanupApplied / d.cleanupTotal) : 0;
    SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETPOS, position, 0);
    const std::wstring counts = std::to_wstring(d.cleanupApplied) + L" / " + std::to_wstring(d.cleanupTotal) +
        L" · 已回收 " + std::to_wstring(d.cleanupSucceeded) + L" · 失败 " + std::to_wstring(d.cleanupFailed);
    if (!finished) {
        SetDlgItemTextW(hDlg, IDC_STATUS, ((d.cancelled.load()
            ? L"正在停止，等待当前文件完成 · " : L"正在清理 ") + counts + L" · " + file).c_str());
        return;
    }
    KillTimer(hDlg, CLEANUP_TIMER);
    if (d.worker.joinable()) d.worker.join();
    d.cleaning = false;
    const wchar_t* status = failed ? L"清理中断 · " :
        d.cleanupApplied < d.cleanupTotal ? L"清理已停止 · " : L"清理完成 · ";
    SetDlgItemTextW(hDlg, IDC_STATUS, (status + counts + (d.cleanupRecoveryPath.empty()
        ? L"" : L" · 未覆盖原位置的新文件，副本保存在：" + d.cleanupRecoveryPath)).c_str());
    updateScanControls(hDlg, &d);
    if (d.closeAfterWork) requestClose(hDlg, &d);
}

void startCleanup(HWND hDlg, DlgData& d) {
    if (d.scanning || d.cleaning || d.result.totalDup == 0) return;
    if (d.worker.joinable()) d.worker.join();
    try {
        std::vector<CleanupTask> tasks;
        tasks.reserve(d.result.totalDup);
        for (size_t row = 0; row < d.items.size(); ++row) {
            const auto& ref = d.items[row];
            if (ref.g >= 0 && ref.f > 0 && !ref.removed) {
                const auto& group = d.result.groups[ref.g];
                tasks.push_back({row, group.files[ref.f], group.files.front(), group.md5, group.fileSize});
            }
        }
        if (tasks.empty()) return;
        d.cleanup.outcomes.clear();
        d.cleanup.outcomes.reserve(tasks.size());
        d.cleanup.file.clear();
        d.cleanup.finished = false;
        d.cleanup.failed = false;
        d.cleanupTotal = tasks.size();
        d.cleanupApplied = d.cleanupSucceeded = d.cleanupFailed = 0;
        d.cleanupRecoveryPath.clear();
        d.cancelled.store(false);
        d.cleaning = true;
        setProgressMarquee(hDlg, false);
        SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETRANGE32, 0, 1000);
        SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETPOS, 0, 0);
        SetDlgItemTextW(hDlg, IDC_STATUS,
            (L"正在清理 0 / " + std::to_wstring(d.cleanupTotal) + L" · 正在准备回收文件…").c_str());
        updateScanControls(hDlg, &d);
        if (!SetTimer(hDlg, CLEANUP_TIMER, 80, nullptr))
            throw std::runtime_error("cleanup timer");
        d.worker = std::thread(cleanupThread, &d, std::move(tasks));
    } catch (...) {
        KillTimer(hDlg, CLEANUP_TIMER);
        d.cleaning = false;
        SetDlgItemTextW(hDlg, IDC_STATUS, L"无法启动清理，请稍后重试");
        updateScanControls(hDlg, &d);
    }
}
