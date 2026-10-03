#!/usr/bin/env python3
"""Run external OpenGL correctness tests locally or in an SSH-accessible guest."""
import argparse
import bz2
from collections import Counter
from datetime import datetime, timezone
import gzip
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import uuid


# Match either spelling used by Piglit versions for group separators.
SMOKE = (
    r"^spec[/@](!?opengl 1\.0|gl-1\.0)[/@].*(blend|depth|stencil|texture|scissor)",
    r"^spec[/@](!?opengl 1\.1|gl-1\.1)[/@](depthfunc|depthrange-clear|copyteximage 2d|texsubimage)",
    r"^spec[/@](!?opengl 2\.[01]|gl-2\.[01])[/@]",
    r"^spec[/@]ext_framebuffer_object[/@]fbo-(alphatest|blending|depth|drawbuffers|colormask|copyteximage-simple)",
    r"^spec[/@]arb_depth_texture[/@](depth-level-clamp|depth-tex-modes|texdepth)$",
    r"^spec[/@]ext_packed_depth_stencil[/@](fbo-blit-d24s8|depth_stencil texture|readpixels-24_8|texsubimage|errors|readdrawpixels)$",
    r"^spec[/@]glsl-1\.(10|20)[/@]execution[/@](.*discard|glsl-fs-frontfacing|tex-miplevel-selection|const-builtin[/@]glsl-const-builtin-clamp)",
)
CLEAN_ENV = (
    "LIBGL_ALWAYS_SOFTWARE", "GALLIUM_DRIVER", "MESA_LOADER_DRIVER_OVERRIDE",
    "MESA_GL_VERSION_OVERRIDE", "MESA_GLSL_VERSION_OVERRIDE", "LD_PRELOAD",
    "LIBGL_ALWAYS_INDIRECT", "PIGLIT_PLATFORM", "PIGLIT_CONFIG",
)


def load_results(directory, planned=None):
    for name, opener in (("results.json", open), ("results.json.bz2", bz2.open),
                         ("results.json.gz", gzip.open)):
        path = directory / name
        if path.exists():
            with opener(path, "rt") as stream:
                return json.load(stream)["tests"]
    if (directory / "metadata.json").exists() and (directory / "tests").is_dir():
        tests = {}
        for path in sorted((directory / "tests").glob("*.json"), key=lambda path: int(path.stem)):
            try:
                tests.update(json.loads(path.read_text()))
            except json.JSONDecodeError:
                # Preserve the damaged raw file; incomplete runs always fail.
                continue
        for name in planned or ():
            tests.setdefault(name, {"result": "notrun"})
        return tests
    raise RuntimeError(f"No completed Piglit JSON results in {directory}")


