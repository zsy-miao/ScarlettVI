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

The connected Windows machine reports an **NVIDIA GeForce RTX 5060 Ti**, and the supplied title executable exists beside the ScarlettVI project. `Auto` therefore selects **ztrx**. Official **Rust 1.98.1 was installed successfully as a project-local portable toolchain** under `.external-compat/.cargo` and `.external-compat/.rustup`, without modifying the system PATH. Its official installer SHA-256 was verified before execution. The ztrx **Release `victory.exe` build succeeded** at upstream Git revision `90b529f878a4`, and its `--help` and read-only PE `inspect` commands passed. Actual game execution, GPU rendering and FPS have **not** been tested on this machine.

## Verified upstream PE-binding result (without game execution)

The locally built NVIDIA/ztrx `victory.exe` at GPL upstream revision `90b529f878a4` successfully ran its own **read-only `inspect`** command against this title. It identified a PE32+ executable with **587 imported symbols from 68 modules**, relocations and **two TLS callbacks**.

The new ScarlettVI `-Mode Bind` was then tested on the same title using `victory.exe run <EXE>` **without `--execute`**. Its actual output ended `not executing (pass --execute to start the title)` and exited **0**, reporting:

| ztrx upstream image | HLE bindings | Host bindings | Title-local bindings | Missing bindings |
| --- | ---: | ---: | ---: | ---: |
| Main `game_scarlett_master.exe` | 88 | 427 | 72 | **0** |
| `XboxBvhBuilder.dll` | 9 | 48 | 0 | **0** |
| `amd_fidelityfx_loader_xbox.dll` | 4 | 18 | 0 | **0** |
| `libHttpClient.GDK.dll` | 27 | 171 | 0 | **0** |
| `Bink2Scarlett.dll` | 6 | 54 | 0 | **0** |

These are **ztrx's own** counts for this specific run and loader, not ScarlettVI native resolver counts. `0 missing` means named imports had callable addresses; it does **not** establish correct GPU/APU/DirectStorage semantics, title entry execution, sound, frames or FPS. ztrx maps patched image copies only in the separate Git-ignored cache; the source game remains unchanged. No title entry was started during these probes.

### Important difference from upstream memory HLE

Read-only MSVC Dumpbin of the supplied title shows a direct `XMemTransferMemory` call at VA `0x14221608E` via thunk `0x145F3FD70`: it passes **kind=1** in ECX and a size rounded to **2 MiB** up to **16 MiB** in RDX, then tests the BOOL result, with a breakpoint on failure. Both checked GPL upstream forks currently implement successful memory transfer only for **kind=0** (`crates/victory-hle/src/modules/xmem.rs`). Blindly copying their kind-0-only policy cannot satisfy this title's observed call. The meaning and budget effect of kind=1 require independently verified host accounting, not a fake-success shim. This is a concrete ScarlettVI-native compatibility task, and no GPL source code was copied.

## Commands (PowerShell, from ScarlettVI/)

**1. Inspect, with no download or execution:**

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\ScarlettVI-Compat.ps1 -Mode Check

To choose the AMD branch explicitly (on an AMD machine), add `-Backend BelobugAMD`.

**2. Reproducibly install pinned Rust only within `.external-compat` when missing:**

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\Install-ScarlettVI-Rust.ps1 -VerifyOnly
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\Install-ScarlettVI-Rust.ps1

The first command downloads the **official rust-lang.org Windows installer**, verifies its official SHA-256, and **does not execute it**. The second re-verifies the hash then installs **Rust 1.98.1** into local ignored `CARGO_HOME`/`RUSTUP_HOME` roots with `--no-modify-path`, never requiring a global Rust installation. Both upstream repos pin Rust 1.98.1. The main launcher automatically detects this portable Cargo. This setup was completed on the connected PC; do not needlessly reinstall it.

**3. Prepare only, no game execution:**

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\ScarlettVI-Compat.ps1 -Mode Prepare

This clones the chosen GPL source into the isolated ignored directory, checks that its Git origin is exactly the expected upstream, reports its Git revision, and builds `victory.exe`. ztrx is built in release mode; the AMD branch uses the diagnostics build described in its own README. It never copies or executes the title.

**4. Optionally verify the intended upstream invocation without executing it:**

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\ScarlettVI-Compat.ps1 -Mode Plan

This checks the exact external checkout origin, build product and title path, then prints the real launch command without starting it or writing saves/caches.

**5. Reproduce the real title's full PE import binding without executing its entry point:**

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\ScarlettVI-Compat.ps1 -Mode Bind

`Bind` invokes the upstream `victory.exe run <game>` **without `--execute`**. Upstream help explicitly says this stops after binding. It may create patched image-cache copies and separate virtual-storage directories under the ignored `.external-compat` folder; it never writes changes back to the original title. Do not confuse this with game boot or rendering.

**6. Explicitly start the separate upstream executable (only after preparation and if you choose to run the title):**

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\ScarlettVI-Compat.ps1 -Mode Play

Use `-Game 'D:\my-game-folder\game_scarlett_master.exe'` to choose an existing title executable if the default adjacent file is absent. Saves and shader caches are kept in separate, ignored `.external-compat/data` and `.external-compat/cache` directories. `-Fast` opts into a documented upstream scheduler setting: `VICTORY_RELAY=2` for ztrx or `VICTORY_HOIST_CLAMPS=7` for AMD. Do not assume either improves this PC until benchmarked. The wrapper never downloads or compiles as a side effect of `Play`; run `Prepare` explicitly first. It calls the actual upstream executable directly and preserves its real process exit code, rather than hiding errors behind the upstream PowerShell play script.

**Test the launcher without launching or downloading anything:**

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\Test-ScarlettVI-Compat.ps1

The test creates a synthetic **data-only** fake `.exe` under the ignored `reports/` folder, checks both explicit backends in `Check` mode and verifies that `Play` refuses a missing game. When `.external-compat` does not exist, it also tests the non-executing `Plan` command with a disposable synthetic Git checkout and **non-executable** placeholder. It never deletes or modifies existing real upstream checkouts, caches or saves. On the connected Windows host the tests passed, and the actual built ztrx checkout also passed `Plan` and `Bind` without starting title code.

## Boundaries and next native implementation steps

The external projects already implement substantially more startup services than native ScarlettVI. Directly combining Rust code with ScarlettVI's unrelated C++ PE mapper is not a reliable drop-in fix. Continue native implementation and generated test coverage for AcpHal/XAPU/CHAD queues, XG texture COM interfaces, Xbox DirectStorage object model, D3D12 shader translation, guest TLS and process lifecycle, and GPU memory write detection. An import address alone is not gameplay readiness. Once ScarlettVI has a real in-game benchmark, compare the two external projects under identical GPU/resolution/cache conditions and port *concepts* where independently tested. Avoid promising frame-rate gains from synthetic fixtures.
