#pragma once

#include <windows.h>

#include <string>

namespace abx::xgameruntime {

DWORD OpenTitleFolder(const wchar_t* name, std::wstring& folder, const char* function);
DWORD TitleFolderPath(const std::wstring& folder, std::string& path);

}
