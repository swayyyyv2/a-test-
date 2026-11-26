# Dawn title screen port (2026-11-24)

Ported from Dawn 1.7.3 (isinternets/Dawn, production) into this Sunrise checkout:
- Dawn title art (23 DDS, IDs 2007-2039) replaces the old Sunrise bootflow art.
- Bungie splash / character select recoloured in memory (bootflow_texture_recolour + bc7_codec).
- graphics_splash_invert: inverts the bright loading screens.
- graphics_title_filigree: animated medallions behind the title.
- Settings (client): custom_bootflow_textures, invert_splash_screens, title_filigree (all true), dump_gpu_entries (false).
- State signal: retail_log_enqueue_observer forwards "Entering state '...'" to both layers.

Backup of every replaced file: ..\backup_before_dawn_title\Sunrise
Build: build_merged.cmd -> build\x64\Release\steam_api64.dll
