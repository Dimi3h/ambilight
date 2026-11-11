#!/usr/bin/env python3
"""Build a webOS IPK (opkg/ar format) from a staging directory.

usage: build_ipk.py --root STAGING --out FILE.ipk --id PKG_ID --version X.Y.Z [--arch arm]

The staging directory holds the final file tree (usr/palm/...). Files are stored
with uid/gid 0 and their executable bits preserved; timestamps are fixed so the
same input yields the same IPK.
"""
import argparse
import gzip
import io
import os
import tarfile

MTIME = 1700000000


def tar_gz(members):
    """members: list of (arcname, bytes|None for dir, mode)"""
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.GNU_FORMAT) as tf:
        for name, data, mode in members:
            ti = tarfile.TarInfo(name)
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = "root"
            ti.mtime = MTIME
            ti.mode = mode
            if data is None:
                ti.type = tarfile.DIRTYPE
                tf.addfile(ti)
            else:
                ti.size = len(data)
                tf.addfile(ti, io.BytesIO(data))
    out = io.BytesIO()
    with gzip.GzipFile(fileobj=out, mode="wb", mtime=MTIME) as gz:
        gz.write(raw.getvalue())
    return out.getvalue()


def ar_archive(entries):
    out = io.BytesIO()
    out.write(b"!<arch>\n")
    for name, data in entries:
        hdr = f"{name + '/':<16}{MTIME:<12}{0:<6}{0:<6}{0o100644:<8o}{len(data):<10}`\n".encode()
        assert len(hdr) == 60
        out.write(hdr)
        out.write(data)
        if len(data) % 2:
            out.write(b"\n")
    return out.getvalue()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--id", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--arch", default="arm")
    ap.add_argument("--description", default="")
    a = ap.parse_args()

    members = []
    for dirpath, dirnames, filenames in os.walk(a.root):
        dirnames.sort()
        rel = os.path.relpath(dirpath, a.root)
        if rel != ".":
            members.append(("./" + rel, None, 0o755))
        for fn in sorted(filenames):
            if fn == ".DS_Store":
                continue
            p = os.path.join(dirpath, fn)
            mode = 0o755 if os.access(p, os.X_OK) else 0o644
            with open(p, "rb") as f:
                members.append(("./" + os.path.normpath(os.path.join(rel, fn)), f.read(), mode))

    size = sum(len(d) for _, d, _ in members if d is not None)
    control = (
        f"Package: {a.id}\nVersion: {a.version}\nSection: misc\nPriority: optional\n"
        f"Architecture: {a.arch}\nInstalled-Size: {size}\nMaintainer: local\n"
        f"Description: {a.description or a.id}\nwebOS-Package-Format-Version: 2\n"
        f"webOS-Packager-Version: x.y.x\n"
    ).encode()
    control_tgz = tar_gz([(".", None, 0o755), ("./control", control, 0o644)])
    data_tgz = tar_gz(members)
    ipk = ar_archive([("debian-binary", b"2.0\n"), ("control.tar.gz", control_tgz), ("data.tar.gz", data_tgz)])
    with open(a.out, "wb") as f:
        f.write(ipk)
    print(f"{a.out}: {len(ipk)} bytes, {len(members)} entries")


if __name__ == "__main__":
    main()
