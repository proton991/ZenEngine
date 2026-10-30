"""Rewrite tracked working-tree files with the line endings .gitattributes requests.

Git applies new attributes only when it next writes a file, so a checkout made
before .gitattributes keeps its CRLF copies. Run this once per checkout. Every
rewritten file is hashed through Git's filters and must match its index blob,
so content never changes.

Git reports any file whose size differs from its index stat as modified without
reading it, which is why an ending-only rewrite looks like a change. Files whose
filtered content equals the index blob get their stat re-recorded; real edits
are left untouched.
"""

import argparse
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def git(*arguments, data=None):
    return subprocess.run(["git", *arguments], cwd=ROOT, input=data, check=True, capture_output=True).stdout


def candidates():
    """Return (path, current bytes, normalized bytes) for files that need rewriting."""
    pending = []
    for entry in git("ls-files", "-z", "--eol").decode("utf-8").split("\0"):
        if "\t" in entry:
            info, path = entry.split("\t", 1)
            index, worktree, attributes = info.split(None, 2)
            ending = b"\r\n" if "eol=crlf" in attributes else b"\n" if "eol=lf" in attributes else None
            # Only text committed without CR; binary and CRLF-committed files are left alone.
            if ending is not None and index == "i/lf" and worktree in ("w/lf", "w/crlf", "w/mixed"):
                current = (ROOT / path).read_bytes()
                normalized = current.replace(b"\r\n", b"\n").replace(b"\n", ending)
                if normalized != current:
                    pending.append((path, current, normalized))
    return pending


def index_blobs():
    blobs = {}
    for line in git("ls-files", "-s", "-z").decode("utf-8").split("\0"):
        if line:
            metadata, path = line.split("\t", 1)
            blobs[path] = metadata.split()[1]
    return blobs


def unchanged(paths, blobs):
    """Return the paths whose working file, filtered as Git would add it, equals its index blob."""
    present = [path for path in paths if (ROOT / path).is_file()]
    hashed = git("hash-object", "--stdin-paths", data="\n".join(present).encode("utf-8")).decode().split()
    return [path for path, blob in zip(present, hashed) if blobs.get(path) == blob] if present else []


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="list files that would be rewritten")
    args = parser.parse_args()
    pending = candidates()
    restored = []
    if args.check:
        for path, _, _ in pending:
            print(path)
        print(f"{len(pending)} files need line-ending normalization.")
    else:
        blobs = index_blobs()
        for path, _, normalized in pending:
            (ROOT / path).write_bytes(normalized)
        verified = set(unchanged([path for path, _, _ in pending], blobs))
        for path, current, _ in pending:
            if path not in verified:
                (ROOT / path).write_bytes(current)
                restored.append(path)
                print(f"Restored {path}: the normalized file did not match its index blob.")
        stale = [path for path in git("diff-files", "-z", "--name-only").decode("utf-8").split("\0") if path]
        refreshed = unchanged(stale, blobs)
        if refreshed:
            git("update-index", "-z", "--stdin", data="\0".join(refreshed).encode("utf-8") + b"\0")
        print(f"Normalized {len(pending) - len(restored)} files, restored {len(restored)}, "
              f"re-recorded {len(refreshed)} unchanged files; {len(stale) - len(refreshed)} have real edits.")
    return 1 if restored or (args.check and pending) else 0


if __name__ == "__main__":
    raise SystemExit(main())
