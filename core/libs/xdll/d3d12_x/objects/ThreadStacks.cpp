#define PSAPI_VERSION 2

#include "ThreadStacks.hpp"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <utility>

#if defined(__GNUC__)
#include <cxxabi.h>
#endif

namespace abx::d3d12x {

namespace {

constexpr std::size_t kFrames = 20;
constexpr std::size_t kThreads = 512;
constexpr std::size_t kModules = 1024;
constexpr std::size_t kThreadsNamed = 10;
constexpr std::size_t kNameChars = 140;
constexpr std::uint64_t kFrameLimit = 64ull << 20;
constexpr std::uint64_t kSymbolFileLimit = 256ull << 20;
constexpr std::uint32_t kExportReach = 0x1000;
constexpr std::uint32_t kExportNames = 200000;
constexpr int kChainedInfos = 32;
constexpr std::size_t kTebLastError = 0x68;
constexpr DWORD kLastErrorProbe = 0x2AB0C0DEu;

constexpr std::uint8_t kUnwindPushNonvolatile = 0;
constexpr std::uint8_t kUnwindAllocLarge = 1;
constexpr std::uint8_t kUnwindAllocSmall = 2;
constexpr std::uint8_t kUnwindSetFrame = 3;
constexpr std::uint8_t kUnwindSaveNonvolatile = 4;
constexpr std::uint8_t kUnwindSaveNonvolatileFar = 5;
constexpr std::uint8_t kUnwindEpilog = 6;
constexpr std::uint8_t kUnwindSpare = 7;
constexpr std::uint8_t kUnwindSaveXmm = 8;
constexpr std::uint8_t kUnwindSaveXmmFar = 9;
constexpr std::uint8_t kUnwindPushMachineFrame = 10;

static_assert(sizeof(IMAGE_SYMBOL) == IMAGE_SIZEOF_SYMBOL);

struct Symbol {
    std::uint32_t rva = 0;
    std::string name;
};

struct Module {
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    std::string name;
    std::wstring path;
    const RUNTIME_FUNCTION* functions = nullptr;
    std::uint32_t functionCount = 0;
    bool symbolsRead = false;
    std::vector<Symbol> symbols;
    bool exportsRead = false;
    std::vector<Symbol> exports;
};

struct Captured {
    DWORD id = 0;
    HANDLE handle = nullptr;
    bool walked = false;
    std::uint32_t count = 0;
    std::uint64_t frames[kFrames]{};
    std::uint64_t cpuBefore = 0;
    std::uint64_t cpuAfter = 0;
    bool lastErrorRead = false;
    DWORD lastError = 0;
    std::string name;
};

struct ThreadBasics {
    LONG exitStatus = 0;
    void* environment = nullptr;
    void* process = nullptr;
    void* thread = nullptr;
    ULONG_PTR affinity = 0;
    LONG priority = 0;
    LONG basePriority = 0;
};

using QueryThread = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);

QueryThread LastErrorQuery() {
    static const QueryThread verified = [] {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto query = ntdll != nullptr ? reinterpret_cast<QueryThread>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryInformationThread"))) : nullptr;
        if (query == nullptr) return QueryThread{nullptr};
        ThreadBasics basics;
        ULONG length = 0;
        if (query(GetCurrentThread(), ThreadBasicInformation, &basics, sizeof(basics), &length) != 0 || length != sizeof(basics) ||
            basics.environment != static_cast<void*>(NtCurrentTeb()) || static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(basics.thread)) != GetCurrentThreadId()) {
            return QueryThread{nullptr};
        }
        const DWORD saved = GetLastError();
        SetLastError(kLastErrorProbe);
        DWORD seen = 0;
        std::memcpy(&seen, static_cast<const std::uint8_t*>(basics.environment) + kTebLastError, sizeof(seen));
        SetLastError(saved);
        return seen == kLastErrorProbe ? query : QueryThread{nullptr};
    }();
    return verified;
}

std::string Narrow(const wchar_t* text) {
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string narrow(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow.data(), needed, nullptr, nullptr);
    narrow.resize(static_cast<std::size_t>(needed) - 1);
    return narrow;
}

