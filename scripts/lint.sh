#!/usr/bin/env bash
# Static analysis gate: cppcheck (all checks except style noise listed below) over runtime + apps,
# and a clang-format conformance check. Exit nonzero on any finding.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
SRC=$(git ls-files '*.c' | grep -v -E '^(third_party|tests|baselines)/')
echo "== cppcheck $(cppcheck --version)"
cppcheck --std=c11 --enable=warning,performance,portability --inline-suppr --error-exitcode=1 \
  --suppress=missingIncludeSystem -D__GNUC__ -D__ARM_ARCH_7EM__ -DKESTREL_BUILD_ID='"lint"' \
  -I . -I board -I third_party/cmsis/Include -I third_party/renesas/Include \
  --suppress='*:third_party/*' -q $SRC
echo "cppcheck: clean"
echo "== clang-format --dry-run"
if clang-format --dry-run --Werror $(git ls-files '*.c' '*.h' | grep -v third_party) 2>/tmp/kestrel_fmt.txt; then
  echo "clang-format: clean"
else
  echo "clang-format: $(grep -cE 'warning|error' /tmp/kestrel_fmt.txt) deviations (run: make format)"; exit 1
fi
