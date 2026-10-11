#include "context_menu.h"
#include <windows.h>
#include <shlobj.h>

bool isContextMenuRegistered() {
    HKEY key = nullptr;
    const HKEY roots[] = { HKEY_CURRENT_USER, HKEY_CLASSES_ROOT };
    const wchar_t* paths[] = {
        L"Software\\Classes\\Directory\\shell\\FindDupFiles",
        L"Directory\\shell\\FindDupFiles",
    };

    for (int i = 0; i < 2; ++i) {
        LSTATUS status = RegOpenKeyExW(roots[i], paths[i], 0, KEY_READ, &key);
        if (status == ERROR_SUCCESS) {
            RegCloseKey(key);
            return true;
        }
    }
    return false;
}

static std::wstring getModulePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = GetModuleFileNameW(
            nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (len == 0) return {};
        if (len < path.size()) {
            path.resize(len);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

static bool setRegistryString(HKEY key, const wchar_t* valueName,
                              const std::wstring& value) {
    DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(
        key, valueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()), bytes) == ERROR_SUCCESS;
}

static bool createMenuKey(const wchar_t* path, HKEY& key,
                          const wchar_t* failureMessage, std::wstring& error) {
    const LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, path,
        0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key, nullptr);
    if (status == ERROR_SUCCESS) return true;
    error = failureMessage;
    return false;
}

bool registerContextMenu(std::wstring& error) {
    std::wstring exePath = getModulePath();
    if (exePath.empty()) {
        error = L"无法获取程序路径。";
        return false;
    }

    HKEY menuKey = nullptr;
    if (!createMenuKey(
        L"Software\\Classes\\Directory\\shell\\FindDupFiles",
        menuKey, L"无法创建右键菜单注册表项。", error)) return false;

    std::wstring iconValue = exePath + L",0";
    bool ok = setRegistryString(menuKey, nullptr, L"查找重复文件") &&
              setRegistryString(menuKey, L"Icon", iconValue);
    RegCloseKey(menuKey);
    if (!ok) {
        error = L"无法写入右键菜单注册表值。";
        return false;
    }

    HKEY commandKey = nullptr;
    if (!createMenuKey(
        L"Software\\Classes\\Directory\\shell\\FindDupFiles\\command",
        commandKey, L"无法创建右键菜单命令注册表项。", error)) return false;

    std::wstring command = L"\"" + exePath + L"\" \"%1\"";
    ok = setRegistryString(commandKey, nullptr, command);
    RegCloseKey(commandKey);
    if (!ok) {
        error = L"无法写入右键菜单命令。";
        return false;
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

bool unregisterContextMenu(std::wstring& error) {
    LSTATUS userStatus = RegDeleteTreeW(
        HKEY_CURRENT_USER,
        L"Software\\Classes\\Directory\\shell\\FindDupFiles");
    LSTATUS machineStatus = RegDeleteTreeW(
        HKEY_CLASSES_ROOT,
        L"Directory\\shell\\FindDupFiles");

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    if (machineStatus == ERROR_ACCESS_DENIED) {
        error = L"已移除当前用户的注册；旧的系统级注册需要管理员权限才能移除。";
        return false;
    }

    if (userStatus == ERROR_SUCCESS || machineStatus == ERROR_SUCCESS ||
        (userStatus == ERROR_FILE_NOT_FOUND &&
         machineStatus == ERROR_FILE_NOT_FOUND)) {
        return true;
    } else {
        error = L"无法移除右键菜单。";
    }
    return false;
}
