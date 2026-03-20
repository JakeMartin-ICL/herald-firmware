#!/usr/bin/env bash
set -e

BUMP=patch
RETAG=false

for arg in "$@"; do
  case $arg in
    --patch) BUMP=patch ;;
    --minor) BUMP=minor ;;
    --major) BUMP=major ;;
    --retag) RETAG=true ;;
    *) echo "Unknown argument: $arg"; exit 1 ;;
  esac
done

# Read current version from platformio.ini
CURRENT=$(grep -o "DFIRMWARE_VERSION='\"[^\"]*\"'" platformio.ini | grep -o '[0-9]*\.[0-9]*\.[0-9]*')

if [ -z "$CURRENT" ]; then
  echo "Error: could not find DFIRMWARE_VERSION in platformio.ini"
  exit 1
fi

if [ "$RETAG" = true ]; then
  TAG="v$CURRENT"
  echo "Current version: $CURRENT"
  echo ""
  read -p "About to force-move $TAG to HEAD and re-trigger the pipeline. Continue? [y/N] " confirm
  case $confirm in
    [yY]*) ;;
    *) echo "Aborted."; exit 1 ;;
  esac
  git tag -f "$TAG"
  git push --force origin "$TAG"
  echo ""
  echo "Re-tagged $TAG — pipeline running at:"
  echo "https://github.com/$(git remote get-url origin | sed 's/.*github.com[:/]\(.*\)\.git/\1/')/actions"
  exit 0
fi

# Parse and bump
MAJOR=$(echo "$CURRENT" | cut -d. -f1)
MINOR=$(echo "$CURRENT" | cut -d. -f2)
PATCH=$(echo "$CURRENT" | cut -d. -f3)

case $BUMP in
  major) MAJOR=$((MAJOR + 1)); MINOR=0; PATCH=0 ;;
  minor) MINOR=$((MINOR + 1)); PATCH=0 ;;
  patch) PATCH=$((PATCH + 1)) ;;
esac

NEW="$MAJOR.$MINOR.$PATCH"
TAG="v$NEW"

echo "Current version: $CURRENT"
echo "New version:     $NEW"
echo ""
read -p "About to create release $TAG. Continue? [y/N] " confirm
case $confirm in
  [yY]*) ;;
  *) echo "Aborted."; exit 1 ;;
esac

# Update platformio.ini
sed -i "s/-DFIRMWARE_VERSION='\"[^\"]*\"'/-DFIRMWARE_VERSION='\"$NEW\"'/" platformio.ini

git add platformio.ini
git commit -m "Release $TAG"
git push
git tag "$TAG"
git push origin "$TAG"

echo ""
echo "Released $TAG — pipeline running at:"
echo "https://github.com/$(git remote get-url origin | sed 's/.*github.com[:/]\(.*\)\.git/\1/')/actions"
