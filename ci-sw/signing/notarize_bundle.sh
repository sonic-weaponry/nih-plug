#!/usr/bin/env bash
# Sends the signed Crisp bundle (zipped with ditto) to Apple's notary service, waits for Apple's answer, saves Apple's
# log, and staples the ticket to the bundle itself, so a Mac can check it offline (ci-sw/README.md, "Signing").
# Adapted from our Wolf Shaper fix (packaging/macos/notarize.sh), which notarized an installer.
#
# Usage: ci-sw/signing/notarize_bundle.sh <signed Crisp.vst3> <folder for Apple's log>
# Needs APPLE_API_KEY_PATH (the App Store Connect API key, a .p8 file), APPLE_API_KEY_ID and APPLE_API_ISSUER_ID.
# NOTARY_TIMEOUT (default 60m) is how long to wait for Apple.
# Exit 0: accepted and stapled. Exit 1: Apple rejected it, hasn't finished in time, or stapling failed.
set -euo pipefail

bundle=$1
logs=$2
: "${APPLE_API_KEY_PATH:?}" "${APPLE_API_KEY_ID:?}" "${APPLE_API_ISSUER_ID:?}"
timeout=${NOTARY_TIMEOUT:-60m}
auth=(--key "$APPLE_API_KEY_PATH" --key-id "$APPLE_API_KEY_ID" --issuer "$APPLE_API_ISSUER_ID")
mkdir -p "$logs"
zip=$logs/submitted.zip

field() {
    python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get(sys.argv[2], ""))' "$1" "$2" 2>/dev/null || true
}

ditto -c -k --keepParent --norsrc --noextattr "$bundle" "$zip"
shasum -a 256 "$zip" | tee "$logs/submitted.sha256"
echo "--- the team's latest notarization submissions"
xcrun notarytool history "${auth[@]}" 2>&1 | head -n 25 || true

echo "--- submitting"
xcrun notarytool submit "$zip" "${auth[@]}" --output-format json >"$logs/submission.json"
cat "$logs/submission.json"
echo
id=$(field "$logs/submission.json" id)
if [ -z "$id" ]; then
    echo "::error::Apple's notary service didn't return a submission ID"
    exit 1
fi
echo "submission $id: waiting up to $timeout for Apple"
xcrun notarytool wait "$id" "${auth[@]}" --timeout "$timeout" || true
xcrun notarytool info "$id" "${auth[@]}" --output-format json >"$logs/info.json" || true
cat "$logs/info.json"
echo
status=$(field "$logs/info.json" status)
xcrun notarytool log "$id" "${auth[@]}" "$logs/notary-log.json" || echo "(no log from Apple yet)"
if [ -f "$logs/notary-log.json" ]; then
    echo "--- Apple's log"
    cat "$logs/notary-log.json"
    echo
fi
rm -f "$zip"

case $status in
    Accepted) echo "Apple accepted submission $id" ;;
    "In Progress" | "")
        echo "::error::Apple hasn't finished checking submission $id after $timeout, so nothing was stapled. Start a new signing run once Apple has accepted it."
        exit 1
        ;;
    *)
        echo "::error::Apple's notary service answered '$status' for submission $id; its log above says why."
        exit 1
        ;;
esac

for attempt in 1 2 3 4 5; do
    if xcrun stapler staple -v "$bundle"; then
        break
    fi
    [ $attempt -lt 5 ] || { echo "::error::stapling the ticket failed 5 times"; exit 1; }
    echo "stapling failed (attempt $attempt), trying again in 20 s"
    sleep 20
done
xcrun stapler validate -v "$bundle"
echo "--- stapled"
