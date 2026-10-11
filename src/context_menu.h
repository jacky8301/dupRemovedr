#pragma once
#include <string>

bool isContextMenuRegistered();
bool registerContextMenu(std::wstring& error);
bool unregisterContextMenu(std::wstring& error);
