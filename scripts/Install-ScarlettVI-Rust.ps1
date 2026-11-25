# Install official, checksum-verified portable Rust inside ScarlettVI's ignored
# external compatibility directory. No global PATH changes, no game execution.
[CmdletBinding()]
param(
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')]
    [string]$Toolchain = '1.98.1',
    [switch]$VerifyOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$project = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$isolated = Join-Path $project '.external-compat'
$cargoRoot = Join-Path $isolated '.cargo'
$rustupRoot = Join-Path $isolated '.rustup'
$rustupInstaller = Join-Path $isolated 'rustup-init.exe'
$rustupUrl = 'https://static.rust-lang.org/rustup/dist/x86_64-pc-windows-msvc/rustup-init.exe'
$hashResponse = Invoke-WebRequest -UseBasicParsing -Uri ($rustupUrl + '.sha256') -TimeoutSec 45
$hashContent = if ($hashResponse.Content -is [byte[]]) {
    [Text.Encoding]::UTF8.GetString($hashResponse.Content)
} else {
    [string]$hashResponse.Content
}
$expected = ($hashContent.Trim() -split '\s+')[0].ToLowerInvariant()
if ($expected -notmatch '^[0-9a-f]{64}$') {
    throw 'Official rustup checksum endpoint returned an invalid SHA-256 digest.'
}
New-Item -ItemType Directory -Force -Path $isolated | Out-Null
$needDownload = -not (Test-Path -LiteralPath $rustupInstaller -PathType Leaf)
if (-not $needDownload) {
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $rustupInstaller).Hash.ToLowerInvariant()
    $needDownload = ($actual -ne $expected)
}
if ($needDownload) {
    # Only fetch from rust-lang.org after a trusted checksum was obtained.
    Write-Host 'Downloading the official Rustup Windows x64 installer.'
    Invoke-WebRequest -UseBasicParsing -Uri $rustupUrl -TimeoutSec 180 -OutFile $rustupInstaller
}
$actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $rustupInstaller).Hash.ToLowerInvariant()
if ($actual -ne $expected) {
    throw 'Rustup installer SHA-256 mismatch. Refusing to execute anything.'
}
Write-Host "Verified official Rustup SHA256=$actual"
if ($VerifyOnly) {
    Write-Host 'Verification only; installer not executed, no toolchain installed.'
    exit 0
}
$env:CARGO_HOME = $cargoRoot
$env:RUSTUP_HOME = $rustupRoot
Write-Host "Installing Rust $Toolchain only under $isolated (no global PATH change)"
& $rustupInstaller -y --no-modify-path --profile minimal --default-toolchain $Toolchain
if ($LASTEXITCODE -ne 0) {
    throw "Portable rustup installer failed with exit code $LASTEXITCODE."
}
$cargo = Join-Path $cargoRoot 'bin\cargo.exe'
if (-not (Test-Path -LiteralPath $cargo -PathType Leaf)) {
    throw "Rustup finished but expected portable cargo was not found: $cargo"
}
& $cargo --version
if ($LASTEXITCODE -ne 0) { throw 'Portable Cargo verification failed.' }
Write-Host "Portable Rust is ready: $cargo"
Write-Host 'No game code executed. Next: ScarlettVI-Compat.ps1 -Mode Prepare'
