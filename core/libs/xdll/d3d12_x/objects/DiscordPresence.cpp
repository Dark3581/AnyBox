#include "DiscordPresence.hpp"

#include "AbxApi.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace abx::d3d12x {

namespace {

constexpr std::uint32_t kHandshake = 0;
constexpr std::uint32_t kFrame = 1;
constexpr std::uint32_t kClose = 2;
constexpr std::uint32_t kMaxPayload = 64 * 1024;
constexpr auto kSendEvery = std::chrono::seconds(15);
constexpr auto kRetryEvery = std::chrono::seconds(30);
constexpr auto kReplyWait = std::chrono::milliseconds(2000);

std::string Setting(const char* key, const char* fallback) {
    const char* value = AbxSetting(key);
    return value != nullptr && value[0] != 0 ? std::string(value) : std::string(fallback);
}

std::string ClientId() {
    const std::string id = Setting("discord.client_id", "");
    if (id.empty() || id.size() > 24) return {};
    for (const char c : id) {
        if (c < '0' || c > '9') return {};
    }
    return id;
}

struct Connection {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::chrono::steady_clock::time_point nextAttempt{};
    std::chrono::steady_clock::time_point nextSend{};
    std::int64_t started = 0;
    std::uint64_t nonce = 0;
    bool reportedAbsent = false;
    bool reportedConnected = false;
    bool reportedOff = false;
};

Connection& State() {
    static Connection connection;
    return connection;
}

void Close(Connection& connection) {
    if (connection.pipe != INVALID_HANDLE_VALUE) CloseHandle(connection.pipe);
    connection.pipe = INVALID_HANDLE_VALUE;
}

bool WriteAll(HANDLE pipe, const std::string& bytes) {
    DWORD written = 0;
    return WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
}

bool ReadExact(HANDLE pipe, void* out, DWORD bytes, std::chrono::steady_clock::time_point deadline) {
    auto* target = static_cast<std::uint8_t*>(out);
    DWORD done = 0;
    while (done < bytes) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available == 0) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            Sleep(20);
            continue;
        }
        DWORD read = 0;
        const DWORD want = (std::min)(available, bytes - done);
        if (!ReadFile(pipe, target + done, want, &read, nullptr) || read == 0) return false;
        done += read;
    }
    return true;
}

bool ReadFrame(HANDLE pipe, std::uint32_t& opcode, std::string& payload, std::chrono::steady_clock::time_point deadline) {
    std::uint32_t header[2] = {};
    if (!ReadExact(pipe, header, sizeof(header), deadline)) return false;
    if (header[1] > kMaxPayload) return false;
    opcode = header[0];
    payload.assign(header[1], '\0');
    return header[1] == 0 || ReadExact(pipe, payload.data(), header[1], deadline);
}

void Drain(Connection& connection) {
    for (int frames = 0; frames < 16; ++frames) {
        DWORD available = 0;
        if (!PeekNamedPipe(connection.pipe, nullptr, 0, nullptr, &available, nullptr)) {
            Close(connection);
            return;
        }
        if (available < 8) return;
        std::uint32_t opcode = 0;
        std::string payload;
        if (!ReadFrame(connection.pipe, opcode, payload, std::chrono::steady_clock::now() + kReplyWait)) {
            Close(connection);
            return;
        }
        if (opcode == kClose) {
            AbxLog("d3d12_x", "Discord closed the activity connection: %s", payload.c_str());
            Close(connection);
            return;
        }
        if (payload.find("\"evt\":\"ERROR\"") != std::string::npos) AbxLog("d3d12_x", "Discord refused the activity: %s", payload.c_str());
    }
}

