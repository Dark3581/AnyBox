#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace abx {

struct Setting {
    std::string value;
    std::string source;
};

using Settings = std::map<std::string, Setting>;

void ParseSettings(std::string_view text, const std::string& source, Settings& settings, std::vector<std::string>& problems);
void ParseSettingOverrides(std::string_view text, Settings& settings, std::vector<std::string>& problems);

}
