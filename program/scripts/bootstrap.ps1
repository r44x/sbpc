# One-time setup: initialise git, add SDL3 and TinyCC as submodules, make the first commit.
# Safe to re-run; steps that are already done are skipped.
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "../..")   # the repository root (SBPC)
Write-Host "SBPC bootstrap in $(Get-Location)"

if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) {
    Write-Host "git was not found. Install it with:  winget install --id Git.Git -e"
    Write-Host "then open a NEW terminal and run scripts\bootstrap.bat again."
    exit 1
}

# Runs git.exe and stops on failure. (Named Invoke-Git: PowerShell names are case-insensitive,
# so a function called "Git" would shadow git itself.)
function Invoke-Git {
    & git.exe @args
    if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') failed (exit $LASTEXITCODE)" }
}

if (-not (Test-Path ".git")) { Invoke-Git init -b main }

function Add-Sub($url, $path) {
    if (Test-Path (Join-Path $path ".git")) { Write-Host "$path already present"; return }
    Write-Host "Adding $path from $url ..."
    # Clean up what a previously failed 'submodule add' may have left behind.
    $leftover = Join-Path ".git/modules" $path
    if (Test-Path $leftover) { Remove-Item -Recurse -Force $leftover }
    if (Test-Path $path) { Remove-Item -Recurse -Force $path }
    # These fail harmlessly when the section doesn't exist.
    cmd /c "git config -f .gitmodules --remove-section submodule.$path 2>nul"
    cmd /c "git config --remove-section submodule.$path 2>nul"
    Invoke-Git submodule add --force $url $path
}

# A clone already lists the submodules: just check them out.
if (Test-Path ".gitmodules") { Invoke-Git submodule update --init }

# repo.or.cz (TinyCC's main host) refuses UK connections (HTTP 451), so use the GitHub mirror.
Add-Sub "https://github.com/libsdl-org/SDL.git" "libs/SDL"
Add-Sub "https://github.com/TinyCC/tinycc.git"   "libs/tinycc"

# Pin SDL to the newest 3.x release tag.
Write-Host "Pinning SDL to the newest 3.x release ..."
Invoke-Git -C libs/SDL fetch --tags --quiet
$sdlTag = (& git.exe -C libs/SDL tag -l "release-3.*" --sort=-v:refname | Select-Object -First 1)
Invoke-Git -C libs/SDL checkout --quiet $sdlTag
$tccRev = (& git.exe -C libs/tinycc rev-parse --short HEAD)
Write-Host "SDL pinned to $sdlTag"
Write-Host "TinyCC pinned to $tccRev"

Invoke-Git add -A
& git.exe diff --cached --quiet
if ($LASTEXITCODE -eq 0) {
    Write-Host "Nothing new to commit."
} else {
    & git.exe commit -m "Add submodules: SDL $sdlTag, TinyCC $tccRev"
    if ($LASTEXITCODE -ne 0) { Write-Host "Commit failed: set git user.name / user.email, then commit manually." }
}
Write-Host "Done."
