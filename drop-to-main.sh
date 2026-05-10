#!/bin/sh
# drop-to-main.sh : squash full-history tree onto main as a single version commit
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)
#
# Usage: ./drop-to-main.sh [VERSION]
#   VERSION defaults to git describe on full-history (e.g. v0.2.0-17-g2d5b4b4)

set -e

VERSION="${1:-$(git describe --tags --always full-history 2>/dev/null || echo unknown)}"
DATE=$(date +%Y.%m.%d)
BACKUP="old/main-$DATE"

if [ "$(git rev-parse --abbrev-ref HEAD)" != "full-history" ]; then
    echo "error: must be on full-history branch" >&2
    exit 1
fi

if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "error: working tree or index is dirty, commit or stash first" >&2
    exit 1
fi

if git show-ref --verify --quiet "refs/heads/$BACKUP"; then
    echo "error: backup branch $BACKUP already exists" >&2
    exit 1
fi

MAIN=$(git rev-parse main)
TREE=$(git rev-parse full-history^{tree})

echo "backing up main ($MAIN) to $BACKUP"
git branch "$BACKUP" main

NEW=$(git commit-tree "$TREE" -p "$MAIN" -m "$VERSION")
echo "created commit $NEW ($VERSION)"

git update-ref refs/heads/main "$NEW" "$MAIN"
echo "main updated: $(git log --oneline -1 main)"
echo "backup: $BACKUP -> $(git log --oneline -1 "$BACKUP")"
