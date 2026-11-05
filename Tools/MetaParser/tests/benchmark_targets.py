"""Compare uncached, cold shared-cache, and warm multi-target generation."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

from validate_parallel import ParserFixture, write_text


def snapshot(directory):
    return {str(path.relative_to(directory)): path.read_bytes()
            for path in directory.rglob("*") if path.is_file()}


def checksum(files):
    digest = hashlib.sha256()
    for name, content in sorted(files.items()):
        digest.update(name.encode("utf-8"))
        digest.update(content)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--parser", type=Path, required=True)
    parser.add_argument("--baseline-parser", type=Path)
    parser.add_argument("--configs", type=Path, nargs="+", required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--parallel-targets", type=int, default=1)
    parser.add_argument("--reuse-baseline", action="store_true", help="Reuse an existing baseline snapshot and timing")
    args = parser.parse_args()
    artifacts = args.artifacts.resolve()
    fixture = ParserFixture(args.parser, args.configs[0], artifacts / "environment", count=1)
    for name in ("NLS_META_PARSER_CACHE", "NLS_META_PARSER_CACHE_DIR", "NLS_META_PARSER_TOTAL_JOBS"):
        fixture.environment.pop(name, None)
    cache = artifacts / f"shared-cache-{time.monotonic_ns()}"
    inputs = [json.loads(path.read_text(encoding="utf-8-sig")) for path in args.configs]
    references = {}
    results = []
    scenarios = ("baseline", "cold-cache", "warm-cache")
    if args.reuse_baseline:
        prior = json.loads((artifacts / "timings.json").read_text(encoding="utf-8"))
        results.append(next(item for item in prior if item["scenario"] == "baseline"))
        for config in inputs:
            name = config["TargetName"]
            references[name] = snapshot(artifacts / "baseline" / name)
            if not references[name]:
                raise RuntimeError(f"Missing baseline snapshot for {name}")
        scenarios = ("cold-cache", "warm-cache")

    def run_target(config, scenario, executable):
        config = dict(config)
        name = config["TargetName"]
        output = artifacts / scenario / name
        macro = name.upper() + "_EXPORT"
        config.update(OutputDir=str(output), ParseMode="threads", ParseJobs=args.jobs,
                      TotalParseJobs=max(8, args.jobs), UseCache=scenario != "baseline", CacheDirectory=str(cache),
                      VisibilityDefines=[macro] if macro in config["Defines"] else [])
        params = artifacts / f"{scenario}-{name}.json"
        write_text(params, json.dumps(config, indent=2))
        print(f"{scenario}: starting {name}", flush=True)
        started = time.perf_counter()
        process = subprocess.run([str(executable.resolve()), str(params)], env=fixture.environment,
                                 capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=900)
        seconds = time.perf_counter() - started
        write_text(artifacts / f"{scenario}-{name}.log", process.stdout + process.stderr)
        if process.returncode:
            raise RuntimeError(f"{scenario}/{name}:\n" + process.stdout + process.stderr)
        files = snapshot(output)
        if scenario == "baseline":
            references[name] = files
        elif files != references[name]:
            changed = sorted(path for path in set(files) | set(references[name])
                             if files.get(path) != references[name].get(path))
            raise AssertionError(f"{scenario}/{name} differs from baseline: {changed}")
        match = re.search(r"CacheHits=(\d+), CacheMisses=(\d+), SharedCacheHits=(\d+)", process.stdout)
        counters = dict(zip(("hits", "misses", "shared_hits"), map(int, match.groups()))) if match else {}
        result = dict(target=name, seconds=round(seconds, 3), files=len(files), sha256=checksum(files), **counters)
        print(json.dumps(result), flush=True)
        return result

    for scenario in scenarios:
        executable = args.baseline_parser if scenario == "baseline" and args.baseline_parser else args.parser
        started = time.perf_counter()
        with ThreadPoolExecutor(max_workers=args.parallel_targets) as pool:
            futures = [pool.submit(run_target, config, scenario, executable) for config in inputs]
            targets = [future.result() for future in futures]
        results.append(dict(scenario=scenario, parallel_targets=args.parallel_targets,
                            seconds=round(time.perf_counter() - started, 3), targets=targets))
        write_text(artifacts / "timings.json", json.dumps(results, indent=2))
    print(json.dumps(results, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
