#!/usr/bin/env bash
# Crisp CI checks on one bundle (ci-sw/README.md). Runs every check, keeps going after a failure, and exits 1 if any
# failed. Usage: run_checks.sh <vst3_check> <bundle> <label> <results folder> [<command prefix, e.g. "arch -x86_64">]
set -u
tool=$1; bundle=$2; label=$3; out=$4; prefix=${5:-}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out/$label/renders"
failed=0
run() { # <log name> <args...>
    local name=$1; shift
    $prefix "$tool" "$@" > "$out/$label/$name.log" 2>&1
    local status=$?
    echo "[$label] $(tail -n 1 "$out/$label/$name.log")"
    [ $status -eq 0 ] || failed=1
}
run ids ids "$bundle" "$here/frozen/class-ids.txt"
run info info "$bundle" "$here/frozen/plugin-info.json" "$out/$label/plugin-info.json"
run params params "$bundle" "$here/frozen/parameters.json" "$out/$label/parameters.json"
run savestate savestate "$bundle" "$out/$label/default.json"
states=("$here"/states/*.json)
run states states "$bundle" "${states[@]}"
for rate in 44100 48000; do
    run "render-default-$rate" render "$bundle" - "$here/signal/signal-$rate.wav" 512 "$out/$label/renders/default-$rate.wav"
    for state in "$here"/states/*.json; do
        name=$(basename "$state" .json)
        [ "$name" = default ] && continue
        run "render-$name-$rate" render "$bundle" "$state" "$here/signal/signal-$rate.wav" 512 \
            "$out/$label/renders/$name-$rate.wav"
    done
done
exit $failed
