#!/usr/bin/env bash
# No-op override that disables the SummonAI Kit harness for THIS project only.
#
# The global hook (~/.claude/settings.json) resolves its script as:
#     h="$(git rev-parse --show-toplevel)/.claude/hooks/summonaikit-harness.sh"
#     [ -f "$h" ] || h="$HOME/.claude/hooks/summonaikit-harness.sh"
# so a project-level file wins. This is that file. The kit stays fully active
# in every other project.
#
# Why disabled here (agreed with stuart, 2026-08-15):
#   - The harness hardcodes "the user is non-technical, cannot read code" and
#     bans file names / code / jargon. False for stuart and actively harmful on
#     an ESP32-S3 firmware project where the work IS pin numbers, partition
#     offsets and register-level detail.
#   - It forbids asking the user technical decisions. This project needs the
#     opposite: stuart is consulted on irreversible calls (e.g. repartitioning
#     flash over the only factory backup).
#   - It mandates 3 subagents per turn regardless of task. Fine for real code
#     changes, wrong for hardware diagnosis and capability exploration.
#   - Its skills (frontend-patterns, payments-webhooks, database, design) target
#     web apps; this is C/C++ on a 512 KB microcontroller.
#
# To re-enable the harness here, delete this file.

cat >/dev/null 2>&1   # drain the hook payload on stdin so the caller sees no SIGPIPE
exit 0
