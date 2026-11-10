#!/bin/sh
# pipewatch.sh <seconds> [interval_ms]
# Phase 1 probe: polls luna://com.webos.media/getActivePipelines and logs, per
# poll, the TV uptime plus a compact classification of the media pipelines:
#   svp=<n>  pipelines holding an SVP_* (secure video path) resource
#   vdec=<n> pipelines holding a VDEC resource
#   fgmedia=<n> foreground pipelines of type "media"
#   types=<foreground pipeline types>
# Lines are only printed when the classification changes, plus a heartbeat
# every 5 s. Must run inside a TTY session (luna-send prints nothing otherwise).
SECS=${1:-30}
INT_MS=${2:-100}
END=$(( $(cut -d. -f1 /proc/uptime) + SECS ))
SLEEP=$(awk "BEGIN{print $INT_MS/1000}")
LAST=""
BEAT=0
while [ "$(cut -d. -f1 /proc/uptime)" -lt "$END" ]; do
  T=$(cut -d' ' -f1 /proc/uptime)
  J=$(luna-send -n 1 luna://com.webos.media/getActivePipelines '{}' 2>/dev/null)
  SVP=$(echo "$J" | grep -o '"resource":"SVP[A-Z_]*"' | wc -l)
  VDEC=$(echo "$J" | grep -o '"resource":"VDEC[A-Z_0-9]*"' | wc -l)
  FG=$(echo "$J" | tr '{' '\n' | grep '"is_foreground":true' | grep -o '"type":"[a-z_]*"' | sort -u | tr '\n' ' ')
  FGM=$(echo "$J" | tr '{' '\n' | grep '"is_foreground":true' | grep -c '"type":"media"')
  RES=$(echo "$J" | grep -o '"resource":"[A-Z_0-9]*"' | sort | uniq -c | tr -s ' ' | tr '\n' ' ')
  CUR="svp=$SVP vdec=$VDEC fgmedia=$FGM types=[$FG] res=[$RES]"
  NOW=$(cut -d. -f1 /proc/uptime)
  if [ "$CUR" != "$LAST" ] || [ "$NOW" -ge "$BEAT" ]; then
    echo "$T $CUR"
    LAST="$CUR"
    BEAT=$(( NOW + 5 ))
  fi
  sleep "$SLEEP"
done
