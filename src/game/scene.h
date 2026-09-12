#pragma once

#include <cstdint>
#include <vector>

#include "data/excel.h"
#include "proto/gen/protos.h"

namespace game {

class Player;

// Positions are quantised to 1/1000 of a unit, the way the client sends them.
struct Position {
    int32_t x = 42548;
    int32_t y = 3716;
    int32_t z = -38422;
    int32_t rotY = 325717;
};

// Jarilo-VI's Outlying Snow Plains, on the space anchor beside a calyx, so a new
// account has a fight to hand.
struct SceneLocation {
    uint32_t planeId = 20101;
    uint32_t floorId = 20101001;
    uint32_t entryId = 2010101;
};

enum class EntityKind : uint8_t { Actor, Npc, Monster, Prop };

// Entity ids are handed out in fixed bands so a kind can be told from the id alone;
// SceneCastSkill target filtering relies on that.
constexpr uint32_t kPropEntityIdBase = 1000;
constexpr uint32_t kNpcEntityIdBase = 20000;
constexpr uint32_t kMonsterEntityIdBase = 30000;

struct SceneEntity {
    uint32_t entityId = 0;
    uint32_t instId = 0;
    uint32_t groupId = 0;
    EntityKind kind = EntityKind::Prop;
    uint32_t configId = 0;  // prop / npc / monster id
    uint32_t eventId = 0;   // monsters only
    // Monsters only, and only inside a challenge: the arena's fights are not in
    // PlaneEvent, so the maze config names the stage outright.
    uint32_t stageId = 0;
    uint32_t avatarId = 0;  // actors only
    uint32_t state = 0;     // props only, and it changes as they are used
};

// What is currently loaded in the player's scene, so later packets that name an
// entity id can be answered without re-reading the dump.
class SceneState {
public:
    void reset();
    void add(const SceneEntity& entity);
    void remove(uint32_t entityId);
    void removeKind(EntityKind kind);

    const SceneEntity* find(uint32_t entityId) const;
    SceneEntity* find(uint32_t entityId);
    const std::vector<SceneEntity>& entities() const { return entities_; }

    uint32_t nextPropEntityId() { return ++propCursor_; }
    uint32_t nextNpcEntityId() { return ++npcCursor_; }
    uint32_t nextMonsterEntityId() { return ++monsterCursor_; }

private:
    std::vector<SceneEntity> entities_;
    uint32_t propCursor_ = kPropEntityIdBase;
    uint32_t npcCursor_ = kNpcEntityIdBase;
    uint32_t monsterCursor_ = kMonsterEntityIdBase;
};

proto::MotionInfo toMotion(const Position& position);

// A challenge arena is the same floor with almost everything switched off: only one
// scene group keeps its monsters, only the ones the maze config names, and there are no
// npcs at all. Props stay -- they are the arena.
struct ChallengeArena {
    uint32_t mazeGroupId = 0;
    const std::vector<data::ChallengeMonster>* monsters = nullptr;
    // The team standing in it, which is not the squad the player walks around with.
    const std::vector<uint32_t>* party = nullptr;
};

// Assembles SceneInfo out of the scene dump plus the current party.
namespace scene {

// Fills `out` and, when `commit` is set, moves the player there. Returns false when
// the entry id is not in the dump, in which case `out` is untouched.
bool load(Player& player, uint32_t entryId, uint32_t teleportId, bool commit,
          proto::SceneInfo& out, const ChallengeArena* arena = nullptr);

// One squad as the client wants it. Slots are indices into the occupied prefix, so
// the party must already be packed.
proto::LineupInfo lineupInfo(const Player& player, uint32_t index);
// The squad the player is walking around with.
proto::LineupInfo lineupInfo(const Player& player);
proto::SceneMapInfo mapInfo(const Player& player, uint32_t floorId);

// The party actors as their own group, which is how a lineup edit is pushed.
proto::SceneEntityGroupInfo actorGroup(Player& player, const Position& at);
proto::SceneEntityGroupInfo actorGroup(Player& player, const Position& at,
                                       const std::vector<uint32_t>& members);

}  // namespace scene
}  // namespace game
