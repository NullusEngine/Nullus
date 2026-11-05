"""Validate shared parsing, configuration isolation, and content-based invalidation."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time
import unittest

from validate_parallel import ParserFixture, write_text


class SharedCacheTests(unittest.TestCase):
    parser_path = None
    base_config = None
    artifacts = None

    def setUp(self):
        root = self.artifacts / self._testMethodName / str(time.monotonic_ns())
        self.fixture = ParserFixture(self.parser_path, self.base_config, root, count=4)
        self.fixture.config.update(UseCache=True, CacheDirectory=str(root / "Cache"),
                                   VisibilityDefines=[], TotalParseJobs=2)
        for variable in ("NLS_META_PARSER_CACHE", "NLS_META_PARSER_CACHE_DIR", "NLS_META_PARSER_TOTAL_JOBS"):
            self.fixture.environment.pop(variable, None)

    def run_parser(self, name, *, extra=None, mode="threads", fixture=None):
        fixture = fixture or self.fixture
        result, elapsed = fixture.run(mode, 4, name, extra=extra)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        match = re.search(r"CacheHits=(\d+), CacheMisses=(\d+), SharedCacheHits=(\d+)", result.stdout)
        self.assertIsNotNone(match, result.stdout)
        return tuple(map(int, match.groups())), elapsed

    def target_defines(self, macro):
        return self.fixture.config["Defines"] + [macro]

    def test_cold_warm_and_cross_target_reuse(self):
        first = dict(Defines=self.target_defines("NLS_ALPHA_EXPORT"), VisibilityDefines=["NLS_ALPHA_EXPORT"])
        stats, _ = self.run_parser("cold", extra=first)
        self.assertEqual(stats[:2], (0, 5))
        before = self.fixture.snapshot()
        stats, _ = self.run_parser("warm", extra=first)
        self.assertEqual(stats[:2], (5, 0))
        self.assertEqual(before, self.fixture.snapshot())
        second = dict(Defines=self.target_defines("NLS_BETA_EXPORT"), VisibilityDefines=["NLS_BETA_EXPORT"],
                      TargetName="OtherTarget", ModuleName="OtherTarget")
        stats, _ = self.run_parser("other-target", extra=second)
        self.assertEqual(stats, (5, 0, 5))
        expected = self.fixture.snapshot()
        self.run_parser("other-target-reference", extra=dict(second, UseCache=False))
        self.assertEqual(expected, self.fixture.snapshot())

    def test_visibility_defines_are_verified(self):
        visibility = self.fixture.root / "Runtime/Fixture/Visibility.h"
        write_text(visibility, '''#pragma once
#ifdef NLS_ALPHA_EXPORT
#define FIXTURE_API __declspec(dllexport)
#else
#define FIXTURE_API __declspec(dllimport)
#endif
''')
        header = Path(self.fixture.headers[0])
        text = header.read_text(encoding="utf-8").replace('#include "Foreign.h"',
            '#include "Foreign.h"\n#include "Visibility.h"').replace("STRUCT(Item00)", "STRUCT(FIXTURE_API Item00)")
        write_text(header, text)
        first = dict(Defines=self.target_defines("NLS_ALPHA_EXPORT"), VisibilityDefines=["NLS_ALPHA_EXPORT"])
        second = dict(Defines=self.target_defines("NLS_BETA_EXPORT"), VisibilityDefines=["NLS_BETA_EXPORT"])
        self.run_parser("export", extra=first)
        stats, _ = self.run_parser("import", extra=second)
        self.assertEqual(stats, (5, 0, 5))
        expected = self.fixture.snapshot()
        self.run_parser("import-reference", extra=dict(second, UseCache=False))
        self.assertEqual(expected, self.fixture.snapshot())

    def test_export_macro_that_changes_fields_is_not_shared(self):
        header = Path(self.fixture.headers[0])
        write_text(header, header.read_text(encoding="utf-8").replace("    GENERATED_BODY()", '''    GENERATED_BODY()
#ifdef NLS_ALPHA_EXPORT
    PROPERTY() int alphaOnly;
#endif'''))
        first = dict(Defines=self.target_defines("NLS_ALPHA_EXPORT"), VisibilityDefines=["NLS_ALPHA_EXPORT"])
        second = dict(Defines=self.target_defines("NLS_BETA_EXPORT"), VisibilityDefines=["NLS_BETA_EXPORT"])
        self.run_parser("alpha", extra=first)
        self.assertIn('"alphaOnly"', (self.fixture.output / "Item00.generated.cpp").read_text(encoding="utf-8"))
        stats, _ = self.run_parser("beta", extra=second)
        self.assertEqual(stats, (4, 1, 4))
        self.assertNotIn('"alphaOnly"', (self.fixture.output / "Item00.generated.cpp").read_text(encoding="utf-8"))
        self.run_parser("alpha-again", extra=first)
        self.assertIn('"alphaOnly"', (self.fixture.output / "Item00.generated.cpp").read_text(encoding="utf-8"))

    def test_feature_macros_and_compiler_options_have_separate_contexts(self):
        self.run_parser("first")
        stats, _ = self.run_parser("feature", extra={"Defines": self.fixture.config["Defines"] + ["FEATURE=1"]})
        self.assertEqual(stats[:2], (0, 5))
        stats, _ = self.run_parser("flags", extra={
            "CompilerOptions": self.fixture.config["CompilerOptions"] + ["-Wno-unused-command-line-argument"],
        })
        self.assertEqual(stats[:2], (0, 5))

    def test_transitive_content_change_even_with_preserved_timestamp(self):
        foreign = self.fixture.root / "Runtime/Fixture/Foreign.h"
        write_text(foreign, foreign.read_text(encoding="utf-8") + "\n#define FIELD_SCALAR int \n")
        header = Path(self.fixture.headers[0])
        write_text(header, header.read_text(encoding="utf-8").replace("    GENERATED_BODY()",
            "    GENERATED_BODY()\n    PROPERTY() FIELD_SCALAR scalar;"))
        self.run_parser("int")
        stat = foreign.stat()
        write_text(foreign, foreign.read_text(encoding="utf-8").replace("FIELD_SCALAR int ", "FIELD_SCALAR bool"))
        self.assertEqual(stat.st_size, foreign.stat().st_size)
        os.utime(foreign, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        stats, _ = self.run_parser("bool")
        self.assertEqual(stats[:2], (0, 5))
        text = (self.fixture.output / "Item00.generated.cpp").read_text(encoding="utf-8")
        self.assertIn('bool>("scalar"', text)

    def test_single_header_edit_reuses_unaffected_headers(self):
        self.run_parser("before")
        header = Path(self.fixture.headers[0])
        write_text(header, header.read_text(encoding="utf-8").replace("value00", "editedValue"))
        stats, _ = self.run_parser("after")
        self.assertEqual(stats[:2], (4, 1))
        self.assertIn('"editedValue"', (self.fixture.output / "Item00.generated.cpp").read_text(encoding="utf-8"))

    def test_new_optional_include_invalidates_namespace(self):
        header = Path(self.fixture.headers[0])
        write_text(header, header.read_text(encoding="utf-8").replace('#include "Foreign.h"', '''#include "Foreign.h"
#if __has_include("Optional.h")
#include "Optional.h"
#else
#define OPTIONAL_SCALAR int
#endif''').replace("    GENERATED_BODY()", "    GENERATED_BODY()\n    PROPERTY() OPTIONAL_SCALAR optional;"))
        self.run_parser("missing")
        write_text(header.parent / "Optional.h", "#pragma once\n#define OPTIONAL_SCALAR bool\n")
        stats, _ = self.run_parser("present")
        self.assertGreater(stats[1], 0)
        self.assertIn('bool>("optional"', (self.fixture.output / "Item00.generated.cpp").read_text(encoding="utf-8"))

    def test_raw_include_search_flags_bypass_cache(self):
        lower = self.fixture.root / "lower"
        higher = self.fixture.root / "higher"
        write_text(lower / "Shadow.h", "#define SHADOW_SCALAR int\n")
        header = Path(self.fixture.headers[0])
        write_text(header, header.read_text(encoding="utf-8").replace('#include "Foreign.h"',
            '#include "Foreign.h"\n#include "Shadow.h"').replace("    GENERATED_BODY()",
            "    GENERATED_BODY()\n    PROPERTY() SHADOW_SCALAR shadow;"))
        options = {"IncludeDirs": self.fixture.config["IncludeDirs"] + [str(lower)],
                   "CompilerOptions": self.fixture.config["CompilerOptions"] + ["-I" + str(higher)]}
        self.run_parser("lower", extra=options)
        write_text(higher / "Shadow.h", "#define SHADOW_SCALAR bool\n")
        stats, _ = self.run_parser("higher", extra=options)
        self.assertEqual(stats[:2], (0, 5))
        self.assertIn('bool>("shadow"', (self.fixture.output / "Item00.generated.cpp").read_text(encoding="utf-8"))

    def test_corruption_and_schema_change_recover(self):
        self.run_parser("valid")
        expected = self.fixture.snapshot()
        path = next(Path(self.fixture.config["CacheDirectory"]).rglob("*.json"))
        write_text(path, "{broken")
        stats, _ = self.run_parser("corrupt")
        self.assertEqual(stats[:2], (4, 1))
        self.assertEqual(expected, self.fixture.snapshot())
        envelope = json.loads(path.read_text(encoding="utf-8"))
        payload = json.loads(envelope["Payload"])
        payload["Version"] = -1
        envelope["Payload"] = json.dumps(payload, separators=(",", ":"))
        envelope["Checksum"] = hashlib.sha256(envelope["Payload"].encode()).hexdigest().upper()
        write_text(path, json.dumps(envelope))
        stats, _ = self.run_parser("schema")
        self.assertEqual(stats[:2], (4, 1))

    def test_concurrent_targets_coalesce_cold_work(self):
        outputs = [self.fixture.root / f"Gen{index}" for index in range(2)]
        for output in outputs:
            output.mkdir(parents=True)
        with ThreadPoolExecutor(max_workers=2) as executor:
            futures = [executor.submit(self.run_parser, f"parallel-{index}", extra={
                "OutputDir": str(output),
                "Defines": self.target_defines(f"NLS_TARGET_{index}_EXPORT"),
                "VisibilityDefines": [f"NLS_TARGET_{index}_EXPORT"],
            }) for index, output in enumerate(outputs)]
            stats = [future.result()[0] for future in futures]
        self.assertEqual(sum(item[1] for item in stats), 5)
        snapshots = [{str(path.relative_to(output)): path.read_bytes() for path in output.rglob("*") if path.is_file()}
                     for output in outputs]
        self.assertEqual(snapshots[0], snapshots[1])

    def test_process_workers_and_cache_disabled(self):
        self.run_parser("threads")
        expected = self.fixture.snapshot()
        stats, _ = self.run_parser("workers", mode="processes")
        self.assertEqual(stats[:2], (5, 0))
        stats, _ = self.run_parser("disabled", extra={"UseCache": False})
        self.assertEqual(stats[:2], (0, 5))
        self.assertEqual(expected, self.fixture.snapshot())

    def test_global_native_budget_waits_for_other_process(self):
        slot = Path(self.fixture.config["CacheDirectory"]) / "slots/0.lock"
        slot.parent.mkdir(parents=True)
        header = Path(self.fixture.headers[0])
        write_text(header, '#include "Reflection/Macros.h"\nSTRUCT(Broken) { invalid syntax; };\n')
        config = dict(self.fixture.config, ParseMode="threads", ParseJobs=4, TotalParseJobs=1, UseCache=False)
        params = self.fixture.root / "budget.json"
        write_text(params, json.dumps(config))
        if os.name == "nt":
            import ctypes
            from ctypes import wintypes
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                          ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
            kernel.CreateFileW.restype = wintypes.HANDLE
            kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            handle = kernel.CreateFileW(str(slot), 0xC0000000, 0, None, 4, 0x80, None)
            self.assertNotEqual(handle, ctypes.c_void_p(-1).value, ctypes.get_last_error())
            release = lambda: kernel.CloseHandle(handle)
        else:
            import fcntl
            stream = slot.open("w")
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            release = stream.close
        process = subprocess.Popen([str(self.fixture.parser), str(params)], env=self.fixture.environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
        try:
            # With caching disabled, this tiny invalid input normally fails
            # immediately. An occupied native slot must defer that parse.
            time.sleep(3)
            self.assertIsNone(process.poll(), "Native parsing bypassed the occupied global slot")
        finally:
            release()
        try:
            output, errors = process.communicate(timeout=60)
            write_text(self.fixture.root / "budget.log", output + errors)
            self.assertNotEqual(process.returncode, 0)
            self.assertIn("Item00.h", output + errors)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--parser", type=Path, required=True)
    parser.add_argument("--base-config", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--case", help="Run one named test method")
    args = parser.parse_args()
    SharedCacheTests.parser_path = args.parser
    SharedCacheTests.base_config = args.base_config
    SharedCacheTests.artifacts = args.artifacts
    suite = (unittest.TestSuite([SharedCacheTests(args.case)]) if args.case
             else unittest.defaultTestLoader.loadTestsFromTestCase(SharedCacheTests))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    write_text(args.artifacts / "test-results.json", json.dumps({
        "tests": result.testsRun, "failures": len(result.failures), "errors": len(result.errors),
        "success": result.wasSuccessful(),
    }, indent=2))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
