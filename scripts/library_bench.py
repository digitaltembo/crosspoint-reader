#!/usr/bin/env python3
"""
Benchmark the library index build in the simulator, before and after a change.

Builds the headless benchmark (env:simulator_bench, src/bench/LibraryBench.cpp)
for the working tree and, optionally, for a baseline commit checked out into a
git worktree. It then runs every scenario against the same book collection and
prints medians side by side.

Scenarios:
  cold     no /.crosspoint at all: full walk, every EPUB's metadata parsed
  warm     unchanged rebuild after a priming build (the freshness check)
  changed  one book's mtime bumped before each rebuild

Storage-call counts (reads, seeks, bytes) carry over to the device far better
than host wall time, which only compares CPU work between builds.

The book collection is never copied or modified: the simulated SD card under
.pio/bench/ mirrors it with symlinks, plus one real copy of the book that the
"changed" scenario touches.

Requires env:simulator / env:simulator_bench in platformio.local.ini.

Usage:
  scripts/library_bench.py --library ~/Books --baseline 7a2ae218
  scripts/library_bench.py --library ~/Books --scenarios cold --runs 3
"""

import argparse
import datetime
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BENCH_DIR = REPO / ".pio" / "bench"
ENV = "simulator_bench"
RUNNER = Path("src/bench/LibraryBench.cpp")
LOCAL_INI = "platformio.local.ini"
SCENARIOS = ("cold", "warm", "changed")
PHASE_RE = re.compile(r"\[LIBIDX\] phase ([^:]+): (\d+)ms")
BUILD_START_PHASE = "prepare/prior"

# Columns of the comparison table: (label, value getter).
METRICS = [
    ("wall ms", lambda r: r["wallMs"]),
    ("cpu ms", lambda r: r["cpuMs"]),
    ("parsed", lambda r: r["parsed"]),
    ("reused", lambda r: r["metadataReused"]),
    ("opens", lambda r: r["io"]["opens"]),
    ("reads", lambda r: r["io"]["reads"]),
    ("KiB read", lambda r: r["io"]["bytesRead"] / 1024),
    ("seeks", lambda r: r["io"]["seeks"]),
    ("writes", lambda r: r["io"]["writes"]),
    ("KiB written", lambda r: r["io"]["bytesWritten"] / 1024),
    ("dir entries", lambda r: r["io"]["dirEntries"]),
    ("path ops", lambda r: r["io"]["pathOps"]),
]


def fail(message):
    sys.exit(f"library_bench: {message}")


def running_under_rosetta():
    if platform.system() != "Darwin":
        return False
    result = subprocess.run(["sysctl", "-n", "sysctl.proc_translated"], capture_output=True, text=True)
    return result.stdout.strip() == "1"


def native(command):
    """Run children natively on Apple Silicon even from an x86_64 (Rosetta) Python.

    A translated parent makes universal tools such as the compiler build for
    x86_64, which cannot link Homebrew's arm64 SDL.
    """
    return ["arch", "-arm64", *command] if running_under_rosetta() else command


def find_pio():
    pio = shutil.which("pio") or str(Path.home() / ".platformio/penv/bin/pio")
    if not Path(pio).exists():
        fail("PlatformIO CLI not found (looked on PATH and in ~/.platformio/penv/bin)")
    return pio


