#!/bin/bash
#
# Rebase the hpplite branch onto a new upstream SQLite release tag.
#
# Usage:
#   ext/hpplite/tools/sync_upstream.sh version-3.55.0 [--push]
#
# What it does:
#   1. fetch upstream tags
#   2. tag the current head (hpplite-pre-<newtag>) so it can be restored
#   3. git rebase --onto <newtag> <currentbase> hpplite
#   4. rebuild the amalgamation, build ext/hpplite (mock L1), run ctest
#   5. refresh patches/ from the bottom commit and record the new base
#   6. optionally force-push (with lease)
#
# The branch layout is: <base tag> -> core patch -> ext/hpplite commits.
# Only the core patch can conflict; if it does, fix it, `git rebase --continue`,
# then rerun this script with the same tag to finish steps 4-6.

set -euo pipefail

NEW="${1:?usage: $0 version-X.Y.Z [--push]}"
PUSH="${2:-}"
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
EXT="$ROOT/ext/hpplite"
BASE_FILE="$EXT/patches/UPSTREAM_BASE"
BRANCH=hpplite

cd "$ROOT"
[ -z "$(git status --porcelain | grep -v '^??')" ] || { echo "working tree has uncommitted changes"; exit 1; }
[ "$(git rev-parse --abbrev-ref HEAD)" = "$BRANCH" ] || { echo "checkout $BRANCH first"; exit 1; }

git remote get-url upstream >/dev/null 2>&1 || git remote add upstream https://github.com/sqlite/sqlite.git
git fetch -q upstream --tags
git rev-parse -q --verify "refs/tags/$NEW" >/dev/null || { echo "no such tag: $NEW"; exit 1; }

OLD="$(cat "$BASE_FILE")"
if [ "$OLD" != "$NEW" ]; then
  echo "== rebasing $BRANCH: $OLD -> $NEW"
  git tag -f "hpplite-pre-$NEW" HEAD >/dev/null
  git rebase --onto "$NEW" "$OLD" "$BRANCH"
fi

echo "== rebuilding amalgamation"
mkdir -p build && (cd build && ../configure >/dev/null && make -s sqlite3.c >/dev/null)
echo "   $(grep -m1 'define SQLITE_VERSION ' build/sqlite3.h)"

echo "== building ext/hpplite (mock L1) and running unit tests"
cmake -S "$EXT" -B "$EXT/build_mock" >/dev/null
cmake --build "$EXT/build_mock" -j8 >/dev/null
(cd "$EXT/build_mock" && ctest --timeout 120 | tail -3)

echo "== refreshing patches/"
CORE="$(git rev-list --reverse "$NEW..$BRANCH" | head -1)"
git show --stat --format= "$CORE" -- src/ | grep -q . || { echo "bottom commit does not touch src/ — branch layout broken"; exit 1; }
rm -f "$EXT"/patches/0001-*.patch
git format-patch -1 "$CORE" -o "$EXT/patches/" --no-signature --zero-commit >/dev/null
echo "$NEW" > "$BASE_FILE"
sed -i.bak -E "s/version-[0-9]+\.[0-9]+\.[0-9]+/$NEW/g" "$EXT/patches/README.md" && rm -f "$EXT/patches/README.md.bak"
if [ -n "$(git status --porcelain -- "$EXT/patches")" ]; then
  git add "$EXT/patches"
  git commit -q -m "Rebase onto SQLite $NEW" -m "Base: $OLD -> $NEW. Core patch unchanged unless noted." -m "Claude-Session: https://claude.ai/code/session_01Q53TJXLGu6nHNKu2Lz7TUL"
  echo "   committed patches/ refresh"
fi

echo "== done: $BRANCH is on $NEW"
echo "   integration tests (real L1): $EXT/tests/integration/run_integration.sh"
if [ "$PUSH" = "--push" ]; then
  git push --force-with-lease origin "$BRANCH"
else
  echo "   push with: git push --force-with-lease origin $BRANCH"
fi
