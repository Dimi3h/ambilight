#!/bin/sh
# usage: snaprun.sh <outdir> <count> [<seconds between>]
# Has the running service write <count> snapshots (UI plane + raw histogram). Dev tool.
D=/media/developer/ambilight
OUT=$1; N=$2; W=${3:-0.5}
mkdir -p "$OUT"
L() { luna-send $1 "$2" < /dev/null 2> /dev/null; }
orig=$(cat $D/args)
pid=$(cat $D/service.pid)
L "-n 1 luna://com.webos.settingsservice/getSystemSettings" '{"category":"dimensionInfo"}' > "$OUT/dimension.json"
L "-n 1 luna://com.webos.settingsservice/getSystemSettings" '{"category":"picture","keys":["pictureMode","colorTemperature","contrast","brightness","backlight","energySaving"]}' > "$OUT/picture.json"
L "-n 1 luna://com.webos.applicationManager/getForegroundAppInfo" '{}' > "$OUT/foreground.json"
i=0
while [ $i -lt "$N" ]; do
    echo "$orig --snap $OUT/$(printf %03d $i)" > $D/args
    kill -USR1 "$pid"
    sleep "$W"
    i=$((i + 1))
done
echo "$orig" > $D/args
kill -USR1 "$pid"
echo "done: $(ls "$OUT" | grep -c '\.ui$') UI snapshots, $(ls "$OUT" | grep -c '\.hist$') histograms; args restored"
