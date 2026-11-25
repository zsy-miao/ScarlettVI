# Separately licensed upstream runtime backends

ScarlettVI's **original C++20 native compatibility layer remains distinct and not yet game-bootable**. The script `scripts/ScarlettVI-Compat.ps1` offers a practical way to inspect, prepare and explicitly launch two existing external GPL-3.0 Rust compatibility executables while native ScarlettVI HLE development continues. This is an **external process integration**, not a claim that ScarlettVI's own Xbox audio, graphics, storage, TLS or shader translation is complete.

## Upstream findings, 2026-11-25

| Backend | Developer-reported result | Hardware | Unfinished work |
| --- | --- | --- | --- |
| [Zettrox/ztrx](https://github.com/Zettrox/ztrx) | Boot/menu and some prologue gameplay, **1–3 FPS** on one RTX 3050 | NVIDIA RTX 20+ class, D3D12 SM 6.6 + mesh shaders | Ray-traced lighting, overexposure and scene artifacts |
| [Belobug/VIctory-Belobug-AMD](https://github.com/Belobug/VIctory-Belobug-AMD) | Game world lit and moving, **3–5 FPS** on one RX 9070 XT | AMD Radeon tested; not validated on NVIDIA | Missing ray-traced indirect light; phone rendering broken; high RAM/VRAM usage; occasional GPU driver timeout |

Belobug documents three optimization/correctness ideas that are relevant to ScarlettVI's future real GPU work: **pre-recorded indirect-draw clamp batches** (one scene's GPU time reportedly 225 to roughly 80 ms), **guest physical-page alias change detection** for writes through more than one VA, and better shader quad/helper-lane handling for AMD. These are their measured or observed claims, not proven ScarlettVI improvements. ztrx describes optional `VICTORY_RELAY=2` and pipeline/shader caches on NVIDIA. Neither project is playable at normal FPS, and no performance result on this user's RTX 5060 Ti was measured.

**Licensing:** both upstream repositories are GPL-3.0-or-later. ScarlettVI's native implementation does not copy or link their GPL code, generated shaders, game assets or private SDKs. Instead, this script clones an unmodified separate repository under `.external-compat/`, retains its licence and Git history there, and invokes its own executable as a separate process. `.external-compat/` is Git-ignored; never commit those source checkouts, Rust build artifacts, game files, derived shader caches or saves. Any actual direct source-code integration would require a deliberate licence-compliance decision and correct attribution.

## Verified machine prerequisites

The currently connected Windows machine reports an **NVIDIA GeForce RTX 5060 Ti** and the supplied title executable exists beside the ScarlettVI project. The `Auto` backend therefore selects **ztrx**. Git is installed. **Rust/Cargo is not installed**, so the external runner was not compiled or run. D3D12 feature-level eligibility and actual game compatibility have not been verified on this GPU.

## Commands (PowerShell, from ScarlettVI/)

**1. Inspect, with no download or execution:**

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\ScarlettVI-Compat.ps1 -Mode Check

To choose the AMD branch explicitly (on an AMD machine), add `-Backend BelobugAMD`.

**2. Install the official Rust toolchain** from [rustup.rs](https://rustup.rs/) if Cargo is missing, then open a new PowerShell window. The upstream repos pin their Rust toolchain (Belobug specifies 1.98.1); the launcher does not install toolchains automatically.

**3. Prepare only, no game execution:**

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\ScarlettVI-Compat.ps1 -Mode Prepare

This clones the chosen GPL source into the isolated ignored directory, checks that its Git origin is exactly the expected upstream, reports its Git revision, and builds `victory.exe`. ztrx is built in release mode; the AMD branch uses the diagnostics build described in its own README. It never copies or executes the title.

**4. Optionally verify the intended upstream invocation without executing it:**

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\ScarlettVI-Compat.ps1 -Mode Plan

This checks the exact external checkout origin, build product and title path, then prints the real launch command without starting it or writing saves/caches.

**5. Explicitly use that external backend (only after preparation):**

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\ScarlettVI-Compat.ps1 -Mode Play

Use `-Game 'D:\my-game-folder\game_scarlett_master.exe'` to choose an existing title executable if the default adjacent file is absent. Saves and shader caches are kept in separate, ignored `.external-compat/data` and `.external-compat/cache` directories. `-Fast` opts into a documented upstream scheduler setting: `VICTORY_RELAY=2` for ztrx or `VICTORY_HOIST_CLAMPS=7` for AMD. Do not assume either improves this PC until benchmarked. The wrapper never downloads or compiles as a side effect of `Play`; run `Prepare` explicitly first. It calls the actual upstream executable directly and preserves its real process exit code, rather than hiding errors behind the upstream PowerShell play script.

**Test the launcher without launching or downloading anything:**

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\Test-ScarlettVI-Compat.ps1

The test creates a synthetic **data-only** fake `.exe` under the Git-ignored `reports/` folder, checks both explicit backends in `Check` mode, verifies that `Play` refuses a missing game before accessing an upstream program, tests the planned command with a disposable locally generated fake Git checkout and a non-executable text placeholder, and cleans up all fixture-created data. The connected Windows host passed these tests. An attempted `Prepare` without Cargo correctly refused before cloning. No game code was run.

## Boundaries and next native implementation steps

The external projects already implement substantially more startup services than native ScarlettVI. Directly combining Rust code with ScarlettVI's unrelated C++ PE mapper is not a reliable drop-in fix. Continue native implementation and generated test coverage for AcpHal/XAPU/CHAD queues, XG texture COM interfaces, Xbox DirectStorage object model, D3D12 shader translation, guest TLS and process lifecycle, and GPU memory write detection. An import address alone is not gameplay readiness. Once ScarlettVI has a real in-game benchmark, compare the two external projects under identical GPU/resolution/cache conditions and port *concepts* where independently tested. Avoid promising frame-rate gains from synthetic fixtures.
