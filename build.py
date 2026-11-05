#!/usr/bin/env python3
"""Build one release: a core image, an app image, and full applications.

One release, one version, one tree. A fresh download and a hot-swapped
install must end at the same version, so every artifact here is cut from the
same run and stamped with the same number.

Layout:

    dist/releases/<version>/<platform>/
        core/<library>/<section>.dylib   + core.tar.gz
        app/<library>/<section>.dylib    + app.tar.gz
        full/<application>/<binary>      + full/<application>.tar.gz
        release.json                     what this release contains

A section is a directory holding a b.assembly.json with "link": "module".
A full application is an executable unit outside a test tree. Missing either
is reported, never invented.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform as host
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TERRITORY = HERE.parent
B = TERRITORY / "b" / "b.sh"
BUILD_OUT = TERRITORY / ".b" / "out"


def current_platform() -> str:
    system = host.system().lower()
    machine = host.machine().lower()
    name = {"darwin": "darwin", "linux": "linux", "windows": "windows"}.get(system, system)
    arch = {"arm64": "arm64", "aarch64": "arm64", "x86_64": "x86_64", "amd64": "x86_64"}.get(
        machine, machine
    )
    return f"{name}-{arch}"


def units(repo_dir: Path) -> dict[str, list[tuple[str, Path]]]:
    """Module and executable units under one repository, split by kind."""
    found: dict[str, list[tuple[str, Path]]] = {"module": [], "executable": []}
    for manifest in repo_dir.rglob("b.assembly.json"):
        if ".b" in set(manifest.parts):
            continue
        try:
            data = json.loads(manifest.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        link = (data.get("build") or {}).get("link")
        if link not in found:
            continue
        name = (data.get("application") or {}).get("name") or manifest.parent.name
        found[link].append((name, manifest.parent))
    return {kind: sorted(entries) for kind, entries in found.items()}


def build(name: str, unit_dir: Path) -> tuple[bool, str]:
    result = subprocess.run(
        [str(B), "build", name], cwd=unit_dir, capture_output=True, text=True
    )
    return result.returncode == 0, (result.stdout + result.stderr).strip()


def digest(path: Path) -> str:
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            sha.update(block)
    return sha.hexdigest()


def archive(source: Path, destination: Path) -> None:
    with tarfile.open(destination, "w:gz") as bundle:
        bundle.add(source, arcname=source.name)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", default=None, help="version; defaults to images.json")
    parser.add_argument("--platform", default=None, help="defaults to this host")
    parser.add_argument("--out", default=str(HERE / "dist"))
    parser.add_argument("--check", action="store_true", help="report, build nothing")
    args = parser.parse_args()

    spec = json.loads((HERE / "images.json").read_text())
    version = args.release or spec["version"]
    target = args.platform or current_platform()
    images = spec["images"]

    root = Path(args.out) / "releases" / version / target
    release = {
        "name": spec["name"],
        "org": spec["org"],
        "version": version,
        "platform": target,
        "images": {},
        "applications": {},
        "artifacts": {},
    }
    missing: list[str] = []
    failed: list[str] = []

    for image in ("core", "app"):
        for repo in images[image]:
            repo_dir = TERRITORY / repo
            if not repo_dir.is_dir():
                print(f"  MISSING {image}/{repo:<18} no such repository")
                missing.append(f"{image}/{repo}")
                continue
            modules = [
                entry
                for entry in units(repo_dir)["module"]
                if "test" not in entry[1].parts
            ]
            if not modules:
                print(f'  MISSING {image}/{repo:<18} no module unit ("link": "module")')
                missing.append(f"{image}/{repo}")
                continue
            for name, unit_dir in modules:
                if args.check:
                    print(f"  OK      {image}/{repo:<18} section {name}")
                    continue
                ok, log = build(name, unit_dir)
                produced = BUILD_OUT / f"{name}.dylib"
                if not ok or not produced.exists():
                    reason = log.splitlines()[-1] if log else "no output"
                    print(f"  FAILED  {image}/{repo}/{name}\n          {reason}")
                    failed.append(f"{image}/{repo}/{name}")
                    continue
                destination = root / image / repo
                destination.mkdir(parents=True, exist_ok=True)
                shutil.copy2(produced, destination / produced.name)
                print(f"  built   {image}/{repo}/{produced.name}")
            release["images"].setdefault(image, {})[repo] = [name for name, _ in modules]

    for application in images["full"]:
        repo_dir = TERRITORY / application
        if not repo_dir.is_dir():
            print(f"  MISSING full/{application:<18} no such repository")
            missing.append(f"full/{application}")
            continue
        executables = [
            entry
            for entry in units(repo_dir)["executable"]
            if "test" not in entry[1].parts
        ]
        if not executables:
            print(f"  MISSING full/{application:<18} no application executable unit")
            missing.append(f"full/{application}")
            continue
        name, unit_dir = executables[0]
        if args.check:
            print(f"  OK      full/{application:<18} application {name}")
            continue
        ok, log = build(name, unit_dir)
        produced = BUILD_OUT / name
        if not ok or not produced.exists():
            reason = log.splitlines()[-1] if log else "no output"
            print(f"  FAILED  full/{application}/{name}\n          {reason}")
            failed.append(f"full/{application}")
            continue
        destination = root / "full" / application
        destination.mkdir(parents=True, exist_ok=True)
        shutil.copy2(produced, destination / produced.name)
        release["applications"][application] = {"binary": produced.name, "version": version}
        print(f"  built   full/{application}/{produced.name}")

    if args.check:
        print(f"\n{len(missing)} missing, {len(failed)} failed.")
        return 1 if missing or failed else 0

    for image in ("core", "app"):
        if (root / image).is_dir():
            archive(root / image, root / f"{image}.tar.gz")
    for application in release["applications"]:
        if (root / "full" / application).is_dir():
            archive(root / "full" / application, root / "full" / f"{application}.tar.gz")

    for artifact in sorted(root.rglob("*.tar.gz")):
        release["artifacts"][artifact.relative_to(root).as_posix()] = {
            "sha256": digest(artifact),
            "size": artifact.stat().st_size,
        }

    root.mkdir(parents=True, exist_ok=True)
    (root / "release.json").write_text(json.dumps(release, indent=2) + "\n")
    print(f"\nrelease {version} {target}: {len(failed)} failed, {len(missing)} missing")
    print(f"  -> {root / 'release.json'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
