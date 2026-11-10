#!/bin/sh
# Builds the throwaway Phase 1 probe IPK (io.github.dimi3h.ambilight.probe).
set -e
cd "$(dirname "$0")"
APP=io.github.dimi3h.ambilight.probe
SVC=$APP.service
mkdir -p build
zig cc -target arm-linux-gnueabi.2.35 -mcpu=cortex_a9 -O2 -Wall -Wextra -rdynamic -o build/vtprobe vtprobe.c -ldl
ST=build/stage
rm -rf "$ST"
mkdir -p "$ST/usr/palm/applications/$APP" "$ST/usr/palm/services/$SVC" "$ST/usr/palm/packages/$APP"
cp build/vtprobe pipewatch.sh "$ST/usr/palm/services/$SVC/"
chmod 755 "$ST/usr/palm/services/$SVC/vtprobe" "$ST/usr/palm/services/$SVC/pipewatch.sh"
cat > "$ST/usr/palm/services/$SVC/services.json" <<J
{"id":"$SVC","description":"Ambilight phase 1 probe","engine":"native","executable":"vtprobe",
 "services":[{"name":"$SVC","description":"Ambilight phase 1 probe"}]}
J
cat > "$ST/usr/palm/applications/$APP/appinfo.json" <<J
{"id":"$APP","version":"0.0.1","vendor":"local","type":"web","main":"index.html",
 "title":"Ambilight probe","icon":"icon.png"}
J
printf '<!doctype html><title>Ambilight probe</title><body style="background:#000;color:#fff">Phase 1 probe. Nothing to see here.</body>\n' \
  > "$ST/usr/palm/applications/$APP/index.html"
python3 -I -c "
import struct,zlib,sys
w=h=80
raw=b''.join(b'\x00'+bytes([40,90,200])*w for _ in range(h))
def ch(t,d): return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
open(sys.argv[1],'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+ch(b'IDAT',zlib.compress(raw))+ch(b'IEND',b''))
" "$ST/usr/palm/applications/$APP/icon.png"
cat > "$ST/usr/palm/packages/$APP/packageinfo.json" <<J
{"id":"$APP","version":"0.0.1","app":"$APP","services":["$SVC"]}
J
python3 -I ../tools/build_ipk.py --root "$ST" --out "build/${APP}_0.0.1_arm.ipk" --id "$APP" --version 0.0.1