const IMAGE_NT_HEADERS64* Headers(const Module& module) {
    if (module.size < sizeof(IMAGE_DOS_HEADER)) return nullptr;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
    const std::uint64_t at = static_cast<std::uint64_t>(dos->e_lfanew);
    if (at > module.size || module.size - at < sizeof(IMAGE_NT_HEADERS64)) return nullptr;
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module.base + at);
    if (headers->Signature != IMAGE_NT_SIGNATURE || headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;
    return headers;
}

const IMAGE_DATA_DIRECTORY* Directory(const Module& module, std::uint32_t index, std::uint64_t least) {
    const IMAGE_NT_HEADERS64* headers = Headers(module);
    if (headers == nullptr || headers->OptionalHeader.NumberOfRvaAndSizes <= index) return nullptr;
    const IMAGE_DATA_DIRECTORY& directory = headers->OptionalHeader.DataDirectory[index];
    if (directory.VirtualAddress == 0 || directory.Size < least || directory.VirtualAddress > module.size || directory.Size > module.size - directory.VirtualAddress) return nullptr;
    return &directory;
}

std::vector<Module> LoadedModules() {
    std::vector<Module> modules;
    std::vector<HMODULE> handles(kModules);
    DWORD needed = 0;
    const HANDLE process = GetCurrentProcess();
    if (!EnumProcessModules(process, handles.data(), static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &needed)) return modules;
    const std::size_t count = (std::min)(static_cast<std::size_t>(needed / sizeof(HMODULE)), handles.size());
    modules.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        MODULEINFO info{};
        if (!GetModuleInformation(process, handles[index], &info, sizeof(info)) || info.lpBaseOfDll == nullptr || info.SizeOfImage == 0) continue;
        Module module;
        module.base = reinterpret_cast<std::uint64_t>(info.lpBaseOfDll);
        module.size = info.SizeOfImage;
        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameExW(process, handles[index], path, MAX_PATH) != 0) {
            module.path = path;
            const wchar_t* leaf = std::wcsrchr(path, L'\\');
            module.name = Narrow(leaf != nullptr ? leaf + 1 : path);
        }
        if (module.name.empty()) {
            char text[32];
            std::snprintf(text, sizeof(text), "module_%llx", static_cast<unsigned long long>(module.base));
            module.name = text;
        }
        if (const IMAGE_DATA_DIRECTORY* directory = Directory(module, IMAGE_DIRECTORY_ENTRY_EXCEPTION, sizeof(RUNTIME_FUNCTION))) {
            module.functions = reinterpret_cast<const RUNTIME_FUNCTION*>(module.base + directory->VirtualAddress);
            module.functionCount = directory->Size / sizeof(RUNTIME_FUNCTION);
        }
        modules.push_back(std::move(module));
    }
    return modules;
}

const Module* ModuleAt(const std::vector<Module>& modules, std::uint64_t address) {
    for (const Module& module : modules) {
        if (address >= module.base && address - module.base < module.size) return &module;
    }
    return nullptr;
}

const RUNTIME_FUNCTION* FunctionAt(const Module& module, std::uint64_t rva) {
    std::uint32_t low = 0;
    std::uint32_t high = module.functionCount;
    while (low < high) {
        const std::uint32_t middle = low + (high - low) / 2;
        const RUNTIME_FUNCTION& function = module.functions[middle];
        if (rva < function.BeginAddress) {
            high = middle;
        } else if (rva >= function.EndAddress) {
            low = middle + 1;
        } else {
            if ((function.UnwindData & 1u) == 0) return &function;
            const std::uint64_t other = function.UnwindData & ~1u;
            if (other > module.size || module.size - other < sizeof(RUNTIME_FUNCTION)) return nullptr;
            return reinterpret_cast<const RUNTIME_FUNCTION*>(module.base + other);
        }
    }
    return nullptr;
}

