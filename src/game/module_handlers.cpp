// Gameplay modules CapySR does not implement.
//
// The client reads a response body without null-checking its sub-messages and throws
// inside its own module, and for a few of these it then drops the session -- which
// looks like being kicked to an error dialog. Filling the body (`onFilled`) fixes the
// simple ones but only pushes the throw deeper into modules whose data is a populated
// tree of lists, and guessing at those shapes made the client bail *earlier* than
// answering empty did.
//
// So these say nothing at all. The response handler never runs, so there is no shape to
// get wrong. The client tolerates it: during the first successful login four requests
// (GetMatchPlayData, SwitchHandData, VoracityInvasionGetData, PlayerReturnInfoQuery)
// went unanswered for the whole burst and it still worked through to the end of its
// query list.
//
// Anything here becomes a real handler the day the mode is implemented.
#include "game/handlers.h"
#include "net/cmd_ids.h"
#include "net/handler.h"

namespace game {
namespace {

// Every one of these appeared as a `at RPG.Client.<Module>._On...ScRsp` frame in the
// client's own Player.log.
constexpr uint16_t kSilent[] = {
    // Simulated Universe and its variants.
    cmd::GetRogueHandbookDataCsReq,
    cmd::RogueTournQueryCsReq,
    cmd::ChessRogueQueryCsReq,
    cmd::CommonRogueQueryCsReq,
    cmd::GetRogueCommonDialogueDataCsReq,
    cmd::GetChessRogueNousStoryInfoCsReq,
    cmd::RogueTournGetCurRogueCocoonInfoCsReq,
    cmd::GetRogueEndlessActivityDataCsReq,
    cmd::RogueArcadeGetInfoCsReq,
    cmd::RogueMagicQueryCsReq,
    // Activities and side modes.
    cmd::GetAlleyInfoCsReq,
    cmd::GridFightGetDataCsReq,
    cmd::TrainPartyGetDataCsReq,
    cmd::SocialPlayGetDataCsReq,
    cmd::ChimeraDuelGetDataCsReq,
    cmd::ChenLingGetDataCsReq,
    cmd::GetEvolveBuildQueryInfoCsReq,
    cmd::GetJukeboxDataCsReq,
    // Second round, from the OS 4.5.53 client's own log: CycleScore.Sync,
    // PetMarbleModule._OnPetMarbleGetDataScRsp, ArchiveModule._OnGetArchiveDataScRsp
    // (through AvatarArchiveData.Sync) and DrinkMakerBar._UpdateGameplayData all
    // dereference their way through an empty body.
    cmd::CycleScoreRewardGetDataCsReq,
    cmd::PetMarbleGetDataCsReq,
    cmd::GetArchiveDataCsReq,
    cmd::GetDrinkMakerDataCsReq,
};

// Fire and forget: the dump has no PlayerLogoutScRsp, and the client is already on its
// way out, so warning that it "will wait on it" is just noise.
constexpr uint16_t kNoReplyExpected[] = {cmd::PlayerLogoutCsReq};

}  // namespace

void registerModuleHandlers() {
    for (uint16_t cmdId : kSilent) net::onSilent(cmdId);
    for (uint16_t cmdId : kNoReplyExpected) net::onSilent(cmdId);
}

}  // namespace game
