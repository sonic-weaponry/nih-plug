#!/usr/bin/env python3
# Reads Apple's notarization log for the Crisp zip (ci-sw/signing/check_signed_bundle.sh) and prints a PROBLEM line for
# anything wrong: not accepted, issues, a different file than the one submitted, or a ticket that doesn't cover both
# slices of Crisp. Usage: check_notary_log.py <notary-log.json> <submitted.sha256>
# SPDX-License-Identifier: GPL-3.0-or-later. Copyright (C) 2026 Sonic Weaponry.
import json
import sys

log = json.load(open(sys.argv[1]))
submitted = open(sys.argv[2]).read().split()[0]
problems = []
if log.get("status") != "Accepted":
    problems.append("status %r" % log.get("status"))
if log.get("issues"):
    problems.append("issues: %s" % json.dumps(log["issues"]))
if log.get("sha256") != submitted:
    problems.append("Apple checked a different file (sha256 %s, submitted %s)" % (log.get("sha256"), submitted))
archs = sorted({t.get("arch") for t in log.get("ticketContents") or []
                if "Crisp.vst3/Contents/MacOS/Crisp" in t.get("path", "")})
print("ticket covers Crisp's slices: %s" % archs)
if archs != ["arm64", "x86_64"]:
    problems.append("the ticket doesn't cover both slices of Crisp")
for p in problems:
    print("PROBLEM " + p)
