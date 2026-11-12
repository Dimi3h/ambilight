#!/bin/sh
# usage: patch.sh <outdir> <rrggbb> [<rrggbb> ...]
# Paints the whole screen in the top UI layer, colour by colour, and has the running service
# write the raw histogram bins of each (three reads per colour). Needs the development page
# of the level app (level_dev.py). Run on the TV; nothing is restarted.
D=/media/developer/ambilight
OSD=io.github.dimi3h.ambilight.osd
OUT=$1; shift
mkdir -p "$OUT"
L() { luna-send $1 "$2" < /dev/null 2> /dev/null; }
orig=$(cat $D/args)
pid=$(cat $D/service.pid)
L "-n 1 luna://com.webos.settingsservice/getSystemSettings" '{"category":"dimensionInfo"}' > "$OUT/dimension.json"
L "-n 1 luna://com.webos.settingsservice/getSystemSettings" '{"category":"picture","keys":["pictureMode","colorTemperature","contrast","brightness","backlight","energySaving"]}' > "$OUT/picture.json"
L "-n 1 luna://com.webos.applicationManager/getForegroundAppInfo" '{}' > "$OUT/foreground.json"
# a plain coloured page does not bring a new window to the screen (see osd/index.html): show
# the level once first, then every colour is a relaunch of a loaded page
L "-n 1 luna://com.webos.applicationManager/launch" "{\"id\":\"$OSD\",\"params\":{\"pct\":50}}" > /dev/null
sleep 2.6
for c in "$@"; do
    L "-n 1 luna://com.webos.applicationManager/launch" "{\"id\":\"$OSD\",\"params\":{\"fill\":\"#$c\"}}" > /dev/null
    sleep 0.9
    for k in 1 2 3; do
        echo "$orig --hist-dump $OUT/$c.$k.txt" > $D/args
        kill -USR1 "$pid"
        sleep 0.3
    done
done
echo "$orig" > $D/args
kill -USR1 "$pid"
L "-n 1 luna://com.webos.applicationManager/launch" "{\"id\":\"$OSD\",\"params\":{\"fill\":null}}" > /dev/null
echo "done: $(ls "$OUT" | wc -l) files; args restored: $(cat $D/args)"