def git(*args, cwd=REPO):
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def prepare_baseline(ref):
    """Detached worktree at `ref`, set up to build the same runner as the working tree."""
    sha = git("rev-parse", "--verify", f"{ref}^{{commit}}")
    tree = BENCH_DIR / "worktrees" / sha[:12]
    if not tree.exists():
        tree.parent.mkdir(parents=True, exist_ok=True)
        git("worktree", "add", "--detach", str(tree), sha)
    if not (tree / "lib/LibraryIndex/LibraryBuilder.h").exists():
        fail(f"{ref} has no lib/LibraryIndex/LibraryBuilder.h to benchmark")

    # Share the working tree's SDK checkout rather than cloning the submodule.
    if git("ls-tree", sha, "freeink-sdk") != git("ls-tree", "HEAD", "freeink-sdk"):
        print(f"warning: {ref} pins a different freeink-sdk; using the working tree's checkout", file=sys.stderr)
    sdk = tree / "freeink-sdk"
    if not sdk.is_symlink():
        if sdk.exists():
            shutil.rmtree(sdk)
        sdk.symlink_to(REPO / "freeink-sdk")

    # Relative symlink:// paths in the local ini are relative to the project,
    # which is no longer next to the simulator checkout.
    ini = (REPO / LOCAL_INI).read_text()
    ini = ini.replace("symlink://../", f"symlink://{REPO.parent}/")
    (tree / LOCAL_INI).write_text(ini)

    # The same runner on both sides, so only the library code differs.
    (tree / RUNNER).parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(REPO / RUNNER, tree / RUNNER)
    return tree


def build(pio, project):
    print(f"building {ENV} in {project} ...", file=sys.stderr)
    result = subprocess.run(native([pio, "run", "-e", ENV, "-d", str(project)]), capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout[-4000:] + result.stderr[-4000:])
        fail(f"build failed in {project}")
    program = project / ".pio" / "build" / ENV / "program"
    if not program.exists():
        fail(f"built, but {program} is missing")
    return program


def make_sd_root(root, library, touch_rel):
    """Simulated SD card: /books mirrors the library through symlinks."""
    if root.exists():
        shutil.rmtree(root)
    books = root / "books"
    for dirpath, dirnames, filenames in os.walk(library):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        rel = Path(dirpath).relative_to(library)
        (books / rel).mkdir(parents=True, exist_ok=True)
        for name in sorted(filenames):
            if name.startswith("."):
                continue
            source = Path(dirpath) / name
            target = books / rel / name
            if touch_rel is not None and rel / name == touch_rel:
                # Real copy: the "changed" scenario rewrites its mtime.
                shutil.copy2(source, target)
            else:
                target.symlink_to(source)


def pick_touch_book(library):
    for dirpath, dirnames, filenames in os.walk(library):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        for name in sorted(filenames):
            if name.lower().endswith(".epub") and not name.startswith("."):
                return (Path(dirpath) / name).relative_to(library)
    fail(f"no .epub found under {library}")


def run_scenario(program, sd_root, scenario, runs, touch_rel, read_metadata):
    command = [str(program), "--runs", str(runs), "--prep", scenario]
    if scenario == "changed":
        command += ["--touch", f"/books/{touch_rel.as_posix()}"]
    if not read_metadata:
        command.append("--no-metadata")
    env = dict(os.environ, CROSSPOINT_SIM_SD=str(sd_root))
    result = subprocess.run(native(command), env=env, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr[-4000:])
        fail(f"{program} --prep {scenario} exited with {result.returncode}")

    records = [json.loads(line[len("BENCH "):]) for line in result.stdout.splitlines() if line.startswith("BENCH ")]

    # Group the builder's phase timings per build; each build logs prepare/prior first.
    builds = []
    for name, ms in PHASE_RE.findall(result.stderr):
        if name == BUILD_START_PHASE or not builds:
            builds.append({})
        builds[-1][name] = int(ms)
    if scenario != "cold":
        builds = builds[1:]  # the untimed priming build
    for record, phases in zip(records, builds):
        record["phasesMs"] = phases
    return records


def median(records, getter):
    values = [getter(r) for r in records]
    return statistics.median(values) if values else None


def format_value(value):
    if value is None:
        return "-"
    return f"{value:,.1f}" if isinstance(value, float) and not value.is_integer() else f"{value:,.0f}"


