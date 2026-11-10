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
VER=$(cat VERSION 2>/dev/null || echo 0.1.0)
TARGET="arm-linux-gnueabi.2.35"
CPU="cortex_a9"

mkdir -p build
echo "Compiling ambilight $VER for $TARGET..."
zig cc -target "$TARGET" -mcpu="$CPU" -O2 -Wall -Wextra -rdynamic \
    -o build/ambilight src/ambilight.c -ldl -lm

ST=build/stage
rm -rf "$ST"
mkdir -p "$ST/usr/palm/applications/$APP" \
         "$ST/usr/palm/services/$SVC" \
         "$ST/usr/palm/packages/$APP"

# --- native service ---
cp build/ambilight "$ST/usr/palm/services/$SVC/ambilight"
cp service/autostart.sh service/supervise.sh service/ambilightctl "$ST/usr/palm/services/$SVC/"
chmod 755 "$ST/usr/palm/services/$SVC/ambilight" "$ST/usr/palm/services/$SVC/autostart.sh" \
    "$ST/usr/palm/services/$SVC/supervise.sh" "$ST/usr/palm/services/$SVC/ambilightctl"
cat > "$ST/usr/palm/services/$SVC/services.json" <<JSON
{"id":"$SVC","description":"Ambilight hybrid capture service","engine":"native",
 "executable":"ambilight","services":[{"name":"$SVC","description":"Ambilight capture"}]}
JSON

# --- app (placeholder UI; settings app comes in a later step) ---
cat > "$ST/usr/palm/applications/$APP/appinfo.json" <<JSON
{"id":"$APP","version":"$VER","vendor":"Dimi3h","type":"web","main":"index.html",
 "title":"Ambilight","icon":"icon.png","appDescription":"Hybrid ambilight capture for HyperHDR"}
JSON
printf '<!doctype html><meta charset="utf-8"><title>Ambilight</title><body style="background:#111;color:#eee;font-family:sans-serif;padding:2em"><h1>Ambilight</h1><p>Capture service for HyperHDR. Settings UI coming soon.</p></body>\n' \
    > "$ST/usr/palm/applications/$APP/index.html"
# small solid icon (80x80 PNG)
python3 -I - "$ST/usr/palm/applications/$APP/icon.png" <<'PY'
import struct,zlib,sys
w=h=80; raw=b''.join(b'\x00'+bytes([40,90,200])*w for _ in range(h))
def ch(t,d): return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
open(sys.argv[1],'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+ch(b'IDAT',zlib.compress(raw))+ch(b'IEND',b''))
PY

cat > "$ST/usr/palm/packages/$APP/packageinfo.json" <<JSON
{"id":"$APP","version":"$VER","app":"$APP","services":["$SVC"]}
JSON

python3 -I tools/build_ipk.py --root "$ST" --out "build/${APP}_${VER}_arm.ipk" \
    --id "$APP" --version "$VER" --description "Ambilight hybrid capture for HyperHDR"
