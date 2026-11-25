# Original ScarlettVI launcher for separately licensed upstream executables.
# Check, Prepare and Plan NEVER execute the title. Play is explicitly opt-in.
[CmdletBinding()]
param(
    [ValidateSet('Check','Prepare','Plan','Play')][string]$Mode = 'Check',
    [ValidateSet('Auto','ztrx','BelobugAMD')][string]$Backend = 'Auto',
    [string]$Game,
    [switch]$Fast
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$project = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$sources = @{
    ztrx = 'https://github.com/Zettrox/ztrx.git'
    BelobugAMD = 'https://github.com/Belobug/VIctory-Belobug-AMD.git'
}
$isolated = Join-Path $project '.external-compat'
function Fail([string]$why, [int]$code = 2) {
    [Console]::Error.WriteLine("ScarlettVI compat: $why")
    exit $code
}
$gpuNames = @()
try {
    $gpuNames = @(Get-CimInstance Win32_VideoController -ErrorAction Stop |
                  ForEach-Object { $_.Name } | Where-Object { $_ })
} catch {
    $gpuNames = @()
}
$gpu = if ($gpuNames.Count) { $gpuNames -join '; ' } else { 'unknown' }
if ($Backend -eq 'Auto') {
    $nvidia = @($gpuNames | Where-Object { $_ -match 'NVIDIA' })
    $amd = @($gpuNames | Where-Object { $_ -match '(AMD|Radeon)' })
    if ($nvidia.Count -gt 0 -and $amd.Count -eq 0) {
        $Backend = 'ztrx'
    } elseif ($amd.Count -gt 0 -and $nvidia.Count -eq 0) {
        $Backend = 'BelobugAMD'
    } else {
        Fail "No unambiguous GPU/backend match for $gpu. Use -Backend ztrx or -Backend BelobugAMD."
    }
}
if ($Backend -eq 'ztrx' -and $gpu -notmatch 'NVIDIA') {
    Write-Warning 'ztrx documents NVIDIA GPUs only; the chosen backend may not work.'
}
if ($Backend -eq 'BelobugAMD' -and $gpu -notmatch '(AMD|Radeon)') {
    Write-Warning 'Belobug documents AMD Radeon GPUs only; the chosen backend may not work.'
}
$checkout = Join-Path $isolated $Backend
$source = $sources[$Backend]
$defaultGame = Join-Path $project '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'
$gameCandidate = if ($Game) { $Game } else { $defaultGame }
$gamePath = $null
if (Test-Path -LiteralPath $gameCandidate -PathType Leaf) {
    $gamePath = (Resolve-Path -LiteralPath $gameCandidate).ProviderPath
}
$foundGame = ($null -ne $gamePath -and [System.IO.Path]::GetExtension($gamePath) -ieq '.exe')
$git = Get-Command git -ErrorAction SilentlyContinue
$cargo = Get-Command cargo -ErrorAction SilentlyContinue
$checkoutReady = Test-Path -LiteralPath (Join-Path $checkout '.git') -PathType Container
$exe = if ($Backend -eq 'ztrx') {
    Join-Path $checkout 'target\release\victory.exe'
} else {
    Join-Path $checkout 'target\diag\victory.exe'
}
$cache = Join-Path $isolated 'cache'
$saves = Join-Path $isolated 'data'
Write-Host 'ScarlettVI | external compatibility runner'
Write-Host "Mode=$Mode | Backend=$Backend | GPU=$gpu"
Write-Host "Read-only title executable found=$foundGame"
Write-Host "Upstream=$source"
Write-Host "Isolated checkout=$checkout"
Write-Host "Git found=$([bool]$git) | Rust Cargo found=$([bool]$cargo)"
Write-Host "Upstream checkout found=$checkoutReady"
Write-Host "Upstream binary found=$(Test-Path -LiteralPath $exe -PathType Leaf)"
Write-Host "Cache=$cache | Saves=$saves"
Write-Host 'ScarlettVI native title execution remains BLOCKED.'
if ($Mode -eq 'Check') {
    if (-not $cargo) {
        Write-Warning 'Rust/Cargo is missing; use official rustup at https://rustup.rs/ to build the separate upstream runner.'
    }
    if (-not $foundGame) {
        Write-Warning 'No title exe found; pass -Game with your own existing title executable.'
    }
    Write-Host 'CHECK ONLY: no game started, repository cloned or tool installed.'
    exit 0
}
if (-not $foundGame) { Fail 'No valid game executable. Pass -Game to your own title executable.' }
if (-not $git) { Fail 'Git is required.' }
if ($Mode -eq 'Prepare' -and -not $cargo) {
    Fail 'Rust/Cargo missing. Install official rustup before preparing the upstream runner; no download was performed.'
}
if ($Mode -eq 'Prepare') {
    if ((Test-Path -LiteralPath $checkout) -and -not $checkoutReady) {
        Fail "Refusing to overwrite non-Git checkout directory: $checkout"
    }
    if (-not $checkoutReady) {
        New-Item -ItemType Directory -Force -Path $isolated | Out-Null
        Write-Host 'Cloning GPL-3.0 upstream separately (not vendored into ScarlettVI Git).'
        & $git.Source clone --depth 1 -- $source $checkout
        if ($LASTEXITCODE -ne 0) { Fail 'Clone failed.' 3 }
    }
} elseif (-not $checkoutReady) {
    Fail 'Upstream not prepared. Run -Mode Prepare first.'
}
$actualOrigin = (& $git.Source -C $checkout remote get-url origin 2>$null)
if ($LASTEXITCODE -ne 0) { Fail 'Cannot verify upstream checkout origin.' }
$actualOrigin = ($actualOrigin.Trim() -replace '\.git$', '').TrimEnd('/')
$expectedOrigin = ($source -replace '\.git$', '').TrimEnd('/')
if ($actualOrigin -ine $expectedOrigin) {
    Fail "Unexpected checkout origin: $actualOrigin"
}
$revision = (& $git.Source -C $checkout rev-parse --short=12 HEAD)
if ($LASTEXITCODE -ne 0) { Fail 'Cannot identify upstream revision.' }
Write-Host "Upstream revision: $revision"
Write-Host 'The upstream GPL-3.0 licence remains in its separate repository.'
if ($Mode -eq 'Prepare') {
    Push-Location $checkout
    try {
        if ($Backend -eq 'ztrx') {
            & $cargo.Source build --locked --release -p victory
        } else {
            & $cargo.Source build --locked --profile diag -p victory --features diagnostics
        }
        if ($LASTEXITCODE -ne 0) { Fail 'Upstream compilation failed.' 3 }
    } finally {
        Pop-Location
    }
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
        Fail 'Build passed but the expected upstream executable is missing.' 3
    }
    Write-Host 'Prepared independent backend. Play requires a separate explicit -Mode Play invocation.'
    Write-Host 'No title code was executed.'
    exit 0
}
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    Fail "Missing upstream binary: $exe. Run -Mode Prepare."
}
$extra = if ($Backend -eq 'ztrx') { '' } else { ' --console series-x' }
Write-Host ('Backend execution: "{0}" run "{1}" --execute{2}' -f $exe, $gamePath, $extra)
if ($Mode -eq 'Plan') {
    Write-Host 'PLAN ONLY: no upstream executable or game code executed.'
    exit 0
}
New-Item -ItemType Directory -Force -Path $cache, $saves | Out-Null
$env:VICTORY_GAME_DIR = Split-Path -Parent $gamePath
$env:VICTORY_CACHE_DIR = $cache
$env:VICTORY_DATA_DIR = $saves
if ($Fast) {
    if ($Backend -eq 'ztrx') {
        $env:VICTORY_RELAY = '2'
    } else {
        $env:VICTORY_HOIST_CLAMPS = '7'
    }
    Write-Host 'Optional upstream fast setting enabled; not benchmarked on this PC.'
}
if ($Backend -eq 'ztrx') {
    # ztrx's public play script clears automated diagnostic presses/captures.
    # Invoke its native engine directly here so the real process exit code
    # (including crashes) is preserved rather than hidden by the script.
    foreach ($name in @('VICTORY_KEYS','VICTORY_CAPTURE','VICTORY_CAPTURE_FRAMES','VICTORY_DUMP_TARGETS')) {
        Remove-Item "Env:$name" -ErrorAction SilentlyContinue
    }
}
Write-Host 'Launching independent upstream process, NOT ScarlettVI native execution.'
Push-Location $checkout
try {
    if ($Backend -eq 'ztrx') {
        & $exe run $gamePath --execute
    } else {
        & $exe run $gamePath --execute --console series-x
    }
    $upstreamExitCode = $LASTEXITCODE
} finally {
    Pop-Location
}
exit $upstreamExitCode
