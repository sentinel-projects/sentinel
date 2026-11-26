# Core Warden CW-01


The Rust TUI and desktop candidate packaging use the runtime catalog under
`animations/` and `state-contract.json`. Keep their relative paths intact.
Blender masters, reference sheets, previews, renders and authoring scripts
are source assets. The Rust staging scripts package the runtime catalog
without bundling the authoring files. Desktop candidate staging compares each
packaged runtime asset with the checkout by SHA-256.

- `reference/Core_Warden_Original_Reference.png`: existing approved concept sheet; preserved.
- `scripts/build_cw01.py`: deterministic procedural mesh/material/stage builder.
- `scripts/validate_cw01.py`: reopen validation, independent mesh and RGBA checks.
- `blender/core_warden_cw01.blend`: editable master, no animation.
- `renders/preview/cw01_{front,three_quarter,side,back,hero}.png`: transparent 640px previews.

Run from the repository root:

```sh
/Applications/Blender.app/Contents/MacOS/Blender --background --python-exit-code 1 --python assets/core-warden/scripts/build_cw01.py
/Applications/Blender.app/Contents/MacOS/Blender --background assets/core-warden/blender/core_warden_cw01.blend --python-exit-code 1 --python assets/core-warden/scripts/validate_cw01.py
```

Use the installed Blender executable on other platforms. The builder clears its current scene and replaces generated CW-01 outputs: run in a fresh background process, not an unsaved user session. It never edits the reference. Source geometry remains low resolution; bevel modifiers are editable. Part origins sit at component bounds, while head/face centers are local. Each eye has its own Glacier material for independent emission animation. Animate object transforms directly; no armature or keyframes are included. The saved master uses Eevee for interactive preview; batch previews use CPU Cycles for headless portability. No runtime integration is included.

## CW-02 refinement

`blender/core_warden_cw02.blend` is a derivative opened from the existing CW-01 master. CW-01 files and previews remain unchanged. All 22 original object names are retained; head and face vertex coordinates remain intact. Armor and eyes are refined in place, with supporting detail added separately.

```sh
/Applications/Blender.app/Contents/MacOS/Blender --background --python-exit-code 1 --python assets/core-warden/scripts/build_core_warden_cw02.py
/Applications/Blender.app/Contents/MacOS/Blender --background assets/core-warden/blender/core_warden_cw02.blend --python-exit-code 1 --python assets/core-warden/scripts/validate_core_warden_cw02.py
python3 assets/core-warden/scripts/compare_core_warden_cw02.py
```

The comparison command needs Pillow. `renders/cw02/` contains five views, three native size tests, and the concept/CW-01/CW-02 comparison. The hero image has a dark opaque background; other rendered views are transparent. Preview settings: 640px, 24 CPU Cycles samples; saved viewport engine: Eevee.

`CTRL_Float` moves the complete mascot. `CTRL_HeadMotion` rotates the head, cheeks and rear assembly. `CTRL_LeftShell`, `CTRL_RightShell`, three `CTRL_Upper_Armor_*` controls and `CTRL_LowerV` provide independent local movement. Side liners and luminous trims follow their respective shells. On `CTRL_Float`, `eye_left_intensity`, `eye_right_intensity` and `accent_intensity` drive independent material emission. No keyframes, state animations or armature are included. Coordinates: +Z up, -Y forward.

The CW-02 builder replaces only its own derivative and renders; it opens CW-01 afresh each time and verifies baseline SHA-256 checksums. Run in a dedicated background Blender process, never an unsaved interactive scene. Local logs and `.blend1` backups are ignored; build/validation/readability JSON files record the checks performed. See the CW-02 refinement report for visual limitations and the approval boundary before CW-03.

## CW-02.1 visual polish

`blender/core_warden_cw02_1.blend` loads CW-02 and applies vertex-only polish. All 29 meshes, base topology, object names, pivots, parenting and eight controls are retained. Obsidian shader values and head/face mesh coordinates are unchanged. Lighting, fuller eye lenses, shell presentation, crown sweep and V readability are refined. Existing eye/accent controls remain available, with new default intensities.

```sh
/Applications/Blender.app/Contents/MacOS/Blender --background --python-exit-code 1 --python assets/core-warden/scripts/polish_core_warden_cw02_1.py
/Applications/Blender.app/Contents/MacOS/Blender --background assets/core-warden/blender/core_warden_cw02_1.blend --python-exit-code 1 --python assets/core-warden/scripts/validate_core_warden_cw02_1.py
python3 assets/core-warden/scripts/compare_core_warden_cw02_1.py
```

The comparison script requires Pillow. Outputs are in `renders/cw02_1/`: front, three-quarter and side at 512px, hero at 768px, and native 96px/128px front previews. All six are transparent RGBA. `comparison.png` composites these for review against CW-02 and the original concept. The saved master uses Eevee; static renders use 24 CPU Cycles samples. CW-01 and CW-02 are preserved and checked by SHA-256. No animations or runtime integration are included. See `docs/development/CORE_WARDEN_CW02_1_POLISH_REPORT.md` for validation and remaining differences.

## CW-03A animation production

`blender/core_warden_cw03a.blend` derives from CW-02.1 with three real eight-slot Actions: Idle (16 frames), Thinking (16) and Tool Running (12), all at 8 FPS. The 29 meshes and eight controls remain intact. `animations/manifest.json` lists 132 transparent PNGs across 96/128/192px. `previews/cw03a/approval.html` plays the three states at native sizes. Run the embedded `CW03A_select_state.py` helper to select a complete state or return to neutral. Build/export/package/validation commands, measured performance and approval limitations are in `docs/development/CORE_WARDEN_CW03A_ANIMATION_REPORT.md`. No TUI integration is included.

## CW-03B remaining states — visual review pending

`blender/core_warden_cw03b.blend` derives from CW-03A and adds Listening,
Processing, Approval, Speaking, Success, Warning and Error Actions while retaining
all prior masters and outputs. Open `previews/cw03b/approval.html` for all ten states
at native 96/128/192px; one-shots play once and hold their final neutral pose.
The seven new candidates are metadata-only: existing shared `states` mappings remain
unchanged. Use the CW03B build/export/package/validation scripts and see
`docs/development/CORE_WARDEN_CW03B_ANIMATION_REPORT.md` for reproduction and evidence.
An extended ten-state source catalog requires the compatible Rust asset validator;
do not replace the protected V4-B package's assets independently of its old binary.
No Voice/Agent/Desktop/TUI event integration or CW-05 is included.
