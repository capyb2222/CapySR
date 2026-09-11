#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace net {

constexpr uint32_t kHeadMagic = 0x9D74C714;
constexpr uint32_t kTailMagic = 0xD7A152C8;
constexpr size_t kPacketOverhead = 16;

constexpr int kKcpMtu = 1400;
// Non-stream KCP splits one message across at most 255 fragments, and each fragment
// pays the 28 byte Star Rail header. Anything bigger cannot be sent at all.
constexpr size_t kMaxKcpMessage = 255 * (kKcpMtu - 28);

struct Packet {
    uint16_t cmdId = 0;
    std::string head;
    std::string body;
};

std::string encodePacket(uint16_t cmdId, std::string_view head, std::string_view body);

// A KCP message may carry several packets back to back.
bool decodePackets(std::string_view data, std::vector<Packet>& out, std::string* error);

}  // namespace net
