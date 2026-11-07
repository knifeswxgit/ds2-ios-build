#!/usr/bin/env bash
# One command to launch the whole DS2 -> .ipa pipeline.
#
# The ONLY manual step left is a one-time GitHub login:
#     gh auth login --web --git-protocol https
# Then run:  bash ios_port/ci/launch.sh
#
# Everything else (private repo, private release with 21 MB XEX + 501 MB
# shader pack, secrets, workflow run, .ipa download) is done here.
set -euo pipefail

PROJECT_ROOT="${DS2_PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
# When launched from a copy inside ds2-ios-build/, the game files still live
# in the original project directory; DS2_PROJECT_ROOT overrides it.
REPO="$PROJECT_ROOT/ds2-ios-build"
RELEASE_TAG="ds2-private-assets"
XEX="$PROJECT_ROOT/darksouls_nintendo/DARK SOULS II Scholar of the First Sin (homebrew port) for Nintendo Switch [NSP]/switch/ds2/game_root/Default.xex"
SPVZ="$PROJECT_ROOT/ios_port/out/ds2_shaders.spvz"
IDX="$PROJECT_ROOT/ios_port/out/ds2_shaders.spvz.idx"
REPO_NAME="${DS2_REPO_NAME:-ds2-ios-build}"

echo "=== DS2 iOS pipeline launcher ==="

command -v gh >/dev/null || { echo "install GitHub CLI: https://cli.github.com"; exit 1; }
if ! gh auth status >/dev/null 2>&1; then
  echo "GitHub CLI is not logged in. Run this once, then re-run the script:"
  echo "    gh auth login --web --git-protocol https"
  exit 1
fi

for f in "$XEX" "$SPVZ" "$IDX" "$REPO/.git/config" "$REPO/.github/workflows/ds2-full.yml"; do
  [ -e "$f" ] || { echo "missing: $f"; exit 1; }
done

cd "$REPO"
echo "[1/6] Creating PRIVATE repository $REPO_NAME (or reusing it)..."
if ! gh repo view "$REPO_NAME" >/dev/null 2>&1; then
  gh repo create "$REPO_NAME" --private --source . --remote origin --push
else
  git remote remove origin 2>/dev/null || true
  git remote add origin "https://github.com/$(gh api user --jq .login)/$REPO_NAME.git"
  git push -u origin HEAD
fi
REPO_SLUG="$(gh api user --jq .login)/$REPO_NAME"
echo "      $REPO_SLUG (private)"

echo "[2/6] Uploading ONLY Default.xex (21 MB); shaders go to the phone via Files..."
mkdir -p "$PROJECT_ROOT/ios_port/out/_upload"
cp "$XEX" "$PROJECT_ROOT/ios_port/out/_upload/Default.xex"
STAGED="$PROJECT_ROOT/ios_port/out/_upload/Default.xex"
if gh release view "$RELEASE_TAG" --repo "$REPO_SLUG" >/dev/null 2>&1; then
  gh release upload "$RELEASE_TAG" "$STAGED" --repo "$REPO_SLUG" --clobber
else
  gh release create "$RELEASE_TAG" \
    --repo "$REPO_SLUG" --title "private build inputs" \
    --notes "Personal use only. No game data archives." \
    "$STAGED"
fi
# A draft release is invisible to `gh release download` inside Actions.
gh release edit "$RELEASE_TAG" --repo "$REPO_SLUG" --draft=false >/dev/null 2>&1

echo "[3/6] Signing secrets (optional)..."
if [ -n "${DS2_P12:-}" ] && [ -n "${DS2_MOBILEPROVISION:-}" ]; then
  gh secret set P12_BASE64             --repo "$REPO_SLUG" --body "$(base64 -w0 "$DS2_P12")"
  gh secret set P12_PASSWORD           --repo "$REPO_SLUG" --body "${DS2_P12_PASSWORD:-}"
  gh secret set MOBILEPROVISION_BASE64 --repo "$REPO_SLUG" --body "$(base64 -w0 "$DS2_MOBILEPROVISION")"
  DO_SIGN=true
else
  echo "      DS2_P12 / DS2_MOBILEPROVISION not set -> unsigned .ipa (sign locally later)"
  DO_SIGN=false
fi

echo "[4/6] Starting workflow..."
gh workflow run ds2-full.yml --repo "$REPO_SLUG" \
  -f asset_release="$RELEASE_TAG" -f do_sign="$DO_SIGN"
sleep 5
RUN_ID="$(gh run list --workflow=ds2-full.yml --repo "$REPO_SLUG" --limit 1 --json databaseId --jq '.[0].databaseId')"
echo "      run id: $RUN_ID"
gh run watch "$RUN_ID" --repo "$REPO_SLUG" --exit-status || true

echo "[5/6] Downloading artifacts..."
mkdir -p "$PROJECT_ROOT/ios_port/out/ci"
gh run download "$RUN_ID" --repo "$REPO_SLUG" --dir "$PROJECT_ROOT/ios_port/out/ci" || true
find "$PROJECT_ROOT/ios_port/out/ci" -name '*.ipa' -exec ls -la {} \;

echo "[6/6] Done. Logs if anything failed:"
echo "      gh run view $RUN_ID --repo $REPO_SLUG --log-failed"
