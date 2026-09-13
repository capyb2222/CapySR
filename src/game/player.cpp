#include "game/player.h"

#include "core/config.h"
#include "core/files.h"
#include "core/logger.h"
#include "core/util.h"
#include "data/excel.h"
#include "data/scene_res.h"
#include "game/player_store.h"
#include "game/srtools.h"

namespace game {
namespace {

constexpr uint64_t kSaveIntervalMs = 2000;

// MazePlane's Challenge type, as the scene dump numbers it, and the Express floor to
// fall back to.
constexpr uint32_t kChallengePlaneType = 4;
constexpr uint32_t kStartEntry = 1000002;

// A run that never got to finish -- a server restart mid-floor, a crash -- leaves the
// saved spot inside a challenge arena with nothing left to rebuild it from. The client
// then gets a floor with no ground and no party under it: a black screen it can only
// look around in. Start on the Express instead.
void leaveStrandedArena(Player& player) {
    const data::SceneRes& res = data::SceneRes::get();
    const data::ResFloor* floor = res.byEntry(player.location().entryId);
    if (floor == nullptr || floor->planeType != kChallengePlaneType) return;

    const data::ResFloor* start = res.byEntry(kStartEntry);
    if (start == nullptr) return;
    logging::info("player", "uid {} was saved inside arena {}; starting at {} instead",
                  player.uid(), player.location().entryId, kStartEntry);
    player.location().entryId = kStartEntry;
    player.location().planeId = start->planeId;
    player.location().floorId = start->floorId;
    player.position() = Position{};
}

// The last saved spot can be mid-jump or mid-fall, and coming back to it leaves the
// player hanging in the air. Logging in lands on the floor's nearest anchor instead.
void landOnAnchor(Player& player) {
    const data::ResFloor* floor = data::SceneRes::get().byEntry(player.location().entryId);
    if (floor == nullptr) return;

    Position& at = player.position();
    const data::ResTeleport* nearest = nullptr;
    int64_t best = 0;
    for (const data::ResGroup& group : floor->groups) {
        for (const data::ResTeleport& anchor : group.teleports) {
            int64_t dx = static_cast<int64_t>(anchor.pos.x) - at.x;
            int64_t dz = static_cast<int64_t>(anchor.pos.z) - at.z;
            int64_t distance = dx * dx + dz * dz;
            if (nearest == nullptr || distance < best) {
                nearest = &anchor;
                best = distance;
            }
        }
    }
    if (nearest == nullptr) return;
    at.x = nearest->pos.x;
    at.y = nearest->pos.y;
    at.z = nearest->pos.z;
    at.rotY = nearest->rot.y;
}

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
    leaveStrandedArena(*this);
    landOnAnchor(*this);
}

void Player::save() {
    uint64_t now = util::nowMs();
    if (now - lastSaveMs_ < kSaveIntervalMs) return;
    saveNow();
}

void Player::saveNow() {
    lastSaveMs_ = util::nowMs();
    // Built here, where the state is consistent; written off the packet thread.
    files::writeLater(core::Config::get().paths.playerFile, playerStateJson(*this));
}

}  // namespace game
