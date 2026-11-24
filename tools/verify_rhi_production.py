"""Run the RHI production acceptance suites and optional renderer smoke matrix.

Run once for each Debug/Release build. Results include commands, test counts,
driver information from test logs, and validation failures; a skipped test is
recorded as skipped, never counted as a pass. Hardware runs require a desktop.
"""

import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import subprocess
import time
import xml.etree.ElementTree as ET

from check_common import rtss_running, uses_installed_rhi

ROOT = Path(__file__).resolve().parents[1]


HAZARD_TEST = "VulkanValidationTest.OverlappingBufferClears"
ACTIVE_LAYER = re.compile(r"Active validation layer=VK_LAYER_KHRONOS_validation spec=\d+ implementation=\d+ "
                          r"requested_validate_sync=true requested_enable_message_limit=false")


def validation_failures(name, output, require_probe=False):
    errors = []
    gpu_check = require_probe or "IntegrationTest" in name or name.startswith("smoke-")
    in_probe = False
    expected = 0
    for line in output.splitlines():
        if line.startswith("[ RUN      ] "):
            in_probe = line.endswith(" " + HAZARD_TEST)
        if line.startswith(("[       OK ]", "[  FAILED  ]", "[  SKIPPED ]")):
            in_probe = False
        if in_probe and "Vulkan validation error " in line and " - SYNC-HAZARD-WRITE-AFTER-WRITE:" in line:
            expected += 1
        elif any(token in line for token in ("VUID-", "SYNC-HAZARD")) or (
                name.startswith("smoke-") and "[error]" in line) or (gpu_check and "Vulkan validation error " in line):
            errors.append(line)
    if (require_probe or expected or name == "ZenRHIVulkanIntegrationTests") and (expected != 1 or "[       OK ] " + HAZARD_TEST not in output):
        errors.append("Required synchronization validation probe did not pass with exactly one expected hazard")
    if gpu_check and not ACTIVE_LAYER.search(output):
        errors.append("Missing active validation layer record")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, help="Engine build containing bin/")
    parser.add_argument("--rhi-build-dir", type=Path, help="Standalone ZenRHI build containing tests/")
    parser.add_argument("--only", nargs="+", metavar="SUITE", help="Select specific suites from the supplied builds")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--unit-only", action="store_true")
    parser.add_argument("--software-driver", action="store_true", help="Record an unsupported adapter profile as a native skip")
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--setup", type=Path, help="Prepared .zenrender setup required for ZenApp smoke checks")
    args = parser.parse_args()
    if not args.build_dir and not args.rhi_build_dir:
        parser.error("Supply --build-dir or --rhi-build-dir")
    if args.smoke and (args.build_dir is None or args.setup is None or not args.setup.is_file()):
        parser.error("--smoke requires --setup pointing to a prepared .zenrender file")
    args.output.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ,
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
            # ZenApp writes a sibling .log. Preserve the source setup and each smoke run independently.
            setup = (args.output / (name + ".zenrender")).resolve()
            setup.write_bytes(args.setup.read_bytes())
            command = [renderer, f"--setup={setup}", f"--rhi={thread}",
                       f"--async-compute={compute}", "--frames=44", "--windowed",
                       "--width=640", "--height=360", "--fixed-step", "--vsync=0", "--validation=required", "--no-ui"]
            commands.append((name, command))
    # Engine filters and smoke jobs can omit the overlapping-clears test. Use the
    # engine build's probe when available, with the same adapter and layer environment.
    native_suite = "ZenRHIVulkanIntegrationTests"
    if any(name != native_suite and ("IntegrationTest" in name or name.startswith("smoke-")) for name, _ in commands):
        candidates = []
        if args.build_dir and not uses_installed_rhi(args.build_dir):
            candidates.append(args.build_dir / "bin" / ("VulkanRHIIntegrationTest" + suffix))
        if args.rhi_build_dir:
            candidates.append(args.rhi_build_dir / "tests" / (native_suite + suffix))
        probe = next((path for path in candidates if path.is_file()), None)
        if probe is None:
            parser.error("GPU acceptance needs the synchronization probe: build VulkanRHIIntegrationTest "
                         "or supply --rhi-build-dir containing ZenRHIVulkanIntegrationTests")
        commands.insert(0, ("sync-validation", [str(probe.resolve()), "--gtest_filter=" + HAZARD_TEST]))
    sync_verified = False
    for name, command in commands:
        started = time.monotonic()
        with Path(command[0]).open("rb") as binary:
            executable_sha256 = hashlib.file_digest(binary, "sha256").hexdigest()
        # ZenApp owns <setup>.log. Capture stdout separately so the two writers cannot overwrite evidence.
        log = args.output / (name + (".stdout.log" if name.startswith("smoke-") else ".log"))
        if args.software_driver and name == "ZenRHIVulkanIntegrationTests":
            probe = subprocess.run(command + ["--check-device-profile"], cwd=ROOT, env=environment,
                                   capture_output=True, text=True, errors="replace", timeout=60)
            profile = probe.stdout + probe.stderr
            (args.output / "adapter-profile.log").write_text(profile, encoding="utf-8")
            if probe.returncode == 77 and ACTIVE_LAYER.search(profile):
                reasons = [line for line in profile.splitlines() if line.startswith("Vulkan adapter=")]
                if reasons:
                    results.append(dict(name=name, executable_sha256=executable_sha256, skipped_suite=True, reasons=reasons))
                    (args.output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
                    print("Native suite skipped:", *reasons, flush=True)
                    continue
            if probe.returncode:
                raise SystemExit("Adapter profile probe failed. See adapter-profile.log")
        report = args.output / (name + ".xml")
        if not name.startswith("smoke-"):
            report.unlink(missing_ok=True)
            command.append("--gtest_output=xml:" + str(report.resolve()))
        with log.open("w", encoding="utf-8") as stream:
            try:
                process = subprocess.run(command, cwd=ROOT, env=environment, stdout=stream,
                                         stderr=subprocess.STDOUT, timeout=300)
                code = process.returncode
            except subprocess.TimeoutExpired:
                code = "timeout"
        output = log.read_text(encoding="utf-8", errors="replace")
        validates_gpu = "IntegrationTest" in name or name.startswith("smoke-")
        preflight = name == "sync-validation"
        validation = validation_failures(name, output, require_probe=preflight)
        if preflight or name == native_suite:
            sync_verified = code == 0 and not validation
        if validates_gpu and not sync_verified:
            validation.append("Effective synchronization validation was not verified")
        skip_reasons = []
        if report.exists():
            for case in ET.parse(report).iter("testcase"):
                skip = case.find("skipped")
                if skip is not None:
                    skip_reasons.append(dict(test=f"{case.get('classname')}.{case.get('name')}", reason=skip.get("message")))
        passed = re.search(r"\[  PASSED  \] (\d+) tests?", output)
        skipped = re.search(r"\[  SKIPPED \] (\d+) tests?,", output)
        results.append(dict(name=name, command=command, executable_sha256=executable_sha256, exit=code,
                            elapsed_seconds=round(time.monotonic() - started, 2),
                            passed=int(passed[1]) if passed else None,
                            skipped=int(skipped[1]) if skipped else 0,
                            skipped_tests=skip_reasons,
                            devices=sorted(set(re.findall(r"Selected Vulkan GPU: (.+)", output))),
                            active_layers=sorted(set(ACTIVE_LAYER.findall(output))),
                            sync_validation_verified=sync_verified if validates_gpu or preflight else None,
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
