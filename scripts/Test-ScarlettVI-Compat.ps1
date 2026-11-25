# Host-generated safety/CLI regression. No game code, GPL source or network required.
$ErrorActionPreference = 'Stop'
$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$runner = Join-Path $PSScriptRoot 'ScarlettVI-Compat.ps1'
$reports = Join-Path $root 'reports'
$external = Join-Path $root '.external-compat'
$syntheticGame = Join-Path $reports 'synthetic-compat-fixture.exe'
$missingGame = Join-Path $reports 'synthetic-compat-fixture-absent.exe'
if (Test-Path -LiteralPath $missingGame) {
    throw 'An existing file collides with the missing-game test.'
}
New-Item -ItemType Directory -Force -Path $reports | Out-Null
[System.IO.File]::WriteAllText($syntheticGame, 'SYNTHETIC DATA; NOT EXECUTABLE')
$createdFixtureRoot = $false
try {
    foreach ($backend in @('ztrx','BelobugAMD')) {
        $message = (& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $runner -Mode Check -Backend $backend -Game $syntheticGame 2>&1 | Out-String)
        if ($LASTEXITCODE -ne 0 -or
            $message -notmatch "Backend=$backend" -or
            $message -notmatch 'Read-only title executable found=True' -or
            $message -notmatch 'CHECK ONLY: no game started' -or
            $message -notmatch 'ScarlettVI native title execution remains BLOCKED') {
            throw "Unexpected Check results for $backend : $message"
        }
    }
    # Run an expected-failure child in a scope where PowerShell 5.1 does not
    # throw on the child's deliberately emitted stderr text.
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $rejected = (& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $runner -Mode Play -Backend ztrx -Game $missingGame 2>&1 | Out-String)
        $rejectExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($rejectExit -ne 2 -or $rejected -notmatch 'No valid game executable') {
        throw "Missing-game Play rejection failed: $rejected"
    }

    # Existing real upstream is user data: verify its Plan against the
    # generated, non-executable title marker without ever invoking it.
    $realCheckout = Join-Path $external 'ztrx'
    $realBinary = Join-Path $realCheckout 'target\release\victory.exe'
    if ((Test-Path -LiteralPath (Join-Path $realCheckout '.git') -PathType Container) -and
        (Test-Path -LiteralPath $realBinary -PathType Leaf)) {
        $planned = (& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $runner -Mode Plan -Backend ztrx -Game $syntheticGame 2>&1 | Out-String)
        if ($LASTEXITCODE -ne 0 -or $planned -notmatch 'PLAN ONLY: no upstream executable') {
            throw "Existing upstream Plan validation failed: $planned"
        }
        $playCmd = @($planned -split '[\r\n]+' | Where-Object { $_ -match '^Backend play command:' })
        $bindCmd = @($planned -split '[\r\n]+' | Where-Object { $_ -match '^Backend bind-only command:' })
        if ($playCmd.Count -ne 1 -or $bindCmd.Count -ne 1 -or
            $playCmd[0] -notmatch '\-\-execute' -or
            $bindCmd[0] -match '\-\-execute') {
            throw "Plan incorrectly adds/removes the execution flag: $planned"
        }
    }

    # Only create a temporary local fake upstream checkout if none exists.
    # In real use .external-compat may contain a user's compiled runner and
    # valuable shader cache/saves; tests MUST NEVER remove or overwrite it.
    if (-not (Test-Path -LiteralPath $external)) {
        $git = Get-Command git -ErrorAction SilentlyContinue
        if ($git) {
            $createdFixtureRoot = $true
            $repo = Join-Path $external 'ztrx'
            $folder = Join-Path $repo 'scripts'
            $outputFolder = Join-Path $repo 'target\release'
            New-Item -ItemType Directory -Force -Path $folder, $outputFolder | Out-Null
            & $git.Source init --quiet $repo
            if ($LASTEXITCODE -ne 0) { throw 'Synthetic Git init failed' }
            & $git.Source -C $repo remote add origin https://github.com/Zettrox/ztrx.git
            if ($LASTEXITCODE -ne 0) { throw 'Synthetic remote setup failed' }
            Set-Content -LiteralPath (Join-Path $repo 'fixture.txt') -Value 'Local dummy Git fixture' -Encoding ascii
            & $git.Source -C $repo add fixture.txt
            if ($LASTEXITCODE -ne 0) { throw 'Synthetic Git add failed' }
            & $git.Source -C $repo -c user.name=ScarlettVI-Test -c user.email=fixture@example.invalid commit --quiet -m fixture
            if ($LASTEXITCODE -ne 0) { throw 'Synthetic Git commit failed' }

            # Plan checks the complete validated upstream launch path
            # without executing the deliberately NON-executable fake binary.
            Set-Content -LiteralPath (Join-Path $outputFolder 'victory.exe') -Value 'NOT AN EXECUTABLE; SYNTHETIC' -Encoding ascii
            $simulated = (& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $runner -Mode Plan -Backend ztrx -Game $syntheticGame -Fast 2>&1 | Out-String)
            if ($LASTEXITCODE -ne 0 -or
                $simulated -notmatch 'Upstream revision:' -or
                $simulated -notmatch 'synthetic-compat-fixture.exe' -or
                $simulated -notmatch '\-\-execute' -or
                $simulated -notmatch 'PLAN ONLY: no upstream executable') {
                throw "Synthetic validated command plan failed: $simulated"
            }
        }
    } else {
        Write-Host 'Upstream cache exists; synthetic Plan path skipped to protect real user data.'
    }
    Write-Host 'ScarlettVI | separate upstream compatibility safety tests: PASS'
    Write-Host 'Both backends, missing-game refusal and verified non-executing Plan where available; no title execution or upstream downloads.'
    exit 0
} finally {
    Remove-Item -LiteralPath $syntheticGame -Force -ErrorAction SilentlyContinue
    if ($createdFixtureRoot) {
        Remove-Item -LiteralPath $external -Recurse -Force -ErrorAction SilentlyContinue
    }
}
