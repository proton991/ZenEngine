"""Run the RHI production acceptance suites and optional renderer smoke matrix.

Run once for each Debug/Release build. Results include commands, test counts,
driver information from test logs, and validation failures; a skipped test is
recorded as skipped, never counted as a pass. Hardware runs require a desktop.
"""

import argparse
import itertools
import json
import os
from pathlib import Path
import re
import subprocess
import time

from check_common import rtss_running, uses_installed_rhi

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, help="Engine build containing bin/")
    parser.add_argument("--rhi-build-dir", type=Path, help="Standalone ZenRHI build containing tests/")
    parser.add_argument("--only", nargs="+", metavar="SUITE", help="Select specific suites from the supplied builds")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--unit-only", action="store_true")
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--setup", type=Path, help="Prepared .zenrender setup required for ZenApp smoke checks")
    args = parser.parse_args()
    if not args.build_dir and not args.rhi_build_dir:
        parser.error("Supply --build-dir or --rhi-build-dir")
    if args.smoke and (args.build_dir is None or args.setup is None or not args.setup.is_file()):
        parser.error("--smoke requires --setup pointing to a prepared .zenrender file")
    args.output.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, VK_LAYER_VALIDATE_SYNC="1",
                       VK_LOADER_LAYERS_DISABLE="~implicit~", DISABLE_RTSS_LAYER="1")
    suffix = ".exe" if os.name == "nt" else ""
    results = []
    binaries = {}
    if args.build_dir:
        engine_suites = ["CommonTest", "RenderCoreTest"]
        if not uses_installed_rhi(args.build_dir):
            engine_suites.append("VulkanRHITest")
            if not args.unit_only:
                engine_suites.append("VulkanRHIIntegrationTest")
        if not args.unit_only:
            engine_suites.append("RayQueryIntegrationTest")
        binaries.update((name, args.build_dir / "bin") for name in engine_suites)
    if args.rhi_build_dir:
        rhi_suites = ["ZenRHICoreTests", "ZenRHIVulkanTests"]
        if not args.unit_only:
            rhi_suites.append("ZenRHIVulkanIntegrationTests")
        binaries.update((name, args.rhi_build_dir / "tests") for name in rhi_suites)
    if args.only and set(args.only).difference(binaries):
        parser.error("Unknown or unavailable suite: " + ", ".join(sorted(set(args.only).difference(binaries))))
    commands = [(name, [str((folder / (name + suffix)).resolve())])
                for name, folder in binaries.items() if not args.only or name in args.only]
    if args.smoke:
        renderer = str((args.build_dir / "bin" / ("ZenApp" + suffix)).resolve())
        for thread, compute in itertools.product(("inline", "threaded"), (0, 1)):
            name = f"smoke-{thread}-a{compute}"
            setup = args.setup.resolve()
            command = [renderer, f"--setup={setup}", f"--rhi={thread}",
                       f"--async-compute={compute}", "--frames=44", "--windowed",
                       "--width=640", "--height=360", "--fixed-step", "--vsync=0", "--validation=1", "--no-ui"]
            commands.append((name, command))
    for name, command in commands:
        started = time.monotonic()
        log = args.output / (name + ".log")
        with log.open("w", encoding="utf-8") as stream:
            try:
                process = subprocess.run(command, cwd=ROOT, env=environment, stdout=stream,
                                         stderr=subprocess.STDOUT, timeout=300)
                code = process.returncode
            except subprocess.TimeoutExpired:
                code = "timeout"
        output = log.read_text(encoding="utf-8", errors="replace")
        validation = [line for line in output.splitlines()
                      if "VUID-" in line or "SYNC-HAZARD" in line or (name.startswith("smoke-") and "[error]" in line)]
        passed = re.search(r"\[  PASSED  \] (\d+) tests?", output)
        skipped = re.search(r"\[  SKIPPED \] (\d+) tests?,", output)
        results.append(dict(name=name, command=command, exit=code,
                            elapsed_seconds=round(time.monotonic() - started, 2),
                            passed=int(passed[1]) if passed else None,
                            skipped=int(skipped[1]) if skipped else 0,
                            validation_errors=validation))
        (args.output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
        print(f"{name}: exit={code}, validation_errors={len(validation)}", flush=True)
        if code != 0 or validation:
            if rtss_running():
                print("RTSS is running and can cause this failure; exclude this executable in RTSS or close RTSS, then rerun",
                      flush=True)
            raise SystemExit(1)


if __name__ == "__main__":
    main()
