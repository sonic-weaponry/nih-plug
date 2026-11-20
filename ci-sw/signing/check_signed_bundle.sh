#!/usr/bin/env bash
# Checks the signature of the signed Crisp bundle (ci-sw/README.md, "Signing"). Adapted from our Wolf Shaper fix
# (ci/check_signed_pkg.sh), for a bundle shipped in a zip instead of an installer.
#
#   ci-sw/signing/check_signed_bundle.sh rehearsal <Crisp.vst3>
#     After sign_bundle.sh with the throwaway identity: the seal holds in both slices, with the hardened runtime, no
#     entitlements, the bundle's own identifier, LICENSE.txt and the credits sealed, signed by the throwaway identity.
#   ci-sw/signing/check_signed_bundle.sh developer-id <Crisp.vst3> [<folder with Apple's notarization log>]
#     The same with Apple's chain: "Developer ID Application: $EXPECTED_SIGNER ($APPLE_TEAM_ID)", a secure timestamp,
#     Apple's Developer ID requirement for our team. With the log folder (notarize_bundle.sh's): Apple accepted exactly
#     the submitted zip with no issues and its ticket covers both slices of Crisp; the ticket is stapled to the bundle
#     and validates.
#
# Run it on macOS from the repository root. Logs go to ci-results/signing-<kind>/. Exits 1 if any check failed.
set -u

kind=$1
bundle=$2
logs=${3:-}
here=$(cd "$(dirname "$0")" && pwd)
case $kind in
    rehearsal) signer_prefix="Crisp CI rehearsal" ;;
    developer-id)
        : "${APPLE_TEAM_ID:?the Team ID is missing (the APPLE_TEAM_ID secret)}"
        signer_prefix="Developer ID"
        ;;
    *) echo "usage: check_signed_bundle.sh rehearsal|developer-id <Crisp.vst3> [log folder]"; exit 2 ;;
esac

out=ci-results/signing-$kind
mkdir -p "$out"
failed=0
table="| Check | Result |
|---|---|"

result() {
    if [ "$1" = PASS ]; then
        echo "PASS $2"
    else
        echo "::error::FAIL $2"
        failed=1
    fi
    table="$table
| $2 | $1 |"
}
check() {
    local name=$1
    shift
    if "$@"; then result PASS "$name"; else result FAIL "$name"; fi
}
has_line() { grep -qxF -- "$1" <<<"$2"; }

check "the seal holds (codesign --verify --deep --strict)" codesign --verify --deep --strict --verbose=2 "$bundle"
identifier=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$bundle/Contents/Info.plist")
check "identifier is com.nih-plug.crisp (the Info.plist's)" test "$identifier" = com.nih-plug.crisp
for arch in arm64 x86_64; do
    info=$(codesign -dvvv --architecture "$arch" "$bundle" 2>&1)
    echo "$info" >"$out/codesign-$arch.txt"
    echo "::group::codesign -dvvv --architecture $arch"
    echo "$info"
    echo "::endgroup::"
    check "$arch: the signature verifies" codesign --verify --strict --architecture "$arch" "$bundle"
    check "$arch: identifier $identifier" has_line "Identifier=$identifier" "$info"
    check "$arch: hardened runtime" grep -Eq '^CodeDirectory .*flags=0x[0-9a-f]+\([^)]*runtime[^)]*\)' <<<"$info"
    check "$arch: Info.plist and resources sealed" grep -Eq '^Sealed Resources version=2 rules=[0-9]+ files=[1-9]' <<<"$info"
    check "$arch: signed by a '$signer_prefix' identity" grep -q "^Authority=$signer_prefix" <<<"$info"
    if [ "$kind" = developer-id ]; then
        check "$arch: chain Developer ID Application: ${EXPECTED_SIGNER:-} (team), Developer ID CA, Apple Root CA" \
            test "$(grep '^Authority=' <<<"$info")" = "Authority=Developer ID Application: ${EXPECTED_SIGNER:-?} ($APPLE_TEAM_ID)
Authority=Developer ID Certification Authority
Authority=Apple Root CA"
        check "$arch: our team" has_line "TeamIdentifier=$APPLE_TEAM_ID" "$info"
        check "$arch: secure timestamp" grep -q '^Timestamp=' <<<"$info"
    else
        echo "REPORT $arch: $(grep -E '^(Timestamp|Signed Time)=' <<<"$info" | head -n 1)"
    fi
done
check "LICENSE.txt is Crisp's GPL, unchanged" cmp -s "$bundle/Contents/Resources/LICENSE.txt" plugins/crisp/COPYING
check "the credits name this build's source commit" grep -qF "commit ${GITHUB_SHA:-unknown}" \
    "$bundle/Contents/Resources/CREDITS and SOURCE.txt"
ls -R "$bundle/Contents" >"$out/bundle-files.txt"
entitlements=$(codesign -d --entitlements - --xml "$bundle" 2>/dev/null)
check "no entitlements" test -z "$entitlements"
if [ "$kind" = developer-id ]; then
    requirement="anchor apple generic and certificate 1[field.1.2.840.113635.100.6.2.6] exists and certificate leaf[field.1.2.840.113635.100.6.1.13] exists and certificate leaf[subject.OU] = \"$APPLE_TEAM_ID\""
    check "meets Apple's Developer ID requirement for our team" \
        codesign --verify --deep --strict -R="$requirement" "$bundle"
fi

if [ "$kind" = developer-id ] && [ -n "$logs" ]; then
    notary=$(python3 "$here/check_notary_log.py" "$logs/notary-log.json" "$logs/submitted.sha256") ||
        notary="PROBLEM the log couldn't be read"
    echo "$notary"
    check "notarization: Apple accepted exactly this zip with no issues, its ticket covers both slices" \
        test -z "$(grep '^PROBLEM' <<<"$notary")"
    check "staple: the ticket is stapled to the bundle and validates" xcrun stapler validate -v "$bundle"
    gatekeeper=$(spctl --assess --type open --context context:primary-signature -vvv "$bundle" 2>&1)
    echo "REPORT Gatekeeper (spctl; report only, it doesn't formally assess plugin bundles): $(tr '\n' ' ' <<<"$gatekeeper")"
fi

if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    {
        echo "### Signature ($kind)"
        echo
        echo "$table"
        echo
    } >>"$GITHUB_STEP_SUMMARY"
fi
exit $failed
