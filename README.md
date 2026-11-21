# KillerArm — UltraCombo ARM64 / Anbernic RG34XX Port

An **ARM64 (AArch64) & OpenGL ES 2.0** handheld port of [UltraCombo](https://github.com/thefixinhixon/UltraCombo) (the static-recompilation native engine for **Killer Instinct** (1994) and **Killer Instinct 2** (1996) arcade boards), specifically engineered and tuned for the **Anbernic RG34XX** (Allwinner H700, 720×480 3:2 GBA form factor) and compatible Linux **PortMaster** handhelds.

> **Bring Your Own Data (BYOD):** No copyrighted ROMs, CHD disk images, or recompiled game code are included in this repository or its releases. You must supply your own legally obtained MAME `kinst` and/or `kinst2` ROM sets and CHD/raw disk images.
>
> *"Killer Instinct"* is a trademark of its respective owners. This project is independent and not affiliated with them.

---

## Hardware Feasibility & 60 FPS Analysis (Anbernic RG34XX / Allwinner H700)

| Subsystem | Anbernic RG34XX Hardware | Killer Instinct / KI2 Arcade Target | Feasibility & KillerArm Engineering |
|---|---|---|---|
| **CPU** | Allwinner H700 — 4× ARM Cortex-A53 @ 1.5 GHz (`aarch64`) | MIPS R4600 @ 100 MHz + ADSP-2105 @ 10 MHz | **Feasible at 58.96–60 FPS** with `-O2 -mcpu=cortex-a53` static recompilation (both MIPS & ADSP-2105). Idle vblank/disk-wait loops are skipped in O(1); Cortex-A53's 31× 64-bit registers map cleanly onto MIPS III's 31 registers. |
| **GPU** | ARM Mali-G31 MP2 (OpenGL ES 2.0 / 3.2, **no Desktop GL 2.1**) | 320×240 15-bit BGR555 framebuffer | **Trivial (< 1.5 ms/frame)** once ported to native **OpenGL ES 2.0** with **RGB565** uploads (`153 KB/frame`) and empty-overlay bypass. |
| **RAM** | **1 GB LPDDR4** (~750–850 MB usable) | 8.5 MB RAM + 0.5 MB ROM + 4 MB Sound ROM | **Fits comfortably (~40–55 MB runtime RSS)** after removing desktop defaults (`256 MB` rewind buffer, `17.6 MB` eager mirror/scratch, `16 MB` unpacked sound ROM). |
| **Display** | 3.4" IPS **720×480** (3:2, 60 Hz) | 320×240 (4:3, 58.96 Hz) | **Pixel-perfect 2× integer height (`640×480`)** centered inside 720×480 with crisp 40 px side pillarboxes. |
| **First-Run Compile** | 1 GB RAM + 1.5 GHz A53 | Translates ~300–600 KB MIPS + DSP to C | **Critical bottleneck on-device:** desktop UltraCombo spawns 4 parallel `zig cc` jobs (~1.5+ GB RAM → instant OOM on 1 GB handhelds). Solved via **PC cross-cache builder** (`--build-arm64`, recommended) + **RAM-throttled on-device builds** & **AArch64 TinyCC fallback**. |

### Expected Performance Breakdown on Allwinner H700 (1.5 GHz Cortex-A53)

Frame budget at 58.96 Hz / 60 Hz vsync: **16.67 – 16.96 ms/frame**.

| Build Mode | MIPS R4600 + ADSP-2105 | Video + Audio + State | Total / Frame | Expected FPS |
|---|---|---|---|---|
| **PC Pre-Built AArch64 Cache (`-O2 -mcpu=cortex-a53`, recommended)** | ~7.5 – 12.5 ms | ~1.0 – 1.8 ms (NEON RGB565 + GLES 2.0) | **~8.5 – 14.3 ms** | **58.96–60 FPS** (locked during fights; optional `frameskip = auto` absorbs rare FMV load spikes) |
| **On-Device TinyCC Fallback (`libtcc.so` AArch64)** | ~18 – 28 ms (unoptimized + interpreted DSP) | ~1.5 ms | ~20 – 30 ms | ~30 – 48 FPS (instant ~4 s compile, < 30 MB RAM) |
| **Pure Interpreter (`KI2_NOREC=1`)** | ~55 – 90 ms | ~1.5 ms | ~60+ ms | ~10 – 18 FPS (not playable) |

### Compatible Handhelds
Built against **`aarch64-linux-gnu` / `glibc 2.28`** and **OpenGL ES 2.0** (dynamically linked to the firmware's hardware-accelerated `libSDL2-2.0.so.0`):
- **Anbernic XX (Allwinner H700):** RG34XX, RG35XX Plus / H / SP / 2024, RG40XX H / V, RG CubeXX, RG28XX (muOS, Knulli, ROCKNIX, Stock OS)
- **Rockchip RK3566 (Cortex-A55 @ 1.8 GHz):** Powkiddy RGB30 / X55, Anbernic RG353 / RG503, Miyoo Flip (even more CPU headroom — solid 60 FPS)
- **Allwinner A133P / RK3326:** TrimUI Smart Pro, Anbernic RG351 / R36S (works; use `-O2` PC pre-built cache + `video.frameskip = yes` for RK3326)
- **Single-Board Computers:** Raspberry Pi 3 / 4 / 5, Orange Pi Zero 2W / 3 (64-bit Linux)

---

## Key ARM64 & Handheld Optimizations

1. **Native OpenGL ES 2.0 / GLSL ES 1.00 Backend (`src/video.c`, `src/shaders.c`, `src/gl_funcs.h`)**
   - Automatically selects OpenGL ES 2.0 (`SDL_GL_CONTEXT_PROFILE_ES`) on ARM/ARM64 while keeping full Desktop OpenGL 2.1 compatibility on PC.
   - Rewrote built-in filters (`sharp`, `smooth`, `scanlines`, `crt`) for strict GLSL ES 1.00 (unrolled bicubic taps, cached uniform locations, zero-copy `.bgra` overlay swizzle without requiring `GL_EXT_texture_format_BGRA8888`).
   - Adapts libretro `.glslp` shaders (`GL_CLAMP_TO_EDGE` wrap fallback, GLES 2.0 `GL_RGBA` FBOs, precision qualifiers).
2. **ARM NEON BGR555 → RGB565 Fast Video Path (`src/main.c`, `src/video.c`)**
   - Vectorized NEON (`vld1q_u16` / `vst1q_u16`) converts the 320×240 15-bit arcade framebuffer directly to RGB565 (`GL_UNSIGNED_SHORT_5_6_5`), cutting GPU upload bandwidth in half (**153 KB vs 307 KB per frame**) with zero colour precision loss.
3. **Zero-Cost Empty Overlay Bypass (`src/osd.c`, `src/frontend.c`, `src/video.c`)**
   - Skips the 307 KB overlay `memset`, 307 KB texture upload, and full-screen alpha-blended quad whenever no menu, OSD toast, input display, or perf overlay is visible.
4. **Low-RAM Runtime Footprint (~273 MB Saved!) (`src/state.c`, `src/dcs.c`, `src/config.c`)**
   - Lazy allocation of save-state `mirror` & `scratch` buffers (saves **17.6 MB**) and skips per-frame `state_checkpoint()` whenever rewind, run-ahead, and netplay are inactive (saves **~1.5–3.5 ms/frame** on Cortex-A53).
   - Handheld defaults: `rewind = no`, `rewind_mb = 32` (instead of `256 MB`), `audio.latency_ms = 90`, `video.fullscreen = yes`.
   - Compacted DCS sound ROM storage in `src/dcs.c` from `16 MB` (`uint16_t` sparse) to **`4 MB`** dense `uint8_t` (saves **12 MB RAM** and improves L2 cache hit rate on Cortex-A53's 512 KB L2).
5. **Cortex-A53 Static Recompiler Tuning (`src/rec_gen.h`, `src/adsp_core.h`, `src/adsprec.c`, `src/rec_build.c`)**
   - Caches `code_lo`, `code_size`, `deadline`, and `irq_dirty` pointers in `REC_LOCALS` to remove double indirections in hot loops.
   - Adds `__builtin_expect` branch-prediction hints on `STOP()`, `LD`/`ST` fast paths, and ADSP `AT()` checks to keep hot loops straight-line inside Cortex-A53's 32 KB L1I cache.
   - Eliminates x86-only unaligned pointer casts in `adsp_core.h` for ARM64 TinyCC safety.
   - RAM-aware compiler throttling (`/proc/meminfo` `MemAvailable`) caps parallel jobs to `1` on `< 1.5 GB` RAM devices, uses smaller 96 KB translation units under `UC_HANDHELD`, cleans up intermediate `.o` files, and uses `-O2 -mcpu=cortex-a53`.
6. **PortMaster Launcher & Optional Auto-Frameskip (`portmaster/UltraCombo.sh`, `src/frontend.c`, `src/picker.c`)**
   - Locks CPU governor to `performance` during play (prevents H700 `ondemand`/`schedutil` downclocking during vblank waits) and restores it on exit.
   - Fullscreen gamepad-driven game picker, welcome screen, and optional `Graphics → Frameskip (Auto)` (max 1 skipped render frame while keeping 100% deterministic emulation and uninterrupted audio).

---

## Quick Start (Anbernic RG34XX / PortMaster)

### 1. Build the PortMaster Package (on Linux x86_64)
Requires `curl`, `git`, `python3`, `make`, `tar`, `zip` (Zig 0.17.0 cross-compiler, SDL2 2.32.10 headers, and TinyCC are fetched automatically with SHA-256 verification):

```bash
make portmaster
```

This creates **`dist/UltraCombo-PortMaster-aarch64.zip`** containing:
- `UltraCombo.sh` (PortMaster launcher)
- `ultracombo/ultracombo.aarch64` (ARM64 Cortex-A53, GLES 2.0, NEON, `glibc <= 2.28`)
- `ultracombo/tools/tcc/libtcc.so` (AArch64 TinyCC fallback)
- `ultracombo/shaders/` (lightweight permissive CRT shaders)
- `ultracombo/port.json`, `gameinfo.xml`, and licences

### 2. Install on Your Handheld SD Card
1. Extract `UltraCombo-PortMaster-aarch64.zip` into your PortMaster `ports/` directory (e.g. `/mnt/sdcard/ports/` on muOS, `/userdata/roms/ports/` on Knulli, `/roms/ports/` on ArkOS).
2. Copy your own legal MAME game files into `ports/ultracombo/run/`:
   - **Killer Instinct (`kinst`):** `ki-l15d.u98` (optional `ki-l14.u98`), `u10-l1`…`u13-l1`, `u33-l1`…`u36-l1`, and `kinst.chd` (or `kinst.raw`).
   - **Killer Instinct 2 (`kinst2`):** `ki2-l14.u98`, `ki2_l1.u10`…`ki2_l1.u13`, `ki2_l1.u33`…`ki2_l1.u36`, and `kinst2.chd` (or `kinst2.raw`).

### 3. Pre-Build the `-O2` Cortex-A53 Cache on PC (Recommended for 60 FPS!)
Because compiling optimized `-O2` code with Clang/LLVM exceeds 1 GB RAM if run in parallel on the RG34XX, you can pre-compile your own ROMs' native AArch64 module on a PC in ~15 seconds:

```bash
# Point at a folder containing run/ with your own ROMs & CHDs
tools/build-arm64-cache.sh /path/to/ports/ultracombo cortex-a53
```

*(For RK3566 handhelds like RGB30/Miyoo Flip, pass `cortex-a55` instead of `cortex-a53`.)*

This runs UltraCombo headlessly on your PC, translates your game's MIPS + ADSP-2105 code to C, and cross-compiles **`cache/rec-<hash>-aarch64.so`** using `zig cc -target aarch64-linux-gnu.2.28 -mcpu=cortex_a53 -O2`. Copy `cache/rec-*-aarch64.so` into `ports/ultracombo/cache/` on your SD card — the RG34XX will load it immediately in < 0.1 s at full 60 FPS!

> **Tip (CHD vs Raw):** MAME `.chd` files work directly via bundled `libchdr`. If you notice brief hitches during FMV/stage loads on a slower microSD card, extracting a raw disk image (`./ultracombo --game ki1 --write-raw kinst.raw`) removes on-the-fly CHD decompression CPU overhead.

---

## Controls (Anbernic RG34XX)

| Action | Default RG34XX / PortMaster Mapping | Classic SNES-Style Tip (Rebind in Menu) |
|---|---|---|
| Move | D-Pad | D-Pad |
| Quick / Medium / Fierce Punch | `X` / `Y` / `R1` | `Y` / `X` / `L1` |
| Quick / Medium / Fierce Kick | `A` / `B` / `R2` | `B` / `A` / `R1` |
| Start / Coin | `Start` / `Select` | `Start` / `Select` |
| Open Menu | `Menu` (Guide) or tap `Select + Start` | `Menu` or tap `Select + Start` |
| Quit to PortMaster | Hold `Select + Start` for 2 seconds | Hold `Select + Start` for 2 seconds |

All buttons and gamepad hotkeys (Save/Load state, Screenshot, Rewind, Fast-forward) can be rebound in **Menu → Controls → Gamepad buttons / Gamepad hotkeys**.

---

## Desktop Build & Verification

Standard desktop builds remain 100% supported and deterministic:

```bash
make                  # native x86_64/arm64 desktop build
make test-state-files # synthetic save-state admission & round-trip tests
make portmaster       # cross-compile AArch64 binary, libtcc.so & PortMaster zip
```

To test the OpenGL ES 2.0 path on a Linux desktop with Mesa EGL:

```bash
KI2_GLES=1 ./ultracombo
```

---

## Credits & Licence

- **Upstream Engine:** [UltraCombo](https://github.com/thefixinhixon/UltraCombo) by `thefixinhixon` (MIT Licence).
- **ARM64 / GLES 2.0 / Anbernic RG34XX PortMaster Port:** KillerArm (MIT Licence — see [LICENSE](LICENSE)).
- Bundled third-party components retain their original licences: `stb_image` (MIT / Public Domain), `libchdr` + `zstd` + `LZMA` + `miniz` (BSD / MIT / Public Domain), `TinyCC` (LGPL 2.1, loaded dynamically), and permissive CRT shaders (see `shaders/README.txt`).