def print_comparison(results, variants):
    for scenario in sorted({r["scenario"] for r in results}, key=SCENARIOS.index):
        rows = {v: [r for r in results if r["variant"] == v and r["scenario"] == scenario] for v in variants}
        counts = ", ".join(f"{v}: {len(rows[v])} runs, {rows[v][0]['books']} books" for v in variants if rows[v])
        print(f"\n== {scenario} ({counts})")

        phase_names = []
        for variant in variants:
            for record in rows[variant]:
                for name in record.get("phasesMs", {}):
                    if name not in phase_names:
                        phase_names.append(name)
        metrics = METRICS + [(f"phase {n} ms", lambda r, n=n: r.get("phasesMs", {}).get(n, 0)) for n in phase_names]

        header = f"{'metric':<34}" + "".join(f"{v:>14}" for v in variants)
        if len(variants) == 2:
            header += f"{'delta':>10}"
        print(header)
        for label, getter in metrics:
            values = [median(rows[v], getter) for v in variants]
            line = f"{label:<34}" + "".join(f"{format_value(x):>14}" for x in values)
            if len(variants) == 2 and values[0] not in (None, 0) and values[1] is not None:
                line += f"{(values[1] - values[0]) / values[0] * 100:>+9.1f}%"
            print(line)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--library", required=True, type=Path, help="directory of books (read, never modified)")
    parser.add_argument("--baseline", help="git ref to compare against the working tree")
    parser.add_argument("--runs", type=int, default=5, help="timed runs per scenario (default 5)")
    parser.add_argument("--scenarios", default=",".join(SCENARIOS), help="comma-separated subset of: " + ", ".join(SCENARIOS))
    parser.add_argument("--touch", type=Path, help="book (relative to --library) for the changed scenario")
    parser.add_argument("--no-metadata", action="store_true", help="filename-only builds")
    parser.add_argument("--skip-build", action="store_true", help="reuse existing benchmark binaries")
    args = parser.parse_args()

    library = args.library.expanduser().resolve()
    if not library.is_dir():
        fail(f"{library} is not a directory")
    scenarios = [s.strip() for s in args.scenarios.split(",") if s.strip()]
    for scenario in scenarios:
        if scenario not in SCENARIOS:
            fail(f"unknown scenario {scenario}")
    if not (REPO / LOCAL_INI).exists():
        fail(f"{LOCAL_INI} with env:simulator_bench is required")

    touch_rel = args.touch or pick_touch_book(library)
    if not (library / touch_rel).is_file():
        fail(f"--touch {touch_rel} is not a file under {library}")

    projects = {}
    if args.baseline:
        projects["baseline"] = prepare_baseline(args.baseline)
    projects["current"] = REPO

    pio = None if args.skip_build else find_pio()
    programs = {}
    for variant, project in projects.items():
        program = project / ".pio" / "build" / ENV / "program"
        programs[variant] = program if args.skip_build else build(pio, project)
        if not programs[variant].exists():
            fail(f"{programs[variant]} is missing; run without --skip-build")

    results = []
    for scenario in scenarios:
        for variant, program in programs.items():
            print(f"running {variant} / {scenario} ...", file=sys.stderr)
            sd_root = BENCH_DIR / "sd" / variant
            make_sd_root(sd_root, library, touch_rel)
            for record in run_scenario(program, sd_root, scenario, args.runs, touch_rel, not args.no_metadata):
                record.update(variant=variant, scenario=scenario)
                results.append(record)

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = BENCH_DIR / "results" / f"{stamp}.jsonl"
    out.parent.mkdir(parents=True, exist_ok=True)
    meta = {
        "library": str(library),
        "baseline": args.baseline,
        "current": git("rev-parse", "HEAD") + ("+dirty" if git("status", "--porcelain", "--untracked-files=no") else ""),
        "runs": args.runs,
        "metadata": not args.no_metadata,
        "touch": touch_rel.as_posix(),
    }
    with out.open("w") as f:
        f.write(json.dumps({"meta": meta}) + "\n")
        for record in results:
            f.write(json.dumps(record) + "\n")

    print_comparison(results, list(programs))
    print(f"\nraw results: {out.relative_to(REPO)}")


if __name__ == "__main__":
    main()
