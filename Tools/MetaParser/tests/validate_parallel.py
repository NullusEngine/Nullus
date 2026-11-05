"""Exercise the published parser without building the engine's C++ test binaries."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import time
import unittest


def write_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class ParserFixture:
    def __init__(self, parser, base_config, artifacts, count=12):
        self.parser = parser.resolve()
        self.base_config = base_config
        self.root = artifacts.resolve()
        self.repo = Path(__file__).resolve().parents[3]
        self.config = json.loads(base_config.read_text(encoding="utf-8-sig"))
        source = self.root / "Runtime" / "Fixture"
        self.output = self.root / "Gen"
        self.headers = []
        write_text(source / "Foreign.h", '''#pragma once
#include "Reflection/Macros.h"
namespace NLS::Fixture {
STRUCT(Foreign) { GENERATED_BODY() PROPERTY() int externalValue; };
ENUM(Choice) { First, Second };
}
''')
        for index in range(count):
            header = source / f"Item{index:02}.h"
            self.headers.append(str(header))
            write_text(header, f'''#pragma once
#include "Reflection/Macros.h"
#include "Foreign.h"
namespace NLS::Fixture {{
STRUCT(Item{index:02})
{{
    GENERATED_BODY()
    PROPERTY() int value{index:02};
    PROPERTY() Foreign foreign;
    PROPERTY() Choice choice;
    FUNCTION() int Read{index:02}() const {{ return value{index:02}; }}
}};
}}
''')
        self.config.update(
            RootDir=str(self.root), SourceDir=str(source), OutputDir=str(self.output),
            TargetName="Fixture", ModuleName="Fixture", Headers=self.headers,
        )
        self.config["IncludeDirs"] = [str(source)] + self.config["IncludeDirs"]
        self.environment = dict(os.environ)
        for variable in ("NLS_META_PARSER_MODE", "NLS_META_PARSER_JOBS"):
            self.environment.pop(variable, None)
        architecture = "arm64" if platform.machine().lower() in ("arm64", "aarch64") else "x64"
        system = {"Windows": "windows", "Darwin": "macos"}.get(platform.system(), "linux")
        dotnet = self.repo / "Tools" / "Dotnet" / system / architecture
        if dotnet.is_dir():
            self.environment["DOTNET_ROOT"] = str(dotnet)
        if os.name == "nt":
            native_dirs = []
            packages = self.repo / "Tools/MetaParser/ThirdParty/.nuget/packages"
            for package in ("libclang", "libclangsharp"):
                native_dirs.extend(str(path) for path in packages.glob(
                    f"{package}.runtime.win-{architecture}/*/runtimes/win-{architecture}/native"))
            self.environment["PATH"] = os.pathsep.join(native_dirs + [self.environment.get("PATH", "")])

    def run(self, mode="threads", jobs=4, name="run", extra=None, environment=None, command=None):
        config = dict(self.config, ParseMode=mode, ParseJobs=jobs)
        config.update(extra or {})
        params = self.root / f"{name}.json"
        write_text(params, json.dumps(config, indent=2))
        started = time.perf_counter()
        result = subprocess.run([*(command or [str(self.parser)]), str(params)],
                                env=dict(self.environment, **(environment or {})),
                                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=180)
        elapsed = time.perf_counter() - started
        write_text(self.root / f"{name}.log", result.stdout + result.stderr)
        return result, elapsed

    def snapshot(self):
        return {str(path.relative_to(self.output)): path.read_bytes()
                for path in self.output.rglob("*") if path.is_file()}


class ParallelParserTests(unittest.TestCase):
    fixture = None

    def success(self, mode="threads", jobs=4, name="run", extra=None):
        result, elapsed = self.fixture.run(mode, jobs, name, extra)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result, elapsed

    def test_executor_output_equivalence_and_catalog_reuse(self):
        snapshots = []
        for mode, jobs in (("serial", 1), ("threads", 4), ("processes", 4)):
            result, _ = self.success(mode, jobs, name=mode)
            snapshots.append(self.fixture.snapshot())
            self.assertIn("UniqueHeaders=13", result.stdout)
            self.assertIn("Types=12", result.stdout)
        self.assertEqual(snapshots[0], snapshots[1])
        self.assertEqual(snapshots[0], snapshots[2])
        self.assertFalse(any("Foreign.generated" in path for path in snapshots[0]))
        for index in range(12):
            content = snapshots[0][f"Item{index:02}.generated.cpp"].decode("utf-8")
            self.assertIn(f'"value{index:02}"', content)
            self.assertIn(f'"Read{index:02}"', content)
            self.assertNotIn(f'"value{(index + 1) % 12:02}"', content)

    def test_unchanged_outputs_keep_timestamps(self):
        self.success(name="unchanged-first")
        before = {path: path.stat().st_mtime_ns for path in self.fixture.output.rglob("*") if path.is_file()}
        self.success("processes", name="unchanged-second")
        self.assertEqual(before, {path: path.stat().st_mtime_ns for path in before})

    def test_single_worker_and_duplicate_headers(self):
        result, _ = self.success("processes", 1, name="one-worker", extra={
            "Headers": self.fixture.headers + self.fixture.headers,
        })
        self.assertIn("UniqueHeaders=13", result.stdout)
        self.assertIn("Types=12", result.stdout)

    def test_invalid_execution_settings_fail_before_generation(self):
        for extra in ({"ParseJobs": -1}, {"ParseMode": "invalid"}):
            with self.subTest(extra=extra):
                before = self.fixture.snapshot()
                result, _ = self.fixture.run(name="invalid-options", extra=extra)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(before, self.fixture.snapshot())

    def test_environment_overrides_and_dotnet_host(self):
        dotnet = Path(self.fixture.environment["DOTNET_ROOT"]) / ("dotnet.exe" if os.name == "nt" else "dotnet")
        result, _ = self.fixture.run("invalid", -1, name="dotnet-host", environment={
            "NLS_META_PARSER_MODE": "processes", "NLS_META_PARSER_JOBS": "2",
        }, command=[str(dotnet), str(self.fixture.parser.with_suffix(".dll"))])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Mode=processes, Jobs=2", result.stdout)

    def test_concurrent_targets_share_only_complete_stubs(self):
        fixture = ParserFixture(self.fixture.parser, self.fixture.base_config,
                                self.fixture.root / "concurrent-targets" / str(time.monotonic_ns()), count=4)
        shared_gen = fixture.root / "Runtime/Fixture/Gen"
        fixture.config["IncludeDirs"].insert(0, str(shared_gen))
        for header_path in fixture.headers:
            header = Path(header_path)
            original = header.read_text(encoding="utf-8")
            write_text(header, original.replace('#include "Reflection/Macros.h"',
                f'#include "Reflection/Macros.h"\n#include "{header.stem}.generated.h"'))
        # Each target writes to its own output, but they share Runtime stubs.
        with ThreadPoolExecutor(max_workers=2) as executor:
            tasks = [executor.submit(fixture.run, "threads", 2, f"target-{index}", {
                "OutputDir": str(fixture.root / f"Gen{index}"),
            }) for index in range(2)]
            for task in tasks:
                result, _ = task.result()
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        snapshots = [{str(path.relative_to(fixture.root / f"Gen{index}")): path.read_bytes()
                      for path in (fixture.root / f"Gen{index}").rglob("*") if path.is_file()}
                     for index in range(2)]
        self.assertEqual(snapshots[0], snapshots[1])

    def test_parse_failure_preserves_existing_outputs(self):
        self.success(name="before-failure")
        before = self.fixture.snapshot()
        broken = self.fixture.root / "Runtime/Fixture/Broken.h"
        write_text(broken, '#include "Reflection/Macros.h"\nSTRUCT(Broken) { invalid C++ syntax; };\n')
        try:
            for mode in ("threads", "processes"):
                with self.subTest(mode=mode):
                    result, _ = self.fixture.run(mode, name=f"failure-{mode}", extra={
                        "Headers": [str(broken)] + self.fixture.headers,
                    })
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("Broken.h", result.stdout + result.stderr)
                    self.assertEqual(before, self.fixture.snapshot())
        finally:
            broken.unlink()

    def test_header_edit_is_observed(self):
        header = Path(self.fixture.headers[0])
        original = header.read_text(encoding="utf-8")
        try:
            write_text(header, original.replace("value00", "editedValue"))
            self.success(name="edited-header")
            text = (self.fixture.output / "Item00.generated.cpp").read_text(encoding="utf-8")
            self.assertIn('"editedValue"', text)
            self.assertNotIn('"value00"', text)
        finally:
            write_text(header, original)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--parser", type=Path, required=True)
    parser.add_argument("--base-config", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--benchmark-only", action="store_true")
    parser.add_argument("--baseline", action="store_true")
    args = parser.parse_args()
    fixture = ParserFixture(args.parser, args.base_config, args.artifacts)
    if not args.benchmark_only:
        ParallelParserTests.fixture = fixture
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(ParallelParserTests)
        if not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful():
            return 1
    timings = []
    modes = ("serial",) if args.baseline else ("serial", "threads", "processes")
    expected = None
    for mode in modes:
        result, elapsed = fixture.run(mode, 4, name=f"benchmark-{mode}")
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        snapshot = fixture.snapshot()
        if expected is not None and snapshot != expected:
            raise AssertionError(f"{mode} output differs from serial")
        expected = snapshot
        digest = hashlib.sha256()
        for path, contents in sorted(snapshot.items()):
            digest.update(path.encode("utf-8"))
            digest.update(contents)
        timings.append(dict(mode=mode, seconds=round(elapsed, 3), files=len(snapshot), sha256=digest.hexdigest()))
    write_text(fixture.root / "timings.json", json.dumps(timings, indent=2))
    print(json.dumps(timings, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