std::uint32_t CodeWord(const std::uint8_t* codes, std::uint32_t index) {
    std::uint16_t word = 0;
    std::memcpy(&word, codes + std::size_t{index} * 2, sizeof(word));
    return word;
}

bool FrameReadable(const Module& module, const RUNTIME_FUNCTION* function, const CONTEXT& context, std::uint64_t low, std::uint64_t high) {
    std::uint64_t extent = sizeof(std::uint64_t);
    std::uint64_t reach = 0;
    std::uint64_t frameBase = 0;
    bool framed = false;
    for (int depth = 0; depth < kChainedInfos; ++depth) {
        const std::uint64_t at = function->UnwindData & ~1u;
        if (at > module.size || module.size - at < 4) return false;
        const auto* info = reinterpret_cast<const std::uint8_t*>(module.base + at);
        const std::uint8_t version = info[0] & 7u;
        const std::uint8_t flags = static_cast<std::uint8_t>(info[0] >> 3);
        const std::uint32_t codeCount = info[2];
        const std::uint8_t frameRegister = info[3] & 15u;
        const std::uint8_t frameOffset = static_cast<std::uint8_t>(info[3] >> 4);
        if (version != 1 && version != 2) return false;
        const std::uint64_t codeBytes = (std::uint64_t{codeCount} + 1) / 2 * 4;
        if (module.size - at < 4 + codeBytes + sizeof(RUNTIME_FUNCTION)) return false;
        if (depth == 0 && frameRegister != 0) {
            frameBase = (&context.Rax)[frameRegister] - std::uint64_t{frameOffset} * 16;
            framed = true;
        }
        const std::uint8_t* codes = info + 4;
        for (std::uint32_t index = 0; index < codeCount;) {
            const std::uint8_t operation = codes[std::size_t{index} * 2 + 1] & 15u;
            const std::uint8_t detail = static_cast<std::uint8_t>(codes[std::size_t{index} * 2 + 1] >> 4);
            std::uint32_t slots = 1;
            if (operation == kUnwindAllocLarge) slots = detail == 0 ? 2 : 3;
            if (operation == kUnwindSaveNonvolatile || operation == kUnwindEpilog || operation == kUnwindSaveXmm) slots = 2;
            if (operation == kUnwindSaveNonvolatileFar || operation == kUnwindSpare || operation == kUnwindSaveXmmFar) slots = 3;
            if (operation > kUnwindPushMachineFrame || index + slots > codeCount) return false;
            const std::uint64_t oneWord = slots >= 2 ? CodeWord(codes, index + 1) : 0;
            const std::uint64_t twoWords = slots == 3 ? oneWord | (std::uint64_t{CodeWord(codes, index + 2)} << 16) : 0;
            switch (operation) {
            case kUnwindPushNonvolatile: extent += 8; break;
            case kUnwindAllocLarge: extent += detail == 0 ? oneWord * 8 : twoWords; break;
            case kUnwindAllocSmall: extent += std::uint64_t{detail} * 8 + 8; break;
            case kUnwindSaveNonvolatile: reach = (std::max)(reach, oneWord * 8 + 8); break;
            case kUnwindSaveNonvolatileFar: reach = (std::max)(reach, twoWords + 8); break;
            case kUnwindSaveXmm: reach = (std::max)(reach, oneWord * 16 + 16); break;
            case kUnwindSaveXmmFar: reach = (std::max)(reach, twoWords + 16); break;
            case kUnwindPushMachineFrame: extent += detail != 0 ? 48 : 40; break;
            case kUnwindSetFrame:
            case kUnwindEpilog:
            case kUnwindSpare:
            default: break;
            }
            index += slots;
        }
        if ((flags & UNW_FLAG_CHAININFO) == 0) break;
        function = reinterpret_cast<const RUNTIME_FUNCTION*>(codes + codeBytes);
    }
    const std::uint64_t needed = (std::max)(extent, reach);
    if (needed > kFrameLimit) return false;
    const auto inside = [&](std::uint64_t base) { return base >= low && base <= high && needed <= high - base; };
    return inside(context.Rsp) && (!framed || inside(frameBase));
}

