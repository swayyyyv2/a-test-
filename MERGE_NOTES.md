# Second merge: Stan's master f5a4c676 (2026-11-20, 246 commits)

Your features are all still in (Prophecy/Menagerie/event rewards, unrestricted socket plugs,
weapon editor, Ultra boss bar, select_state spawn sets, transmog, item icons, mission flags,
character objectives).

What Stan changed that your code had to follow
- Reward queue: upstream dropped the reward "kind" and the item_grant_route helper. Kind is back
  (pending_rewards.kind, DEFAULT 0) so Menagerie weapons still roll sockets; item_route() is a
  local replacement in bap_world_reward_queue.cpp. Iteration now uses upstream's afterId reader.
- prepare_item_reward / stage_socket_plug take nowSeconds: callers pass server_clock_seconds().
- PreparedRecordReward lost stateIndex: roll_menagerie_reward_sockets finds the weapon by
  instanceSoid.
- prepare_item_acquisition_for_item removed: insert_item_definition_unrestricted (weapon editor)
  now uses prepare_item_acquisition(kNoCollectibleIndex, hash, nullopt, now) -- same free grant.
- select_state spawn sets: upstream has its own arrivalSpawnHash; your note_declared_spawn_set /
  sameRegion path is kept, duplicate helper removed.

Save schema
- Now 10. Upstream's v8 storage rebuild keeps your kind column; v9 = character objectives,
  v10 = mission flags. Older saves upgrade in order. Back up your save before first launch.

Resources: IDR_FACTION_TURN_IN_CONTENT 112 (upstream), item names/icons/rarity moved to 115-117.

Not verified here: no MSVC on this machine. Build Release x64 in Visual Studio; any error will
most likely be a missing include or a signature I couldn't see.

# Sunrise merge: your local build + Stan's master (2026-11-10)

Base of your copy: stanuwu/Sunrise ec2df8bd (2026-10-20). Merged in: master 9ab7b50b
(2026-11-09: "tag rewrite, cleanup, more" + PR #115 co-op shared exploration).

Your changes kept: Prophecy rewards / reward queue hold, Menagerie+Prophecy socket rolls, Ultra
boss bar, select_state spawn sets, Lua contract refresh, player_position, dash, movement panel,
weapon editor, transmog / appearance store, item icons, save_with_cheats.

Hand-merged files
- state/investment/investment_database.cpp + resources/database/investment_schema.sql:
  save schema is now 7. Your saves (v6) upgrade with upstream's header cleanup and parked_items.
- state/investment/investment_account_store.cpp: deletes/moves both appearance_preview_pose and
  parked_items rows.
- core/settings/client/*: upstream "exports" block + your save_with_cheats.
- server/activity/mission/mission_script_runtime_region.cpp, _dispatch.cpp, server/bap/bap_route.cpp,
  server/bap/runtime.h: upstream's shared-region arm (arm_shared_mission_region) now takes
  sameRegion; a script-named spawn set still teleports a player already in the region (Prophecy).
- state/runtime/state_account_reward_grant_runtime.cpp: upstream rewrite + your
  roll_menagerie_reward_sockets.
- account_snapshot() calls renamed to upstream's bound_account_snapshot().