def summarize(out, renderers):
    plan = out / "test-list.txt"
    planned = plan.read_text().splitlines() if plan.exists() else None
    tests = {name: load_results(out / name, planned) for name in renderers}
    report = {"count_unit": "top-level Piglit cases", "counts": {}, "subtest_counts": {},
              "completed": {},
              "problems": {}, "svga_problems_passing_llvmpipe": [],
              "svga_skips_passing_llvmpipe": [], "svga_unverified_passing_llvmpipe": []}
    failed = False
    for name, cases in tests.items():
        complete = any((out / name / filename).exists() for filename in
                       ("results.json", "results.json.bz2", "results.json.gz"))
        report["completed"][name] = complete
        counts = Counter(case["result"] for case in cases.values())
        problems = {key: value["result"] for key, value in cases.items()
                    if value["result"] not in {"pass", "skip"}}
        report["counts"][name] = dict(sorted(counts.items()))
        subtests = Counter(value for case in cases.values()
                           for key, value in case.get("subtests", {}).items() if key != "__type__")
        report["subtest_counts"][name] = dict(sorted(subtests.items()))
        report["problems"][name] = problems
        failed |= not complete or bool(problems) or not counts["pass"] or any(
            value not in {"pass", "skip"} for value in subtests)
        print(f"{name}: {dict(counts)}", flush=True)
    if len(tests) == 2:
        if tests["svga"].keys() != tests["llvmpipe"].keys():
            raise RuntimeError("The two renderers did not run the same test names")
        for key, value in tests["svga"].items():
            if tests["llvmpipe"][key]["result"] == "pass":
                if value["result"] == "skip":
                    report["svga_skips_passing_llvmpipe"].append(key)
                elif value["result"] in {"notrun", "incomplete"}:
                    report["svga_unverified_passing_llvmpipe"].append(key)
                elif value["result"] != "pass":
                    report["svga_problems_passing_llvmpipe"].append(key)
    (out / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    return int(failed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ssh", help="Guest user@host; omit to run locally")
    parser.add_argument("--piglit", default="piglit", help="Piglit executable on the test machine")
    parser.add_argument("--display", default=":0")
    parser.add_argument("--renderer", choices=("compare", "svga", "llvmpipe"), default="compare")
    parser.add_argument("--suite", choices=("smoke", "quick"), default="smoke")
    parser.add_argument("--include", action="append", help="Replace smoke selection with these regexes")
    parser.add_argument("--exclude", action="append", default=[])
    parser.add_argument("--last", action="append", help="Run matching cases last; smoke defaults to copyteximage/texsubimage")
    parser.add_argument("--test-timeout", type=int, default=60)
    parser.add_argument("--run-timeout", type=int, default=1800)
    parser.add_argument("--output", type=Path, help="New local artifact directory")
    args = parser.parse_args()
    if args.test_timeout < 1 or args.run_timeout < 1:
        parser.error("timeouts must be positive")
    if args.ssh and (args.ssh.startswith("-") or any(c.isspace() for c in args.ssh)):
        parser.error("--ssh must be a single user@host destination")
    tag = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
    out = (args.output or Path(__file__).resolve().parents[1] / "artifacts" / f"piglit-{tag}").resolve()
    out.mkdir(parents=True, exist_ok=False)
    ssh = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8"]

    def command(argv):
        return ssh + [args.ssh, shlex.join(argv)] if args.ssh else argv

    def capture(argv):
        return subprocess.check_output(command(argv), text=True, timeout=30)

    remote = capture(["mktemp", "-d", "/tmp/vlkn-piglit-XXXXXXXX"]).strip() if args.ssh else str(out)
    renderers = ["svga", "llvmpipe"] if args.renderer == "compare" else [args.renderer]
    filters = args.include if args.include is not None else (SMOKE if args.suite == "smoke" else ())
    last = args.last if args.last is not None else (
        (r"copyteximage|texsubimage",) if args.suite == "smoke" and args.include is None else ())
    metadata = vars(args).copy()
    metadata.update(output=str(out), remote_output=remote, source_commit=None)
    try:
        metadata["source_commit"] = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], text=True,
            cwd=Path(__file__).resolve().parents[1]).strip()
        metadata["source_dirty"] = bool(subprocess.check_output(
            ["git", "status", "--porcelain"], text=True,
            cwd=Path(__file__).resolve().parents[1]).strip())
    except subprocess.CalledProcessError:
        pass
    (out / "run.json").write_text(json.dumps(metadata, default=str, indent=2) + "\n")
    print(f"Artifacts: {out}\nTest-machine results: {remote}", flush=True)
    try:
        (out / "system.txt").write_text(capture(["uname", "-a"]))
        with (out / "packages.txt").open("w") as log:
            subprocess.run(command(["dpkg-query", "-W", "piglit", "libgl1-mesa-dri"]),
                           stdout=log, stderr=subprocess.STDOUT, timeout=30)
        selection = []
        for pattern in filters:
            selection += ["-t", pattern]
        for pattern in args.exclude:
            selection += ["-x", pattern]
        listing = capture([args.piglit, "print-cmd", "--format", "{name}"] + selection + ["quick"])
        names = listing.splitlines()
        if last:
            names.sort(key=lambda name: any(re.search(pattern, name, re.I) for pattern in last))
        if not names or len(set(names)) != len(names):
            raise RuntimeError("Piglit returned an empty or duplicated test list")
        test_list = out / "test-list.txt"
        test_list.write_text("\n".join(names) + "\n")
        if args.ssh:
            subprocess.run(["scp", "-q", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
                            str(test_list), f"{args.ssh}:{remote}/test-list.txt"],
                           check=True, timeout=30)
        selection = ["--test-list", f"{remote}/test-list.txt"]
        runner_errors = {}
        for renderer in renderers:
            env = ["env"]
            for key in CLEAN_ENV:
                env += ["-u", key]
            env += [f"DISPLAY={args.display}"]
            if renderer == "llvmpipe":
                env += ["LIBGL_ALWAYS_SOFTWARE=1", "GALLIUM_DRIVER=llvmpipe"]
            identity = capture(env + ["glxinfo", "-B"])
            (out / f"{renderer}-glxinfo.txt").write_text(identity)
            renderer_lines = [line for line in identity.splitlines() if "OpenGL renderer string:" in line]
            expected = "SVGA3D" if renderer == "svga" else "llvmpipe"
            if len(renderer_lines) != 1 or expected not in renderer_lines[0]:
                raise RuntimeError(f"Expected {expected}; inspect {renderer}-glxinfo.txt")
            print(f"{renderer}: {renderer_lines[0].strip()}", flush=True)
            argv = env + [args.piglit, "run", "-p", "glx", "-1", "--timeout",
                          str(args.test_timeout), "-b", "json", "-n", renderer]
            argv += selection
            argv += ["quick", f"{remote}/{renderer}"]
            (out / f"{renderer}-command.txt").write_text(shlex.join(argv) + "\n")
            with (out / f"{renderer}-run.log").open("w") as log:
                result = subprocess.run(command(["timeout", "--kill-after=15s",
                                                 str(args.run_timeout)] + argv),
                                        stdout=log, stderr=subprocess.STDOUT,
                                        timeout=args.run_timeout + 30)
            (out / f"{renderer}-exitcode.txt").write_text(str(result.returncode) + "\n")
            if result.returncode:
                runner_errors[renderer] = result.returncode
                print(f"{renderer}: runner exited {result.returncode}; retaining partial results", flush=True)
        (out / "runner-errors.json").write_text(json.dumps(runner_errors, indent=2) + "\n")
        paths = [f"{remote}/{name}" for name in renderers]
        for summary_args, filename in ((["console", "-s"], "console.txt"),
                                       (["html", "-e", "pass", f"{remote}/html"], "html.log")):
            with (out / filename).open("w") as log:
                subprocess.run(command([args.piglit, "summary"] + summary_args + paths),
                               stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
    finally:
        # Keep guest artifacts for interrupted runs as well. No destructive cleanup.
        if args.ssh:
            subprocess.run(["scp", "-q", "-r", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
                            f"{args.ssh}:{remote}/.", str(out)], check=True, timeout=300)
    return max(summarize(out, renderers), int(bool(runner_errors)))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Piglit testing failed: {error}", file=sys.stderr)
        sys.exit(1)
