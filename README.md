# CapySR

A Honkai: Star Rail server reimplementation rewritten in C#.
Current version: `CNBETAWin4.5.53`

Goals: a full combat system including combat events, and
[srtools](https://srtools.neonteam.dev/) support. Currency Wars and Divergent Universe might be focused on in the far future.

## Requirements

- .NET 10 SDK
- `protoc` (only to regenerate the protocol)

Everything the server reads but does not own lives in `stuff/` and is not tracked here: the
game data it merges (`stuff/turnbasedgamedata/ExcelOutput` from Dimbreath and
`stuff/pearl-sr/resources` for the beta overlay), the proto dump, and the reference servers.
`Data.Sources` in `config/config.json` points at the first two, in priority order.

## Running

```
dotnet run --project src/CapySR.SdkServer     # dispatch + sdk, port 21000
dotnet run --project src/CapySR.GameServer    # kcp gateway, port 23301
```

Or just `run.bat`.

## What works

- Teams: six squads, join/quit/swap/leader/rename/switch, virtual lineups for challenges,
  shared technique points (Phainon and Himeko Nova raise the cap), all remembered between runs.
- Overworld fights: hitting a monster resolves its own stage for your world level, the attacker's
  element breaks toughness on entry, ambushes work, destructible props break, and fodder-clearing
  techniques (Acheron, Phainon, Silver Wolf 999, Himeko Nova) skip the fight. Won fights remove
  the monsters; HP and energy carry over until you teleport.
- Calyxes, Stagnant Shadows / Caverns, Battle College (trial characters), story stages by event.
- Memory of Chaos, Pure Fiction, Apocalyptic Shadow with both nodes, scores, stars, records; Anomaly
  Arbitration with saved mob teams, hard mode and settle screens. Every floor reports as cleared so
  none sit locked behind the one before them; set `GameServer.UnlockAllChallenges` to false to
  report only what has actually been played.
- Position, paths, gender, head icon, server-side settings and challenge records persist in
  `data/player.json`.

## Chat commands

The friend list has one entry, CapySR. Message it:

| command                     | effect                                              |
|-----------------------------|-----------------------------------------------------|
| `/help`                     | list the commands                                   |
| `/tp <entryId> [teleportId]`| travel to a map entrance                            |
| `/unstuck`                  | respawn at the current map's anchor                 |
| `/heal`                     | full health and technique points                    |
| `/mp [n]`                   | set technique points                                |
| `/wl <0-6>`                 | set the equilibrium level (monsters scale with it)  |
| `/stage <stageId>`          | the next fight you start plays that stage           |
| `/sync`                     | reload `freesr-data.json` now                       |
| `/lineup`, `/pos`, `/clear` | show the team, show where you are, clear the map    |

## srtools

Point srtools at `http://127.0.0.1:21000/srtools` and upload. The game server watches
`data/freesr-data.json` and pushes the new roster, gear and teams to a logged-in client without a relog.

`GameServer.BattleSource` in `config/config.json` decides what the uploaded `battle_config` drives:

| value               | behaviour                                                     |
|---------------------|---------------------------------------------------------------|
| `auto` (default)    | every non-challenge fight plays the srtools battle            |
| `srtools`           | the same, and never falls back to the encounter's own stage   |
| `stage`             | everything plays its real stage                               |

So a calyx runs whatever stage `freesr-data.json` names. That is how an older Apocalyptic
Shadow, Pure Fiction or Memory of Chaos floor gets replayed, while F4 keeps the current
rotation. Challenges always play their own stage.

### Hotfix URLs

Unknown versions are fetched from the official dispatch once and cached in
`config/versions.json`. A beta dispatch stops answering as soon as its window closes, so a
client newer than anything cached cannot be looked up by anyone any more. When that happens
the closest older build of the same channel is lent out, which keeps a packaged client
booting: an empty `ex_resource_url` hangs it at about 99% before it ever reaches the game
server. Turn `Hotfix.EnableDesignDataUpdate` off while a version is running on borrowed urls,
or it will try to download design data that does not match its build.

The version a client will ask for, and its dispatch seed, are in
`StarRail_Data/StreamingAssets/BinaryVersion.bytes`.

For OS:
```json
{
  "OSBETAWin4.5.51": {
    "asset_bundle_url": "https://autopatchos.starrails.com/asb/...",
    "ex_resource_url": "https://autopatchos.starrails.com/design_data/...",
    "lua_url": "https://autopatchos.starrails.com/lua/...",
    "ifix_url": "https://autopatchos.starrails.com/ifix/..."
  }
}
```

## Tests

```
dotnet test tests/CapySR.Tests
```

`FlowTests` drive a real gateway end to end: login, team edits, an overworld fight, technique
points, and a two-node Memory of Chaos floor.

## Regenerating the protocol

```
dotnet run --project tools/CapySR.ProtoGen
```

The generated protocol is committed, so this is only needed after a proto dump changes. Clone
[TurnBaseGameProto](https://github.com/Mar7thLover/TurnBaseGameProto) into `stuff/` first.

Reads `stuff/TurnBaseGameProto/Raw/StarRail.proto`, resolves the 11 duplicate type names the dump's
translation pass produces, runs protoc, and writes `CmdId.g.cs` / `CmdIdTable.g.cs`. Duplicate
resolution is scored against the `CmdXxxType` service enums and `packetIds.txt`; deliberate calls
live in `DuplicateResolver.Overrides`.

## Credits
- capyb2222
- Various other server reimplementation.
