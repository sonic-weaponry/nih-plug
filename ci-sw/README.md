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
