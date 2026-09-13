// Drives the real gateway over a real socket: handshake, KCP, framing, handlers.
// Everything below the game logic is the same code the client talks to.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <ikcp.h>

#include "core/config.h"
#include "core/util.h"
#include "game/handlers.h"
#include "game/lineup.h"
#include "net/cmd_ids.h"
#include "http/http_server.h"
#include "net/gateway.h"
#include "net/packet.h"
#include "net/socket.h"
#include "proto/gen/protos.h"
#include "sdk/admin.h"
#include "tests/harness.h"

namespace {

using testing::check;

constexpr uint16_t kTestPort = 23399;
constexpr uint16_t kAdminPort = 21098;
constexpr int kMtu = 1400;
constexpr uint32_t kConnectHead = 0x000000FF;
constexpr uint32_t kConnectTail = 0xFFFFFFFF;
constexpr uint32_t kAcceptHead = 0x00000145;

void writeBE32(char* p, uint32_t v) {
    p[0] = static_cast<char>((v >> 24) & 0xFF);
    p[1] = static_cast<char>((v >> 16) & 0xFF);
    p[2] = static_cast<char>((v >> 8) & 0xFF);
    p[3] = static_cast<char>(v & 0xFF);
}

uint32_t readBE32(const char* p) {
    const uint8_t* u = reinterpret_cast<const uint8_t*>(p);
    return (static_cast<uint32_t>(u[0]) << 24) | (static_cast<uint32_t>(u[1]) << 16) |
           (static_cast<uint32_t>(u[2]) << 8) | static_cast<uint32_t>(u[3]);
}

// The client half of the protocol, just enough of it to hold a conversation.
class TestClient {
public:
    ~TestClient() {
        if (kcp_ != nullptr) ikcp_release(kcp_);
        if (socket_ != net::kInvalidSocket) net::closeSocket(socket_);
    }

    bool connect(uint16_t port) {
        net::initSockets();
        socket_ = net::udpBind("127.0.0.1", 0);
        if (socket_ == net::kInvalidSocket) return false;
        net::setRecvTimeout(socket_, 200);

        server_.port = port;
        net::UdpAddress local;
        // 127.0.0.1 in network byte order.
        uint8_t loopback[4] = {127, 0, 0, 1};
        std::memcpy(&server_.ip, loopback, 4);
        (void)local;

        char handshake[20];
        writeBE32(handshake, kConnectHead);
        writeBE32(handshake + 4, 0);
        writeBE32(handshake + 8, 0);
        writeBE32(handshake + 12, 1234);
        writeBE32(handshake + 16, kConnectTail);

        for (int attempt = 0; attempt < 20; ++attempt) {
            net::udpSend(socket_, handshake, sizeof(handshake), server_);
            char reply[64];
            net::UdpAddress from;
            int n = net::udpRecv(socket_, reply, sizeof(reply), &from);
            if (n != 20) continue;
            if (readBE32(reply) != kAcceptHead) continue;
            conv_ = readBE32(reply + 4);
            token_ = readBE32(reply + 8);
            break;
        }
        if (conv_ == 0) return false;

        kcp_ = ikcp_create(conv_, this);
        ikcp_settoken(kcp_, token_);
        ikcp_setoutput(kcp_, &TestClient::output);
        ikcp_nodelay(kcp_, 1, 10, 2, 1);
        ikcp_wndsize(kcp_, 256, 256);
        ikcp_setmtu(kcp_, kMtu);
        return true;
    }

    template <class T>
    void send(uint16_t cmdId, const T& message) {
        queue(net::encodePacket(cmdId, {}, message.serialize()));
    }

    void sendEmpty(uint16_t cmdId) { queue(net::encodePacket(cmdId, {}, {})); }

    // ikcp_flush stamps segments from kcp->current, which only ikcp_update sets, so
    // never flush without updating first -- the same rule the server follows.
    void queue(const std::string& packet) {
        ikcp_send(kcp_, packet.data(), static_cast<int>(packet.size()));
        ikcp_update(kcp_, static_cast<uint32_t>(util::nowMs()));
        ikcp_flush(kcp_);
    }

    // Pumps until a packet with `cmdId` arrives; everything else is queued for later.
    bool await(uint16_t cmdId, net::Packet& out, int timeoutMs = 4000) {
        uint64_t deadline = util::nowMs() + static_cast<uint64_t>(timeoutMs);
        while (true) {
            for (size_t i = 0; i < inbox_.size(); ++i) {
                if (inbox_[i].cmdId != cmdId) continue;
                out = std::move(inbox_[i]);
                inbox_.erase(inbox_.begin() + static_cast<long>(i));
                return true;
            }
            if (util::nowMs() > deadline) return false;
            pump();
        }
    }

    bool sawNotify(uint16_t cmdId) {
        for (const net::Packet& packet : inbox_) {
            if (packet.cmdId == cmdId) return true;
        }
        return false;
    }

    // "waited for X, got Y and Z" is the difference between a one-line diagnosis and
    // a guessing game when a handler answers on the wrong cmd id.
    void reportInbox(uint16_t wanted) const {
        std::printf("      waited for %s (%u), inbox held:", cmd::name(wanted).c_str(), wanted);
        if (inbox_.empty()) std::printf(" nothing");
        for (const net::Packet& packet : inbox_) {
            std::printf(" %s(%u)", cmd::name(packet.cmdId).c_str(), packet.cmdId);
        }
        std::printf("\n");
    }

    void pump() {
        ikcp_update(kcp_, static_cast<uint32_t>(util::nowMs()));
        char datagram[2048];
        net::UdpAddress from;
        int n = net::udpRecv(socket_, datagram, sizeof(datagram), &from);
        if (n > 0) ikcp_input(kcp_, datagram, n);

        while (true) {
            int peek = ikcp_peeksize(kcp_);
            if (peek <= 0) break;
            std::vector<char> buffer(static_cast<size_t>(peek));
            int got = ikcp_recv(kcp_, buffer.data(), peek);
            if (got <= 0) break;
            std::vector<net::Packet> packets;
            std::string error;
            if (!net::decodePackets(std::string_view(buffer.data(), static_cast<size_t>(got)),
                                    packets, &error)) {
                std::printf("FAIL client could not decode a packet (%s)\n", error.c_str());
                break;
            }
            for (net::Packet& packet : packets) inbox_.push_back(std::move(packet));
        }
    }

private:
    static int output(const char* buf, int len, IKCPCB* kcp, void* user) {
        (void)kcp;
        auto* self = static_cast<TestClient*>(user);
        net::udpSend(self->socket_, buf, static_cast<size_t>(len), self->server_);
        return 0;
    }