bool Connect(Connection& connection, const std::string& clientId) {
    for (int index = 0; index < 10; ++index) {
        const std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(index);
        const HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) continue;
        connection.pipe = pipe;
        if (!WriteAll(pipe, DiscordFrame(kHandshake, "{\"v\":1,\"client_id\":\"" + clientId + "\"}"))) {
            Close(connection);
            continue;
        }
        std::uint32_t opcode = 0;
        std::string payload;
        if (!ReadFrame(pipe, opcode, payload, std::chrono::steady_clock::now() + kReplyWait)) {
            Close(connection);
            continue;
        }
        if (opcode != kFrame || payload.find("\"READY\"") == std::string::npos) {
            AbxLog("d3d12_x", "Discord refused the handshake for application %s (opcode %u): %s", clientId.c_str(), opcode, payload.c_str());
            Close(connection);
            return false;
        }
        if (!connection.reportedConnected) {
            connection.reportedConnected = true;
            AbxLog("d3d12_x", "Discord activity is shown through application %s (pipe discord-ipc-%d)", clientId.c_str(), index);
        }
        return true;
    }
    if (!connection.reportedAbsent) {
        connection.reportedAbsent = true;
        AbxLog("d3d12_x", "Discord activity: no Discord client answers on discord-ipc-0..9; trying again every 30 s");
    }
    return false;
}

}

std::string DiscordFrame(std::uint32_t opcode, const std::string& json) {
    std::string frame(8, '\0');
    const std::uint32_t length = static_cast<std::uint32_t>(json.size());
    std::memcpy(frame.data(), &opcode, 4);
    std::memcpy(frame.data() + 4, &length, 4);
    return frame + json;
}

std::string JsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 2);
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (u < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", u);
            out += escaped;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string DiscordActivityJson(std::uint32_t processId, const std::string& details, const std::string& state, std::int64_t startSeconds, const std::string& largeImage,
                                const std::string& largeText, std::uint64_t nonce) {
    std::string activity = "{\"details\":\"" + JsonEscape(details) + "\",\"state\":\"" + JsonEscape(state) + "\",\"timestamps\":{\"start\":" + std::to_string(startSeconds) + "}";
    if (!largeImage.empty()) activity += ",\"assets\":{\"large_image\":\"" + JsonEscape(largeImage) + "\",\"large_text\":\"" + JsonEscape(largeText) + "\"}";
    activity += "}";
    return "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(processId) + ",\"activity\":" + activity + "},\"nonce\":\"" + std::to_string(nonce) + "\"}";
}

bool DiscordPresenceConfigured() {
    return !ClientId().empty();
}

void DiscordPresenceTick(std::uint64_t framesPerSecond) {
    Connection& connection = State();
    const std::string clientId = ClientId();
    if (clientId.empty()) {
        if (!connection.reportedOff) {
            connection.reportedOff = true;
            AbxLog("d3d12_x", "Discord activity is off: discord.client_id in anybox.cfg names no Discord application (create one at discord.com/developers, named as the game should appear)");
        }
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (connection.started == 0) {
        connection.started = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    if (connection.pipe == INVALID_HANDLE_VALUE) {
        if (now < connection.nextAttempt) return;
        connection.nextAttempt = now + kRetryEvery;
        if (!Connect(connection, clientId)) return;
        connection.nextSend = now;
    }
    Drain(connection);
    if (connection.pipe == INVALID_HANDLE_VALUE || now < connection.nextSend) return;
    connection.nextSend = now + kSendEvery;
    const std::string details = Setting("discord.details", "Running through AnyBox");
    const std::string state = framesPerSecond != 0 ? std::to_string(framesPerSecond) + " fps" : std::string("Loading");
    const std::string json = DiscordActivityJson(GetCurrentProcessId(), details, state, connection.started, Setting("discord.large_image", ""),
                                                 Setting("discord.large_text", Setting("d3d12_x.window_title", "").c_str()), ++connection.nonce);
    if (!WriteAll(connection.pipe, DiscordFrame(kFrame, json))) {
        AbxLog("d3d12_x", "Discord activity: writing to the Discord pipe failed (%lu); reconnecting in 30 s", GetLastError());
        Close(connection);
        connection.nextAttempt = now + kRetryEvery;
    }
}

}
