#!/usr/bin/env bash
# Reruns after the budget / sequence / resync fixes (see docs/PHASE2_RESULTS.md, "campaign 2").
set -uo pipefail
cd "$(dirname "$0")/.."
R="python3 host/phase2_run.py"
S=30
$R F4v2  --secs $S --esp "fault malformed 15000 500"
$R F5v2  --secs $S --esp "fault flood 15000"
$R F9v2  --secs $S --esp "fault noread 15000"
$R F10v2 --secs $S --esp "fault notx 15000"
$R F11v2 --secs $S --esp "config 2 60" --esp "fault replay"
python3 host/esp_ctrl.py config 2 50
$R F12v2 --secs 40 --esp "fault txlow 15000"
echo CAMPAIGN_DONE
