#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace abx {

struct DriveMapping {
    wchar_t letter = 0;
    std::wstring root;
    bool readOnly = false;
    std::wstring overlay;
};

std::optional<DriveMapping> ParseDriveSetting(wchar_t letter, std::string_view value, std::string& error);
std::wstring RelativePart(std::wstring_view path);
std::wstring MapConsolePath(std::wstring_view path, bool write);
void AddPathRedirects(const std::function<void(const char*, void*)>& add);

}
