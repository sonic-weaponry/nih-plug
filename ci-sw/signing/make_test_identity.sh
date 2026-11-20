#!/usr/bin/env bash
# Makes a throwaway, self-signed code-signing identity for the signing rehearsal on a CI runner: "Crisp CI rehearsal
# Application (not trusted)". Valid for 2 days, trusted by no Mac, never uploaded. With it every push runs the same
# keychain, signing and checking code that the release job runs with the real Developer ID certificate
# (ci-sw/README.md, "Signing"). Adapted from our Wolf Shaper fix (ci/make_test_identity.sh).
#
# Usage: ci-sw/signing/make_test_identity.sh <folder>
# Writes application.p12 into the folder, protected by the password in $REHEARSAL_P12_PASSWORD, in the same format as
# the real one (3DES, SHA-1: what macOS's keychain imports). Run it on macOS.
set -euo pipefail

dir=$1
: "${REHEARSAL_P12_PASSWORD:?set REHEARSAL_P12_PASSWORD}"
# macOS's own LibreSSL: a Homebrew OpenSSL 3 earlier in PATH would write a .p12 the keychain can't read.
openssl=/usr/bin/openssl
mkdir -p "$dir"
cat >"$dir/application.cnf" <<CNF
[req]
distinguished_name = dn
x509_extensions = ext
prompt = no
[dn]
CN = Crisp CI rehearsal Application (not trusted)
O = Crisp CI rehearsal
[ext]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,codeSigning
subjectKeyIdentifier = hash
CNF
"$openssl" version
"$openssl" req -x509 -newkey rsa:2048 -nodes -days 2 -config "$dir/application.cnf" \
    -keyout "$dir/application.key" -out "$dir/application.crt" 2>/dev/null
"$openssl" pkcs12 -export -inkey "$dir/application.key" -in "$dir/application.crt" \
    -name "Crisp CI rehearsal Application (not trusted)" \
    -keypbe PBE-SHA1-3DES -certpbe PBE-SHA1-3DES -passout env:REHEARSAL_P12_PASSWORD -out "$dir/application.p12"
rm -f "$dir/application.key" "$dir/application.cnf"
"$openssl" x509 -in "$dir/application.crt" -noout -subject -enddate