std::uint32_t Walk(const std::vector<Module>& modules, CONTEXT context, std::uint64_t low, std::uint64_t high, std::uint64_t* frames) {
    std::uint32_t count = 0;
    while (count < kFrames && context.Rip != 0) {
        frames[count++] = context.Rip;
        const Module* module = ModuleAt(modules, context.Rip);
        const RUNTIME_FUNCTION* function = module != nullptr && module->functions != nullptr ? FunctionAt(*module, context.Rip - module->base) : nullptr;
        if (function != nullptr) {
            if (!FrameReadable(*module, function, context, low, high)) break;
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, module->base, context.Rip, const_cast<PRUNTIME_FUNCTION>(function), &context, &handlerData, &establisher, nullptr);
        } else {
            if (context.Rsp < low || context.Rsp > high || high - context.Rsp < sizeof(DWORD64)) break;
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += sizeof(DWORD64);
        }
        if (context.Rsp < low || context.Rsp > high) break;
    }
    return count;
}

std::uint64_t CpuTime(HANDLE thread) {
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) return 0;
    const auto ticks = [](const FILETIME& time) { return (std::uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime; };
    return ticks(kernel) + ticks(user);
}

std::string ThreadName(HANDLE thread) {
    using Describe = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const Describe describe = [] {
        const HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
        return kernelbase != nullptr ? reinterpret_cast<Describe>(reinterpret_cast<void*>(GetProcAddress(kernelbase, "GetThreadDescription"))) : nullptr;
    }();
    if (describe == nullptr) return {};
    PWSTR text = nullptr;
    if (FAILED(describe(thread, &text)) || text == nullptr) return {};
    std::string name = Narrow(text);
    LocalFree(text);
    return name;
}

