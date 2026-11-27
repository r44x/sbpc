#!/bin/sh
# One-time setup: initialise git, add SDL3 and TinyCC as submodules, make the first commit.
# Safe to re-run; steps that are already done are skipped.
set -eu
cd "$(dirname "$0")/../.."   # the repository root (SBPC)

command -v git >/dev/null || { echo "git not found - install it with your package manager"; exit 1; }

if [ ! -d .git ]; then
  git init -b main
fi

add_sub() { # url path
  if [ -e "$2/.git" ]; then return; fi
  # Clean up what a previously failed 'submodule add' may have left behind.
  rm -rf ".git/modules/$2" "$2"
  git config -f .gitmodules --remove-section "submodule.$2" 2>/dev/null || true
  git config --remove-section "submodule.$2" 2>/dev/null || true
  git submodule add --force "$1" "$2"
}

# A clone already lists the submodules: just check them out.
[ -f .gitmodules ] && git submodule update --init

# repo.or.cz (TinyCC's main host) refuses UK connections (HTTP 451), so use the GitHub mirror.
add_sub https://github.com/libsdl-org/SDL.git libs/SDL
add_sub https://github.com/TinyCC/tinycc.git   libs/tinycc

# Pin SDL to the newest 3.x release tag.
git -C libs/SDL fetch --tags --quiet
SDL_TAG=$(git -C libs/SDL tag -l 'release-3.*' --sort=-v:refname | head -n 1)
git -C libs/SDL checkout --quiet "$SDL_TAG"
TCC_REV=$(git -C libs/tinycc rev-parse --short HEAD)
echo "SDL pinned to $SDL_TAG"
echo "TinyCC pinned to $TCC_REV"

git add -A
if git diff --cached --quiet; then
  echo "Nothing new to commit."
else
  git commit -m "Add submodules: SDL $SDL_TAG, TinyCC $TCC_REV" ||
    echo "Commit failed: set git user.name / user.email, then commit manually."
fi
