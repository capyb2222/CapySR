// Pom-Pom's outfits, the Jukebox, the Data Bank and phone messages. A private server
// has nothing to earn them with, so each collection is handed over whole.
#include <algorithm>

#include "core/logger.h"
#include "core/util.h"
#include "data/excel.h"
#include "game/handlers.h"
#include "game/player.h"
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

void onGetPamSkinData(net::Session& session, const proto::GetPamSkinDataCsReq&) {
    Player* player = playerOf(session, "GetPamSkinData");
    if (player == nullptr) return;

    proto::GetPamSkinDataScRsp rsp;
    rsp.retcode = 0;
    rsp.cur_skin = player->pamSkin();
    rsp.unlock_skin_list = data::Tables::get().pamSkins();
    session.send(cmd::GetPamSkinDataScRsp, rsp);
}

void onSelectPamSkin(net::Session& session, const proto::SelectPamSkinCsReq& req) {
    Player* player = playerOf(session, "SelectPamSkin");
    if (player == nullptr) return;

    const std::vector<uint32_t>& skins = data::Tables::get().pamSkins();
    if (std::find(skins.begin(), skins.end(), req.pam_skin) != skins.end()) {
        player->setPamSkin(req.pam_skin);
        player->save();
    }
    // Both fields carry the outfit now worn.
    proto::SelectPamSkinScRsp rsp;
    rsp.retcode = 0;
    rsp.cur_skin = player->pamSkin();
    rsp.set_skin = player->pamSkin();
    session.send(cmd::SelectPamSkinScRsp, rsp);
}

void onGetJukeboxData(net::Session& session, const proto::GetJukeboxDataCsReq&) {
    Player* player = playerOf(session, "GetJukeboxData");
    if (player == nullptr) return;

    proto::GetJukeboxDataScRsp rsp;
    rsp.retcode = 0;
    for (const data::MusicInfo& track : data::Tables::get().music()) {
        proto::LHHGCDLCJDA entry;
        entry.id = track.id;
        entry.group_id = track.groupId;
        entry.BIMDKNLMICG = true;  // unlocked
        rsp.IOKAJIBHLMP.push_back(entry);
    }
    // What is playing.
    rsp.GFFOBALDBPM.emplace().JKNLCEEDAHJ.emplace().id = player->music();
    session.send(cmd::GetJukeboxDataScRsp, rsp);
}

void onPlayBackGroundMusic(net::Session& session, const proto::PlayBackGroundMusicCsReq& req) {
    Player* player = playerOf(session, "PlayBackGroundMusic");
    if (player == nullptr) return;

    uint32_t id = req.IKJNIKIGFLF && req.IKJNIKIGFLF->JKNLCEEDAHJ ? req.IKJNIKIGFLF->JKNLCEEDAHJ->id : 0;
    proto::PlayBackGroundMusicScRsp rsp;
    if (data::Tables::get().musicTrack(id) == nullptr) {
        rsp.retcode = fail(proto::Retcode::RET_MUSIC_NOT_EXIST);
    } else {
        player->setMusic(id);
        player->save();
        rsp.retcode = 0;
        // Echoed whole, pause state included.
        rsp.GFFOBALDBPM = *req.IKJNIKIGFLF;
    }
    session.send(cmd::PlayBackGroundMusicScRsp, rsp);
}

void onGetArchiveData(net::Session& session, const proto::GetArchiveDataCsReq&) {
    const data::Tables& tables = data::Tables::get();
    proto::GetArchiveDataScRsp rsp;
    rsp.retcode = 0;
    auto& archive = rsp.archive_data.emplace();
    for (const auto& [id, lightcone] : tables.lightcones()) archive.archive_equipment_id_list.push_back(id);
    std::sort(archive.archive_equipment_id_list.begin(), archive.archive_equipment_id_list.end());
    for (const data::RelicSetPiece& piece : tables.relicSetPieces()) {
        proto::RelicList relic;
        relic.set_id = piece.setId;
        relic.type = piece.type;
        archive.relic_list.push_back(relic);
    }
    std::vector<uint32_t> monsters;
    for (const auto& [id, monster] : tables.monsters()) monsters.push_back(id);
    std::sort(monsters.begin(), monsters.end());
    for (uint32_t id : monsters) {
        proto::MonsterList entry;
        entry.monster_id = id;
        entry.num = 1;
        archive.kill_monster_list.push_back(entry);
    }
    session.send(cmd::GetArchiveDataScRsp, rsp);
}

void onGetNpcMessageGroup(net::Session& session, const proto::GetNpcMessageGroupCsReq& req) {
    // Nothing tracks how far a conversation got, so each one reads as finished: where the
    // story leaves them, and what unlocks gated on a finished chat look for.
    const data::Tables& tables = data::Tables::get();
    proto::GetNpcMessageGroupScRsp rsp;
    rsp.retcode = 0;
    auto refreshed = static_cast<int64_t>(util::nowSec());
    for (uint32_t contact : req.BJIPBBFIBPD) {  // contact ids
        const std::vector<data::MessageGroupInfo>* groups = tables.messageGroups(contact);
        if (groups == nullptr) continue;
        for (const data::MessageGroupInfo& info : *groups) {
            proto::MessageGroup group;
            group.id = info.id;
            group.status = proto::MessageGroupStatus::MessageGroupStatus_MessageGroupFinish;
            group.refresh_time = refreshed;
            for (uint32_t sectionId : info.sections) {
                proto::MessageSection section;
                section.id = sectionId;
                section.status = proto::MessageSectionStatus::MessageSectionStatus_MessageSectionFinish;
                group.message_section_list.push_back(section);
            }
            if (!info.sections.empty()) group.message_section_id = info.sections.back();
            rsp.message_group_list.push_back(std::move(group));
        }
    }
    session.send(cmd::GetNpcMessageGroupScRsp, rsp);
}

}  // namespace

void registerCollectionHandlers() {
    net::on<proto::GetPamSkinDataCsReq>(cmd::GetPamSkinDataCsReq, onGetPamSkinData);
    net::on<proto::SelectPamSkinCsReq>(cmd::SelectPamSkinCsReq, onSelectPamSkin);
    net::on<proto::GetJukeboxDataCsReq>(cmd::GetJukeboxDataCsReq, onGetJukeboxData);
    net::on<proto::PlayBackGroundMusicCsReq>(cmd::PlayBackGroundMusicCsReq, onPlayBackGroundMusic);
    net::on<proto::GetArchiveDataCsReq>(cmd::GetArchiveDataCsReq, onGetArchiveData);
    net::on<proto::GetNpcMessageGroupCsReq>(cmd::GetNpcMessageGroupCsReq, onGetNpcMessageGroup);
}

}  // namespace game
