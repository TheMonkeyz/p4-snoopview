#!/usr/bin/env python3
"""Assemble the web installer site (GitHub Pages) from an ESP-IDF build folder.

    python tools/make_site.py --build build/p4 --out _site [--version TEXT]

Copies web/flash/* to the output, the images named in the build's flasher_args.json to firmware/, and writes
firmware/manifest.json (ESP Web Tools) and version.json (shown on the page). Python's standard library only.
"""
import argparse
import datetime
import json
import os
import shutil
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAMILY = {"esp32p4": "ESP32-P4"}


def git_version():
    try:
        return subprocess.check_output(["git", "describe", "--tags", "--always", "--dirty"], cwd=ROOT, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build", default=os.path.join(ROOT, "build", "p4"), help="the ESP-IDF build folder")
    ap.add_argument("--out", default=os.path.join(ROOT, "_site"), help="the site folder to write")
    ap.add_argument("--version", default=None, help="the version text (default: git describe)")
    a = ap.parse_args()

    with open(os.path.join(a.build, "flasher_args.json")) as f:
        args = json.load(f)
    chip = args["extra_esptool_args"]["chip"]
    version = a.version or git_version()

    if os.path.isdir(a.out):
        shutil.rmtree(a.out)
    shutil.copytree(os.path.join(ROOT, "web", "flash"), a.out)
    fw = os.path.join(a.out, "firmware")
    os.makedirs(fw)

    parts = []
    for offset, rel in sorted(args["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
        name = os.path.basename(rel)
        shutil.copyfile(os.path.join(a.build, rel), os.path.join(fw, name))
        parts.append({"path": name, "offset": int(offset, 16)})      # the path is relative to the manifest

    manifest = {
        "name": "snoopview",
        "version": version,
        "new_install_prompt_erase": False,      # nothing is saved on the board: no need to erase
        "builds": [{"chipFamily": FAMILY[chip], "parts": parts}],
    }
    with open(os.path.join(fw, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    built = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")
    with open(os.path.join(a.out, "version.json"), "w") as f:
        json.dump({"version": version, "built": built}, f)
        f.write("\n")
    open(os.path.join(a.out, ".nojekyll"), "w").close()
    print(f"site: {a.out}  version {version}  {chip}  " + ", ".join(f"{p['path']}@0x{p['offset']:x}" for p in parts))


if __name__ == "__main__":
    main()
