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

from check_common import rtss_running

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--unit-only", action="store_true")
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--setup", type=Path, help="Prepared .zenrender setup required for ZenApp smoke checks")
    args = parser.parse_args()
    if args.smoke and (args.setup is None or not args.setup.is_file()):
        parser.error("--smoke requires --setup pointing to a prepared .zenrender file")
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
        renderer = str((args.build_dir / "bin" / ("ZenApp" + suffix)).resolve())
        for thread, compute in itertools.product(("inline", "threaded"), (0, 1)):
            name = f"smoke-{thread}-a{compute}"
            setup = (args.output / (name + ".zenrender")).resolve()
            setup.write_bytes(args.setup.read_bytes())
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
