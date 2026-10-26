#!/bin/bash
# Packs what the VEGAS machine needs for the comparison suite into one .tar.gz (a few MB: the media compress well),
# to be copied into the bridge folder there and unpacked:
#   suite/   the media, every <id>.reno (for VEGAS to build) and <id>.reno.veg (Reno's own .veg)
#   scripts/ Export to Reno.cs and Import Reno Project.cs (Reno Bridge looks for them there too)
#   jobs/    the build-and-render jobs (with --renoveg also the jobs that render Reno's own .veg files)
#   manifest.json
# The jobs are packed under jobs_pending/ so that nothing starts while the archive is unpacked: move them into
# jobs/ afterwards (a mv inside one folder is instant, so the bridge never sees half a job).
#
#   pack_upload.sh <bridge folder> <out.tar.gz> [--renoveg] [--only id,id...]
# On the VEGAS machine, in the bridge folder (e.g. with the remote shell that has it mounted):
#   tar xzf upload.tar.gz && mv jobs_pending/*.job.json jobs/ && rmdir jobs_pending
# Results come back the other way:
#   tar czf back.tar.gz results info renders/vegas suite/*.veg heartbeat.json bridge.log
set -eu
ROOT=$(readlink -f "$1"); OUT=$(readlink -f "$2"); shift 2
HERE=$(cd "$(dirname "$0")" && pwd); TOOLS=$(cd "$HERE/../../tools/vegas" && pwd)
RENOVEG=0; ONLY=""
while [ $# -gt 0 ]; do
  case "$1" in
    --renoveg) RENOVEG=1; shift;;
    --only) ONLY=",$2,"; shift 2;;
    *) echo "unknown option $1"; exit 2;;
  esac
done
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/suite" "$T/scripts" "$T/jobs_pending" "$T/jobs" "$T/results" "$T/renders/vegas"
cp "$TOOLS/Export to Reno.cs" "$TOOLS/Import Reno Project.cs" "$TOOLS/Reno Bridge.cs" "$TOOLS/BRIDGE.txt" "$T/scripts/"
cp "$ROOT/manifest.json" "$T/"
for f in "$ROOT"/suite/*; do
  case "$f" in *.veg) case "$f" in *.reno.veg) ;; *) continue;; esac;; esac      # (VEGAS's own .veg files are made there)
  cp "$f" "$T/suite/"
done
pick() { local id=${1#*_}; id=${id%.job.json}; [ -z "$ONLY" ] || [[ "$1" == 000* ]] || [[ "$ONLY" == *",$id,"* ]]; }
for j in "$ROOT"/jobs/*.job.json; do pick "$(basename "$j")" && cp "$j" "$T/jobs_pending/"; done
if [ $RENOVEG = 1 ]; then
  mkdir -p "$T/renders/renoveg"
  for j in "$ROOT"/jobs_renoveg/*.job.json; do [[ "$(basename "$j")" == 000* ]] && continue; pick "$(basename "$j")" && cp "$j" "$T/jobs_pending/r$(basename "$j")"; done
fi
tar czf "$OUT" -C "$T" .
echo "$OUT: $(du -h "$OUT" | cut -f1), $(ls "$T/jobs_pending" | wc -l) jobs, $(ls "$T/suite" | wc -l) suite files"
