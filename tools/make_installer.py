#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
"""VESPER 2: a one-file web installer, fm1-base's web/installer.html with the package inside it.

The page works opened straight from the disk (Chrome or Edge, Web MIDI): the package is a data: URL in it, so
nothing is fetched. fm1-base's offer to replace the FM-1's bootloader (m-bootloader) is left out: VESPER never
touches the bootloader. "Back to the stock firmware" (M-VAVE's V15) stays.

  make_installer.py PACKAGE.fwsc VERSION OUT.html [--rescue]

--rescue: also installs over VESPER 2.0.0-test3, which took the installer's handshake for itself and so cannot be
found. If no FM-1 answers, the page uses the port named like an FM-1 (Felucca, FM-1, VESPER) and starts the update
there directly: the update request itself is taken by the USB driver, below the app, so test 3 answers it.
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


RESCUE = """    if (!dev) {   // VESPER rescue: a port named like an FM-1 that does not answer the handshake (2.0.0-test3)
      for (const input of this.access.inputs.values()) {
        if (input.state === "disconnected" || !/felucca|fm-1|fm1|vesper/i.test(input.name || "")) continue;
        const output = [...this.access.outputs.values()].find((o) => o.name === input.name);
        if (!output) continue;
        try { await input.open(); await output.open(); } catch (_) { continue; }
        const link = new Link(input, output);
        this.watch();
        try { return await this.stages({ link, id: { text: "VESPER 2.0.0-test3", model: "FM-1", open: false }, name: input.name }, image, product, step); }
        finally { this.unwatch(); }
      }
    }
"""


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    pkg, version, out = Path(args[0]), args[1], Path(args[2])
    conf = read_conf(ROOT / "fm1.toml")
    raw = pkg.read_bytes()
    product = product_of(raw)
    if not re.fullmatch(re.escape(conf["id_prefix"]) + (r"\d{3}" if conf.get("id_digits") == 3 else r"\d{6}"), product):
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
    if "--rescue" in sys.argv:
        old = '    const dev = await this.find((id) => !IS_OTA(id));\n'
        if lib.count(old) != 1:
            raise SystemExit("fm1ota.js: install() is not as expected")
        lib = lib.replace(old, old + RESCUE)
    html = html.replace("/*LIB*/", lib).replace("/*META*/", meta).replace("/*APP*/", "null")
    left = sorted(set(re.findall(r"\{\{[A-Z_]+\}\}", html)))
    if left:
        raise SystemExit(f"installer: {' '.join(left)} not filled in")
    out.write_text(html, encoding="utf-8")
    print(f"installer: {out} ({product}, {len(html) // 1024} KB)")


if __name__ == "__main__":
    main()
