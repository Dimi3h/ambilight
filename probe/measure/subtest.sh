#!/bin/sh
# usage: subtest.sh <outdir> yellow|white <cycles> [keep]
# Shows subtitle-like text in the top UI layer on and off over whatever plays, while the
# running service writes its zone means (--calib-dump). "keep" = with the filter off.
D=/media/developer/ambilight
OSD=io.github.dimi3h.ambilight.osd
OUT=$1; STYLE=$2; N=$3; KEEP=$4
mkdir -p "$OUT"; rm -f "$OUT/cal.txt"
L() { luna-send -n 1 "$1" "$2" < /dev/null > /dev/null 2>&1; }
orig=$(cat $D/args); pid=$(cat $D/service.pid)
extra="--calib-dump $OUT/cal.txt"
[ "$KEEP" = keep ] && extra="$extra --keep-subtitles"
L luna://com.webos.applicationManager/launch "{\"id\":\"$OSD\",\"params\":{\"pct\":50}}"
sleep 2.6
echo "$orig $extra" > $D/args; kill -USR1 "$pid"
i=0
while [ $i -lt "$N" ]; do
    L luna://com.webos.applicationManager/launch "{\"id\":\"$OSD\",\"params\":{\"subs\":\"$STYLE\"}}"
    sleep 1.3
    L luna://com.webos.applicationManager/launch "{\"id\":\"$OSD\",\"params\":{\"subs\":null}}"
    sleep 1.3
    i=$((i + 1))
done
echo "$orig" > $D/args; kill -USR1 "$pid"
sleep 0.3
echo "done: $(grep -c '^H' "$OUT/cal.txt") frames, $(grep -c '^U' "$OUT/cal.txt") UI changes; args restored"
