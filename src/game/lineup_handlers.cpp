#include <algorithm>

#include "core/logger.h"
#include "game/handlers.h"
#include "game/notify.h"
#include "game/player.h"
#include "game/scene.h"
#include "net/cmd_ids.h"
#include "net/handler.h"
#include "net/session.h"
#include "proto/gen/protos.h"

namespace game {
namespace {

Player* playerOf(net::Session& session, const char* what) {
    Player* player = session.player();
    if (player == nullptr) logging::warn("game", "{} before login", what);
    return player;
}

void onGetCurLineupData(net::Session& session, const proto::GetCurLineupDataCsReq&) {
    Player* player = playerOf(session, "GetCurLineupData");
    if (player == nullptr) return;

    proto::GetCurLineupDataScRsp rsp;
    rsp.retcode = 0;
    rsp.lineup = scene::lineupInfo(*player);
    session.send(cmd::GetCurLineupDataScRsp, rsp);
}

void onGetAllLineupData(net::Session& session, const proto::GetAllLineupDataCsReq&) {
    Player* player = playerOf(session, "GetAllLineupData");
    if (player == nullptr) return;
    LineupBook& book = player->lineups();

    proto::GetAllLineupDataScRsp rsp;
    rsp.retcode = 0;
    rsp.cur_index = book.curIndex();
    for (uint32_t index = 0; index < kSquadCount; ++index) {
        rsp.lineup_list.push_back(scene::lineupInfo(*player, index));
    }
    session.send(cmd::GetAllLineupDataScRsp, rsp);
}

void onJoinLineup(net::Session& session, const proto::JoinLineupCsReq& req) {
    Player* player = playerOf(session, "JoinLineup");
    if (player == nullptr) return;

    // Field 1 is the base avatar id; the dump never got a readable name for it.
    uint32_t avatarId = req.OPCJCDKMJEI;
    if (!player->lineups().join(req.index, req.slot, avatarId)) {
        logging::debug("game", "could not put {} in squad {} slot {}", avatarId, req.index,
                       req.slot);
    }
    player->save();
    if (req.index == player->lineups().curIndex()) notify::lineupChanged(session, *player);
    session.sendEmpty(cmd::JoinLineupScRsp);
}

void onQuitLineup(net::Session& session, const proto::QuitLineupCsReq& req) {
    Player* player = playerOf(session, "QuitLineup");
    if (player == nullptr) return;

    player->lineups().quit(req.index, req.base_avatar_id);
    player->save();
    if (req.index == player->lineups().curIndex()) notify::lineupChanged(session, *player);
    session.sendEmpty(cmd::QuitLineupScRsp);
}

void onSwapLineup(net::Session& session, const proto::SwapLineupCsReq& req) {
    Player* player = playerOf(session, "SwapLineup");
    if (player == nullptr) return;

    // The two slot fields were never named in the dump; swapping is symmetric, so
    // which one is the source does not matter.
    player->lineups().swap(req.index, req.MPIEGFCNBEN, req.ENBIBCPPLDC);
    player->save();
    if (req.index == player->lineups().curIndex()) notify::lineupChanged(session, *player);
    session.sendEmpty(cmd::SwapLineupScRsp);
}

void onReplaceLineup(net::Session& session, const proto::ReplaceLineupCsReq& req) {
    Player* player = playerOf(session, "ReplaceLineup");
    if (player == nullptr) return;

    // The client does not always send the slots in order, and a zero id is a hole.
    std::vector<proto::LineupSlotData> slots = req.lineup_slot_list;
    std::sort(slots.begin(), slots.end(),
              [](const proto::LineupSlotData& a, const proto::LineupSlotData& b) {
                  return a.slot < b.slot;
              });
    std::vector<uint32_t> avatarIds;
    for (const proto::LineupSlotData& slot : slots) {
        if (slot.id != 0) avatarIds.push_back(slot.id);
    }
    player->lineups().replace(req.index, avatarIds, req.leader_slot);
    player->saveNow();
    if (req.index == player->lineups().curIndex()) notify::lineupChanged(session, *player);
    session.sendEmpty(cmd::ReplaceLineupScRsp);
}

void onChangeLineupLeader(net::Session& session, const proto::ChangeLineupLeaderCsReq& req) {
    Player* player = playerOf(session, "ChangeLineupLeader");
    if (player == nullptr) return;

    LineupBook& book = player->lineups();
    book.setLeaderSlot(book.curIndex(), req.slot);
    player->save();

    proto::ChangeLineupLeaderScRsp rsp;
    rsp.retcode = 0;
    rsp.slot = req.slot;
    session.send(cmd::ChangeLineupLeaderScRsp, rsp);
}

void onSetLineupName(net::Session& session, const proto::SetLineupNameCsReq& req) {
    Player* player = playerOf(session, "SetLineupName");
    if (player == nullptr) return;

    if (req.index < kSquadCount) player->lineups().squad(req.index).name = req.name;
    player->saveNow();

    proto::SetLineupNameScRsp rsp;
    rsp.retcode = 0;
    rsp.index = req.index;
    rsp.name = req.name;
    session.send(cmd::SetLineupNameScRsp, rsp);
}

void onSwitchLineupIndex(net::Session& session, const proto::SwitchLineupIndexCsReq& req) {
    Player* player = playerOf(session, "SwitchLineupIndex");
    if (player == nullptr) return;
    LineupBook& book = player->lineups();

    proto::SwitchLineupIndexScRsp rsp;
    rsp.index = req.index;
    if (req.index >= kSquadCount) {
        rsp.retcode = fail(proto::Retcode::RET_LINEUP_INVALID_INDEX);
    } else if (book.members(req.index).empty()) {
        // Answering nothing here would leave the client waiting on a request it
        // never sees completed.
        rsp.retcode = fail(proto::Retcode::RET_LINEUP_IS_EMPTY);
    } else {
        book.setCurIndex(req.index);
        player->saveNow();
        notify::lineupChanged(session, *player);
        rsp.retcode = 0;
    }
    session.send(cmd::SwitchLineupIndexScRsp, rsp);
}

void onGetLineupAvatarData(net::Session& session, const proto::GetLineupAvatarDataCsReq&) {
    Player* player = playerOf(session, "GetLineupAvatarData");
    if (player == nullptr) return;
    Roster roster = player->roster();

    // The team screen reads hp from here rather than from the lineup, so every owned
    // avatar has to be listed -- including the ones sitting on the bench.
    proto::GetLineupAvatarDataScRsp rsp;
    rsp.retcode = 0;
    for (uint32_t baseId : roster.baseAvatarIds()) {
        proto::LineupAvatarData data;
        data.id = baseId;
        data.hp = kFullHp;
        data.avatar_type = proto::AvatarType::AvatarType_AvatarFormalType;
        rsp.avatar_data_list.push_back(data);
    }
    session.send(cmd::GetLineupAvatarDataScRsp, rsp);
}

void onRecoverAllLineup(net::Session& session, const proto::RecoverAllLineupCsReq&) {
    Player* player = playerOf(session, "RecoverAllLineup");
    if (player == nullptr) return;

    // Nobody ever loses hp here, so this only has to push the party back so the client
    // redraws the bars it just emptied.
    proto::SyncLineupNotify sync;
    sync.lineup = scene::lineupInfo(*player);
    sync.reason_list.push_back(proto::SyncLineupReason::SyncLineupReason_SyncReasonMpAdd);
    session.send(cmd::SyncLineupNotify, sync);

    proto::RecoverAllLineupScRsp rsp;
    rsp.retcode = 0;
    session.send(cmd::RecoverAllLineupScRsp, rsp);
}

void onMarkPresetLineup(net::Session& session, const proto::MarkPresetLineupCsReq& req) {
    Player* player = playerOf(session, "MarkPresetLineup");
    if (player == nullptr) return;

    proto::SetTeamFavourite rsp;
    rsp.index = req.index;
    rsp._is_favourite = req._is_favourite;
    if (req.index >= kSquadCount) {
        rsp.retcode = fail(proto::Retcode::RET_LINEUP_INVALID_INDEX);
    } else {
        player->lineups().squad(req.index).favourite = req._is_favourite;
        player->saveNow();
        rsp.retcode = 0;
    }
    // The dump never names a MarkPresetLineupScRsp; 773 is it, under the name the
    // client's own symbols use.
    session.send(cmd::SetTeamFavourite, rsp);
}

}  // namespace

void registerLineupHandlers() {
    net::on<proto::GetCurLineupDataCsReq>(cmd::GetCurLineupDataCsReq, onGetCurLineupData);
    net::on<proto::GetAllLineupDataCsReq>(cmd::GetAllLineupDataCsReq, onGetAllLineupData);
    net::on<proto::JoinLineupCsReq>(cmd::JoinLineupCsReq, onJoinLineup);
    net::on<proto::QuitLineupCsReq>(cmd::QuitLineupCsReq, onQuitLineup);
    net::on<proto::SwapLineupCsReq>(cmd::SwapLineupCsReq, onSwapLineup);
    net::on<proto::ReplaceLineupCsReq>(cmd::ReplaceLineupCsReq, onReplaceLineup);
    net::on<proto::ChangeLineupLeaderCsReq>(cmd::ChangeLineupLeaderCsReq, onChangeLineupLeader);
    net::on<proto::SetLineupNameCsReq>(cmd::SetLineupNameCsReq, onSetLineupName);
    net::on<proto::SwitchLineupIndexCsReq>(cmd::SwitchLineupIndexCsReq, onSwitchLineupIndex);
    net::on<proto::GetLineupAvatarDataCsReq>(cmd::GetLineupAvatarDataCsReq, onGetLineupAvatarData);
    net::on<proto::RecoverAllLineupCsReq>(cmd::RecoverAllLineupCsReq, onRecoverAllLineup);
    net::on<proto::MarkPresetLineupCsReq>(cmd::MarkPresetLineupCsReq, onMarkPresetLineup);
}

}  // namespace game
