#!/bin/bash
# One progress line per minute for a running build.sh / deploy-opt.sh (use as a Claude Code Monitor command).
# Usage: progress.sh [start-epoch]   (default: now). A logs/build.log older than start-epoch belongs to an earlier
# run and is ignored. Exits when no build.sh, or verify-on-host.sh process is left, or on a failure.
cd "$(dirname "$0")"
S=${1:-$(date +%s)}; L=logs/build.log; t0=""; n0=""
while true; do
  now=$(date +%s); el="$(( (now-S)/60 ))m$(( (now-S)%60 ))s"
  fresh=0; [ -f $L ] && [ "$(stat -c %Y $L)" -ge "$S" ] && fresh=1
  if [ $fresh = 1 ] && grep -qE "FAILED:|error:|ERROR|is not ELF|too new for the host" $L; then
    echo "$el: FAILED"; grep -m3 -E "FAILED:|error:|ERROR|is not ELF|too new for the host" $L; exit 1
  fi
  if ! pgrep -f "build\.sh|deploy-opt\.sh|verify-on-host\.sh|build-in-container\.sh" >/dev/null; then
    echo "$el: finished (no build/deploy process left)"; exit 0
  fi
  p=""; [ $fresh = 1 ] && p=$(grep -oE '^\[[0-9]+/[0-9]+\]' $L | tail -1)
  n=$(echo "$p" | grep -oE '[0-9]+' | head -1); N=$(echo "$p" | grep -oE '[0-9]+' | tail -1)
  if pgrep -f "deploy-opt\.sh" >/dev/null; then msg="installing to /opt and verifying"
  elif pgrep -f "verify-on-host\.sh" >/dev/null; then msg="verifying on the host"
  elif [ $fresh = 1 ] && grep -q "^bundled " $L; then msg="glibc check, copying to the host"
  elif [ $fresh = 1 ] && grep -q "Installing files" $L; then msg="installing and bundling libraries"
  elif [ $fresh = 0 ]; then msg="preparing (docker image, fetching sources)"
  elif [ -z "$n" ]; then msg="building dependencies (lzfse, libcbor, nettle, gnutls, spice), configure"
  else
    [ -z "$t0" ] && { t0=$now; n0=$n; }
    if [ $((now-t0)) -ge 50 ] && [ $((n-n0)) -gt 0 ]; then
      msg="compiling $p $((n*100/N))%, ETA ~$(( (N-n)*(now-t0)/(n-n0)/60 + 3 ))m (incl. ~3m deploy)"
    else msg="compiling $p $((n*100/N))%"; fi
  fi
  echo "$el: $msg"
  sleep 60
done
