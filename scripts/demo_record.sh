#!/usr/bin/env bash
# Screen-recording wrapper for scripts/demo.sh --esp: clears the terminal, runs the live demo and keeps a
# plain-text transcript in build/demo_transcript.txt. Output is the demo's own output, unedited.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
mkdir -p build
clear
scripts/demo.sh --esp 2>&1 | tee build/demo_transcript.txt
