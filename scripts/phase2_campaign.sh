#!/usr/bin/env bash
# Phase 2 fault campaign F1-F12 (docs/PHASE2_FAULT_MODEL.md). Real images: APP=phase2 + fork.
set -uo pipefail
cd "$(dirname "$0")/.."
R="python3 host/phase2_run.py"
S=30
$R F1  --secs $S --esp sub --flood-mbps 0 --flood-secs 20
$R F2  --secs $S --esp "fault cpu 15000 10"
$R F3  --secs $S --esp "fault mem 15000 8192"
$R F4  --secs $S --esp "fault malformed 15000 500"
$R F5  --secs $S --esp "fault flood 15000"
$R F6  --secs 40 --esp "fault rebootloop 5 3000"
$R F7  --secs $S --esp "fault crash"
$R F7b --secs $S --esp "fault hang"
$R F8  --secs $S --esp "fault apstorm 5 1000"
$R F9  --secs $S --esp "fault noread 15000"
$R F10 --secs $S --esp "fault notx 15000"
$R F11 --secs $S --esp "config 2 60" --esp "fault replay"
python3 host/esp_ctrl.py config 2 50
$R F12 --secs $S --esp "fault txlow 15000"
echo CAMPAIGN_DONE
