#!/usr/bin/env bash
# Final Phase 2 campaign on the final images (RA4 APP=phase2, ESP fork), all cases, reboot cases last.
# Stops if the RA4 can no longer be read over USB (bridge CDC wedge) instead of recording empty runs.
set -uo pipefail
cd "$(dirname "$0")/.."
run() { n=$1; shift; python3 host/phase2_run.py "$n" "$@"
  python3 -c "import json,sys; d=json.load(open('results/phase2/$n.json')); sys.exit(0 if d['ra4_boot_after'] else 1)" \
    || { echo "STOP: RA4 not readable after $n"; exit 3; }; }
S=30
run FF1  --secs $S --esp sub --flood-mbps 0 --flood-secs 20
run FF2  --secs $S --esp "fault cpu 15000 10"
run FF3  --secs $S --esp "fault mem 15000 8192"
run FF4  --secs $S --esp "fault malformed 15000 500"
run FF5  --secs $S --esp "fault flood 15000"
run FF8  --secs $S --esp "fault apstorm 5 1000"
run FF9  --secs $S --esp "fault noread 15000"
run FF10 --secs $S --esp "fault notx 15000"
run FF12 --secs 40 --esp "fault txlow 15000"
run FF11 --secs $S --esp "config 2 60" --esp "fault replay"
python3 host/esp_ctrl.py config 2 50
run FF6  --secs 40 --esp "fault rebootloop 5 3000"
run FF7  --secs $S --esp "fault crash"
run FF7b --secs $S --esp "fault hang"
echo CAMPAIGN_DONE
