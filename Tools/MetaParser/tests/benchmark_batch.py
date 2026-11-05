"""Compare cached separate processes with precompiled templates and one batch host."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import re
import statistics
import subprocess
import time

from validate_parallel import ParserFixture, write_text
from benchmark_targets import snapshot, checksum


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--parser", type=Path, required=True)
    parser.add_argument("--baseline-parser", type=Path, required=True)
    parser.add_argument("--configs", type=Path, nargs="+", required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--baseline-batch", action="store_true", help="Compare against the baseline batch host")
    args = parser.parse_args()
    artifacts = args.artifacts.resolve()
    fixture = ParserFixture(args.parser, args.configs[0], artifacts / "environment", count=1)
    for name in ("NLS_META_PARSER_CACHE", "NLS_META_PARSER_CACHE_DIR", "NLS_META_PARSER_TOTAL_JOBS"):
        fixture.environment.pop(name, None)
    inputs = [json.loads(path.read_text(encoding="utf-8-sig")) for path in args.configs]
    references = {}
    results = []

    def configs_for(label, cache_name):
        paths = []
        for original in inputs:
            config = dict(original)
            name = config["TargetName"]
            macro = name.upper() + "_EXPORT"
            config.update(OutputDir=str(artifacts / label / name), CacheDirectory=str(artifacts / cache_name),
                          UseCache=True, ParseMode="threads", ParseJobs=4, TotalParseJobs=8,
                          VisibilityDefines=[macro] if macro in config["Defines"] else [])
            path = artifacts / f"{label}-{name}.json"
            write_text(path, json.dumps(config))
            paths.append(path)
        return paths

    old_paths = configs_for("baseline", "baseline-cache")
    separate_paths = configs_for("precompiled", "optimized-cache")
    batch_paths = configs_for("batch", "optimized-cache")

    def invoke(executable, arguments, label):
        result = subprocess.run([str(executable.resolve()), *map(str, arguments)], env=fixture.environment,
                                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=900)
        write_text(artifacts / f"{label}.log", result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        return result.stdout

    def separate(executable, paths, label):
        with ThreadPoolExecutor(max_workers=len(paths)) as pool:
            futures = [pool.submit(invoke, executable, [path], f"{label}-{index}") for index, path in enumerate(paths)]
            return "\n".join(future.result() for future in futures)

    def batch(label):
        manifest = artifacts / "targets.batch.json"
        write_text(manifest, json.dumps({"Configs": list(map(str, batch_paths)), "TargetJobs": len(batch_paths)}))
        return invoke(args.parser, ["--batch", manifest], label)

    def baseline_batch(label):
        manifest = artifacts / "baseline-targets.batch.json"
        write_text(manifest, json.dumps({"Configs": list(map(str, old_paths)), "TargetJobs": len(old_paths)}))
        return invoke(args.baseline_parser, ["--batch", manifest], label)

    print("Warming baseline model cache...", flush=True)
    if args.baseline_batch:
        baseline_batch("baseline-warmup")
    else:
        separate(args.baseline_parser, old_paths, "baseline-warmup")
    for original in inputs:
        name = original["TargetName"]
        references[name] = snapshot(artifacts / "baseline" / name)
    print("Warming precompiled/batch model cache...", flush=True)
    batch("batch-warmup")

    for iteration in range(args.repeat):
        for label, run in (("baseline", lambda: baseline_batch(f"baseline-{iteration}") if args.baseline_batch
                           else separate(args.baseline_parser, old_paths, f"baseline-{iteration}")),
                           ("precompiled", lambda: separate(args.parser, separate_paths, f"precompiled-{iteration}")),
                           ("batch", lambda: batch(f"batch-{iteration}"))):
            started = time.perf_counter()
            output = run()
            seconds = time.perf_counter() - started
            misses = sum(map(int, re.findall(r"CacheMisses=(\d+)", output)))
            if misses:
                raise AssertionError(f"Measured run {label} was not warm: {misses} native parses")
            targets = []
            for original in inputs:
                name = original["TargetName"]
                files = snapshot(artifacts / label / name)
                if files != references[name]:
                    raise AssertionError(f"{label}/{name} differs from the baseline")
                targets.append(dict(target=name, files=len(files), sha256=checksum(files)))
            match = re.search(r"FileHashes=(\d+), DirectoryScans=(\d+)", output)
            item = dict(mode=label, iteration=iteration, seconds=round(seconds, 3), native_parses=misses, targets=targets)
            if match:
                item.update(file_hashes=int(match[1]), directory_scans=int(match[2]))
            results.append(item)
            write_text(artifacts / "measurements.json", json.dumps(results, indent=2))
            print(json.dumps(item), flush=True)
    summary = {label: round(statistics.median(item["seconds"] for item in results if item["mode"] == label), 3)
               for label in ("baseline", "precompiled", "batch")}
    write_text(artifacts / "summary.json", json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
