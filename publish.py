#!/usr/bin/env python3
"""Publish a release as a channel, and move that channel's pointer.

A channel is a mutable label over immutable releases: publishing 0.4.2 never
changes it, and moving `stable` to it is one edit. The document written here
is exactly what job/updates.c resolves against, so the C finder and this
publisher cannot drift.

Dry by default. Nothing leaves the machine without --execute.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_DIST = HERE / "dist"


def collect(version_root: Path, base: str) -> tuple[dict, str, list[Path]]:
    """Merge every platform's release.json into one channel document."""
    document: dict = {"core": {}, "app": {}, "applications": {}}
    artifacts: list[Path] = []
    release_version = None

    for release_file in sorted(version_root.glob("*/release.json")):
        data = json.loads(release_file.read_text())
        release_version = release_version or data["version"]
        platform = data["platform"]
        known = data.get("artifacts", {})

        for image in ("core", "app"):
            entry = known.get(f"{image}.tar.gz")
            if entry is None:
                continue
            document[image][platform] = {
                "version": data["version"],
                "url": f"{base}/{image}.tar.gz",
                "sha256": entry["sha256"],
                "size": entry["size"],
            }
            artifacts.append(release_file.parent / f"{image}.tar.gz")

        for application, info in data.get("applications", {}).items():
            entry = known.get(f"full/{application}.tar.gz")
            if entry is None:
                continue
            document["applications"].setdefault(application, {}).setdefault(platform, {})[
                "tar.gz"
            ] = {
                "version": info["version"],
                "url": f"{base}/{application}.tar.gz",
                "sha256": entry["sha256"],
                "size": entry["size"],
            }
            artifacts.append(release_file.parent / "full" / f"{application}.tar.gz")

    return document, release_version or "0.0.0", artifacts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", required=True, help="version to publish")
    parser.add_argument("--channel", default="stable")
    parser.add_argument("--dist", default=str(DEFAULT_DIST))
    parser.add_argument(
        "--base-url",
        default="https://github.com/vex-graph/job/releases/download/{tag}",
        help="where the uploaded artifacts will live; {tag} is substituted",
    )
    parser.add_argument("--tag", default=None, help="release tag; defaults to v<version>")
    parser.add_argument("--execute", action="store_true", help="upload and move the pointer")
    args = parser.parse_args()

    dist = Path(args.dist)
    version_root = dist / "releases" / args.release
    if not version_root.is_dir():
        print(f"no release at {version_root}; run build.py first")
        return 1

    tag = args.tag or f"v{args.release}"
    base = args.base_url.replace("{tag}", tag).rstrip("/")
    images, version, artifacts = collect(version_root, base)

    if not artifacts:
        print("nothing to publish: no archives in this release")
        return 1

    document = {
        "channel": args.channel,
        "release": version,
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "core": images["core"],
        "app": images["app"],
        "applications": images["applications"],
    }

    channels = dist / "channels"
    channels.mkdir(parents=True, exist_ok=True)
    document_path = channels / f"{args.channel}.json"
    document_path.write_text(json.dumps(document, indent=2) + "\n")

    platforms = sorted({platform for image in ("core", "app") for platform in images[image]})
    applications = sorted(images["applications"])
    print(f"channel {args.channel} -> release {version}")
    print(f"  platforms    {', '.join(platforms) or 'none'}")
    print(f"  applications {', '.join(applications) or 'none'}")
    print(f"  artifacts    {len(artifacts)}")
    print(f"  -> {document_path}")

    if not args.execute:
        print("\ndry run: pass --execute to upload")
        return 0

    view = subprocess.run(["gh", "release", "view", tag], capture_output=True, text=True)
    if view.returncode != 0:
        create = subprocess.run(
            [
                "gh", "release", "create", tag,
                "--title", f"vex {version}",
                "--notes", f"channel {args.channel}, {version}",
            ],
            capture_output=True,
            text=True,
        )
        if create.returncode != 0:
            print(f"release create failed: {create.stderr.strip()}")
            return 1
        print(f"created release {tag}")

    upload = subprocess.run(
        ["gh", "release", "upload", tag, *map(str, artifacts), str(document_path), "--clobber"],
        capture_output=True,
        text=True,
    )
    if upload.returncode != 0:
        print(f"upload failed: {upload.stderr.strip()}")
        return 1
    print(f"uploaded {len(artifacts)} artifact(s) + {args.channel}.json to {tag}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
