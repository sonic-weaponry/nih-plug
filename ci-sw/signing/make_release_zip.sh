#!/usr/bin/env bash
# Packs the signed Crisp bundle into the zip that goes into the packs (ci-sw/README.md, "Signing"): one folder
# "Crisp (Mac)" with Crisp.vst3, LICENSE.txt, "CREDITS and SOURCE.txt" and HOW TO INSTALL.txt. Zipped with ditto
# (Apple's tool for signed bundles), without extended attributes or "._" files. Checks that the zip unpacks to exactly
# that folder and that the unpacked bundle's seal (and stapled ticket, when there is one) still validates.
#
# Usage: ci-sw/signing/make_release_zip.sh <signed Crisp.vst3> <output .zip>
set -euo pipefail

bundle=$1
output=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
check=$(mktemp -d)
trap 'rm -rf "$work" "$check"' EXIT
folder="$work/Crisp (Mac)"
mkdir -p "$folder"
ditto --norsrc --noextattr "$bundle" "$folder/Crisp.vst3"
cp "$bundle/Contents/Resources/LICENSE.txt" "$folder/LICENSE.txt"
cp "$bundle/Contents/Resources/CREDITS and SOURCE.txt" "$folder/CREDITS and SOURCE.txt"
cp "$here/HOW-TO-INSTALL.txt" "$folder/HOW TO INSTALL.txt"
rm -f "$output"
(cd "$work" && ditto -c -k --norsrc --noextattr --keepParent "Crisp (Mac)" "$output")

ditto -x -k "$output" "$check"
diff -r "$folder" "$check/Crisp (Mac)"
codesign --verify --deep --strict --verbose=2 "$check/Crisp (Mac)/Crisp.vst3"
if xcrun stapler validate "$check/Crisp (Mac)/Crisp.vst3" >/dev/null 2>&1; then
    echo "unzipped bundle: seal valid, ticket stapled"
else
    echo "unzipped bundle: seal valid, no stapled ticket"
fi
shasum -a 256 "$output"