void WalkThreads(std::vector<Captured>& threads, const std::vector<Module>& modules, QueryThread query) {
    for (Captured& entry : threads) {
        if (entry.handle == nullptr || SuspendThread(entry.handle) == static_cast<DWORD>(-1)) continue;
        ThreadBasics basics;
        ULONG length = 0;
        if (query != nullptr && query(entry.handle, ThreadBasicInformation, &basics, sizeof(basics), &length) == 0 && length == sizeof(basics) && basics.environment != nullptr) {
            std::memcpy(&entry.lastError, static_cast<const std::uint8_t*>(basics.environment) + kTebLastError, sizeof(entry.lastError));
            entry.lastErrorRead = true;
        }
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        MEMORY_BASIC_INFORMATION stack{};
        if (GetThreadContext(entry.handle, &context) && VirtualQuery(reinterpret_cast<const void*>(context.Rsp), &stack, sizeof(stack)) == sizeof(stack) &&
            stack.State == MEM_COMMIT && (stack.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0) {
            const std::uint64_t low = reinterpret_cast<std::uint64_t>(stack.BaseAddress);
            entry.count = Walk(modules, context, low, low + stack.RegionSize, entry.frames);
            entry.walked = true;
        }
        ResumeThread(entry.handle);
    }
}

void ReadExports(Module& module) {
    module.exportsRead = true;
    const IMAGE_DATA_DIRECTORY* directory = Directory(module, IMAGE_DIRECTORY_ENTRY_EXPORT, sizeof(IMAGE_EXPORT_DIRECTORY));
    if (directory == nullptr) return;
    const auto* exported = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(module.base + directory->VirtualAddress);
    const std::uint64_t names = exported->NumberOfNames;
    const std::uint64_t functions = exported->NumberOfFunctions;
    const auto fits = [&](std::uint64_t rva, std::uint64_t bytes) { return rva != 0 && rva <= module.size && bytes <= module.size - rva; };
    if (names == 0 || names > kExportNames || functions > kExportNames || !fits(exported->AddressOfNames, names * 4) || !fits(exported->AddressOfNameOrdinals, names * 2) ||
        !fits(exported->AddressOfFunctions, functions * 4)) {
        return;
    }
    const auto* nameRvas = reinterpret_cast<const std::uint32_t*>(module.base + exported->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const std::uint16_t*>(module.base + exported->AddressOfNameOrdinals);
    const auto* functionRvas = reinterpret_cast<const std::uint32_t*>(module.base + exported->AddressOfFunctions);
    module.exports.reserve(static_cast<std::size_t>(names));
    for (std::uint64_t index = 0; index < names; ++index) {
        if (ordinals[index] >= functions) continue;
        const std::uint32_t rva = functionRvas[ordinals[index]];
        const std::uint32_t nameRva = nameRvas[index];
        if (rva == 0 || rva >= module.size || nameRva == 0 || nameRva >= module.size) continue;
        if (rva >= directory->VirtualAddress && rva - directory->VirtualAddress < directory->Size) continue;
        const char* name = reinterpret_cast<const char*>(module.base + nameRva);
        const std::size_t length = strnlen(name, static_cast<std::size_t>((std::min)(std::uint64_t{kNameChars}, module.size - nameRva)));
        module.exports.push_back(Symbol{rva, std::string(name, length)});
    }
    std::sort(module.exports.begin(), module.exports.end(), [](const Symbol& left, const Symbol& right) { return left.rva < right.rva; });
}

std::string Readable(const std::string& name) {
    std::string text = name;
#if defined(__GNUC__)
    int status = 0;
    char* const demangled = abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &status);
    if (demangled != nullptr) {
        if (status == 0) text = demangled;
        std::free(demangled);
    }
#endif
    if (text.size() > kNameChars) {
        text.resize(kNameChars);
        text += "...";
    }
    return text;
}

void ReadSymbols(Module& module) {
    module.symbolsRead = true;
    if (module.path.empty()) return;
    std::ifstream file(std::filesystem::path(module.path), std::ios::binary | std::ios::ate);
    if (!file) return;
    const std::streamoff length = file.tellg();
    if (length <= 0 || static_cast<std::uint64_t>(length) > kSymbolFileLimit) return;
    std::vector<char> data(static_cast<std::size_t>(length));
    file.seekg(0);
    if (!file.read(data.data(), length)) return;
    const std::uint64_t size = data.size();
    const auto fits = [&](std::uint64_t at, std::uint64_t bytes) { return at <= size && bytes <= size - at; };
    IMAGE_DOS_HEADER dos{};
    if (!fits(0, sizeof(dos))) return;
    std::memcpy(&dos, data.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) return;
    const std::uint64_t headerAt = static_cast<std::uint64_t>(dos.e_lfanew) + sizeof(DWORD);
    IMAGE_FILE_HEADER header{};
    if (!fits(headerAt, sizeof(header))) return;
    std::memcpy(&header, data.data() + headerAt, sizeof(header));
    const std::uint64_t sectionsAt = headerAt + sizeof(header) + header.SizeOfOptionalHeader;
    const std::uint64_t symbolsAt = header.PointerToSymbolTable;
    const std::uint64_t symbolCount = header.NumberOfSymbols;
    if (symbolsAt == 0 || symbolCount == 0 || !fits(sectionsAt, std::uint64_t{header.NumberOfSections} * sizeof(IMAGE_SECTION_HEADER)) ||
        !fits(symbolsAt, symbolCount * IMAGE_SIZEOF_SYMBOL)) {
        return;
    }
    const std::uint64_t stringsAt = symbolsAt + symbolCount * IMAGE_SIZEOF_SYMBOL;
    std::vector<IMAGE_SECTION_HEADER> sections(header.NumberOfSections);
    if (!sections.empty()) std::memcpy(sections.data(), data.data() + sectionsAt, sections.size() * sizeof(IMAGE_SECTION_HEADER));
    for (std::uint64_t index = 0; index < symbolCount;) {
        IMAGE_SYMBOL symbol{};
        std::memcpy(&symbol, data.data() + symbolsAt + index * IMAGE_SIZEOF_SYMBOL, IMAGE_SIZEOF_SYMBOL);
        index += 1 + std::uint64_t{symbol.NumberOfAuxSymbols};
        if (symbol.SectionNumber <= 0 || static_cast<std::size_t>(symbol.SectionNumber) > sections.size() || !ISFCN(symbol.Type)) continue;
        const IMAGE_SECTION_HEADER& section = sections[static_cast<std::size_t>(symbol.SectionNumber) - 1];
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
        std::string name;
        if (symbol.N.Name.Short != 0) {
            const char* shortName = reinterpret_cast<const char*>(symbol.N.ShortName);
            name.assign(shortName, strnlen(shortName, sizeof(symbol.N.ShortName)));
        } else {
            const std::uint64_t nameAt = stringsAt + symbol.N.Name.Long;
            if (nameAt >= size) continue;
            name.assign(data.data() + nameAt, strnlen(data.data() + nameAt, static_cast<std::size_t>(size - nameAt)));
        }
        if (name.empty() || name.front() == '.') continue;
        module.symbols.push_back(Symbol{static_cast<std::uint32_t>(section.VirtualAddress + symbol.Value), std::move(name)});
    }
    std::sort(module.symbols.begin(), module.symbols.end(), [](const Symbol& left, const Symbol& right) { return left.rva < right.rva; });
}

const Symbol* Nearest(const std::vector<Symbol>& symbols, std::uint64_t rva) {
    auto after = std::upper_bound(symbols.begin(), symbols.end(), rva, [](std::uint64_t value, const Symbol& symbol) { return value < symbol.rva; });
    if (after == symbols.begin()) return nullptr;
    return &*std::prev(after);
}

bool OwnModule(const std::string& name) {
    return name.size() > 3 && (name[0] == 'a' || name[0] == 'A') && (name[1] == 'b' || name[1] == 'B') && name[2] == '_';
}

std::string CodeText(std::vector<Module>& modules, std::uint64_t address) {
    char text[96];
    Module* module = const_cast<Module*>(ModuleAt(modules, address));
    if (module == nullptr) {
        std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
        return text;
    }
    const std::uint64_t rva = address - module->base;
    if (OwnModule(module->name)) {
        if (!module->symbolsRead) ReadSymbols(*module);
        if (const Symbol* symbol = Nearest(module->symbols, rva)) {
            std::snprintf(text, sizeof(text), "+0x%llx", static_cast<unsigned long long>(rva - symbol->rva));
            return module->name + "!" + Readable(symbol->name) + text;
        }
    }
    if (!module->exportsRead) ReadExports(*module);
    const Symbol* exported = Nearest(module->exports, rva);
    if (exported != nullptr && rva - exported->rva < kExportReach) {
        std::snprintf(text, sizeof(text), "+0x%llx", static_cast<unsigned long long>(rva - exported->rva));
        return module->name + "!" + exported->name + text;
    }
    std::snprintf(text, sizeof(text), "+0x%llx", static_cast<unsigned long long>(rva));
    return module->name + text;
}

void AddWrapped(std::vector<std::string>& lines, const std::string& head, const std::vector<std::string>& parts, std::size_t lineChars) {
    std::string line = head;
    bool first = true;
    for (const std::string& part : parts) {
        if (!first && line.size() + part.size() + 4 > lineChars) {
            lines.push_back(line + " <-");
            line = "   ... " + part;
            continue;
        }
        line += first ? part : " <- " + part;
        first = false;
    }
    lines.push_back(line);
}

}

