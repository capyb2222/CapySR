#pragma once

#include <cstdint>

#include "proto/gen/protos.h"

namespace game {

// Every retcode field is a plain uint32 on the wire, while the enum the names live in
// is scoped -- so saying anything but success takes a cast.
inline uint32_t fail(proto::Retcode value) { return static_cast<uint32_t>(value); }

// Each module registers its own packets; called once at startup so nothing
// depends on static initialisation order inside the static library.
void registerLoginHandlers();
void registerAvatarHandlers();
void registerInventoryHandlers();
void registerLineupHandlers();
void registerSceneHandlers();
void registerBattleHandlers();
void registerMissionHandlers();
void registerTalkHandlers();
void registerChallengeHandlers();
void registerTierceHandlers();
void registerPeakHandlers();
void registerGachaHandlers();
void registerShopHandlers();
void registerCollectionHandlers();
void registerModuleHandlers();
void registerMiscHandlers();

void registerAllHandlers();

}  // namespace game
