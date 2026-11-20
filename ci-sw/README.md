# Sonic Weaponry: Crisp CI checks

This branch builds Robbert van der Helm's Crisp (plugins/crisp, GPL-3.0-or-later) for macOS as a universal VST3
(Apple Silicon and Intel), signed and notarized, so Mac users can load projects and presets saved with Crisp on
Windows without turning Gatekeeper off. **Crisp's code is not changed.** Everything here is build, test and packaging
tooling; Robbert van der Helm is not involved with or responsible for this build.

`.github/workflows/sw-crisp.yml` builds Crisp with the same nih-plug commit (40269e1394) and Rust nightly
(2024-11-27) as the Crisp builds that were distributed at the time, then checks that our build shows a host exactly
what the Windows build shows. Upstream's own workflows are removed on this branch.

## Why
A host finds a VST3 plugin by its class ID, stores parameter IDs for automation, and restores the plugin's saved
state. If any of these differ between Windows and macOS, a project saved on one won't open right on the other. These
checks pin all of it to tables frozen from the Windows Crisp build, so the macOS build is provably the same plugin.

## The checks
`ci-sw/vst3_check` is a small VST3 host (adapted from the one we wrote for our Wolf Shaper fix). One check per
process; the last line is always `RESULT <check> PASS|FAIL <detail>`.

| Check | What it does | Frozen file |
|---|---|---|
| `ids` | The class list through the VST3 SDK's FUID code: exactly one class, its canonical ID and the raw bytes this platform hands the host | `frozen/class-ids.txt` |
| `info` | Factory and class info, single-component layout, buses, latency, tail, parameter count | `frozen/plugin-info.json` |
| `params` | Every parameter: ID, title, units, steps, flags, default, and the plain value and text at 0, 25, 50, 75 and 100 % | `frozen/parameters.json` |
| `states` | Each state in `states/` loads into a fresh instance and saves back byte for byte; a fresh instance saves exactly `states/default.json` | `states/*.json` |
| `render` | Loads a state, plays 1 s of silence then the test signal (`signal/`, pinned by `SHA256SUMS`) at 44.1 and 48 kHz, writes a 32-bit float WAV | |
| `nulltest` | Subtracts two renders and reports the largest difference; the compare job requires every macOS render (both slices) to null against the Windows render below `NULL_LIMIT_DBFS` | |

Crisp's noise generator starts from a fixed seed, so its output is deterministic and renders can be compared sample
by sample across platforms.

The frozen tables were measured on 2026-11-19 from the Windows Crisp build that Sonic Weaponry's Wook Bass Generators
ship (sha256 b8467582...): every check passes on it, and two runs of its renders are bit-identical.

`make_test_signal.py` makes the test signal (log sweep, a 55 Hz saw stepped from -30 to 0 dBFS, a noise burst,
impulses); `run_checks.sh` runs every check on one bundle; `null_test.sh` compares two sets of renders.

## Signing
Adapted from our Wolf Shaper fix. Crisp ships as a bundle in a zip (the packs tell users to copy the .vst3), so only
a Developer ID Application certificate is used; there is no installer.

- **Every push (rehearsal):** `signing/make_test_identity.sh` makes a throwaway self-signed identity on the runner,
  `signing/keychain.sh` puts it in a temporary keychain, `signing/sign_bundle.sh` signs a copy of the build (LICENSE.txt
  and "CREDITS and SOURCE.txt" go into Contents/Resources first, so the seal covers them; hardened runtime; Apple's
  timestamp), `signing/macho_same_code.py` proves the signed program has exactly the unsigned build's code and data
  in both slices, `signing/check_signed_bundle.sh rehearsal` checks the signature, the plugin checks run again on the
  signed copy and its renders must be bit-identical, and `signing/make_release_zip.sh` packs and re-checks the zip.
  The keychain is deleted at the end; nothing from the rehearsal is uploaded.
- **By hand (`release: sign`), job `macos-release`:** runs only on sw/ branches after every other job is green, in the
  `mac-release` environment (its secrets can only be used by sw/* branches and v* tags). It takes the exact build the
  macos job checked (its sha256 must match), signs it with "Developer ID Application: SONIC WEAPONRY LLC", checks the
  signature, sends it to Apple's notary service with the App Store Connect API key (`signing/notarize_bundle.sh`),
  staples Apple's ticket to the bundle, checks Apple's log (accepted, no issues, the exact zip, both slices), runs every
  plugin check on both slices of the signed bundle (renders bit-identical to the checked build), and uploads the zip.
  The keychain and the API key file are deleted at the end, also after a failure.
