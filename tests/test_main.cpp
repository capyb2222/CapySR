// Small assert-based tests; the wire codec is hand written so it gets the most cover.
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "core/logger.h"
#include "core/util.h"
#include "net/cmd_ids.h"
#include "net/packet.h"
#include "net/session.h"
#include "proto/gen/protos.h"
#include "tests/harness.h"

namespace {

int g_failures = 0;
int g_checks = 0;

}  // namespace

namespace testing {

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

int failures() { return g_failures; }
int checks() { return g_checks; }

}  // namespace testing

namespace {

using testing::check;

void testVarint() {
    pb::Writer w;
    w.writeVarint(0);
    w.writeVarint(1);
    w.writeVarint(127);
    w.writeVarint(128);
    w.writeVarint(300);
    w.writeVarint(0xFFFFFFFFull);
    w.writeVarint(0xFFFFFFFFFFFFFFFFull);

    pb::Reader r(w.data());
    uint64_t v = 0;
    check(r.readVarint(v) && v == 0, "varint 0");
    check(r.readVarint(v) && v == 1, "varint 1");
    check(r.readVarint(v) && v == 127, "varint 127");
    check(r.readVarint(v) && v == 128, "varint 128");
    check(r.readVarint(v) && v == 300, "varint 300");
    check(r.readVarint(v) && v == 0xFFFFFFFFull, "varint u32 max");
    check(r.readVarint(v) && v == 0xFFFFFFFFFFFFFFFFull, "varint u64 max");
    check(r.eof(), "varint stream drained");
}

void testFixedAndZigzag() {
    pb::Writer w;
    w.writeFixed32(0xDEADBEEF);
    w.writeFixed64(0x0123456789ABCDEFull);
    w.writeFloat(1.5f);
    w.writeDouble(-2.25);
    w.writeSInt32(-1);
    w.writeSInt64(-123456789);

    pb::Reader r(w.data());
    uint32_t f32 = 0;
    uint64_t f64 = 0;
    float f = 0;
    double d = 0;
    int32_t s32 = 0;
    int64_t s64 = 0;
    check(r.readFixed32(f32) && f32 == 0xDEADBEEF, "fixed32");
    check(r.readFixed64(f64) && f64 == 0x0123456789ABCDEFull, "fixed64");
    check(r.readFloat(f) && f == 1.5f, "float");
    check(r.readDouble(d) && d == -2.25, "double");
    check(r.readSInt32(s32) && s32 == -1, "sint32");
    check(r.readSInt64(s64) && s64 == -123456789, "sint64");
}

void testMessageRoundTrip() {
    proto::PlayerLoginScRsp rsp;
    rsp.retcode = 0;
    rsp.login_random = 0x1122334455667788ull;
    rsp.server_timestamp_ms = 1750000000000ull;
    rsp.stamina = 240;
    auto& info = rsp.basic_info.emplace();
    info.nickname = "Capybara";
    info.level = 70;
    info.world_level = 6;
    info.hcoin = 999999;

    std::string bytes = rsp.serialize();
    proto::PlayerLoginScRsp back;
    check(back.parse(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), "login parse");
    check(back.login_random == rsp.login_random, "login_random survives");
    check(back.server_timestamp_ms == rsp.server_timestamp_ms, "timestamp survives");
    check(back.stamina == 240, "stamina survives");
    check(static_cast<bool>(back.basic_info), "basic_info present");
    check(back.basic_info->nickname == "Capybara", "nickname survives");
    check(back.basic_info->level == 70, "level survives");
    check(back.basic_info->hcoin == 999999, "hcoin survives");

    // proto3 skips default values, so an all-default message is zero bytes.
    proto::PlayerLoginFinishScRsp empty;
    check(empty.serialize().empty(), "defaults are not encoded");
}

void testRepeatedAndNested() {
    proto::GetBagScRsp bag;
    for (uint32_t i = 0; i < 5; ++i) {
        auto& item = bag.relic_list.emplace_back();
        item.tid = 61000 + i;
        item.level = 15;
        item.unique_id = 100 + i;
    }
    std::string bytes = bag.serialize();
    proto::GetBagScRsp back;
    check(back.parse(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), "bag parse");
    check(back.relic_list.size() == 5, "repeated message count");
    check(back.relic_list[4].tid == 61004, "repeated message content");
}

void testPacketFraming() {
    std::string body = "hello";
    std::string a = net::encodePacket(cmd::PlayerLoginScRsp, {}, body);
    std::string b = net::encodePacket(cmd::PlayerHeartBeatScRsp, {}, "");
    std::string joined = a + b;

    std::vector<net::Packet> packets;
    std::string error;
    check(net::decodePackets(joined, packets, &error), "framing decode");
    check(packets.size() == 2, "two packets in one message");
    check(packets[0].cmdId == cmd::PlayerLoginScRsp, "first cmd id");
    check(packets[0].body == body, "first body");
    check(packets[1].cmdId == cmd::PlayerHeartBeatScRsp, "second cmd id");
    check(packets[1].body.empty(), "second body empty");

    std::string broken = a;
    broken[0] = 0;
    packets.clear();
    check(!net::decodePackets(broken, packets, &error), "bad magic rejected");
}

void testBase64() {
    check(util::base64Encode("") == "", "b64 empty");
    check(util::base64Encode("f") == "Zg==", "b64 1 byte");
    check(util::base64Encode("fo") == "Zm8=", "b64 2 bytes");
    check(util::base64Encode("foo") == "Zm9v", "b64 3 bytes");
    check(util::base64Encode("foobar") == "Zm9vYmFy", "b64 6 bytes");
    check(util::base64Decode("Zm9vYmFy") == "foobar", "b64 decode");
    std::string binary("\x00\x01\xFF\xFE", 4);
    check(util::base64Decode(util::base64Encode(binary)) == binary, "b64 binary round trip");
}

void testCmdIds() {
    check(cmd::name(cmd::PlayerLoginCsReq) == "PlayerLoginCsReq", "cmd name lookup");
    check(cmd::id("PlayerLoginScRsp") == cmd::PlayerLoginScRsp, "cmd id lookup");
    check(cmd::id("NotAPacket") == 0, "unknown cmd id");
}

void testLineupCmdIds() {
    // 716 is JoinLineupScRsp and 719 ReplaceLineupScRsp; the dump's own table had them
    // mislabelled. Answering a join on 719 leaves the party UI waiting forever, so the
    // names are corrected in proto/cmdids.txt.
    check(cmd::JoinLineupScRsp == 716, "JoinLineupScRsp is 716");
    check(cmd::ReplaceLineupScRsp == 719, "ReplaceLineupScRsp is 719");
    check(cmd::name(716) == "JoinLineupScRsp", "716 names itself");
    check(cmd::name(719) == "ReplaceLineupScRsp", "719 names itself");
    // The generic fallback derives the response name from the request name, so
    // ReplaceLineupCsReq now resolves even without a handler.
    check(cmd::id("ReplaceLineupScRsp") == 719, "the reverse lookup agrees");
    check(cmd::id("JoinLineupScRsp") == 716, "and for the join");
}

void testIdleTimeout() {
    constexpr uint64_t kTimeout = 60000;
    check(!net::isIdle(1000, 1000, kTimeout), "no time passed is not idle");
    check(!net::isIdle(61000, 1000, kTimeout), "exactly the timeout is not idle yet");
    check(net::isIdle(61001, 1000, kTimeout), "past the timeout is idle");

    // The updater thread captures `now` and then walks the session list, so the receive
    // thread can move `last` past it. Subtracting first underflows to ~1.8e19 and drops
    // every live session mid-login, which is what "disconnected (idle timeout)" 156 ms
    // into a session turned out to be.
    check(!net::isIdle(1000, 1001, kTimeout), "activity newer than now is not idle");
    check(!net::isIdle(0, 1, kTimeout), "and it cannot underflow at zero");
    check(!net::isIdle(1000, 0xFFFFFFFFFFFFFFFFull, kTimeout), "nor at the top of the range");
}

void testFillPresence() {
    // An absent sub-message and a present-but-empty one are different on the wire, and
    // the client throws on the former. fill() is what makes the difference.
    proto::GetCurBattleInfoScRsp bare;
    check(bare.serialize().empty(), "an unfilled response is zero bytes");
    check(!bare.battle_info, "and its sub-message is absent");

    proto::GetCurBattleInfoScRsp filled;
    filled.fill(1);
    check(static_cast<bool>(filled.battle_info), "fill emplaces the sub-message");
    std::string bytes = filled.serialize();
    check(!bytes.empty(), "so the body is no longer empty");

    proto::GetCurBattleInfoScRsp back;
    check(back.parse(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()),
          "a filled response parses");
    check(static_cast<bool>(back.battle_info), "and the sub-message survives as present");
    check(back.battle_info->battle_id == 0, "still carrying nothing");

    // Depth is honoured, so filling cannot walk a recursive type forever.
    proto::GetCurBattleInfoScRsp zero;
    zero.fill(0);
    check(!zero.battle_info, "depth 0 fills nothing");

    // One level down: SceneBattleInfo's own sub-messages appear only at depth 2.
    proto::GetCurBattleInfoScRsp deep;
    deep.fill(2);
    check(deep.battle_info && static_cast<bool>(deep.battle_info->battle_action_event_id),
          "depth 2 reaches the second level");
    proto::GetCurBattleInfoScRsp shallow;
    shallow.fill(1);
    check(shallow.battle_info && !shallow.battle_info->battle_action_event_id,
          "depth 1 stops at the first");

    // The one that was disconnecting the session.
    proto::GetRogueHandbookDataScRsp handbook;
    handbook.fill(2);
    check(!handbook.serialize().empty(), "the rogue handbook reply is not empty");
}

void testLargeMessage() {
    // Exercises the length back-patch for payloads over 127 bytes.
    proto::PlayerLoginScRsp rsp;
    rsp.basic_info.emplace().nickname = std::string(5000, 'x');
    std::string bytes = rsp.serialize();
    proto::PlayerLoginScRsp back;
    check(back.parse(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), "large parse");
    check(back.basic_info && back.basic_info->nickname.size() == 5000, "large string survives");
}

void testAgainstProtoc() {
    // Bytes produced by google protobuf for BattleBuff; covers packed repeated and a
    // string->float map, the two shapes the generator is most likely to get wrong.
    static const char kBytes[] =
        "\x08\xb0\x85\x3d\x10\x01\x18\xff\xff\xff\xff\x0f\x20\xff\xff\xff\xff\x0f\x2a\x05\x00"
        "\x01\x02\xac\x02\x32\x0c\x0a\x05\x52\x61\x74\x69\x6f\x15\x00\x00\x00\x3f\x32\x11\x0a"
        "\x0a\x53\x6b\x69\x6c\x6c\x49\x6e\x64\x65\x78\x15\x00\x00\x80\x3f";
    const size_t size = sizeof(kBytes) - 1;

    proto::BattleBuff buff;
    check(buff.parse(reinterpret_cast<const uint8_t*>(kBytes), size), "protoc bytes parse");
    check(buff.id == 1000112, "protoc id");
    check(buff.level == 1, "protoc level");
    check(buff.owner_index == 0xFFFFFFFFu, "protoc owner_index");
    check(buff.wave_flag == 0xFFFFFFFFu, "protoc wave_flag");
    check(buff.target_index_list.size() == 4, "protoc packed count");
    check(buff.target_index_list[3] == 300, "protoc packed multibyte value");
    check(buff.dynamic_values.size() == 2, "protoc map count");
    check(buff.dynamic_values["SkillIndex"] == 1.0f, "protoc map value");
    check(buff.dynamic_values["Ratio"] == 0.5f, "protoc map second value");

    // Re-encoding must produce the same wire bytes (map order is sorted either way).
    std::string again = buff.serialize();
    check(again.size() == size, "re-encoded size matches protoc");
    check(again == std::string(kBytes, size), "re-encoded bytes match protoc");
}

void testLogFormat() {
    using logging::Level;
    namespace detail = logging::detail;
    const char* t = "12:00:00.000";

    check(detail::consoleLine(Level::Info, t, "game", "hello", false) ==
              "12:00:00.000  INFO   game      hello\n",
          "console lines keep their columns");
    check(detail::consoleLine(Level::Info, t, "challenge", "x", false) ==
              "12:00:00.000  INFO   challenge x\n",
          "the longest tag still leaves a gap");
    check(detail::consoleLine(Level::Debug, t, "recv", "DoGachaCsReq (1905) 5 bytes", false) ==
              "12:00:00.000  DEBUG  recv      <- DoGachaCsReq (1905) 5 bytes\n",
          "packets get a direction arrow");
    check(detail::consoleLine(Level::Warn, t, "net", "first\nsecond", false) ==
              "12:00:00.000  WARN   net       first\n" + std::string(31, ' ') + "second\n",
          "a second line lines up under the first");
    check(detail::fileLine(Level::Info, t, "game", "hello") == "12:00:00.000 INFO  game hello\n",
          "the file keeps its plain format");

    std::string colored = detail::consoleLine(Level::Debug, t, "send", "X (1) 0 bytes", true);
    check(colored.find("\033[") != std::string::npos && colored.find("→") != std::string::npos,
          "the console gets colour and arrows");

    std::string box = detail::bannerBox("CapySR is ready",
                                        {{"dispatch", "http://127.0.0.1:21000"}, {"game", "x"}}, false);
    std::vector<std::string> lines;
    for (std::string_view rest = box; !rest.empty();) {
        size_t end = rest.find('\n');
        lines.emplace_back(rest.substr(0, end));
        rest.remove_prefix(end + 1);
    }
    bool even = lines.size() == 4;
    for (const std::string& line : lines) even &= line.size() == lines[0].size();
    check(even, "every banner line is the same width");
    check(box.rfind("+- CapySR is ready -", 0) == 0 &&
              box.find("| dispatch  http://127.0.0.1:21000 |") != std::string::npos,
          "the banner names its rows");
}

}  // namespace

int main() {
    testVarint();
    testFixedAndZigzag();
    testMessageRoundTrip();
    testRepeatedAndNested();
    testPacketFraming();
    testBase64();
    testCmdIds();
    testLineupCmdIds();
    testIdleTimeout();
    testLogFormat();
    testFillPresence();
    testLargeMessage();
    testAgainstProtoc();
    runGameTests();
    runHttpTests();
    runFlowTests();

    std::printf("%d checks, %d failures\n", testing::checks(), testing::failures());
    return testing::failures() == 0 ? 0 : 1;
}
