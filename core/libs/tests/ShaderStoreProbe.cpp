#include "ShaderStore.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int Fail(const char* what) {
    std::printf("FAIL %s\n", what);
    return 1;
}

std::wstring Wide(const char* text) {
    std::wstring wide;
    for (const char* c = text; *c != '\0'; ++c) {
        wide += static_cast<wchar_t>(*c == '/' ? '\\' : *c);
    }
    return wide;
}

std::vector<unsigned char> Read(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::vector<unsigned char> Container(std::uint32_t size) {
    std::vector<unsigned char> data(size, 0x5A);
    std::memcpy(data.data(), "DXBC", 4);
    std::memcpy(data.data() + 24, &size, sizeof(size));
    return data;
}

int SelfTest() {
    using abx::d3d12x::Hex;
    using abx::d3d12x::Sha256Of;
    using abx::d3d12x::ContainerSize;
    const auto empty = Sha256Of("", 0);
    if (!empty || Hex(*empty) != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") return Fail("sha256 of nothing");
    const auto abc = Sha256Of("abc", 3);
    if (!abc || Hex(*abc) != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") return Fail("sha256 of abc");

    std::vector<unsigned char> container = Container(48);
    if (ContainerSize(container.data(), container.size()) != std::optional<std::size_t>(48)) return Fail("container size");
    container.resize(64, 0);
    if (ContainerSize(container.data(), container.size()) != std::optional<std::size_t>(48)) return Fail("bytes after the container are ignored");
    if (ContainerSize(container.data(), 40)) return Fail("size field beyond the data");
    if (ContainerSize(container.data(), 16)) return Fail("shorter than a header");
    container[0] = 'X';
    if (ContainerSize(container.data(), container.size())) return Fail("magic");
    std::vector<unsigned char> small = Container(32);
    const std::uint32_t tiny = 8;
    std::memcpy(small.data() + 24, &tiny, sizeof(tiny));
    if (ContainerSize(small.data(), small.size())) return Fail("size field below a header");
    std::puts("self test ok");
    return 0;
}

int Load(const char* folder, bool expectOk, const char* expectError) {
    abx::d3d12x::ShaderStore store;
    std::string error;
    const bool ok = store.Load(Wide(folder), error);
    if (ok != expectOk) {
        std::printf("FAIL load returned %d (%s)\n", ok, error.c_str());
        return 1;
    }
    if (!ok && error != expectError) {
        std::printf("FAIL error '%s', expected '%s'\n", error.c_str(), expectError);
        return 1;
    }
    std::printf("load %s %zu\n", ok ? "ok" : error.c_str(), store.Size());
    return 0;
}

int Find(const char* folder, int count, char** containers) {
    abx::d3d12x::ShaderStore store;
    std::string error;
    if (!store.Load(Wide(folder), error)) {
        std::printf("FAIL load: %s\n", error.c_str());
        return 1;
    }
    for (int index = 0; index < count; ++index) {
        std::vector<unsigned char> data = Read(containers[index]);
        data.resize(data.size() + 7, 0);
        const abx::d3d12x::ShaderModule* module = store.Find(data.data(), data.size());
        if (module == nullptr) {
            std::printf("%s\tmissing\n", containers[index]);
            continue;
        }
        std::string path;
        for (const wchar_t c : module->path) path += static_cast<char>(c);
        std::printf("%s\t%s\t%u\t%s\t%s\n", containers[index], module->stage.c_str(), module->subgroupSize, path.c_str(), module->key.c_str());
    }
    return 0;
}

int TitleStore() {
    const abx::d3d12x::ShaderStore& store = abx::d3d12x::TitleShaderStore();
    std::printf("title store %zu\n", store.Size());
    return &store == &abx::d3d12x::TitleShaderStore() ? 0 : Fail("one title store");
}

}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string command = argv[1];
    if (command == "selftest") return SelfTest();
    if (command == "load" && argc == 5) return Load(argv[2], std::strcmp(argv[3], "ok") == 0, argv[4]);
    if (command == "find" && argc >= 3) return Find(argv[2], argc - 3, argv + 3);
    if (command == "title") return TitleStore();
    return Fail("usage");
}
