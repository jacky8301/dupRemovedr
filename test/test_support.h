#pragma once

#include <windows.h>
#include <cstdio>

inline bool expect(bool condition, const char* message) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
    return condition;
}

// 等待后台工作时继续派发窗口消息，避免阻塞界面定时器。
inline void pumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
