# Raiden Fighters Remix Collection port changes

## 0.2 — 2026-11-15

- Preserve queued NGS2 PCM blocks and dispatch block-end callbacks so the game
  can refill its three staging buffers continuously.
- Keep callback refill outside the graph lock and suppress stale reset/stop
  events. Add native streaming regression checks and a FEX callback-dispatch fixture.
- Verify real guest refill callbacks and sustained non-silent PCM output during
  an 80-second native run, with normal Exit completing successfully.
- Reach player stages in all three games on Linux, with 60 FPS in steady
  sections, and document the separate Options/Start presses needed after boot.
- Install Android version 0.2/code 2 with verified SD-card data and APK hashes;
  the device gameplay check remains pending.

## 0.1 — 2026-11-15

- Start the separate CUSA53688 v1.20 Linux and Android port, retaining shared
  settings, L3+R3 access and Android graphics-driver ZIP import.
- Read PFS inode records at filesystem block boundaries and stream the nested
  filesystem instead of copying the whole image into memory.
- Add focused extraction regression checks for signed/unsigned inode padding
  and reads spanning noncontiguous file blocks.
- Add real frame-based ATRAC9 Audiodec support, controller lookup, local clock,
  offline NP reachability and cooperative guest-thread cancellation.
- Resolve lowercased game requests against original mixed-case package paths,
  preserving exact spelling, collision handling and save-file case sensitivity.
- Verify native title/menu/arcade-attract rendering and save creation; build and
  install the separate Android app with app-scoped SD-card game storage.
- Player-stage and Android device runtime verification remain in progress.
