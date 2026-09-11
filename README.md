# CapySR - WIP

A Star Rail private server in C++. Targets the `4.5.51/4.6 beta`. This server is in WIP, but you can still use it.

## Requirements

Needs Visual Studio with the C++ workload, CMake, Ninja, Python 3 (with `py -m pip install protobuf`) and `protoc` on PATH.

## Build

```powershell
.\build.ps1            # to build
```

Then find the outputs at `build\bin\`

## Running it

Supports both `CN` and `OS` client. You can get it here: [CN](https://gofile.io/d/vtV5JfkO) and [OS](https://gofile.io/d/wAT1UiJj). Recommends you use 7z to extract.

1. Copy `launcher.exe` and `hkrpg.dll` from launcher folder inside CapySR and paste them inside your client folder (Thank you Reversed Rooms)
2. Start the server: `build\bin\capysr.exe`.
3. Run the `launcher.exe` **as administrator**.
4. Log in with any account name and password; the SDK routes accept anything.

Delete `data/player.json` to start over from the defaults in `config/config.json` if needed.

## srtools

The server also supports <https://srtools.neonteam.dev/>!

## Credit
- capyb2222
- Various other server for references.

## Troubleshooting
- You can create an issue in this repository, but it is not guaranteed I will be there to help.