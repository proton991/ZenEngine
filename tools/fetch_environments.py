"""Download the optional HDR environment starter set listed in Data/Textures/Environments/sources.json.

The panoramas are CC0 assets from Poly Haven and are not stored in the repository.
Files already present with the recorded size and SHA-256 are kept. Downloads are
verified before they are written; a mismatch leaves the destination untouched.
"""

import hashlib
import json
from pathlib import Path
import sys
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
DIRECTORY = ROOT / "Data" / "Textures" / "Environments"


def matches(data, entry):
    return len(data) == entry["bytes"] and hashlib.sha256(data).hexdigest() == entry["sha256"]


def main():
    failures = []
    for entry in json.loads((DIRECTORY / "sources.json").read_text(encoding="utf-8")):
        destination = DIRECTORY / entry["file"]
        if destination.is_file() and matches(destination.read_bytes(), entry):
            print(f"{entry['file']}: present")
            continue
        request = urllib.request.Request(entry["download"], headers={"User-Agent": "ZenEngine-EnvironmentFetch/1.0"})
        try:
            with urllib.request.urlopen(request, timeout=120) as response:
                data = response.read()
        except OSError as error:
            failures.append(f"{entry['file']}: {error}")
            continue
        if matches(data, entry):
            destination.write_bytes(data)
            print(f"{entry['file']}: downloaded {len(data)} bytes")
        else:
            failures.append(f"{entry['file']}: size or SHA-256 differs from sources.json")
    print("\n".join(failures) if failures else "Environment starter set is complete.")
    return int(bool(failures))


if __name__ == "__main__":
    sys.exit(main())
