#!/bin/sh
# usage: sweep.sh <outdir> "<colour temperatures>" "<picture modes>"
# Measures the histogram's white point per colour temperature (in the current picture mode)
# and per picture mode, with test colours in the top UI layer. Changes two picture settings
# for the duration and puts both back (also when interrupted). Dev tool, not shipped.
D=/media/developer/ambilight
OSD=io.github.dimi3h.ambilight.osd
OUT=$1; TEMPS=$2; MODES=$3
SS=luna://com.webos.settingsservice
mkdir -p "$OUT"
L() { luna-send -n 1 "$1" "$2" < /dev/null 2> /dev/null | cat; }
get() { L $SS/getSystemSettings "{\"category\":\"picture\",\"keys\":[\"$1\"]}" | sed -n "s/.*\"$1\":\"\{0,1\}\([^\",}]*\).*/\1/p"; }
set1() { L $SS/setSystemSettings "{\"category\":\"picture\",\"settings\":{\"$1\":\"$2\"}}" | grep -q '"returnValue":true' || echo "SET $1=$2 FAILED"; }
orig=$(cat $D/args); pid=$(cat $D/service.pid)
MODE0=$(get pictureMode); TEMP0=$(get colorTemperature)
echo "start: mode $MODE0, colour temperature $TEMP0"
[ -n "$MODE0" ] && [ -n "$TEMP0" ] || { echo "cannot read the current settings: nothing changed"; exit 1; }
restore() {
    trap - EXIT HUP INT TERM
    set1 pictureMode "$MODE0"; sleep 1; set1 colorTemperature "$TEMP0"; sleep 0.5
    echo "$orig" > $D/args; kill -USR1 "$pid"
    L luna://com.webos.applicationManager/launch "{\"id\":\"$OSD\",\"params\":{\"fill\":null}}" > /dev/null
    echo "restored: mode $(get pictureMode), colour temperature $(get colorTemperature) (was $MODE0, $TEMP0)"
}
trap restore EXIT HUP INT TERM
shot() { # <tag> <colour>...
    tag=$1; shift
    for c in "$@"; do
        L luna://com.webos.applicationManager/launch "{\"id\":\"$OSD\",\"params\":{\"fill\":\"#$c\"}}" > /dev/null
        sleep 0.8
        for k in 1 2; do
            echo "$orig --hist-dump $OUT/$tag.$c.$k.txt" > $D/args; kill -USR1 "$pid"; sleep 0.3
        done
    done
}
L luna://com.webos.applicationManager/launch "{\"id\":\"$OSD\",\"params\":{\"pct\":50}}" > /dev/null
sleep 2.6
for t in $TEMPS; do
    set1 colorTemperature "$t"; sleep 1.2
    echo "T $t $(get colorTemperature)" >> "$OUT/index.txt"
    shot "T$t" ffffff 808080
done
set1 colorTemperature "$TEMP0"; sleep 0.8
for m in $MODES; do
    set1 pictureMode "$m"; sleep 1.5
    echo "M $m $(get pictureMode) temp=$(get colorTemperature) contrast=$(get contrast) brightness=$(get brightness) color=$(get color) gamma=$(get gamma) dyncontrast=$(get dynamicContrast) dyncolor=$(get dynamicColor)" >> "$OUT/index.txt"
    shot "M$m" ffffff 808080 404040 ff0000
done
restore
cat "$OUT/index.txt"
