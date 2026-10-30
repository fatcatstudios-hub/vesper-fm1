#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
"""VESPER 2: a one-file web installer, fm1-base's web/installer.html with the package inside it.

The page works opened straight from the disk (Chrome or Edge, Web MIDI): the package is a data: URL in it, so
nothing is fetched. fm1-base's offer to replace the FM-1's bootloader (m-bootloader) is left out: VESPER never
touches the bootloader. "Back to the stock firmware" (M-VAVE's V15) stays.

  make_installer.py PACKAGE.fwsc VERSION OUT.html
"""
import base64
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "fm1-base"
sys.path.insert(0, str(BASE / "tools"))
from build import read_conf  # noqa: E402
from make_site import product_of, strip_module  # noqa: E402


def main():
    pkg, version, out = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3])
    conf = read_conf(ROOT / "fm1.toml")
    raw = pkg.read_bytes()
    product = product_of(raw)
    if not re.fullmatch(re.escape(conf["id_prefix"]) + r"\d{6}", product):
        raise SystemExit(f"{pkg}: identity {product!r}: a release build only (tools/build.sh --release X.Y.Z)")
    if b"FELUCCA-LOADER-1" not in raw:
        raise SystemExit(f"{pkg}: no update loader in it")
    html = (BASE / "web" / "installer.html").read_text(encoding="utf-8")
    lib = strip_module((BASE / "web" / "fm1pkg.js").read_text(encoding="utf-8")) + "\n" + \
        strip_module((BASE / "web" / "fm1ota.js").read_text(encoding="utf-8"))
    meta = json.dumps({"version": version, "product": product,
                       "pkg": "data:application/octet-stream;base64," + base64.b64encode(raw).decode(),
                       "backup": False, "boot": None})
    repo = conf.get("repo", "")
    credits = conf.get("credits", "")
    for key, val in (("NAME", conf["name"]), ("REPO", repo), ("REPO_SHORT", re.sub(r"^https?://", "", repo)),
                     ("LICENSE", conf.get("licence", "")), ("CREDITS", " " + credits if credits else "")):
        html = html.replace("{{" + key + "}}", val)
    html = html.replace("/*LIB*/", lib).replace("/*META*/", meta).replace("/*APP*/", "null")
    left = sorted(set(re.findall(r"\{\{[A-Z_]+\}\}", html)))
    if left:
        raise SystemExit(f"installer: {' '.join(left)} not filled in")
    out.write_text(html, encoding="utf-8")
    print(f"installer: {out} ({product}, {len(html) // 1024} KB)")


if __name__ == "__main__":
    main()