std::string DescribeCode(std::uint64_t address) {
    std::vector<Module> modules = LoadedModules();
    return CodeText(modules, address);
}

std::vector<std::string> DescribeThreadStacks(std::uint32_t sampleMilliseconds, std::size_t lineChars) {
    std::vector<std::string> lines;
    const QueryThread lastErrors = LastErrorQuery();
    std::vector<Module> modules = LoadedModules();
    std::vector<Captured> threads;
    threads.reserve(kThreads);
    std::size_t unlisted = 0;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        lines.emplace_back("the thread list could not be read (error " + std::to_string(GetLastError()) + ")");
        return lines;
    }
    THREADENTRY32 listed{};
    listed.dwSize = sizeof(listed);
    const DWORD process = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    for (BOOL more = Thread32First(snapshot, &listed); more; more = Thread32Next(snapshot, &listed)) {
        if (listed.th32OwnerProcessID != process || listed.th32ThreadID == self) continue;
        if (threads.size() == kThreads) {
            ++unlisted;
            continue;
        }
        Captured entry;
        entry.id = listed.th32ThreadID;
        threads.push_back(std::move(entry));
    }
    CloseHandle(snapshot);
    for (Captured& entry : threads) {
        entry.handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.id);
        if (entry.handle == nullptr) continue;
        entry.name = ThreadName(entry.handle);
    }
    WalkThreads(threads, modules, lastErrors);
    for (Captured& entry : threads) {
        if (entry.handle != nullptr) entry.cpuBefore = CpuTime(entry.handle);
    }
    Sleep(sampleMilliseconds);
    std::size_t walked = 0;
    for (Captured& entry : threads) {
        if (entry.handle == nullptr) continue;
        entry.cpuAfter = CpuTime(entry.handle);
        CloseHandle(entry.handle);
        entry.handle = nullptr;
        walked += entry.walked ? 1 : 0;
    }
    std::map<std::vector<std::uint64_t>, std::vector<const Captured*>> groups;
    for (const Captured& entry : threads) groups[std::vector<std::uint64_t>(entry.frames, entry.frames + entry.count)].push_back(&entry);
    const auto cpu = [](const Captured* entry) { return entry->cpuAfter >= entry->cpuBefore ? (entry->cpuAfter - entry->cpuBefore) / 10000 : 0; };
    std::vector<const std::pair<const std::vector<std::uint64_t>, std::vector<const Captured*>>*> ordered;
    for (const auto& group : groups) ordered.push_back(&group);
    const auto busiest = [&](const std::vector<const Captured*>& members) {
        std::uint64_t most = 0;
        for (const Captured* entry : members) most = (std::max)(most, cpu(entry));
        return most;
    };
    std::stable_sort(ordered.begin(), ordered.end(), [&](const auto* left, const auto* right) { return busiest(left->second) > busiest(right->second); });
    char text[480];
    std::snprintf(text, sizeof(text),
                  "%zu other thread(s), %zu stack(s) read, %zu different stack(s), %zu thread(s) past the first %zu not looked at; CPU time is what a thread used in the %u ms "
                  "after the stacks were read; %s",
                  threads.size(), walked, groups.size(), unlisted, kThreads, sampleMilliseconds,
                  lastErrors != nullptr ? "a thread's last error is shown when it is not 0 (the value GetLastError would return there; it can be old)"
                                        : "last errors are not shown, as the place SetLastError writes was not found where it is expected");
    lines.emplace_back(text);
    for (const auto* group : ordered) {
        std::string head = group->second.size() == 1 ? "thread " : std::to_string(group->second.size()) + " threads with the same stack: ";
        std::size_t named = 0;
        for (const Captured* entry : group->second) {
            if (named == kThreadsNamed) {
                head += ", and " + std::to_string(group->second.size() - named) + " more";
                break;
            }
            std::snprintf(text, sizeof(text), "%s%lu%s%s%s (%llu ms CPU", named == 0 ? "" : ", ", static_cast<unsigned long>(entry->id), entry->name.empty() ? "" : " \"",
                          entry->name.substr(0, 48).c_str(), entry->name.empty() ? "" : "\"", static_cast<unsigned long long>(cpu(entry)));
            head += text;
            if (entry->lastErrorRead && entry->lastError != 0) head += ", last error " + std::to_string(entry->lastError);
            head += ")";
            ++named;
        }
        if (group->first.empty()) {
            lines.push_back(head + ": its stack could not be read");
            continue;
        }
        std::vector<std::string> parts;
        parts.reserve(group->first.size());
        for (const std::uint64_t frame : group->first) parts.push_back(CodeText(modules, frame));
        AddWrapped(lines, head + ": ", parts, lineChars);
    }
    return lines;
}

}
