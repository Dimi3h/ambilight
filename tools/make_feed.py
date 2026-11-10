#!/usr/bin/env python3
"""Build the Homebrew Channel feed for a release.

usage: make_feed.py --version X.Y.Z --ipk build/<id>_X.Y.Z_arm.ipk --icon build/icon-256.png --out dist

Writes into --out: the IPK, repo.json (the feed Homebrew Channel and the app's update check
read), description.html, icon.png, sha256sums and notes.md (this version's CHANGELOG
section). The release workflow uploads them as the assets of GitHub release vX.Y.Z, so
https://github.com/Dimi3h/ambilight/releases/latest/download/repo.json always points at
the newest version.
"""
import argparse
import hashlib
import html
import json
import os
import re
import shutil

APP_ID = "io.github.dimi3h.ambilight"
REPO = "https://github.com/Dimi3h/ambilight"
SHORT = "Hybrid ambilight for HyperHDR: LG's colour histogram (also for DRM content) plus the UI layer."
POINTS = [
    "Keeps the TV's own 16-zone colour histogram open, so Netflix, Disney+ and other protected "
    "content get ambient light too. No protected frames are captured or decrypted.",
    "Reconstructs the screen edges with a per-model calibration (LG OLED B4), blends menus and "
    "subtitles on top and detects black bars.",
    "Sends to HyperHDR's JSON API with one origin and one priority.",
    "Settings app on the TV; the remote's blue key can toggle the LEDs; update notice in the app.",
    "Needs root (Homebrew Channel) and a HyperHDR instance on the network. Open the app once after "
    "installing: it finishes the setup (permissions, autostart).",
]


def notes_for(version, changelog="CHANGELOG.md"):
    try:
        text = open(changelog, encoding="utf-8").read()
    except OSError:
        return f"Ambilight {version}\n"
    m = re.search(rf"^## {re.escape(version)}\b.*?$(.*?)(?=^## |\Z)", text, re.M | re.S)
    body = m.group(1).strip() if m else ""
    return (body or f"Ambilight {version}") + f"\n\nInstall or update through Homebrew Channel: see the README.\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--ipk", required=True)
    ap.add_argument("--icon", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    if not re.fullmatch(r"\d+\.\d+\.\d+", a.version):
        raise SystemExit("version must be X.Y.Z")
    os.makedirs(a.out, exist_ok=True)
    base = f"{REPO}/releases/download/v{a.version}/"
    ipk_name = os.path.basename(a.ipk)
    if ipk_name != f"{APP_ID}_{a.version}_arm.ipk":
        raise SystemExit(f"unexpected IPK name {ipk_name}")
    shutil.copyfile(a.ipk, os.path.join(a.out, ipk_name))
    shutil.copyfile(a.icon, os.path.join(a.out, "icon.png"))
    data = open(a.ipk, "rb").read()
    sha = hashlib.sha256(data).hexdigest()

    feed = {
        "paging": {"page": 1, "count": 1, "maxPage": 1, "itemsTotal": 1},
        "packages": [{
            "id": APP_ID,
            "title": "Ambilight",
            "shortDescription": SHORT,
            "fullDescriptionUrl": base + "description.html",
            "iconUri": base + "icon.png",
            "manifest": {
                "id": APP_ID,
                "version": a.version,
                "type": "web",
                "title": "Ambilight",
                "appDescription": SHORT,
                "iconUri": base + "icon.png",
                "sourceUrl": REPO,
                "rootRequired": True,
                "ipkUrl": base + ipk_name,
                "ipkHash": {"sha256": sha},
                "ipkSize": len(data),
            },
        }],
    }
    with open(os.path.join(a.out, "repo.json"), "w", encoding="utf-8") as f:
        json.dump(feed, f, indent=2)
        f.write("\n")
    with open(os.path.join(a.out, "description.html"), "w", encoding="utf-8") as f:
        f.write("<p>" + html.escape(SHORT) + "</p>\n<ul>\n")
        f.writelines(f"  <li>{html.escape(p)}</li>\n" for p in POINTS)
        f.write(f'</ul>\n<p>Source and documentation: <a href="{REPO}">{REPO}</a></p>\n')
    with open(os.path.join(a.out, "sha256sums"), "w", encoding="utf-8") as f:
        f.write(f"{sha}  {ipk_name}\n")
    with open(os.path.join(a.out, "notes.md"), "w", encoding="utf-8") as f:
        f.write(notes_for(a.version))
    print(f"feed for {a.version}: {ipk_name} sha256 {sha} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
