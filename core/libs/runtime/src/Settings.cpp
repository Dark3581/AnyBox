#include "Settings.hpp"

#include <cstddef>

namespace abx {

namespace {

std::string_view Trim(std::string_view text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool ValidKey(std::string_view key) {
    if (key.empty()) {
        return false;
    }
    for (const char c : key) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
        if (!allowed) return false;
    }
    return true;
}

void Store(std::string_view entry, const std::string& source, Settings& settings, std::vector<std::string>& problems) {
    const std::size_t equals = entry.find('=');
    const std::string_view key = equals == std::string_view::npos ? std::string_view{} : Trim(entry.substr(0, equals));
    const std::string_view value = equals == std::string_view::npos ? std::string_view{} : Trim(entry.substr(equals + 1));
    if (!ValidKey(key) || value.empty()) {
        problems.push_back(source + ": ignored, not a lower-case key = value");
        return;
    }
    const std::string name(key);
    const auto known = settings.find(name);
    if (known != settings.end()) {
        problems.push_back(source + ": " + name + " replaces the value from " + known->second.source);
    }
    settings[name] = Setting{std::string(value), source};
}

}

void ParseSettings(std::string_view text, const std::string& source, Settings& settings, std::vector<std::string>& problems) {
    std::size_t line = 0;
    while (!text.empty()) {
        ++line;
        const std::size_t end = text.find('\n');
        std::string_view current = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        const std::size_t comment = current.find('#');
        if (comment != std::string_view::npos) current = current.substr(0, comment);
        current = Trim(current);
        if (!current.empty()) Store(current, source + ":" + std::to_string(line), settings, problems);
    }
}

void ParseSettingOverrides(std::string_view text, Settings& settings, std::vector<std::string>& problems) {
    while (!text.empty()) {
        const std::size_t end = text.find(';');
        const std::string_view current = Trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (!current.empty()) Store(current, "ANYBOX_SET", settings, problems);
    }
}

}
