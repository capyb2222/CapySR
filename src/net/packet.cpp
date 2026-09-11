#include "net/packet.h"

namespace net {
namespace {

uint32_t readBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint16_t readBE16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

void appendBE32(std::string& out, uint32_t v) {
    out += static_cast<char>((v >> 24) & 0xFF);
    out += static_cast<char>((v >> 16) & 0xFF);
    out += static_cast<char>((v >> 8) & 0xFF);
    out += static_cast<char>(v & 0xFF);
}

void appendBE16(std::string& out, uint16_t v) {
    out += static_cast<char>((v >> 8) & 0xFF);
    out += static_cast<char>(v & 0xFF);
}

}  // namespace

std::string encodePacket(uint16_t cmdId, std::string_view head, std::string_view body) {
    std::string out;
    out.reserve(kPacketOverhead + head.size() + body.size());
    appendBE32(out, kHeadMagic);
    appendBE16(out, cmdId);
    appendBE16(out, static_cast<uint16_t>(head.size()));
    appendBE32(out, static_cast<uint32_t>(body.size()));
    out.append(head);
    out.append(body);
    appendBE32(out, kTailMagic);
    return out;
}

bool decodePackets(std::string_view data, std::vector<Packet>& out, std::string* error) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
    size_t left = data.size();

    while (left >= 12) {
        if (readBE32(p) != kHeadMagic) {
            if (error) *error = "head magic mismatch";
            return false;
        }
        Packet packet;
        packet.cmdId = readBE16(p + 4);
        size_t headLen = readBE16(p + 6);
        size_t bodyLen = readBE32(p + 8);

        size_t total = 12 + headLen + bodyLen + 4;
        if (total > left) {
            if (error) *error = "truncated packet";
            return false;
        }
        if (readBE32(p + 12 + headLen + bodyLen) != kTailMagic) {
            if (error) *error = "tail magic mismatch";
            return false;
        }
        packet.head.assign(reinterpret_cast<const char*>(p + 12), headLen);
        packet.body.assign(reinterpret_cast<const char*>(p + 12 + headLen), bodyLen);
        out.push_back(std::move(packet));

        p += total;
        left -= total;
    }
    return true;
}

}  // namespace net
