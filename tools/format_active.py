"""Format active, project-owned C++ code with the repository's .clang-format."""

import argparse
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def active_source(name):
    path = Path(name)
    if path.suffix not in {".h", ".hpp", ".cpp", ".inl"}:
        return False
    if "/Graphics/Val/" in name or name.startswith(("ZenSamples/Applications/", "ZenSamples/ZenCoreTest/")):
        return False
    if "/Graphics/RenderCore/" in name and "/RenderCore/V2/" not in name:
        return False
    if name.startswith("ZenSamples/VulkanRHIDemo/"):
        return "/SceneRenderer/" in name
    if path.name in {"VoxelRenderer.h", "VoxelRenderer.cpp"}:
        return False
    # This translation unit is a third-party implementation wrapper.
    return name != "ZenCore/Source/vk_mem_alloc.cpp"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="report formatting differences")
    parser.add_argument("--clang-format", default="clang-format")
    args = parser.parse_args()
    files = subprocess.check_output(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", "ZenCore", "ZenSamples"],
        cwd=ROOT,
    ).decode().split("\0")
    files = sorted({name for name in files if active_source(name)})
    options = ["--dry-run", "--Werror"] if args.check else ["-i"]
    for start in range(0, len(files), 40):
        subprocess.run(
            [args.clang_format, "--style=file", "--fallback-style=none", *options, *files[start:start + 40]],
            cwd=ROOT,
            check=True,
        )
    print(f"{'Checked' if args.check else 'Formatted'} {len(files)} active C++ files.")


if __name__ == "__main__":
    main()
