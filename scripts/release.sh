#!/bin/sh
# Cuts a release: bumps the version in CMakeLists.txt, dates the Unreleased
# section of CHANGELOG.md, commits, tags vX.Y.Z and pushes. GitHub Actions
# (.github/workflows/release.yml) then builds the packages and publishes
# the release from the tag.
#
#   scripts/release.sh 0.2.0
set -e
v=${1:?usage: scripts/release.sh X.Y.Z}
case "$v" in *[!0-9.]*|"") echo "version must look like X.Y.Z" >&2; exit 1;; esac
here=$(cd "$(dirname "$0")/.." && pwd)
cd "$here"
[ -z "$(git status --porcelain)" ] || { echo "commit or stash your changes first" >&2; exit 1; }
[ "$(git rev-parse --abbrev-ref HEAD)" = main ] || { echo "release from main" >&2; exit 1; }
git tag | grep -qx "v$v" && { echo "tag v$v exists" >&2; exit 1; }
grep -q '^## Unreleased' CHANGELOG.md || { echo "CHANGELOG.md needs an '## Unreleased' section" >&2; exit 1; }
sed -i "s/^project(Firn VERSION [0-9.]* /project(Firn VERSION $v /" CMakeLists.txt
sed -i "s/^## Unreleased/## Unreleased\n\n## $v ($(date -u +%Y-%m-%d))/" CHANGELOG.md
git add CMakeLists.txt CHANGELOG.md
git commit -q -m "Release v$v"
git tag -a "v$v" -m "Firn v$v"
git push origin main "v$v"
echo "tagged v$v; the release workflow is building the packages"
