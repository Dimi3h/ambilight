#!/bin/sh
# Build the ambilight service and package it as a webOS IPK.
# One command: `sh build.sh`. Output: build/io.github.dimi3h.ambilight_<ver>_arm.ipk
#
# Requires `zig` (cross-compiler) and python3 on the build host. No webOS NDK or Docker
# is needed for this MVP (histogram + UI capture via dlopen, JSON output).
set -e
cd "$(dirname "$0")"

APP=io.github.dimi3h.ambilight
SVC=$APP.service
VER=$(cat VERSION)
TARGET="arm-linux-gnueabi.2.35"
CPU="cortex_a9"
# webOS userland uses the soft-float calling convention. softfp keeps that convention
# (doubles passed in core registers, compatible with the TV's libc/libm) but lets the
# compiler use the VFP/NEON unit; plain gnueabi would emulate every float op in software.
FLOAT="-mfloat-abi=softfp"

mkdir -p build
echo "Compiling ambilight $VER for $TARGET..."
# -s: no debug info in the package (951 kB -> 109 kB); -rdynamic keeps printLog exported,
# which interposes libvtcapture's logging.
zig cc -target "$TARGET" -mcpu="$CPU" $FLOAT -O2 -s -Wall -Wextra -Wshadow -rdynamic -DAMBI_VERSION="\"$VER\"" \
    -o build/ambilight src/ambilight.c -ldl -lm

ST=build/stage
rm -rf "$ST"
mkdir -p "$ST/usr/palm/applications/$APP" \
         "$ST/usr/palm/services/$SVC" \
         "$ST/usr/palm/packages/$APP"

# --- native service ---
cp build/ambilight "$ST/usr/palm/services/$SVC/ambilight"
cp service/autostart.sh service/supervise.sh service/ambilightctl service/helper.js "$ST/usr/palm/services/$SVC/"
echo "$VER" > "$ST/usr/palm/services/$SVC/VERSION"
chmod 755 "$ST/usr/palm/services/$SVC/ambilight" "$ST/usr/palm/services/$SVC/autostart.sh" \
    "$ST/usr/palm/services/$SVC/supervise.sh" "$ST/usr/palm/services/$SVC/ambilightctl"
chmod 644 "$ST/usr/palm/services/$SVC/helper.js" "$ST/usr/palm/services/$SVC/VERSION"
cat > "$ST/usr/palm/services/$SVC/services.json" <<JSON
{"id":"$SVC","description":"Ambilight hybrid capture service","engine":"native",
 "executable":"ambilight","services":[{"name":"$SVC","description":"Ambilight capture"}]}
JSON

# --- app: the settings screen (ui/) ---
cat > "$ST/usr/palm/applications/$APP/appinfo.json" <<JSON
{"id":"$APP","version":"$VER","vendor":"Dimi3h","type":"web","main":"index.html",
 "title":"Ambilight","icon":"icon.png","largeIcon":"largeIcon.png","resolution":"1920x1080",
 "disableBackHistoryAPI":true,"appDescription":"Hybrid ambilight capture for HyperHDR"}
JSON
cp ui/index.html ui/app.js "$ST/usr/palm/applications/$APP/"
# icons (a screen with a coloured glow around it), generated: no binary assets in the repo
python3 -I tools/make_icon.py 80 "$ST/usr/palm/applications/$APP/icon.png"
python3 -I tools/make_icon.py 130 "$ST/usr/palm/applications/$APP/largeIcon.png"
python3 -I tools/make_icon.py 256 build/icon-256.png

# --- the on-screen level of the brightness keys (osd/) ---
# A second app, because the window type belongs to the app: "topui" is a transparent top
# layer that takes no keys (see osd/index.html). The TV's app manager only registers the one
# app a package names (a second app directory in this package stayed unknown to it, measured),
# so the level app is a small package of its own, carried in the service directory and
# installed from there by `ambilightctl osd` (run at setup and by the supervisor at every
# start). Hidden from the launcher.
OSD="$APP.osd"
OST=build/stage-osd
rm -rf "$OST"
mkdir -p "$OST/usr/palm/applications/$OSD" "$OST/usr/palm/packages/$OSD"
cat > "$OST/usr/palm/applications/$OSD/appinfo.json" <<JSON
{"id":"$OSD","version":"$VER","vendor":"Dimi3h","type":"web","main":"index.html",
 "title":"Ambilight","icon":"icon.png","resolution":"1920x1080","transparent":true,
 "defaultWindowType":"topui","noSplashOnLaunch":true,"spinnerOnLaunch":false,"visible":false,
 "handlesRelaunch":true,"disableBackHistoryAPI":true}
JSON
cp osd/index.html "$OST/usr/palm/applications/$OSD/"
cp "$ST/usr/palm/applications/$APP/icon.png" "$OST/usr/palm/applications/$OSD/icon.png"
cat > "$OST/usr/palm/packages/$OSD/packageinfo.json" <<JSON
{"id":"$OSD","version":"$VER","app":"$OSD"}
JSON
python3 -I tools/build_ipk.py --root "$OST" --out "$ST/usr/palm/services/$SVC/osd.ipk" \
    --id "$OSD" --version "$VER" --description "Ambilight on-screen level" > /dev/null
chmod 644 "$ST/usr/palm/services/$SVC/osd.ipk"

cat > "$ST/usr/palm/packages/$APP/packageinfo.json" <<JSON
{"id":"$APP","version":"$VER","app":"$APP","services":["$SVC"]}
JSON

python3 -I tools/build_ipk.py --root "$ST" --out "build/${APP}_${VER}_arm.ipk" \
    --id "$APP" --version "$VER" --description "Ambilight hybrid capture for HyperHDR"
