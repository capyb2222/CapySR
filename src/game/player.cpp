#include "game/player.h"

#include "core/config.h"
#include "core/util.h"
#include "data/excel.h"
#include "game/player_store.h"
#include "game/srtools.h"

namespace game {
namespace {

constexpr uint64_t kSaveIntervalMs = 2000;

// A squad that at least exists, so a fresh account is not stranded with no party.
void seedDefaultSquad(LineupBook& book) {
    if (book.cur().count() != 0) return;
    const std::vector<uint32_t>& ids = Roster::baseAvatarIds();
    uint32_t slot = 0;
    for (uint32_t id : {8001u, 1001u}) {
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) {
            book.join(book.curIndex(), slot++, id);
        }
    }
    for (uint32_t id : ids) {
        if (slot >= kSquadSlots) break;
        if (id == 8001 || id == 1001) continue;
        book.join(book.curIndex(), slot++, id);
    }
}

}  // namespace

Player::Player(uint32_t uid) : uid_(uid) {
    const auto& defaults = core::Config::get().player;
    name_ = defaults.name;
    signature_ = defaults.signature;
    level_ = defaults.level;
    worldLevel_ = defaults.worldLevel;
    stamina_ = defaults.stamina;
    headIcon_ = defaults.headIcon;
    hcoin_ = defaults.hcoin;
    scoin_ = defaults.scoin;
    mcoin_ = defaults.mcoin;
}

uint32_t Player::gender() const {
    // The Trailblazer's path id carries the gender: odd is the boy model.
    if (const data::MultiPathInfo* info = data::Tables::get().multiPath(mainCharacter_)) {
        if (info->gender == "GENDER_MAN") return 1;
        if (info->gender == "GENDER_WOMAN") return 2;
    }
    return mainCharacter_ % 2 == 1 ? 1u : 2u;
}

Roster Player::roster() const {
    return Roster(SrTools::instance().data(), mainCharacter_, marchType_);
}

void Player::load() {
    loadPlayerState(*this);
    seedDefaultSquad(lineups_);
    lineups_.clampMp();
}

void Player::save() {
    uint64_t now = util::nowMs();
    if (now - lastSaveMs_ < kSaveIntervalMs) return;
    saveNow();
}

void Player::saveNow() {
    lastSaveMs_ = util::nowMs();
    savePlayerState(*this);
}

}  // namespace game
