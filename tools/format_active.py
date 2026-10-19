"""Format active, project-owned C++ and GLSL code with the repository's .clang-format."""

import argparse
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = ("ZenCore", "ZenSamples", "ZenUI", "Data/Shaders")
# GLSL is formatted as C++: shared includes plus every glslang stage suffix.
SOURCE_SUFFIXES = {".h", ".hpp", ".cpp", ".inl", ".glsl", ".vert", ".tesc", ".tese", ".geom", ".frag", ".comp",
                   ".mesh", ".task", ".rgen", ".rint", ".rahit", ".rchit", ".rmiss", ".rcall"}


def active_source(name):
    path = Path(name)
    if path.suffix not in SOURCE_SUFFIXES or not name.startswith(tuple(f"{root}/" for root in SOURCE_ROOTS)):
        return False
    # This translation unit is a third-party implementation wrapper.
    return name != "ZenCore/Source/vk_mem_alloc.cpp"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="report formatting differences")
    parser.add_argument("--clang-format", default="clang-format")
    args = parser.parse_args()
    files = subprocess.check_output(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", *SOURCE_ROOTS],
        cwd=ROOT,
    ).decode().split("\0")
    files = sorted({name for name in files if active_source(name) and (ROOT / name).is_file()})
    options = ["--dry-run", "--Werror"] if args.check else ["-i"]
    for start in range(0, len(files), 40):
        subprocess.run(
            [args.clang_format, "--style=file", "--fallback-style=none", *options, *files[start:start + 40]],
            cwd=ROOT,
            check=True,
        )
    print(f"{'Checked' if args.check else 'Formatted'} {len(files)} active C++ and GLSL files.")


if __name__ == "__main__":
    main()
