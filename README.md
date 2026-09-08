# CapySR

A Honkai: Star Rail server reimplementation rewritten in C#.
Current version: `CNBETAWin4.5.53`

Goals: a full combat system including combat events, and
[srtools](https://srtools.neonteam.dev/) support. Currency Wars and Divergent Universe might be focused on in the far future.

## Requirements

- .NET 10 SDK
- `protoc` (only to regenerate the protocol)

## Running

```
dotnet run --project src/CapySR.SdkServer     # dispatch + sdk, port 21000
dotnet run --project src/CapySR.GameServer    # kcp gateway, port 23301
```

Or just `run.bat`.

## What works

- WIP

## Chat commands

- WIP

## srtools

- WIP

### Hotfix URLs

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

## Regenerating the protocol

```
dotnet run --project tools/CapySR.ProtoGen
```

## Credits
- capyb2222
- Various other server reimplementation.
