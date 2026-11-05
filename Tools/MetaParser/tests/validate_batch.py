"""Check precompiled templates and shared-input multi-target execution."""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import time
import unittest

from validate_parallel import ParserFixture, write_text


def files(root):
    return {str(path.relative_to(root)): path.read_bytes() for path in root.rglob("*") if path.is_file()}


class BatchTests(unittest.TestCase):
    parser_path = None
    baseline = None
    base_config = None
    artifacts = None

    def setUp(self):
        self.root = self.artifacts / self._testMethodName / str(time.monotonic_ns())
        self.fixture = ParserFixture(self.parser_path, self.base_config, self.root, count=4)
        for name in ("NLS_META_PARSER_CACHE", "NLS_META_PARSER_CACHE_DIR", "NLS_META_PARSER_TOTAL_JOBS"):
            self.fixture.environment.pop(name, None)
        self.configs = []
        self.paths = []
        for index, name in enumerate(("Alpha", "Beta")):
            config = dict(self.fixture.config, TargetName=name, ModuleName=name,
                          Headers=self.fixture.headers[index * 2:(index + 1) * 2],
                          OutputDir=str(self.root / f"Gen{name}"), CacheDirectory=str(self.root / "Cache"),
                          VisibilityDefines=[], UseCache=True, TotalParseJobs=2, ParseMode="threads", ParseJobs=2)
            path = self.root / f"{name}.json"
            write_text(path, json.dumps(config))
            self.configs.append(config)
            self.paths.append(path)

    def call(self, arguments, name, *, executable=None, environment=None):
        result = subprocess.run([str(executable or self.parser_path), *map(str, arguments)],
                                env=environment or self.fixture.environment, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=180)
        write_text(self.root / f"{name}.log", result.stdout + result.stderr)
        return result

    def batch(self, name="batch", paths=None, jobs=2):
        manifest = self.root / f"{name}.batch.json"
        write_text(manifest, json.dumps({"Configs": [path.name for path in (paths or self.paths)], "TargetJobs": jobs}))
        return self.call(["--batch", manifest], name)

    def success(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_batch_matches_separate_generation_and_baseline(self):
        for path in self.paths:
            self.success(self.call([path], "reference-" + path.stem, executable=self.baseline or self.parser_path))
        expected = [files(Path(config["OutputDir"])) for config in self.configs]
        self.success(self.batch())
        self.assertEqual(expected, [files(Path(config["OutputDir"])) for config in self.configs])
        for index, name in enumerate(("Alpha", "Beta")):
            text = (Path(self.configs[index]["OutputDir"]) / "MetaGenerated.cpp").read_text(encoding="utf-8")
            self.assertIn(f"LinkReflectionTypes_{name}", text)
            self.assertNotIn(f"LinkReflectionTypes_{'Beta' if name == 'Alpha' else 'Alpha'}", text)

    def test_sdk_hash_and_namespace_work_is_shared(self):
        self.success(self.batch("warmup"))
        single = self.batch("one", paths=[self.paths[0]], jobs=1)
        both = self.batch("two", jobs=2)
        self.success(single)
        self.success(both)
        pattern = r"FileHashes=(\d+), DirectoryScans=(\d+)"
        first = tuple(map(int, re.search(pattern, single.stdout).groups()))
        second = tuple(map(int, re.search(pattern, both.stdout).groups()))
        self.assertGreater(first[0], 0)
        self.assertEqual(first, second)

    def test_cache_loading_and_validation_are_shared(self):
        for index, macro in enumerate(("NLS_ALPHA_EXPORT", "NLS_BETA_EXPORT")):
            self.configs[index]["Defines"] = self.fixture.config["Defines"] + [macro]
            self.configs[index]["VisibilityDefines"] = [macro]
            write_text(self.paths[index], json.dumps(self.configs[index]))
        cold = self.batch("cold-producers")
        self.success(cold)
        self.assertEqual(sum(map(int, re.findall(r"CacheMisses=(\d+)", cold.stdout))), 5)
        result = self.batch("shared-cache-lookups")
        self.success(result)
        match = re.search(r"CacheReads=(\d+), CacheLoadReuses=(\d+), CacheDecodes=(\d+), CacheValidations=(\d+), SourceAnalyses=(\d+), MacroChecks=(\d+)", result.stdout)
        self.assertIsNotNone(match, result.stdout)
        reads, reuses, decodes, validations, analyses, checks = map(int, match.groups())
        self.assertEqual(reads, 5)
        self.assertGreaterEqual(reuses, 5)
        self.assertEqual(decodes, reads * 2)
        self.assertEqual(validations, 5)
        hashes = int(re.search(r"FileHashes=(\d+)", result.stdout).group(1))
        self.assertLessEqual(analyses, hashes)
        self.assertLessEqual(checks, hashes * 2)

    def test_corrupt_shared_entry_is_recovered_in_batch(self):
        for index, macro in enumerate(("NLS_ALPHA_EXPORT", "NLS_BETA_EXPORT")):
            self.configs[index]["Defines"] = self.fixture.config["Defines"] + [macro]
            self.configs[index]["VisibilityDefines"] = [macro]
            write_text(self.paths[index], json.dumps(self.configs[index]))
        self.success(self.batch("before-corruption"))
        expected = [files(Path(config["OutputDir"])) for config in self.configs]
        shared = next(Path(self.configs[0]["CacheDirectory"]).rglob("*.shared.json"))
        write_text(shared, "{corrupt")
        result = self.batch("recover-corruption")
        self.success(result)
        self.assertEqual(expected, [files(Path(config["OutputDir"])) for config in self.configs])
        self.assertLessEqual(sum(map(int, re.findall(r"CacheMisses=(\d+)", result.stdout))), 1)

    def test_parsing_failure_prevents_sibling_generation(self):
        self.success(self.batch("before"))
        expected = files(Path(self.configs[0]["OutputDir"]))
        first = Path(self.fixture.headers[0])
        write_text(first, first.read_text(encoding="utf-8").replace("value00", "changedValue"))
        broken = self.root / "Project/Broken.h"
        write_text(broken, '#include "Reflection/Macros.h"\nSTRUCT(Broken) { invalid syntax; };\n')
        self.configs[1]["Headers"].append(str(broken))
        write_text(self.paths[1], json.dumps(self.configs[1]))
        result = self.batch("failed")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Broken.h", result.stdout + result.stderr)
        self.assertEqual(expected, files(Path(self.configs[0]["OutputDir"])))

    def test_overlapping_outputs_are_rejected(self):
        self.configs[1]["OutputDir"] = self.configs[0]["OutputDir"]
        write_text(self.paths[1], json.dumps(self.configs[1]))
        result = self.batch("overlap")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("overlap", result.stderr)
        self.assertFalse(Path(self.configs[0]["OutputDir"]).exists())

    def test_duplicate_config_paths_are_coalesced(self):
        result = self.batch("duplicates", paths=[self.paths[0], self.paths[1], self.paths[0]])
        self.success(result)
        self.assertIn("BatchTargets=2", result.stdout)

    def test_process_mode_remains_supported(self):
        self.configs[1]["ParseMode"] = "processes"
        write_text(self.paths[1], json.dumps(self.configs[1]))
        result = self.batch("mixed")
        self.success(result)
        self.assertIn("Mode=processes", result.stdout)

    def test_runtime_needs_no_templates_or_dotnet_sdk(self):
        package = self.root / "runtime-package"
        shutil.copytree(self.parser_path.parent, package, ignore=shutil.ignore_patterns("Templates"))
        runtime_source = Path(self.fixture.environment["DOTNET_ROOT"])
        runtime = self.root / "dotnet-runtime-only"
        runtime.mkdir()
        host_name = "dotnet.exe" if (runtime_source / "dotnet.exe").exists() else "dotnet"
        shutil.copy2(runtime_source / host_name, runtime / host_name)
        for directory in ("host", "shared"):
            shutil.copytree(runtime_source / directory, runtime / directory)
        environment = dict(self.fixture.environment, DOTNET_ROOT=str(runtime), DOTNET_ROOT_X64=str(runtime),
                           DOTNET_ROOT_ARM64=str(runtime), DOTNET_HOST_PATH=str(runtime / host_name),
                           DOTNET_MULTILEVEL_LOOKUP="0")
        executable = package / self.parser_path.name
        result = self.call([self.paths[0]], "runtime-only", executable=executable, environment=environment)
        self.success(result)
        self.assertFalse((runtime / "sdk").exists())
        self.assertFalse((package / "Templates").exists())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--parser", type=Path, required=True)
    parser.add_argument("--baseline-parser", type=Path)
    parser.add_argument("--base-config", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--case")
    args = parser.parse_args()
    BatchTests.parser_path = args.parser.resolve()
    BatchTests.baseline = args.baseline_parser.resolve() if args.baseline_parser else None
    BatchTests.base_config = args.base_config
    BatchTests.artifacts = args.artifacts.resolve()
    suite = unittest.TestSuite([BatchTests(args.case)]) if args.case else unittest.defaultTestLoader.loadTestsFromTestCase(BatchTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    write_text(args.artifacts / "test-results.json", json.dumps({"tests": result.testsRun,
        "failures": len(result.failures), "errors": len(result.errors), "success": result.wasSuccessful()}, indent=2))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
