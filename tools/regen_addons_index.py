#!/usr/bin/env python3
"""Rebuild addons/index.json from addons/packages/<id>/<abi>/<plugin>."""

from __future__ import annotations

import hashlib
import json
import re
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGES = ROOT / "addons" / "packages"
INDEX = ROOT / "addons" / "index.json"

# Catalog metadata (per-addon); binary artifacts are discovered on disk.
ADDON_META = {
    "ai-agent": {
        "name": "AI agent",
        "description": (
            "OpenAI-compatible terminal agent with streaming think, multi-step shell tools, "
            "Markdown chat, and auto-run for read-only commands. Loads only while installed "
            "and enabled."
        ),
        "author": "clientosh",
        "homepage": "https://github.com/hdmain/clientosh",
    },
}

RAW_BASE = "https://raw.githubusercontent.com/hdmain/clientosh/main/addons/packages"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def project_version() -> str:
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r"project\s*\(\s*clientosh\s+VERSION\s+([0-9.]+)", text, re.I)
    return m.group(1) if m else "0.0.0"


def main() -> None:
    addons: list[dict] = []
    if not PACKAGES.is_dir():
        raise SystemExit(f"Missing packages dir: {PACKAGES}")

    for addon_dir in sorted(p for p in PACKAGES.iterdir() if p.is_dir()):
        addon_id = addon_dir.name
        meta = ADDON_META.get(addon_id, {
            "name": addon_id,
            "description": "",
            "author": "clientosh",
            "homepage": "https://github.com/hdmain/clientosh",
        })
        artifacts: list[dict] = []
        for abi_dir in sorted(p for p in addon_dir.iterdir() if p.is_dir()):
            abi = abi_dir.name
            plugins = [
                p
                for p in abi_dir.iterdir()
                if p.is_file() and p.suffix.lower() in {".dll", ".so", ".dylib"}
            ]
            if not plugins:
                continue
            plugin = plugins[0]
            artifacts.append(
                {
                    "abi": abi,
                    "url": f"{RAW_BASE}/{addon_id}/{abi}/{plugin.name}",
                    "sha256": sha256_file(plugin),
                    "size": plugin.stat().st_size,
                }
            )
        if not artifacts:
            continue
        addons.append(
            {
                "id": addon_id,
                "name": meta["name"],
                "description": meta["description"],
                "version": project_version(),
                "author": meta["author"],
                "homepage": meta["homepage"],
                "artifacts": artifacts,
            }
        )

    catalog = {
        "format": 1,
        "updated": date.today().isoformat(),
        "notes": (
            "Official clientosh addon catalog. Artifact ABI must match clientoshAddonAbi() "
            "(qtX.Y-win64-mingw / qtX.Y-linux-x64 / qtX.Y-macos)."
        ),
        "addons": addons,
    }
    INDEX.write_text(json.dumps(catalog, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {INDEX} with {len(addons)} addon(s), "
          f"{sum(len(a['artifacts']) for a in addons)} artifact(s).")


if __name__ == "__main__":
    main()
