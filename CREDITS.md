# Credits

Droplet is built on the work of the projects below. Their copyright notices stay in the files they
cover, and their license texts are in [LICENSES](LICENSES).

## Where Droplet comes from

- **[eden-nx2](https://git.eden-emu.dev/Takoroka/eden-nx2)** by Takoroka: Nintendo Switch 2 support
  for Eden. Droplet is a modified version of eden-nx2 commit `50fa86b` ("nx2: fix the Linux (GCC)
  build of the vi2 present stats"), changed from 2026-10-30 on (see below). GPL-3.0-or-later.
- **[Eden](https://git.eden-emu.dev/eden-emu/eden)**, the Eden Emulator Project, which eden-nx2 is
  built on. GPL-3.0-or-later.
- **yuzu** (the yuzu Emulator Project) and **Sudachi**, which Eden continues, and **Citra**, where
  yuzu's code began. yuzu's code is GPL-2.0-or-later.
- **[Eglantine](https://github.com/Djihads80/Eglantine)**, an experimental Switch 2 emulator built on
  eden-nx2 (GPL-3.0). Droplet merges its `nx2-experimental` branch, which Eglantine's newest
  releases are built from and which holds all of `nx2`, up to commit `a23be213d5` (2026-11-06),
  except Eglantine's CI, documentation, translation and branding changes, its notes for AI
  assistants, pull request #313 and a few commits left out on purpose. That is 611 of its commits,
  by Takoroka (eden-nx2's author) 188, kraibse 87, Duck912/duck912/Duck 61, rdenno 42,
  Sssnowyyy/Alex 27, Yuri Olive 22, Jonas 21, snowstormxd 21, Paulo Rossatto 10, a55uka 7, néo
  Luigi 7, givethesourceplox 7, Djihads80 6, zortos293 6, nowhere02 6, Aidan Kinzett 5, SavageCore 5,
  Franz 5, vibecodekun 4, LucyferDev 4, Meow 4, PastaLaPate 3, Frizti 3, SmileyTatsu 3, Mcm190 3,
  Dennys Kaluzny Pereira 3, ZnOw01 3, Wael3rd 3, shiv121231 3, yumekiru/yumerkiru 3, Zarigis 2,
  PreTenD 2, gabrielskaminski 2, 999sian 2, Dee1 2, theofficialhackclean 2, and one each by Adya,
  Andreas Heider, arisqz, CloudP4bbs, Daniel Cabral, Djibril, Franseven, Javier, k-start, Kaydax,
  kipavy, lesmmwe, Mako, Mateescu Adrian, Matt Artist, Omar Mahboubi, PavelBARABANOV,
  QuantumDriftDev, sdas-19, ShahadatTest, spaina93, Whovian64, xtam, ymatrix1, yvonne, zirulo and
  Zoe L.
  [CREDITS-Eglantine.md](CREDITS-Eglantine.md) lists each commit with its author.
- **[PokeDinner](https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork)** by Franseven, an
  Eglantine fork (GPL-3.0). Droplet takes 70 of its commits (2026-11-03 and 2026-11-05), some in
  part: Franseven 43 (21 of them ported from the Eden-NS2 fork and one from monkeefunnee's
  Eglantine raiders-fix branch), Wael3rd 11, BXYMartin 6, Dee1 2, PavelBARABANOV 2 (Eden pull
  requests #4589 and #4591), and one each by 999sian, archie, gabrielskaminski, KaitonGxx, k-start
  and MamamaNanana.
  [CREDITS-PokeDinner.md](CREDITS-PokeDinner.md) lists each commit with its author.
- **[Lichii](https://github.com/zortos293/Lichii)** by Zortos (zortos293), an Eglantine fork
  (GPL-3.0). Droplet takes, from 2026-11-04 on: the kernel part of MythosOfs's Mario Tennis Fever
  boot fix (`3140648a66`, pull request #7); and from Zortos the Star Fox window alignment
  (`f6a8f51324`), Welcome Tour's GPU time session (`8cd5c6a44b`), native shader lowerings of build
  70 (surface loads, stores and atomics, gathers, uniform constant loads, texture sampling and
  others), and, from Zortos's unfinished branches (2026-11-05), DK Bananza's command-list page flush
  (`4071369016`), skipping empty draws (`1aae8cdf1a`), retiring gathered buffers with their owners
  (`5240da156b`), the memory-layout interval reuse (`6242f664da`), building pipelines a draw waits
  on (`457a32de0a`, `f51ec4d2f6`, `7cc964c266`), the physical-memory check for cached pages
  (`c063f4da5a`), the parallel recording prototype (`04cecebc9e`), the draw-run measurement
  (`9617979625`), Ocarina of Time's uniform IMAD (from `35c043b3ef`) and Ampere's nanosecond GPU
  timestamps (`2940d9fab0`, pull request #84); and for Ocarina of Time, shader forms from pull
  request #87 (`c9ebb5c3af`, Zortos with Jonas) and fix/oot-fog, F2I from constant buffers with
  abs/neg (`1ad14bebb8`), store data shadowing (`3c8dc569f4`, Zarigis) and the per-draw
  alpha-to-coverage finding (`82b95a65d4`). On 2026-11-05 also fixes others contributed to Lichii:
  the swapchain wait (`509809ddf4`, #88, matt petters), nested returns before moved demotes
  (`77cbc3e7c3`, #127, Inc0rporate), serialized GPU dirty memory gathers (from `7e1d645557`, #138,
  Paulo Tumabing), dropping draws without a pipeline (`fe6a388a4b`, #142, Felipe Lima), Event waits
  that last until their deadline (`9feec2650e`, #120, Zortos; `90e2d85bb3`, #137, Kaj), zortos293's
  Eglantine pull requests #427 (out-of-date swapchains, `6b7c516b09`), #544 (alpha test reference
  rounding, `ec3a455c6f`) and #547 (present pacing per process, `7b953db74e`); and for Ocarina of
  Time the paused occlusion counter (`5b89a3daf4`, #121, Kaj), two-source F2IP.U8 and indexed ULDC
  (`f356d39ba3`, #144, Kradnah), the built-in Max Resolution patch (`99ac3d5955`, #124, Zortos),
  no upload for render targets a clear overwrites (`27226d2568`, #141, Kaj), cached page-table
  ranges (`7c0b5e791e`, #146, Nyaku) and the multiply fold (from `f26ad432ae`, #150, Zortos).
- **[AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)** v1.1.4 by
  Advanced Micro Devices (MIT), through Eglantine: the FSR 3.1 upscaler's shader code, unmodified, in
  `externals/FidelityFX-SDK`.
- **Third-party libraries** fetched or bundled by the build (see `cpmfile.json` and `externals`),
  each under its own license.

## Changes from eden-nx2

- **Name and policy:**
  - the name Droplet and a new logo;
  - no encrypted content. Droplet loads no keys and has no NCA, NAX, NSP or XCI loaders, no game
    card, and no installing of games, keys or firmware.
- **Switch 2 games:**
  - graphics: mesh and task shaders (Mario Kart World), Ampere shader lowering fixes, small
    GPU-written buffer downloads, MME DMA flushes, sparse images and terrain, ASTC array layers,
    unmapped device memory pages, present pacing, the game's swap interval and a custom refresh rate;
  - system: the software keyboard (UTF-8 or UTF-16 text, as each game reads it), missing service stubs,
    the audio renderer lifetime, the JIT's near calls, and the Linux open file limit.
- **Playing together:**
  - online play on private servers (Reservoir): a server redirect, with Nintendo's hosts blocked;
    socket, SSL and network fixes; per-server friend codes, friends and profiles; room IDs;
  - LAN play: local network requests, emulator rooms in droplet-cli, polling of room sockets.
- **Frontends:**
  - decrypted games open from their folders: File > Open Game Folder (the main way to start a game),
    drag and drop and the command line, with list entries named by their folders;
  - the windowed app without a game and an online settings page;
  - controller automapping, mouse aim, and HD rumble and Switch 2 controller names for SDL
    controllers;
  - the area window filter, unlisted rooms, and frame generation on desktop Linux.
- **From Eglantine** (see [CREDITS-Eglantine.md](CREDITS-Eglantine.md)), among others: FSR 3.1
  upscaling where a game asks for DLSS, NUP game archives (Convert to NUP in a game's right-click
  menu, or `droplet-cli --pack-nup`), faster game discovery, vibration in Switch 2 games, and a
  Switch 2 game's memory released when it stops.
- **From PokeDinner** (see [CREDITS-PokeDinner.md](CREDITS-PokeDinner.md)): an optional 30 fps lock and
  a terrain fix for Donkey Kong Bananza, workarounds for it on AMD's Windows driver, Pokopia's sky,
  Opus audio and larger memory layout, Mario Tennis Fever's built-in fonts and threaded movie
  decoding, more Ampere shader translations, less shader stutter (graphics pipelines share
  translated shaders, and with asynchronous shaders on, Switch 2 pipelines are translated on worker
  threads), and the Switch 2 mouse (Joy-Con 2 mouse mode).
- **From Lichii:** Mario Tennis Fever's boot, Star Fox's window, Welcome Tour's GPU time session,
  shader translations for Star Fox, Welcome Tour, Kirby and Yoshi, DK Bananza's faster command-list
  flushes and draw setup, pipelines built at once when a draw waits for one, nanosecond GPU
  timestamps, an optional parallel recording prototype and draw-run measurement, swapchain and
  timed-wait fixes, and Ocarina of Time's shader forms, full resolution (a built-in patch that can
  be turned off in Add-Ons), occlusion counts and sky.
- **Testing aids:** unattended test hooks, RenderDoc capture, shader IR dumps and stutter diagnostics.
  All are off unless their `EDEN_NX2_*` variables are set.
