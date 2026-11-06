#!/bin/bash
# Checks tools/vegcov_compare.py: its built-in self test, then (when given) a Reno-side round trip on real projects:
#   vegcov_test.sh [<vegmodel or reno_cli> <folder with .veg/.reno files>]
# The round trip dumps each project, saves it again with Reno's writer, dumps the copy and compares; what differs is
# listed (the exit status is 0 as long as the tool itself ran: differences there are findings, not test failures).
set -e
here=$(cd "$(dirname "$0")" && pwd)
python3 "$here/../../tools/vegcov_compare.py" selftest
if [ -n "$1" ] && [ -n "$2" ]; then
  out=$(mktemp -d)
  python3 "$here/../../tools/vegcov_compare.py" roundtrip "$2" --out "$out" --reno-cli "$1" | tail -25 || true
  rm -rf "$out"
fi