    uintptr_t socket_ = net::kInvalidSocket;
    net::UdpAddress server_;
    IKCPCB* kcp_ = nullptr;
    uint32_t conv_ = 0;
    uint32_t token_ = 0;
    std::vector<net::Packet> inbox_;
};

// Minimal HTTP/1.1 GET, for the admin routes.
std::string httpGet(uint16_t port, const std::string& target) {
    uintptr_t sock = net::tcpConnect("127.0.0.1", port);
    if (sock == net::kInvalidSocket) return {};
    std::string req = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    req += "Content-Length: 0\r\nConnection: close\r\n\r\n";
    net::tcpSendAll(sock, req.data(), req.size());

    std::string reply;
    char buffer[4096];
    while (true) {
        int n = net::tcpRecv(sock, buffer, sizeof(buffer));
        if (n <= 0) break;
        reply.append(buffer, static_cast<size_t>(n));
    }
    net::closeSocket(sock);
    return reply;
}

template <class T>
bool parseBody(const net::Packet& packet, T& out) {
    return out.parse(reinterpret_cast<const uint8_t*>(packet.body.data()), packet.body.size());
}

}  // namespace

void runFlowTests() {
    // Keep the test off the real save file.
    core::Config::get().paths.playerFile = "build/test-player.json";
    game::registerAllHandlers();

    net::Gateway gateway;
    if (!gateway.start("127.0.0.1", kTestPort)) {
        std::printf("SKIP flow tests (udp %d is busy)\n", kTestPort);
        return;
    }

    TestClient client;
    if (!client.connect(kTestPort)) {
        std::printf("FAIL flow tests could not complete the handshake\n");
        gateway.stop();
        return;
    }
    check(true, "kcp handshake");

    net::Packet packet;

    proto::PlayerGetTokenCsReq tokenReq;
    tokenReq.account_uid = "capybara";
    client.send(cmd::PlayerGetTokenCsReq, tokenReq);
    if (!client.await(cmd::PlayerGetTokenScRsp, packet)) {
        std::printf("FAIL no PlayerGetTokenScRsp\n");
        gateway.stop();
        return;
    }
    proto::PlayerGetTokenScRsp tokenRsp;
    check(parseBody(packet, tokenRsp), "token response parses");
    check(tokenRsp.retcode == 0, "token accepted");
    check(tokenRsp.uid == core::Config::get().player.uid, "token carries the configured uid");

    proto::PlayerLoginCsReq loginReq;
    loginReq.login_random = 0x1122334455667788ull;
    loginReq.client_res_version = 51;
    client.send(cmd::PlayerLoginCsReq, loginReq);
    check(client.await(cmd::PlayerLoginScRsp, packet), "login answered");
    proto::PlayerLoginScRsp loginRsp;
    check(parseBody(packet, loginRsp), "login response parses");
    check(loginRsp.retcode == 0, "login accepted");
    check(loginRsp.login_random == loginReq.login_random, "login_random echoed");
    check(loginRsp.basic_info && loginRsp.basic_info->nickname == core::Config::get().player.name,
          "basic info carries the player name");

    client.sendEmpty(cmd::PlayerLoginFinishCsReq);
    check(client.await(cmd::PlayerLoginFinishScRsp, packet), "login finish answered");
    check(client.sawNotify(cmd::ContentPackageSyncDataScNotify), "content packages pushed");

    // Avatars, gear and the party.
    proto::GetAvatarDataCsReq avatarReq;
    avatarReq.is_get_all = true;
    client.send(cmd::GetAvatarDataCsReq, avatarReq);
    check(client.await(cmd::GetAvatarDataScRsp, packet), "avatar data answered");
    proto::GetAvatarDataScRsp avatarRsp;
    check(parseBody(packet, avatarRsp), "avatar data parses");
    check(avatarRsp.is_get_all, "is_get_all echoed");
    check(!avatarRsp.avatar_list.empty(), "the roster is not empty");
    check(!avatarRsp.avatar_path_data_info_list.empty(), "path data is sent too");

    client.sendEmpty(cmd::GetBagCsReq);
    check(client.await(cmd::GetBagScRsp, packet), "bag answered");
    proto::GetBagScRsp bagRsp;
    check(parseBody(packet, bagRsp), "bag parses");
    check(!bagRsp.material_list.empty(), "warp currency is stocked");

    client.sendEmpty(cmd::GetCurLineupDataCsReq);
    check(client.await(cmd::GetCurLineupDataScRsp, packet), "lineup answered");
    proto::GetCurLineupDataScRsp lineupRsp;
    check(parseBody(packet, lineupRsp), "lineup parses");
    check(lineupRsp.lineup && !lineupRsp.lineup->avatar_list.empty(), "the party is not empty");
    uint32_t firstMember = lineupRsp.lineup ? lineupRsp.lineup->avatar_list[0].id : 0;

    // The scene, which is the packet the whole login exists to produce.
    client.sendEmpty(cmd::GetCurSceneInfoCsReq);
    check(client.await(cmd::GetCurSceneInfoScRsp, packet), "scene answered");
    proto::GetCurSceneInfoScRsp sceneRsp;
    check(parseBody(packet, sceneRsp), "scene parses");
    check(sceneRsp.scene && sceneRsp.scene->entry_id != 0, "the scene has an entry");
    check(sceneRsp.scene && sceneRsp.scene->leader_entity_id != 0, "and a leader to control");

    bool sceneLoaded = sceneRsp.scene && !sceneRsp.scene->entity_group_list.empty();
    if (!sceneLoaded) {
        std::printf("SKIP scene-dependent flow (no scene dump loaded)\n");
    }

    // Walking around: the position is taken from entity 0.
    proto::SceneEntityMoveCsReq move;
    auto& motion = move.entity_motion_list.emplace_back();
    motion.entity_id = 0;
    auto& pos = motion.motion.emplace().pos.emplace();
    pos.x = 111;
    pos.y = 222;
    pos.z = 333;
    client.send(cmd::SceneEntityMoveCsReq, move);
    check(client.await(cmd::SceneEntityMoveScRsp, packet), "movement acknowledged");

    // A party edit has to push the refresh and the lineup notify before its response.
    if (firstMember != 0) {
        proto::JoinLineupCsReq join;
        join.OPCJCDKMJEI = firstMember;
        join.index = 0;
        join.slot = 0;
        client.send(cmd::JoinLineupCsReq, join);
        bool joined = client.await(cmd::JoinLineupScRsp, packet);
        check(joined, "join answered");
        if (!joined) client.reportInbox(cmd::JoinLineupScRsp);
        check(client.sawNotify(cmd::SyncLineupNotify), "lineup notify pushed with the edit");
        check(client.sawNotify(cmd::SceneGroupRefreshScNotify), "scene refresh pushed too");
    }

    if (sceneLoaded) {
        // Enter a scene by teleport and fight whatever is standing there.
        proto::EnterSceneCsReq enter;
        enter.entry_id = 1000002;
        client.send(cmd::EnterSceneCsReq, enter);
        check(client.await(cmd::EnterSceneScRsp, packet), "enter scene answered");
        proto::EnterSceneScRsp enterRsp;
        check(parseBody(packet, enterRsp), "enter scene parses");

        if (enterRsp.retcode == 0) {
            check(client.sawNotify(cmd::EnterSceneByServerScNotify), "the scene was pushed");
            net::Packet scenePacket;
            check(client.await(cmd::EnterSceneByServerScNotify, scenePacket), "and can be read");
            proto::EnterSceneByServerScNotify pushed;
            check(parseBody(scenePacket, pushed), "pushed scene parses");

            std::vector<uint32_t> monsters;
            if (pushed.scene) {
                for (const proto::SceneEntityGroupInfo& group : pushed.scene->entity_group_list) {
                    for (const proto::SceneEntityInfo& entity : group.entity_list) {
                        if (entity.npc_monster) monsters.push_back(entity.entity_id);
                    }
                }
            }
            check(!monsters.empty(), "the scene has a monster in it");
            size_t partySize = pushed.lineup ? pushed.lineup->avatar_list.size() : 0;

            if (!monsters.empty()) {
                // As the real client sends it: cast_entity_id is a per-cast counter, the
                // actor that swung is attacked_by_entity_id, the monster is an assist.
                uint32_t swinger = partySize >= 2 ? 2 : 1;
                proto::SceneCastSkillCsReq cast;
                cast.cast_entity_id = 42;
                cast.attacked_by_entity_id = swinger;
                cast.skill_index = 0;
                cast.assist_monster_entity_id_list.push_back(monsters[0]);
                client.send(cmd::SceneCastSkillCsReq, cast);
                check(client.await(cmd::SceneCastSkillScRsp, packet), "cast skill answered");

                proto::SceneCastSkillScRsp castRsp;
                check(parseBody(packet, castRsp), "cast skill parses");
                check(castRsp.cast_entity_id == 42, "the cast counter is echoed");
                check(castRsp.battle_info.has(), "a fight was started");
                check(castRsp.monster_battle_info.size() == 1 &&
                          castRsp.monster_battle_info[0].target_monster_entity_id == monsters[0] &&
                          castRsp.monster_battle_info[0].monster_battle_type ==
                              proto::MonsterBattleType::MONSTER_BATTLE_TYPE_TRIGGER_BATTLE,
                      "and the struck monster is reported as joining it");
                if (castRsp.battle_info) {
                    bool ambush = false;
                    bool entryBuffOnSwinger = false;
                    for (const proto::BattleBuff& buff : castRsp.battle_info->buff_list) {
                        if (buff.id == 1000102) ambush = true;
                        if (buff.id >= 1000111 && buff.id <= 1000117) {
                            entryBuffOnSwinger = buff.owner_index == swinger - 1;
                        }
                    }
                    check(!ambush, "striking first is not an ambush");
                    check(entryBuffOnSwinger, "the entry buff goes to the actor that swung");
                    check(castRsp.battle_info->battle_id != 0, "the fight has an id");
                    check(!castRsp.battle_info->battle_avatar_list.empty(), "with the party in it");
                    check(!castRsp.battle_info->monster_wave_list.empty(), "and monsters to beat");

                    proto::PVEBattleResultCsReq result;
                    result.battle_id = castRsp.battle_info->battle_id;
                    result.stage_id = castRsp.battle_info->stage_id;
                    result.end_status = proto::BattleEndStatus::BATTLE_END_WIN;
                    client.send(cmd::PVEBattleResultCsReq, result);
                    check(client.await(cmd::PVEBattleResultScRsp, packet), "battle result answered");

                    proto::PVEBattleResultScRsp resultRsp;
                    check(parseBody(packet, resultRsp), "battle result parses");
                    check(resultRsp.retcode == 0, "the win was accepted");
                    check(resultRsp.battle_id == result.battle_id, "battle id echoed");
                    check(resultRsp.end_status == proto::BattleEndStatus::BATTLE_END_WIN,
                          "end status echoed");
                }
            }

            // A monster that gets the first hit in names itself as the attacker.
            if (monsters.size() >= 2) {
                proto::SceneCastSkillCsReq cast;
                cast.cast_entity_id = 43;
                cast.attacked_by_entity_id = monsters[1];
                cast.hit_target_entity_id_list.push_back(1);
                client.send(cmd::SceneCastSkillCsReq, cast);
                check(client.await(cmd::SceneCastSkillScRsp, packet), "an ambush answers");

                proto::SceneCastSkillScRsp castRsp;
                check(parseBody(packet, castRsp), "the ambush parses");
                check(castRsp.battle_info.has(), "a monster striking first starts a fight");
                bool ambush = false;
                if (castRsp.battle_info) {
                    for (const proto::BattleBuff& buff : castRsp.battle_info->buff_list) {
                        if (buff.id == 1000102 && buff.wave_flag == 1) ambush = true;
                    }
                }
                check(ambush, "and it opens as an ambush");
                check(!castRsp.monster_battle_info.empty() &&
                          castRsp.monster_battle_info[0].target_monster_entity_id == monsters[1],
                      "led by the monster that struck");
            }

            // A swing at nothing, as the real client sends one on the Express.
            proto::SceneCastSkillCsReq miss;
            miss.cast_entity_id = 44;
            miss.attacked_by_entity_id = 1;
            client.send(cmd::SceneCastSkillCsReq, miss);
            check(client.await(cmd::SceneCastSkillScRsp, packet), "a miss answers");
            proto::SceneCastSkillScRsp missRsp;
            check(parseBody(packet, missRsp), "the miss parses");
            check(missRsp.retcode == 0 && !missRsp.battle_info.has() &&
                      missRsp.monster_battle_info.empty(),
                  "and starts nothing");
        }
    }

    // Every npc the client asks about comes back as already met, under the id it asked
    // about: an empty reply is what leaves an npc with no talk option.
    {
        proto::GetFirstTalkNpcCsReq talk;
        talk.npc_id_list = {1001, 1002, 1003};
        client.send(cmd::GetFirstTalkNpcCsReq, talk);
        check(client.await(cmd::GetFirstTalkNpcScRsp, packet), "the npc meet query answers");

        proto::GetFirstTalkNpcScRsp talkRsp;
        check(parseBody(packet, talkRsp), "the npc meet query parses");
        check(talkRsp.npc_meet_status_list.size() == 3, "one status per npc asked about");
        bool echoed = talkRsp.npc_meet_status_list.size() == 3;
        for (size_t i = 0; i < talkRsp.npc_meet_status_list.size(); ++i) {
            if (talkRsp.npc_meet_status_list[i].npc_id != talk.npc_id_list[i] ||
                !talkRsp.npc_meet_status_list[i].is_meet) {
                echoed = false;
            }
        }
        check(echoed, "each carries its own id and is already met");

        proto::GetNpcTakenRewardCsReq reward;
        reward.npc_id = 1003;
        client.send(cmd::GetNpcTakenRewardCsReq, reward);
        check(client.await(cmd::GetNpcTakenRewardScRsp, packet), "the npc reward query answers");
        proto::GetNpcTakenRewardScRsp rewardRsp;
        check(parseBody(packet, rewardRsp), "the npc reward query parses");
        check(rewardRsp.npc_id == 1003, "about the npc that was asked for");
    }

    // A calyx, the way the Survival Index starts one: no prop to walk up to, just a
    // cocoon id and how many runs were bought.
    {
        proto::QuickStartCocoonStageCsReq quick;
        quick.cocoon_id = 1001;
        quick.wave = 2;
        quick.world_level = 6;
        client.send(cmd::QuickStartCocoonStageCsReq, quick);
        check(client.await(cmd::QuickStartCocoonStageScRsp, packet), "the calyx answers");

        proto::QuickStartCocoonStageScRsp cocoonRsp;
        check(parseBody(packet, cocoonRsp), "the calyx parses");
        check(cocoonRsp.retcode == 0, "and was accepted");
        check(cocoonRsp.cocoon_id == 1001, "for the calyx that was asked for");
        check(cocoonRsp.battle_info.has(), "with a fight in it");
        if (cocoonRsp.battle_info) {
            check(cocoonRsp.battle_info->stage_id != 0, "on a real stage");
            check(cocoonRsp.battle_info->monster_wave_list.size() >= 2,
                  "and one wave per run bought");
            check(cocoonRsp.battle_info->world_level == 6, "at the world level asked for");
        }
    }

    // A Stagnant Shadow, from the Survival Index and from the overworld.
    {
        proto::QuickStartFarmElementCsReq shadow;
        shadow.PAOFHFLFFHD = 1012011;
        shadow.world_level = 1;
        client.send(cmd::QuickStartFarmElementCsReq, shadow);
        check(client.await(cmd::QuickStartFarmElementScRsp, packet), "the shadow answers");
        proto::QuickStartFarmElementScRsp shadowRsp;
        check(parseBody(packet, shadowRsp), "the shadow parses");
        check(shadowRsp.retcode == 0 && shadowRsp.battle_info.has(), "with a fight in it");
        check(shadowRsp.battle_info && shadowRsp.battle_info->stage_id == 1012011,
              "on the shadow's own stage");
        check(shadowRsp.world_level == 1, "at the world level asked for");

        proto::ActiveFarmElementCsReq activate;
        activate.entity_id = 1234;
        activate.world_level = 3;
        client.send(cmd::ActiveFarmElementCsReq, activate);
        check(client.await(cmd::ActiveFarmElementScRsp, packet), "activating a shadow answers");
        proto::ActiveFarmElementScRsp activateRsp;
        check(parseBody(packet, activateRsp), "the activation parses");
        check(activateRsp.entity_id == 1234 && activateRsp.world_level == 3,
              "and hands the shadow back");
    }

    // Two login replies the client reads instead of only waiting on.
    {
        client.sendEmpty(cmd::GetGachaInfoCsReq);
        check(client.await(cmd::GetGachaInfoScRsp, packet), "gacha info answered");
        proto::GetGachaInfoScRsp gachaRsp;
        check(parseBody(packet, gachaRsp), "gacha info parses");
        check(!gachaRsp.gacha_info_list.empty() && gachaRsp.gacha_info_list[0].gacha_id == 1001,
              "the standard pool is offered");
        check(!gachaRsp.gacha_info_list.empty() && gachaRsp.gacha_info_list[0].gacha_ceiling &&
                  !gachaRsp.gacha_info_list[0].gacha_ceiling->avatar_list.empty(),
              "with its ceiling picks");

        client.sendEmpty(cmd::GetMissionDataCsReq);
        check(client.await(cmd::GetMissionDataScRsp, packet), "mission data answered");
        proto::GetMissionDataScRsp missionRsp;
        check(parseBody(packet, missionRsp), "mission data parses");
        check(!missionRsp.finished_main_mission_id_list.empty(), "with the main missions done");

        client.sendEmpty(cmd::GetActivityScheduleConfigCsReq);
        check(client.await(cmd::GetActivityScheduleConfigScRsp, packet), "activity schedule answered");
        proto::GetActivityScheduleConfigScRsp scheduleRsp;
        check(parseBody(packet, scheduleRsp), "activity schedule parses");
        bool gridFight = false;
        for (const proto::ActivityScheduleData& data : scheduleRsp.schedule_data) {
            gridFight |= data.activity_id == 7100101 || data.activity_id == 7100501;
        }
        check(!scheduleRsp.schedule_data.empty() && !gridFight, "without Grid Fight in it");
    }

    // The CapySR watermark rides on every pause toggle.
    {
        proto::SetClientPausedCsReq pause;
        pause.paused = true;
        client.send(cmd::SetClientPausedCsReq, pause);
        check(client.await(cmd::SetClientPausedScRsp, packet), "pausing answers");
        net::Packet markPacket;
        check(client.await(cmd::ClientDownloadDataScNotify, markPacket), "and pushes the watermark");
        proto::ClientDownloadDataScNotify mark;
        check(parseBody(markPacket, mark), "the watermark parses");
        std::string lua = mark.download_data ? mark.download_data->data : std::string();
        check(lua.find("VersionText") != std::string::npos, "it rewrites the version label");
        check(lua.find(">C<") != std::string::npos && lua.find(">R<") != std::string::npos,
              "letter by letter");
        check(lua.find("uid") == std::string::npos, "with nothing but the name");
    }

    // A whole Memory of Chaos floor: start it, fight its node, settle it, leave.
    {
        proto::StartChallengeCsReq start;
        start.challenge_id = 1;
        start.first_lineup = {8001, 1001};
        start.second_lineup = {8001, 1001};
        client.send(cmd::StartChallengeCsReq, start);
        check(client.await(cmd::StartChallengeScRsp, packet), "starting a challenge answers");

        proto::StartChallengeScRsp startRsp;
        check(parseBody(packet, startRsp), "the start parses");
        check(startRsp.retcode == 0, "and was accepted");
        check(startRsp.cur_challenge.has(), "with the run in it");
        check(startRsp.lineup_list.size() == 2, "and both teams");
        check(client.sawNotify(cmd::ChallengeLineupNotify), "the challenge lineup was pushed");

        net::Packet arenaPacket;
        check(client.await(cmd::EnterSceneByServerScNotify, arenaPacket), "the arena was pushed");
        proto::EnterSceneByServerScNotify arena;
        check(parseBody(arenaPacket, arena), "the arena parses");

        uint32_t challengeMonster = 0;
        size_t arenaMonsters = 0;
        if (arena.scene) {
            check(arena.scene->entry_id == 3000101, "and it is the floor's own map entrance");
            for (const proto::SceneEntityGroupInfo& group : arena.scene->entity_group_list) {
                for (const proto::SceneEntityInfo& entity : group.entity_list) {
                    if (!entity.npc_monster) continue;
                    ++arenaMonsters;
                    challengeMonster = entity.entity_id;
                }
            }
        }
        // The floor plants exactly one; the dump's own placeholders stay switched off.
        check(arenaMonsters == 1, "the arena holds only the monster the floor names");

        if (challengeMonster != 0) {
            proto::SceneCastSkillCsReq cast;
            cast.cast_entity_id = 1;
            cast.hit_target_entity_id_list.push_back(challengeMonster);
            client.send(cmd::SceneCastSkillCsReq, cast);
            check(client.await(cmd::SceneCastSkillScRsp, packet), "the challenge fight answers");

            proto::SceneCastSkillScRsp castRsp;
            check(parseBody(packet, castRsp), "the challenge fight parses");
            check(castRsp.battle_info.has(), "a fight was started");
            if (castRsp.battle_info) {
                // The stage comes off the maze config's event id, not off PlaneEvent.
                check(castRsp.battle_info->stage_id == 30001011, "against the floor's own stage");
                check(castRsp.battle_info->rounds_limit == 20, "with the floor's cycle limit");
                check(castRsp.battle_info->battle_avatar_list.size() == 2,
                      "and the team the challenge was started with");

                proto::PVEBattleResultCsReq result;
                result.battle_id = castRsp.battle_info->battle_id;
                result.stage_id = castRsp.battle_info->stage_id;
                result.end_status = proto::BattleEndStatus::BATTLE_END_WIN;
                result.stt.emplace().round_cnt = 4;
                client.send(cmd::PVEBattleResultCsReq, result);
                check(client.await(cmd::PVEBattleResultScRsp, packet), "the result answers");

                net::Packet settlePacket;
                check(client.await(cmd::ChallengeSettleNotify, settlePacket),
                      "clearing the last node settles the floor");
                proto::ChallengeSettleNotify settle;
                check(parseBody(settlePacket, settle), "the settle parses");
                check(settle.is_win, "as a win");
                check(settle.challenge_id == 1, "for the floor that was started");
                // 20 cycles less the 4 used still clears both cycle targets, and
                // nobody died, so all three stars.
                check(settle.star == 7, "with all three stars");
            }
        }

        client.sendEmpty(cmd::LeaveChallengeCsReq);
        check(client.await(cmd::LeaveChallengeScRsp, packet), "leaving answers");
        check(client.await(cmd::EnterSceneByServerScNotify, packet), "and puts the player back");

        client.sendEmpty(cmd::GetCurChallengeCsReq);
        check(client.await(cmd::GetCurChallengeScRsp, packet), "the run is queryable");
        proto::GetCurChallengeScRsp curRsp;
        check(parseBody(packet, curRsp), "the run parses");
        check(!curRsp.cur_challenge.has(), "and there is no longer one going");
    }

    // The three-node floor: the packet 4.6 starts Memory of Chaos with. Unlike the older
    // one it carries its arena in the reply rather than in a notify.
    {
        proto::StartChallengeTierceCsReq start;
        start.challenge_id = 5213;
        for (uint32_t stage = 0; stage < 3; ++stage) {
            proto::ChallengeTierceStageLineupInfo info;
            proto::AvatarIdentifier one;
            one.id = 8001;
            proto::AvatarIdentifier two;
            two.id = 1001;
            info.lineup.push_back(one);
            info.lineup.push_back(two);
            start.stage_info_list.push_back(std::move(info));
        }
        client.send(cmd::StartChallengeTierceCsReq, start);
        check(client.await(cmd::StartChallengeTierceScRsp, packet), "starting the floor answers");

        proto::StartChallengeTierceScRsp startRsp;
        check(parseBody(packet, startRsp), "the start parses");
        check(startRsp.retcode == 0, "and was accepted");
        check(startRsp.scene.has(), "with the arena in the reply itself");
        check(startRsp.challenge_tierce_info.has(), "and the run");
        if (startRsp.challenge_tierce_info) {
            check(startRsp.challenge_tierce_info->challenge_id == 5213, "for the floor asked for");
            check(startRsp.challenge_tierce_info->stage_index == 0, "starting on its first node");
            check(startRsp.challenge_tierce_info->lineup_list.size() == 3, "with all three teams");
        }

        uint32_t nodeMonster = 0;
        size_t nodeMonsters = 0;
        if (startRsp.scene) {
            check(startRsp.scene->entry_id == 3014002, "on the floor's own map entrance");
            for (const proto::SceneEntityGroupInfo& group : startRsp.scene->entity_group_list) {
                for (const proto::SceneEntityInfo& entity : group.entity_list) {
                    if (!entity.npc_monster) continue;
                    ++nodeMonsters;
                    nodeMonster = entity.entity_id;
                }
            }
        }
        check(nodeMonsters == 1, "holding only the node's own monster");

        if (nodeMonster != 0) {
            proto::SceneCastSkillCsReq cast;
            cast.cast_entity_id = 1;
            cast.hit_target_entity_id_list.push_back(nodeMonster);
            client.send(cmd::SceneCastSkillCsReq, cast);
            check(client.await(cmd::SceneCastSkillScRsp, packet), "the node's fight answers");

            proto::SceneCastSkillScRsp castRsp;
            check(parseBody(packet, castRsp), "the node's fight parses");
            check(castRsp.battle_info.has(), "a fight was started");
            if (castRsp.battle_info) {
                // The cycle pool is the tierce row's 45, not the floor's own 30.
                check(castRsp.battle_info->rounds_limit == 45, "with the floor-wide cycle pool");
                bool mazeBuff = false;
                for (const proto::BattleBuff& buff : castRsp.battle_info->buff_list) {
                    if (buff.id == 3030146) mazeBuff = true;
                }
                check(mazeBuff, "and the maze buff of the floor it extends");

                proto::PVEBattleResultCsReq result;
                result.battle_id = castRsp.battle_info->battle_id;
                result.stage_id = castRsp.battle_info->stage_id;
                result.end_status = proto::BattleEndStatus::BATTLE_END_WIN;
                result.stt.emplace().round_cnt = 5;
                client.send(cmd::PVEBattleResultCsReq, result);
                check(client.await(cmd::PVEBattleResultScRsp, packet), "the result answers");

                net::Packet syncPacket;
                check(client.await(cmd::ChallengeTierceSyncNotify, syncPacket),
                      "clearing a node reports it");
            }
        }

        client.sendEmpty(cmd::StartNextChallengeTierceCsReq);
        check(client.await(cmd::StartNextChallengeTierceScRsp, packet), "the next node answers");
        proto::StartNextChallengeTierceScRsp nextRsp;
        check(parseBody(packet, nextRsp), "the next node parses");
        check(nextRsp.retcode == 0, "and was accepted");
        check(nextRsp.scene.has(), "carrying its arena too");
        check(nextRsp.challenge_tierce_info.has() &&
                  nextRsp.challenge_tierce_info->stage_index == 1,
              "on the second node now");

        client.sendEmpty(cmd::LeaveChallengeTierceCsReq);
        check(client.await(cmd::LeaveChallengeTierceScRsp, packet), "leaving answers");
        check(client.await(cmd::EnterSceneByServerScNotify, packet), "and puts the player back");

        client.sendEmpty(cmd::GetChallengeTierceDataCsReq);
        check(client.await(cmd::GetChallengeTierceDataScRsp, packet), "the history answers");
        proto::GetChallengeTierceDataScRsp dataRsp;
        check(parseBody(packet, dataRsp), "the history parses");
        bool remembered = false;
        for (const proto::ChallengeTierceData& entry : dataRsp.challenge_info_list) {
            if (entry.challenge_id != 5213) continue;
            remembered = true;
            check(!entry.result_list.empty(), "with what the run did on it");
        }
        check(remembered, "and the floor is in it");
    }

    // Anomaly Arbitration: a knight on its own arena -- team, start, fight, settle, retry,
    // leave -- then season ten's boss on hard, whose arena is newer than the scene dump.
    {
        client.sendEmpty(cmd::GetChallengePeakDataCsReq);
        check(client.await(cmd::GetChallengePeakDataScRsp, packet), "arbitration data answered");
        proto::GetChallengePeakDataScRsp peakData;
        check(parseBody(packet, peakData), "arbitration data parses");
        check(peakData.challenge_peak_groups.size() >= 10, "with every season in it");
        check(peakData.current_peak_group_id == 10, "and the newest on show");

        proto::SetChallengePeakMobLineupAvatarCsReq teams;
        teams.peak_group_id = 9;
        proto::ChallengePeakLineup knightTeam;
        knightTeam.peak_id = 901;
        knightTeam.peak_avatar_id_list = {8001, 1001};
        teams.lineup_list.push_back(knightTeam);
        client.send(cmd::SetChallengePeakMobLineupAvatarCsReq, teams);
        check(client.await(cmd::SetChallengePeakMobLineupAvatarScRsp, packet),
              "setting a knight's team answers");
        net::Packet groupPacket;
        check(client.await(cmd::ChallengePeakGroupDataUpdateScNotify, groupPacket),
              "and re-sends the season");
        proto::ChallengePeakGroupDataUpdateScNotify groupUpdate;
        check(parseBody(groupPacket, groupUpdate), "the season parses");
        bool teamKept = false;
        if (groupUpdate.challenge_peak_group) {
            for (const proto::ChallengePeak& peak : groupUpdate.challenge_peak_group->peaks) {
                if (peak.peak_id == 901 && peak.peak_avatar_id_list.size() == 2) teamKept = true;
            }
        }
        check(teamKept, "with the team in the knight's slot");

        // Sent without a team, the start falls back on the one just set.
        proto::StartChallengePeakCsReq start;
        start.peak_id = 901;
        client.send(cmd::StartChallengePeakCsReq, start);
        check(client.await(cmd::StartChallengePeakScRsp, packet), "starting a knight answers");
        proto::StartChallengePeakScRsp startRsp;
        check(parseBody(packet, startRsp), "the start parses");
        check(startRsp.retcode == 0, "and was accepted");

        auto arenaOf = [&](const char* what, uint32_t& monster, size_t& monsters, size_t& actors,
                           uint32_t& entryId) {
            net::Packet arenaPacket;
            check(client.await(cmd::EnterSceneByServerScNotify, arenaPacket), what);
            proto::EnterSceneByServerScNotify arena;
            check(parseBody(arenaPacket, arena), "the arena parses");
            monster = 0;
            monsters = actors = 0;
            entryId = arena.scene ? arena.scene->entry_id : 0;
            if (!arena.scene) return;
            for (const proto::SceneEntityGroupInfo& group : arena.scene->entity_group_list) {
                for (const proto::SceneEntityInfo& entity : group.entity_list) {
                    if (entity.actor) ++actors;
                    if (!entity.npc_monster) continue;
                    ++monsters;
                    monster = entity.entity_id;
                }
            }
        };
        uint32_t knight = 0;
        size_t monsters = 0;
        size_t actors = 0;
        uint32_t entryId = 0;
        arenaOf("the arena went out before the response", knight, monsters, actors, entryId);
        check(entryId == 3014501, "on the season's own arena");
        check(monsters == 1, "with the knight and nothing else in it");
        check(actors == 2, "and the fight's team standing in it, not the squad");

        auto fight = [&](uint32_t monster, uint32_t rounds, proto::SceneBattleInfo& battle) {
            proto::SceneCastSkillCsReq cast;
            cast.cast_entity_id = 1;
            cast.hit_target_entity_id_list.push_back(monster);
            client.send(cmd::SceneCastSkillCsReq, cast);
            check(client.await(cmd::SceneCastSkillScRsp, packet), "the fight answers");
            proto::SceneCastSkillScRsp castRsp;
            check(parseBody(packet, castRsp), "the fight parses");
            check(castRsp.battle_info.has(), "a fight was started");
            if (!castRsp.battle_info) return false;
            battle = *castRsp.battle_info;

            proto::PVEBattleResultCsReq result;
            result.battle_id = battle.battle_id;
            result.stage_id = battle.stage_id;
            result.end_status = proto::BattleEndStatus::BATTLE_END_WIN;
            result.stt.emplace().round_cnt = rounds;
            client.send(cmd::PVEBattleResultCsReq, result);
            check(client.await(cmd::PVEBattleResultScRsp, packet), "the result answers");
            return true;
        };
        auto targetsIn = [](const proto::SceneBattleInfo& battle) {
            auto slot = battle.battle_target_info.find(5);
            return slot == battle.battle_target_info.end() ? size_t{0}
                                                           : slot->second.battle_target_list.size();
        };

        proto::SceneBattleInfo battle;
        if (knight != 0 && fight(knight, 3, battle)) {
            check(battle.stage_id == 30509011, "against the knight's own stage");
            check(battle.battle_avatar_list.size() == 2, "with the knight's team");
            check(targetsIn(battle) == 3, "and its three targets");

            net::Packet settlePacket;
            check(client.await(cmd::ChallengePeakSettleScNotify, settlePacket),
                  "winning settles the knight");
            proto::ChallengePeakSettleScNotify settle;
            check(parseBody(settlePacket, settle), "the settle parses");
            check(settle.is_win && settle.peak_id == 901, "as a win for that knight");
            // Four cycles or fewer, two or fewer, and nobody lost: three cycles meets two.
            check(settle.finished_target_list.size() == 2, "three cycles meets two targets of three");
        }

        client.sendEmpty(cmd::GetCurChallengePeakCsReq);
        check(client.await(cmd::GetCurChallengePeakScRsp, packet), "the fight is queryable");
        proto::GetCurChallengePeakScRsp cur;
        check(parseBody(packet, cur), "the fight parses");
        check(cur.peak_id == 901, "and it is the knight");

        client.sendEmpty(cmd::ReStartChallengePeakCsReq);
        check(client.await(cmd::ReStartChallengePeakScRsp, packet), "retrying answers");
        proto::ReStartChallengePeakScRsp restart;
        check(parseBody(packet, restart), "the retry parses");
        check(restart.retcode == 0, "and was accepted");
        arenaOf("the arena went out again", knight, monsters, actors, entryId);
        check(monsters == 1, "with the knight standing again");

        client.sendEmpty(cmd::LeaveChallengePeakCsReq);
        check(client.await(cmd::LeaveChallengePeakScRsp, packet), "leaving answers");
        check(client.await(cmd::EnterSceneByServerScNotify, packet), "and puts the player back");

        proto::SetChallengePeakBossHardModeCsReq hardMode;
        hardMode.peak_group_id = 10;
        hardMode.is_hard_mode = true;
        client.send(cmd::SetChallengePeakBossHardModeCsReq, hardMode);
        check(client.await(cmd::SetChallengePeakBossHardModeScRsp, packet), "hard mode answers");
        proto::SetChallengePeakBossHardModeScRsp hardRsp;
        check(parseBody(packet, hardRsp), "hard mode parses");
        check(hardRsp.is_hard_mode && hardRsp.peak_group_id == 10, "and is echoed");

        proto::StartChallengePeakCsReq bossStart;
        bossStart.peak_id = 1004;
        bossStart.peak_avatar_id_list = {8001, 1001};
        client.send(cmd::StartChallengePeakCsReq, bossStart);
        check(client.await(cmd::StartChallengePeakScRsp, packet), "starting the boss answers");
        check(parseBody(packet, startRsp) && startRsp.retcode == 0, "and was accepted");
        uint32_t boss = 0;
        arenaOf("the boss arena went out", boss, monsters, actors, entryId);
        check(entryId == 3013501, "borrowing season one's arena");
        check(monsters == 1, "with the boss alone in it");

        if (boss != 0 && fight(boss, 1, battle)) {
            check(battle.stage_id == 30510022, "against the boss's hard stage");
            check(targetsIn(battle) == 1, "judged by the hard target alone");

            net::Packet settlePacket;
            check(client.await(cmd::ChallengePeakSettleScNotify, settlePacket),
                  "winning settles the boss");
            proto::ChallengePeakSettleScNotify settle;
            check(parseBody(settlePacket, settle), "the settle parses");
            check(settle.is_win && settle.hard_mode_has_passed, "as a hard clear");
        }

        client.sendEmpty(cmd::LeaveChallengePeakCsReq);
        check(client.await(cmd::LeaveChallengePeakScRsp, packet), "leaving the boss answers");
        check(client.await(cmd::EnterSceneByServerScNotify, packet), "and puts the player back");

        client.sendEmpty(cmd::GetCurChallengePeakCsReq);
        check(client.await(cmd::GetCurChallengePeakScRsp, packet), "the run is queryable");
        proto::GetCurChallengePeakScRsp after;
        check(parseBody(packet, after), "the run parses");
        check(after.peak_id == 0, "and there is no longer one going");
    }

    // Every way out of a fight answers. None of these did before, which left the
    // client sitting on the defeat screen or on a half-switched party.
    client.sendEmpty(cmd::QuitBattleCsReq);
    check(client.await(cmd::QuitBattleScRsp, packet), "quitting a battle answers");

    client.sendEmpty(cmd::SceneReviveAfterRebattleCsReq);
    check(client.await(cmd::SceneReviveAfterRebattleScRsp, packet), "reviving answers");
    check(client.sawNotify(cmd::SyncLineupNotify), "and pushes the party back");

    client.sendEmpty(cmd::GetLineupAvatarDataCsReq);
    check(client.await(cmd::GetLineupAvatarDataScRsp, packet), "lineup avatar data answered");
    proto::GetLineupAvatarDataScRsp avatarData;
    check(parseBody(packet, avatarData), "lineup avatar data parses");
    check(!avatarData.avatar_data_list.empty(), "with every avatar's hp in it");

    // Switching to a squad nobody is in must still answer, or the client waits on it
    // forever.
    proto::SwitchLineupIndexCsReq switchTo;
    switchTo.index = game::kSquadCount - 1;
    client.send(cmd::SwitchLineupIndexCsReq, switchTo);
    check(client.await(cmd::SwitchLineupIndexScRsp, packet), "switching to an empty squad answers");
    proto::SwitchLineupIndexScRsp switchRsp;
    check(parseBody(packet, switchRsp), "the switch parses");
    check(switchRsp.retcode != 0, "and is refused rather than silently ignored");
    check(switchRsp.index == switchTo.index, "the index is echoed");

    proto::MarkPresetLineupCsReq mark;
    mark.index = 0;
    mark._is_favourite = true;
    client.send(cmd::MarkPresetLineupCsReq, mark);
    check(client.await(cmd::SetTeamFavourite, packet), "marking a preset answers on 773");
    proto::SetTeamFavourite markRsp;
    check(parseBody(packet, markRsp), "the mark parses");
    check(markRsp.retcode == 0 && markRsp._is_favourite, "the squad is now a favourite");

    // The way out of a black screen. The client has no timeout on a transition it
    // started, so pushing it a scene is what makes it let go and render again.
    {
        http::Server admin;
        sdk::registerAdminRoutes(admin, gateway);
        if (admin.start("127.0.0.1", kAdminPort)) {
            std::string reply = httpGet(kAdminPort, "/unstick?entry=1000002");
            check(reply.find("\"message\":\"ok\"") != std::string::npos,
                  "unstick found the live session");

            net::Packet pushed;
            check(client.await(cmd::EnterSceneByServerScNotify, pushed),
                  "and pushed a scene at it");
            proto::EnterSceneByServerScNotify rescued;
            check(parseBody(pushed, rescued), "the pushed scene parses");
            check(rescued.scene.has() && rescued.scene->entry_id == 1000002,
                  "at the entry the request named");
            check(client.sawNotify(cmd::SyncLineupNotify), "with the party alongside it");
            admin.stop();
        } else {
            std::printf("SKIP unstick (tcp %d is busy)\n", kAdminPort);
        }
    }

    // Warp: results only, and every refusal still answers.
    std::vector<core::WarpBanner> configuredPools = core::Config::get().gameplay.limitedWarpPools;
    core::Config::get().gameplay.limitedWarpPools.clear();
    client.sendEmpty(cmd::GetGachaInfoCsReq);
    check(client.await(cmd::GetGachaInfoScRsp, packet), "warp pools answered");
    proto::GetGachaInfoScRsp standardInfo;
    check(parseBody(packet, standardInfo) && standardInfo.gacha_info_list.size() == 1 &&
              standardInfo.gacha_info_list[0].gacha_id == 1001,
          "only the standard pool by default");

    core::Config::get().gameplay.limitedWarpPools = {{2002, 0}, {3002, 0}};
    client.sendEmpty(cmd::GetGachaInfoCsReq);
    check(client.await(cmd::GetGachaInfoScRsp, packet), "and again with limited pools configured");
    proto::GetGachaInfoScRsp gachaInfo;
    check(parseBody(packet, gachaInfo), "the pools parse");
    check(gachaInfo.gacha_info_list.size() == 3, "the standard pool and the two configured ones");
    check(!gachaInfo.gacha_info_list.empty() && gachaInfo.gacha_info_list[0].gacha_ceiling.has() &&
              gachaInfo.gacha_info_list[0].gacha_ceiling->avatar_list.size() == 7,
          "the standard pool carries its ceiling");
    uint32_t limitedId = 0;
    if (gachaInfo.gacha_info_list.size() > 1) {
        limitedId = gachaInfo.gacha_info_list[1].gacha_id;
        check(gachaInfo.gacha_info_list[1].prize_item_list.size() == 1,
              "a limited pool names its featured 5*");
    }

    proto::DoGachaCsReq tenPull;
    tenPull.gacha_id = limitedId;
    tenPull.gacha_num = 10;
    tenPull.gacha_random = gachaInfo.gacha_random;
    client.send(cmd::DoGachaCsReq, tenPull);
    check(client.await(cmd::DoGachaScRsp, packet), "a ten pull answers");
    proto::DoGachaScRsp tenRsp;
    check(parseBody(packet, tenRsp) && tenRsp.retcode == 0, "and succeeds");
    check(tenRsp.gacha_id == limitedId && tenRsp.gacha_num == 10 &&
              tenRsp.gacha_item_list.size() == 10,
          "with ten results");
    bool cardsWhole = !tenRsp.gacha_item_list.empty();
    for (const proto::GachaItem& card : tenRsp.gacha_item_list) {
        cardsWhole &= card.gacha_item.has() && card.gacha_item->item_id != 0 &&
                      card.transfer_item_list.has() && card.token_item.has();
    }
    check(cardsWhole, "every card is filled in");

    proto::DoGachaCsReq oddPull = tenPull;
    oddPull.gacha_num = 5;
    client.send(cmd::DoGachaCsReq, oddPull);
    proto::DoGachaScRsp oddRsp;
    check(client.await(cmd::DoGachaScRsp, packet) && parseBody(packet, oddRsp) &&
              oddRsp.retcode == static_cast<uint32_t>(proto::Retcode::RET_GACHA_NUM_INVALID) &&
              oddRsp.gacha_item_list.empty(),
          "a five pull is refused");

    proto::DoGachaCsReq oldPool;
    oldPool.gacha_id = 2138;
    oldPool.gacha_num = 1;
    client.send(cmd::DoGachaCsReq, oldPool);
    proto::DoGachaScRsp oldRsp;
    check(client.await(cmd::DoGachaScRsp, packet) && parseBody(packet, oldRsp) &&
              oldRsp.retcode == static_cast<uint32_t>(proto::Retcode::RET_GACHA_ID_NOT_EXIST),
          "a pool that is not offered is refused");

    proto::GetGachaCeilingCsReq ceilingReq;
    ceilingReq.DDMCNOJFGON = 1;
    client.send(cmd::GetGachaCeilingCsReq, ceilingReq);
    proto::GetGachaCeilingScRsp ceilingRsp;
    check(client.await(cmd::GetGachaCeilingScRsp, packet) && parseBody(packet, ceilingRsp) &&
              ceilingRsp.gacha_ceiling.has() && ceilingRsp.gacha_ceiling->is_claimed,
          "the ceiling answers, claimed");

    proto::SetGachaDecideItemCsReq decideReq;
    decideReq.gacha_id = limitedId;
    client.send(cmd::SetGachaDecideItemCsReq, decideReq);
    proto::SetGachaDecideItemScRsp decideRsp;
    check(client.await(cmd::SetGachaDecideItemScRsp, packet) && parseBody(packet, decideRsp) &&
              decideRsp.retcode != 0 && decideRsp.NBLOJLDLBEB.has(),
          "a custom 50/50 pool is refused with a body");
    core::Config::get().gameplay.limitedWarpPools = configuredPools;

    // An unimplemented request still has to complete, or the client hangs on it. This
    // one has no handler at all, so it exercises the name-derived fallback.
    client.sendEmpty(cmd::GetStarFightDataCsReq);
    check(client.await(cmd::GetStarFightDataScRsp, packet), "unhandled requests answer empty");

    // A module we deliberately do not answer stays unanswered: a body we guessed at
    // makes the client throw inside its own module, and for some of them drop the
    // session. Silence is the safer default there.
    client.sendEmpty(cmd::ChessRogueQueryCsReq);
    check(!client.await(cmd::ChessRogueQueryScRsp, packet, 600),
          "a silenced module gets no reply");

    gateway.stop();
}
