#include "ShaderStore.hpp"

#include "AbxApi.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>

namespace abx::d3d12x {

namespace {

const std::set<std::string>& Stages() {
    static const std::set<std::string> stages{"vs", "ps", "gs", "hs", "ds", "cs", "ms", "as", "lib"};
    return stages;
}

std::wstring Widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::optional<std::uint32_t> SubgroupSize(const std::string& text) {
    if (text == "0") {
        return 0u;
    }
    if (text == "32") {
        return 32u;
    }
    if (text == "64") {
        return 64u;
    }
    return std::nullopt;
}

bool IsDigest(const std::string& text) {
    if (text.size() != 64) {
        return false;
    }
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::wstring EnvironmentPath(const wchar_t* name) {
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(name, buffer, MAX_PATH);
    return length > 0 && length < MAX_PATH ? std::wstring(buffer, length) : std::wstring{};
}

std::wstring ExecutableFolder() {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    std::wstring folder(path, length);
    const std::size_t slash = folder.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : folder.substr(0, slash);
}

}

std::optional<Sha256> Sha256Of(const void* data, std::size_t size) {
    if (size > 0xFFFFFFFFu || (data == nullptr && size != 0)) {
        return std::nullopt;
    }
    static const BCRYPT_ALG_HANDLE algorithm = [] {
        BCRYPT_ALG_HANDLE handle = nullptr;
        return BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ? nullptr : handle;
    }();
    if (algorithm == nullptr) {
        return std::nullopt;
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        return std::nullopt;
    }
    Sha256 digest{};
    static unsigned char empty = 0;
    const bool ok = BCryptHashData(hash, size == 0 ? &empty : const_cast<PUCHAR>(static_cast<const unsigned char*>(data)), static_cast<ULONG>(size), 0) >= 0
                    && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    BCryptDestroyHash(hash);
    if (!ok) {
        return std::nullopt;
    }
    return digest;
}

std::string Hex(const Sha256& digest) {
    static constexpr char Digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest) {
        text.push_back(Digits[byte >> 4]);
        text.push_back(Digits[byte & 15]);
    }
    return text;
}

std::optional<std::size_t> ContainerSize(const void* data, std::size_t length) {
    if (data == nullptr || length < 32 || std::memcmp(data, "DXBC", 4) != 0) {
        return std::nullopt;
    }
    std::uint32_t size = 0;
    std::memcpy(&size, static_cast<const unsigned char*>(data) + 24, sizeof(size));
    if (size < 32 || size > length) {
        return std::nullopt;
    }
    return size;
}

bool ShaderStore::Load(const std::wstring& folder, std::string& error) {
    modules_.clear();
    std::ifstream index(std::filesystem::path(folder) / L"modules.txt", std::ios::binary);
    if (!index) {
        error = "modules.txt not found";
        return false;
    }
    std::string line;
    std::size_t number = 0;
    while (std::getline(index, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::size_t first = line.find('\t');
        const std::size_t second = first == std::string::npos ? std::string::npos : line.find('\t', first + 1);
        const std::size_t third = second == std::string::npos ? std::string::npos : line.find('\t', second + 1);
        if (third == std::string::npos || line.find('\t', third + 1) != std::string::npos) {
            error = "line " + std::to_string(number) + " is not <sha256> <stage> <subgroup> <path>";
            modules_.clear();
            return false;
        }
        const std::string digest = line.substr(0, first);
        const std::string stage = line.substr(first + 1, second - first - 1);
        const std::optional<std::uint32_t> subgroup = SubgroupSize(line.substr(second + 1, third - second - 1));
        std::wstring path = Widen(line.substr(third + 1));
        if (!IsDigest(digest) || Stages().count(stage) == 0 || !subgroup || path.empty()) {
            error = "line " + std::to_string(number) + " is not <sha256> <stage> <subgroup> <path>";
            modules_.clear();
            return false;
        }
        for (wchar_t& c : path) {
            if (c == L'/') {
                c = L'\\';
            }
        }
        modules_[digest] = ShaderModule{digest, stage, *subgroup, folder + L"\\" + path};
    }
    return true;
}

const ShaderModule* ShaderStore::Find(const void* bytecode, std::size_t length) const {
    const std::optional<std::size_t> size = ContainerSize(bytecode, length);
    if (!size) {
        AbxTrace("shader store: %zu bytes at %p are not a DXBC container", length, bytecode);
        return nullptr;
    }
    const std::optional<Sha256> digest = Sha256Of(bytecode, *size);
    if (!digest) {
        return nullptr;
    }
    const std::string key = Hex(*digest);
    const auto found = modules_.find(key);
    AbxTrace("shader store: %s %s", key.c_str(), found == modules_.end() ? "missing" : found->second.stage.c_str());
    return found == modules_.end() ? nullptr : &found->second;
}

const ShaderStore& TitleShaderStore() {
    static ShaderStore store;
    static std::once_flag once;
    std::call_once(once, [] {
        std::wstring folder = EnvironmentPath(L"ANYBOX_SHADER_STORE");
        if (folder.empty()) {
            folder = ExecutableFolder() + L"\\shaders";
        }
        std::string error;
        if (store.Load(folder, error)) {
            AbxLog("d3d12_x", "shader store: %zu modules", store.Size());
        } else {
            AbxLog("d3d12_x", "shader store: none loaded (%s)", error.c_str());
        }
    });
    return store;
}

}
