#pragma once

#include <cstdint>
#include <string>

namespace abx::d3d12x {

bool DiscordPresenceConfigured();
void DiscordPresenceTick(std::uint64_t framesPerSecond);

std::string DiscordFrame(std::uint32_t opcode, const std::string& json);
std::string JsonEscape(const std::string& text);
std::string DiscordActivityJson(std::uint32_t processId, const std::string& details, const std::string& state, std::int64_t startSeconds, const std::string& largeImage,
                                const std::string& largeText, std::uint64_t nonce);

}
