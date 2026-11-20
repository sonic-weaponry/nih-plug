#!/usr/bin/env bash
# Nulls every render of one label against the same render of a reference label (ci-sw/README.md).
# Usage: null_test.sh <vst3_check> <results folder> <label> <reference label> <limit dBFS>
set -u
tool=$1; out=$2; label=$3; ref=$4; limit=$5
failed=0; count=0
for wav in "$out/$ref/renders"/*.wav; do
    name=$(basename "$wav")
    if [ ! -f "$out/$label/renders/$name" ]; then echo "[$label vs $ref] MISSING $name"; failed=1; continue; fi
    line=$("$tool" nulltest "$out/$label/renders/$name" "$wav" "$limit" | tail -n 1)
    echo "[$label vs $ref] $line"
    case "$line" in "RESULT nulltest PASS"*) ;; *) failed=1 ;; esac
    count=$((count + 1))
done
[ $count -gt 0 ] || { echo "[$label vs $ref] no renders"; failed=1; }
exit $failed
