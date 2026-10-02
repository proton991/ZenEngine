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
import shutil
import subprocess
import time


ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--unit-only", action="store_true")
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--diagnostics", action="store_true")
    parser.add_argument("--executable-alias", help="Optional executable filename for overlay exclusion")
    args = parser.parse_args()
    if args.executable_alias and Path(args.executable_alias).name != args.executable_alias:
        parser.error("--executable-alias must be a filename")
    args.output.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, VK_LAYER_VALIDATE_SYNC="1",
                       VK_LOADER_LAYERS_DISABLE="~implicit~", DISABLE_RTSS_LAYER="1")
    suffix = ".exe" if os.name == "nt" else ""
    results = []
    suites = ["CommonTest", "RenderCoreTest", "VulkanRHITest"]
    if not args.unit_only:
        suites.append("VulkanRHIIntegrationTest")
    commands = [(suite, [str((args.build_dir / "bin" / (suite + suffix)).resolve())])
                for suite in suites]
    if args.smoke:
        renderer = str((args.build_dir / "bin" / ("scene_renderer_demo" + suffix)).resolve())
        for mode, thread, compute in itertools.product((1, 2, 3), (0, 1), (0, 1)):
            command = [renderer, f"--mode={mode}", f"--rhi-thread={thread}",
                       f"--async-compute={compute}", "--smoke-test", "--frames=44",
                       "--fixed-step", "--vsync=0"]
            if args.diagnostics:
                command.append("--device-loss-diagnostics")
            commands.append((f"smoke-m{mode}-t{thread}-a{compute}", command))
    for name, command in commands:
        if args.executable_alias:
            destination = args.output / "binaries" / Path(command[0]).stem / args.executable_alias
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(command[0], destination)
            command[0] = str(destination.resolve())
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
                      if "VUID-" in line or "SYNC-HAZARD" in line]
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
            raise SystemExit(1)


if __name__ == "__main__":
    main()
