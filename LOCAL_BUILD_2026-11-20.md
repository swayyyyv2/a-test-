# Local Sunrise merge build, 2026-11-20

Stan's `f5a4c676` merge archive was extracted over this checkout. All 512 archive files match the isolated build tree after the local fixes below. The merge overwrote 417 existing files and added 95 files. The overwritten files and addition list are backed up under `C:\Users\ender\Documents\Codex\2026-11-04\a-x20\work\sunrise-essential-overhaul\live_checkout_before_f5a4c676`.

## Local fixes and additions

- Removed a duplicate `spawn_set_hash` receiver field that made `valid_receivers()` fail at compile time.
- Added an activity-scoped `context:award_nightmare_hunt{}` API. An authored Skolas death queues one Tangled Web armor piece for each of the five armor slots of the selected class, once per attempt.
- Set the Release project to use the x64 compiler host. This machine's x86 compiler host failed with C1060 on `pursuit_catalog.cpp`. Release debug symbols are disabled, and that catalog file uses lower optimization as a conservative build setting.
- The matching `hunt_foul.lua` adds HUD boss and objective progress, reward feedback and retry, and boss objective group diagnostics. It is installed in the game's `Sunrise\scripts\hunt_foul` directory.

## Build and install evidence

- Visual Studio 18 BuildTools, Release x64: full build and link succeeded using `build_merged.cmd`.
- Built DLL: `build\x64\Release\steam_api64.dll`, SHA256 `0E5C6B93E83CBAEA6C6DB06B5B5D3B4688384B219C6DA746DEFFB3C5C2183085`.
- The same DLL and matching Lua were installed under the user's Destiny 2 `bin\x64` folder and their hashes verified after copying.
- The previous game DLL, Lua, and SQLite save are backed up under `C:\Users\ender\Documents\Codex\2026-11-04\a-x20\work\sunrise-essential-overhaul\installed_before_update`.
- The old checkout build artifacts, including the stale PDB, are backed up under `C:\Users\ender\Documents\Codex\2026-11-04\a-x20\work\sunrise-essential-overhaul\live_build_before_update`.

## Runtime status

No in-game run has been observed after this install. The Tangled Web grant, boss HUD, and Skolas movement/model still require a Ketch clear test. A successful build and Lua load check do not establish their in-game behavior.
