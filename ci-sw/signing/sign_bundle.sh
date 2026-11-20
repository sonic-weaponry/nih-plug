#!/usr/bin/env bash
# Signs a copy of the Crisp bundle (ci-sw/README.md, "Signing"): puts LICENSE.txt (Crisp's GPL) and
# "CREDITS and SOURCE.txt" into Contents/Resources first, so the seal covers them, then signs with the hardened runtime
# and Apple's secure timestamp, verifies, and proves with macho_same_code.py that the signed program carries exactly
# the unsigned build's code and data in both slices.
#
# Usage: ci-sw/signing/sign_bundle.sh <unsigned Crisp.vst3> <output folder> <identity (SHA-1 or name)>
# Needs: GITHUB_SERVER_URL, GITHUB_REPOSITORY, GITHUB_SHA (for the source link), RUSTUP_TOOLCHAIN. Run it on macOS from
# the repository root. Writes <output folder>/Crisp.vst3.
set -euo pipefail

unsigned=$1
out=$2
identity=$3
here=$(cd "$(dirname "$0")" && pwd)
bundle=$out/Crisp.vst3
program=Contents/MacOS/Crisp

mkdir -p "$out"
rm -rf "$bundle"
ditto --norsrc --noextattr "$unsigned" "$bundle"
mkdir -p "$bundle/Contents/Resources"
cp plugins/crisp/COPYING "$bundle/Contents/Resources/LICENSE.txt"
sed -e "s#@SOURCE_URL@#${GITHUB_SERVER_URL}/${GITHUB_REPOSITORY}/tree/${GITHUB_SHA}#" \
    -e "s#@COMMIT@#${GITHUB_SHA}#" -e "s#@RUST_TOOLCHAIN@#${RUSTUP_TOOLCHAIN}#" \
    "$here/CREDITS.txt.in" >"$bundle/Contents/Resources/CREDITS and SOURCE.txt"
if grep -q '@[A-Z_]*@' "$bundle/Contents/Resources/CREDITS and SOURCE.txt"; then
    echo "::error::a placeholder in CREDITS.txt.in wasn't filled in"
    exit 1
fi

# Apple's timestamp server can hiccup; try three times.
for attempt in 1 2 3; do
    if codesign --force --options runtime --timestamp --sign "$identity" "$bundle"; then
        break
    fi
    [ $attempt -lt 3 ] || { echo "::error::codesign failed 3 times"; exit 1; }
    echo "codesign failed (attempt $attempt), trying again in 10 s"
    sleep 10
done
codesign --verify --deep --strict --verbose=2 "$bundle"
codesign -dvv "$bundle" 2>&1 | grep -E '^(Identifier|Format|Authority|TeamIdentifier|Timestamp|Runtime Version|Sealed Resources|Info.plist)=' || true
python3 "$here/macho_same_code.py" "$unsigned/$program" "$bundle/$program"
shasum -a 256 "$unsigned/$program" "$bundle/$program"
