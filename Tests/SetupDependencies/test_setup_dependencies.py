import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_DIR = REPO_ROOT / "Tools" / "SetupDependencies"
sys.path.insert(0, str(TOOLS_DIR))

import setup_dependencies as setup  # noqa: E402


def write_valid_version_header(root: Path) -> None:
    header = root / "include" / "fbxsdk" / "fbxsdk_version.h"
    header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text(
        "\n".join(
            [
                "#define FBXSDK_VERSION_MAJOR 2020",
                "#define FBXSDK_VERSION_MINOR 3",
                "#define FBXSDK_VERSION_POINT 9",
            ]
        ),
        encoding="utf-8",
    )


def write_valid_windows_sdk(root: Path, arch: str = "x64") -> None:
    (root / "include").mkdir(parents=True, exist_ok=True)
    (root / "include" / "fbxsdk.h").write_text("// sdk", encoding="utf-8")
    write_valid_version_header(root)
    library_dir = root / "lib" / arch / "release"
    library_dir.mkdir(parents=True, exist_ok=True)
    (library_dir / "libfbxsdk.lib").write_bytes(b"lib")
    (library_dir / "libfbxsdk.dll").write_bytes(b"dll")


def write_valid_linux_sdk(root: Path) -> None:
    (root / "include").mkdir(parents=True, exist_ok=True)
    (root / "include" / "fbxsdk.h").write_text("// sdk", encoding="utf-8")
    write_valid_version_header(root)
    library_dir = root / "lib" / "gcc" / "x64" / "release"
    library_dir.mkdir(parents=True, exist_ok=True)
    (library_dir / "libfbxsdk.so").write_bytes(b"so")


def write_valid_macos_sdk(root: Path) -> None:
    (root / "include").mkdir(parents=True, exist_ok=True)
    (root / "include" / "fbxsdk.h").write_text("// sdk", encoding="utf-8")
    write_valid_version_header(root)
    library_dir = root / "lib" / "clang" / "release"
    library_dir.mkdir(parents=True, exist_ok=True)
    (library_dir / "libfbxsdk.dylib").write_bytes(b"dylib")


class SetupDependenciesTests(unittest.TestCase):
    def test_manifest_contains_all_supported_platforms(self):
        manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
        package = setup.resolve_package(manifest, "autodesk-fbx-sdk", None)

        self.assertEqual(package.dependency_id, "autodesk-fbx-sdk")
        self.assertEqual(
            set(manifest["dependencies"]["autodesk-fbx-sdk"]["platforms"].keys()),
            {"windows", "linux", "macos"},
        )
        for platform in ("windows", "linux", "macos"):
            platform_data = manifest["dependencies"]["autodesk-fbx-sdk"]["platforms"][platform]
            self.assertTrue(platform_data["url"].startswith("https://damassets.autodesk.net/"))
            self.assertRegex(platform_data["sha256"], r"^[0-9A-F]{64}$")

    def test_eula_environment_accepts_only_explicit_truthy_values(self):
        accepted = ("1", "true", "TRUE", "yes", "on")
        rejected = ("", "0", "false", "FALSE", "no", "off", "random")

        for value in accepted:
            with self.subTest(value=value):
                self.assertTrue(setup.env_accepts_eula({"NLS_ACCEPT_AUTODESK_FBX_EULA": value}))

        for value in rejected:
            with self.subTest(value=value):
                self.assertFalse(setup.env_accepts_eula({"NLS_ACCEPT_AUTODESK_FBX_EULA": value}))

    def test_non_interactive_missing_eula_fails_before_download(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            stdout = io.StringIO()
            stderr = io.StringIO()
            code = setup.main(
                [
                    "--repo-root",
                    temp_dir,
                    "--platform",
                    "windows",
                    "--non-interactive",
                    "--dry-run",
                ],
                environ={},
                stdout=stdout,
                stderr=stderr,
            )

            self.assertEqual(code, 1)
            self.assertIn("--accept-autodesk-eula", stderr.getvalue())
            self.assertIn("NLS_ACCEPT_AUTODESK_FBX_EULA=1", stderr.getvalue())
            self.assertFalse((Path(temp_dir) / "ThirdParty").exists())

    def test_explicit_empty_environment_does_not_fall_back_to_process_environment(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            old_value = os.environ.get("NLS_ACCEPT_AUTODESK_FBX_EULA")
            os.environ["NLS_ACCEPT_AUTODESK_FBX_EULA"] = "1"
            try:
                stderr = io.StringIO()
                code = setup.main(
                    [
                        "--repo-root",
                        temp_dir,
                        "--platform",
                        "windows",
                        "--non-interactive",
                        "--dry-run",
                    ],
                    environ={},
                    stdout=io.StringIO(),
                    stderr=stderr,
                )
            finally:
                if old_value is None:
                    os.environ.pop("NLS_ACCEPT_AUTODESK_FBX_EULA", None)
                else:
                    os.environ["NLS_ACCEPT_AUTODESK_FBX_EULA"] = old_value

            self.assertEqual(code, 1)
            self.assertIn("EULA acceptance is required", stderr.getvalue())

    def test_non_interactive_env_acceptance_allows_dry_run_without_writes(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            stdout = io.StringIO()
            stderr = io.StringIO()
            code = setup.main(
                [
                    "--repo-root",
                    temp_dir,
                    "--platform",
                    "windows",
                    "--non-interactive",
                    "--dry-run",
                ],
                environ={"NLS_ACCEPT_AUTODESK_FBX_EULA": "1"},
                stdout=stdout,
                stderr=stderr,
            )

            self.assertEqual(code, 0, stderr.getvalue())
            self.assertIn("Dry run", stdout.getvalue())
            self.assertIn("No files were modified", stdout.getvalue())
            self.assertFalse((Path(temp_dir) / "ThirdParty").exists())

    def test_interactive_decline_fails_before_download(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            stdout = io.StringIO()
            stderr = io.StringIO()
            code = setup.main(
                [
                    "--repo-root",
                    temp_dir,
                    "--platform",
                    "windows",
                    "--dry-run",
                ],
                environ={},
                input_func=lambda _prompt: "no",
                stdout=stdout,
                stderr=stderr,
            )

            self.assertEqual(code, 1)
            self.assertIn("EULA was not accepted", stderr.getvalue())
            self.assertFalse((Path(temp_dir) / "ThirdParty").exists())

    def test_valid_existing_sdk_root_is_idempotent(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            sdk_root = root / "ThirdParty" / "FBX" / "sdk" / "windows"
            write_valid_windows_sdk(sdk_root)

            stdout = io.StringIO()
            stderr = io.StringIO()
            code = setup.main(
                [
                    "--repo-root",
                    temp_dir,
                    "--platform",
                    "windows",
                    "--non-interactive",
                ],
                environ={"NLS_ACCEPT_AUTODESK_FBX_EULA": "1"},
                stdout=stdout,
                stderr=stderr,
            )

            self.assertEqual(code, 0, stderr.getvalue())
            self.assertIn("already valid", stdout.getvalue())
            self.assertFalse((root / "ThirdParty" / "FBX" / "packages").exists())

    def test_sdk_validation_reports_missing_header(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
            package = setup.resolve_package(manifest, "autodesk-fbx-sdk", "windows")

            with self.assertRaisesRegex(setup.SetupError, "include/fbxsdk.h"):
                setup.validate_sdk_root(Path(temp_dir), package)

    def test_sdk_root_rejects_symlinked_platform_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            outside = root / "outside-sdk-target"
            outside.mkdir()
            sdk_parent = root / "ThirdParty" / "FBX" / "sdk"
            sdk_parent.mkdir(parents=True)
            symlink = sdk_parent / "windows"
            try:
                symlink.symlink_to(outside, target_is_directory=True)
            except (NotImplementedError, OSError) as exc:
                self.skipTest(f"Directory symlinks are unavailable on this host: {exc}")

            manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
            package = setup.resolve_package(manifest, "autodesk-fbx-sdk", "windows")

            with self.assertRaisesRegex(setup.SetupError, "symbolic link"):
                setup.resolve_sdk_root(root, package)

    def test_sdk_root_rejects_symlinked_parent_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            outside = root / "outside-sdk-parent"
            outside.mkdir()
            fbx_root = root / "ThirdParty" / "FBX"
            fbx_root.mkdir(parents=True)
            symlink = fbx_root / "sdk"
            try:
                symlink.symlink_to(outside, target_is_directory=True)
            except (NotImplementedError, OSError) as exc:
                self.skipTest(f"Directory symlinks are unavailable on this host: {exc}")

            manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
            package = setup.resolve_package(manifest, "autodesk-fbx-sdk", "windows")

            with self.assertRaisesRegex(setup.SetupError, "symbolic link"):
                setup.resolve_sdk_root(root, package)

    def test_sdk_root_rejects_symlinked_third_party_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            outside = root / "outside-third-party"
            outside.mkdir()
            symlink = root / "ThirdParty"
            try:
                symlink.symlink_to(outside, target_is_directory=True)
            except (NotImplementedError, OSError) as exc:
                self.skipTest(f"Directory symlinks are unavailable on this host: {exc}")

            manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
            package = setup.resolve_package(manifest, "autodesk-fbx-sdk", "windows")

            with self.assertRaisesRegex(setup.SetupError, "symbolic link"):
                setup.resolve_sdk_root(root, package)

    def test_setup_stops_before_package_cache_when_third_party_is_symlinked(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            outside = root / "outside-third-party"
            outside.mkdir()
            symlink = root / "ThirdParty"
            try:
                symlink.symlink_to(outside, target_is_directory=True)
            except (NotImplementedError, OSError) as exc:
                self.skipTest(f"Directory symlinks are unavailable on this host: {exc}")

            calls = []
            original_prepare = setup.prepare_package

            def fake_prepare(_package, package_path, _stdout):
                calls.append(package_path)
                return package_path

            setup.prepare_package = fake_prepare
            try:
                stderr = io.StringIO()
                code = setup.main(
                    [
                        "--repo-root",
                        temp_dir,
                        "--platform",
                        "windows",
                        "--accept-autodesk-eula",
                    ],
                    environ={},
                    stdout=io.StringIO(),
                    stderr=stderr,
                )
            finally:
                setup.prepare_package = original_prepare

            self.assertEqual(code, 1)
            self.assertEqual(calls, [])
            self.assertIn("symbolic link", stderr.getvalue())

    def test_sdk_root_rejects_windows_reparse_point_parent(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "ThirdParty" / "FBX" / "sdk").mkdir(parents=True)
            manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
            package = setup.resolve_package(manifest, "autodesk-fbx-sdk", "windows")
            original = setup._is_windows_reparse_point

            def fake_reparse_point(path):
                return path.name == "sdk"

            setup._is_windows_reparse_point = fake_reparse_point
            try:
                with self.assertRaisesRegex(setup.SetupError, "reparse point"):
                    setup.resolve_sdk_root(root, package)
            finally:
                setup._is_windows_reparse_point = original

    def test_sdk_root_rejects_mount_point_platform_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "ThirdParty" / "FBX" / "sdk" / "windows").mkdir(parents=True)
            manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
            package = setup.resolve_package(manifest, "autodesk-fbx-sdk", "windows")
            original = setup._is_mount_point

            def fake_mount_point(path):
                return path.name == "windows"

            setup._is_mount_point = fake_mount_point
            try:
                with self.assertRaisesRegex(setup.SetupError, "mount point"):
                    setup.resolve_sdk_root(root, package)
            finally:
                setup._is_mount_point = original

    def test_windows_sdk_validation_rejects_debug_only_layout(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            sdk_root = root / "ThirdParty" / "FBX" / "sdk" / "windows"
            (sdk_root / "include").mkdir(parents=True, exist_ok=True)
            (sdk_root / "include" / "fbxsdk.h").write_text("// sdk", encoding="utf-8")
            write_valid_version_header(sdk_root)
            (sdk_root / "lib" / "x64" / "debug").mkdir(parents=True, exist_ok=True)
            (sdk_root / "lib" / "x64" / "debug" / "libfbxsdk.lib").write_bytes(b"lib")
            (sdk_root / "lib" / "x64" / "debug" / "libfbxsdk.dll").write_bytes(b"dll")

            manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
            package = setup.resolve_package(manifest, "autodesk-fbx-sdk", "windows")

            with self.assertRaisesRegex(setup.SetupError, "lib/x64/release/libfbxsdk.lib"):
                setup.validate_sdk_root(root, package)

    def test_windows_sdk_validation_accepts_x64_release_only_layout(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_valid_windows_sdk(root / "ThirdParty" / "FBX" / "sdk" / "windows")
            manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
            package = setup.resolve_package(manifest, "autodesk-fbx-sdk", "windows", "x64")

            validated = setup.validate_sdk_root(root, package)

            self.assertEqual(validated, root / "ThirdParty" / "FBX" / "sdk" / "windows")

    def test_windows_sdk_validation_rejects_arm64_release_layout_when_x64_requested(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_valid_windows_sdk(root / "ThirdParty" / "FBX" / "sdk" / "windows", "arm64")
            manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
            package = setup.resolve_package(manifest, "autodesk-fbx-sdk", "windows", "x64")

            with self.assertRaisesRegex(setup.SetupError, "lib/x64/release/libfbxsdk.lib"):
                setup.validate_sdk_root(root, package)

    def test_windows_sdk_validation_accepts_arm64_release_layout_when_arm64_requested(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_valid_windows_sdk(root / "ThirdParty" / "FBX" / "sdk" / "windows", "arm64")
            manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
            package = setup.resolve_package(manifest, "autodesk-fbx-sdk", "windows", "arm64")

            validated = setup.validate_sdk_root(root, package)

            self.assertEqual(validated, root / "ThirdParty" / "FBX" / "sdk" / "windows")

    def test_dry_run_reports_selected_windows_architecture(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            stdout = io.StringIO()
            stderr = io.StringIO()
            code = setup.main(
                [
                    "--repo-root",
                    temp_dir,
                    "--platform",
                    "windows",
                    "--arch",
                    "arm64",
                    "--accept-autodesk-eula",
                    "--dry-run",
                ],
                environ={},
                stdout=stdout,
                stderr=stderr,
            )

            self.assertEqual(code, 0, stderr.getvalue())
            self.assertIn("windows (arm64)", stdout.getvalue())

    def test_windows_architecture_option_accepts_common_uppercase_spelling(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_valid_windows_sdk(root / "ThirdParty" / "FBX" / "sdk" / "windows", "arm64")
            stdout = io.StringIO()
            stderr = io.StringIO()
            code = setup.main(
                [
                    "--repo-root",
                    temp_dir,
                    "--platform",
                    "windows",
                    "--arch",
                    "ARM64",
                    "--validate-only",
                ],
                environ={},
                stdout=stdout,
                stderr=stderr,
            )

            self.assertEqual(code, 0, stderr.getvalue())
            self.assertIn("SDK root", stdout.getvalue())
            self.assertIn(str(root / "ThirdParty" / "FBX" / "sdk" / "windows"), stdout.getvalue())

    def test_windows_architecture_option_is_rejected_for_non_windows_platforms(self):
        manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")

        with self.assertRaisesRegex(setup.SetupError, "--arch is only supported"):
            setup.resolve_package(manifest, "autodesk-fbx-sdk", "linux", "x64")

    def test_manifest_sdk_root_must_stay_under_bundled_fbx_sdk_root(self):
        manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
        manifest = json.loads(json.dumps(manifest))
        manifest["dependencies"]["autodesk-fbx-sdk"]["platforms"]["windows"]["sdkRoot"] = "../../escape"

        with self.assertRaisesRegex(setup.SetupError, "ThirdParty/FBX/sdk/windows"):
            setup.resolve_package(manifest, "autodesk-fbx-sdk", "windows")

    def test_manifest_url_must_use_official_autodesk_host(self):
        manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
        manifest = json.loads(json.dumps(manifest))
        manifest["dependencies"]["autodesk-fbx-sdk"]["platforms"]["windows"]["url"] = (
            "https://example.com/fbx202039_fbxsdk_vs2022_win.exe"
        )

        with self.assertRaisesRegex(setup.SetupError, "damassets.autodesk.net"):
            setup.resolve_package(manifest, "autodesk-fbx-sdk", "windows")

    def test_manifest_file_name_must_be_a_plain_package_name(self):
        manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
        manifest = json.loads(json.dumps(manifest))
        manifest["dependencies"]["autodesk-fbx-sdk"]["platforms"]["windows"]["fileName"] = "../fbx.exe"

        with self.assertRaisesRegex(setup.SetupError, "fileName"):
            setup.resolve_package(manifest, "autodesk-fbx-sdk", "windows")

    def test_manifest_installer_entry_must_be_relative_package_entry(self):
        manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
        manifest = json.loads(json.dumps(manifest))
        manifest["dependencies"]["autodesk-fbx-sdk"]["platforms"]["linux"]["installerEntry"] = "../../installer"

        with self.assertRaisesRegex(setup.SetupError, "installerEntry"):
            setup.resolve_package(manifest, "autodesk-fbx-sdk", "linux")

    def test_manifest_sha256_must_be_uppercase_hash(self):
        manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
        manifest = json.loads(json.dumps(manifest))
        manifest["dependencies"]["autodesk-fbx-sdk"]["platforms"]["windows"]["sha256"] = "not-a-hash"

        with self.assertRaisesRegex(setup.SetupError, "SHA256"):
            setup.resolve_package(manifest, "autodesk-fbx-sdk", "windows")

    def test_manifest_missing_required_field_reports_setup_error(self):
        manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
        manifest = json.loads(json.dumps(manifest))
        del manifest["dependencies"]["autodesk-fbx-sdk"]["platforms"]["windows"]["url"]

        with self.assertRaisesRegex(setup.SetupError, "url"):
            setup.resolve_package(manifest, "autodesk-fbx-sdk", "windows")

    def test_manifest_missing_validation_reports_setup_error(self):
        manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
        manifest = json.loads(json.dumps(manifest))
        del manifest["dependencies"]["autodesk-fbx-sdk"]["platforms"]["windows"]["validation"]

        with self.assertRaisesRegex(setup.SetupError, "validation"):
            setup.resolve_package(manifest, "autodesk-fbx-sdk", "windows")

    def test_hash_mismatch_reports_expected_and_actual(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            package = Path(temp_dir) / "package.bin"
            package.write_bytes(b"not the sdk")
            actual = hashlib.sha256(b"not the sdk").hexdigest().upper()

            with self.assertRaisesRegex(setup.SetupError, actual):
                setup.verify_package_hash(package, "0" * 64)

    def test_downloaded_package_hash_mismatch_removes_bad_cache_file(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            package_path = root / "ThirdParty" / "FBX" / "packages" / "fbx202039_fbxsdk_vs2022_win.exe"
            stdout = io.StringIO()

            def fake_download(_package, destination, _stdout):
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(b"bad package")

            manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
            package = setup.resolve_package(manifest, "autodesk-fbx-sdk", "windows")

            with self.assertRaisesRegex(setup.SetupError, "Package hash mismatch"):
                setup.prepare_package(package, package_path, stdout, fake_download)

            self.assertFalse(package_path.exists())

    def test_download_package_uses_unique_temporary_files(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
            package = setup.resolve_package(manifest, "autodesk-fbx-sdk", "windows")
            destination = Path(temp_dir) / package.file_name
            opened_paths = []
            original_urlopen = setup.urllib.request.urlopen
            original_copyfileobj = setup.shutil.copyfileobj

            class Response:
                def __enter__(self):
                    return io.BytesIO(b"payload")

                def __exit__(self, _exc_type, _exc, _tb):
                    return False

            def fake_urlopen(_url, timeout):
                return Response()

            def fake_copyfileobj(response, output):
                opened_paths.append(Path(output.name))
                output.write(response.read())

            setup.urllib.request.urlopen = fake_urlopen
            setup.shutil.copyfileobj = fake_copyfileobj
            try:
                setup.download_package(package, destination, io.StringIO())
                destination.unlink()
                setup.download_package(package, destination, io.StringIO())
            finally:
                setup.urllib.request.urlopen = original_urlopen
                setup.shutil.copyfileobj = original_copyfileobj

            self.assertEqual(len(opened_paths), 2)
            self.assertNotEqual(opened_paths[0], opened_paths[1])
            self.assertTrue(all(path.parent == destination.parent for path in opened_paths))
            self.assertTrue(all(not path.exists() for path in opened_paths))

    def test_download_package_retries_transient_network_failure(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
            package = setup.resolve_package(manifest, "autodesk-fbx-sdk", "windows")
            destination = Path(temp_dir) / package.file_name
            calls = []
            original_urlopen = setup.urllib.request.urlopen

            class Response:
                def __enter__(self):
                    return io.BytesIO(b"payload")

                def __exit__(self, _exc_type, _exc, _tb):
                    return False

            def fake_urlopen(_url, timeout):
                calls.append(timeout)
                if len(calls) == 1:
                    raise TimeoutError("temporary timeout")
                return Response()

            setup.urllib.request.urlopen = fake_urlopen
            try:
                setup.download_package(package, destination, io.StringIO())
            finally:
                setup.urllib.request.urlopen = original_urlopen

            self.assertEqual(calls, [setup.DOWNLOAD_TIMEOUT_SECONDS, setup.DOWNLOAD_TIMEOUT_SECONDS])
            self.assertEqual(destination.read_bytes(), b"payload")
            self.assertEqual(list(destination.parent.glob("*.download")), [])

    def test_tar_extraction_rejects_path_traversal(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            tar_path = Path(temp_dir) / "unsafe.tar"
            with tarfile.open(tar_path, "w") as archive:
                info = tarfile.TarInfo("../../evil.txt")
                info.size = 0
                archive.addfile(info)

            with tarfile.open(tar_path, "r") as archive:
                with self.assertRaisesRegex(setup.SetupError, "unsafe archive member"):
                    setup.safe_extract_tar(archive, Path(temp_dir) / "out")

    def test_tar_extraction_rejects_links(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            tar_path = Path(temp_dir) / "link.tar"
            with tarfile.open(tar_path, "w") as archive:
                info = tarfile.TarInfo("link")
                info.type = tarfile.SYMTYPE
                info.linkname = "/tmp/outside"
                archive.addfile(info)

            with tarfile.open(tar_path, "r") as archive:
                with self.assertRaisesRegex(setup.SetupError, "unsafe archive member type"):
                    setup.safe_extract_tar(archive, Path(temp_dir) / "out")

    def test_macos_package_does_not_fallback_to_unchecked_payload_tar(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            package_path = temp / "fbx202039_fbxsdk_clang_mac.pkg.tgz"
            pkg_name = "fbx202039_fbxsdk_clang_macos.pkg"
            with tarfile.open(package_path, "w:gz") as archive:
                info = tarfile.TarInfo(pkg_name)
                info.size = 0
                archive.addfile(info)

            manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
            package = setup.resolve_package(manifest, "autodesk-fbx-sdk", "macos")
            calls = []
            original_run = setup.subprocess.run

            def fake_run(command, *args, **kwargs):
                calls.append(command)
                raise subprocess.CalledProcessError(1, command)

            setup.subprocess.run = fake_run
            try:
                with self.assertRaisesRegex(setup.SetupError, "pkgutil --expand-full"):
                    setup.install_package(package, package_path, temp, io.StringIO())
            finally:
                setup.subprocess.run = original_run

            self.assertEqual(len(calls), 1)

    def test_linux_install_dispatch_invokes_installer_with_timeout(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            package_path = temp / "fbx202039_fbxsdk_gcc_linux.tar.gz"
            installer_name = "fbx202039_fbxsdk_linux"
            with tarfile.open(package_path, "w:gz") as archive:
                data = b"#!/bin/sh\n"
                info = tarfile.TarInfo(installer_name)
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))

            manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
            package = setup.resolve_package(manifest, "autodesk-fbx-sdk", "linux")
            calls = []
            original_run = setup.subprocess.run

            def fake_run(command, *args, **kwargs):
                calls.append((command, kwargs))
                write_valid_linux_sdk(temp / "ThirdParty" / "FBX" / "sdk" / "linux")
                return subprocess.CompletedProcess(command, 0)

            setup.subprocess.run = fake_run
            try:
                setup.install_package(package, package_path, temp, io.StringIO())
            finally:
                setup.subprocess.run = original_run

            self.assertEqual(len(calls), 1)
            self.assertEqual(calls[0][1]["timeout"], setup.INSTALLER_TIMEOUT_SECONDS)

    def test_macos_install_copies_expanded_sdk_and_uses_timeout(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            package_path = temp / "fbx202039_fbxsdk_clang_mac.pkg.tgz"
            pkg_name = "fbx202039_fbxsdk_clang_macos.pkg"
            with tarfile.open(package_path, "w:gz") as archive:
                info = tarfile.TarInfo(pkg_name)
                info.size = 0
                archive.addfile(info)

            manifest = setup.load_manifest(TOOLS_DIR / "dependency_manifest.json")
            package = setup.resolve_package(manifest, "autodesk-fbx-sdk", "macos")
            calls = []
            original_run = setup.subprocess.run

            def fake_run(command, *args, **kwargs):
                calls.append((command, kwargs))
                expanded = Path(command[-1])
                write_valid_macos_sdk(expanded / "sdk-root")
                return subprocess.CompletedProcess(command, 0)

            setup.subprocess.run = fake_run
            try:
                setup.install_package(package, package_path, temp, io.StringIO())
            finally:
                setup.subprocess.run = original_run

            self.assertEqual(len(calls), 1)
            self.assertEqual(calls[0][1]["timeout"], setup.INSTALLER_TIMEOUT_SECONDS)
            setup.validate_sdk_root(temp, package)

    def test_docs_and_manifest_mention_supported_platforms(self):
        readme = (REPO_ROOT / "ThirdParty" / "FBX" / "README.md").read_text(encoding="utf-8")
        manifest = json.loads((TOOLS_DIR / "dependency_manifest.json").read_text(encoding="utf-8"))

        for platform in ("windows", "linux", "macos"):
            with self.subTest(platform=platform):
                self.assertIn(platform, readme.lower())
                self.assertIn(platform, manifest["dependencies"]["autodesk-fbx-sdk"]["platforms"])

    def test_fbx_readme_package_table_matches_manifest_urls_and_hashes(self):
        readme = (REPO_ROOT / "ThirdParty" / "FBX" / "README.md").read_text(encoding="utf-8")
        manifest = json.loads((TOOLS_DIR / "dependency_manifest.json").read_text(encoding="utf-8"))
        platforms = manifest["dependencies"]["autodesk-fbx-sdk"]["platforms"]

        for data in platforms.values():
            with self.subTest(package=data["fileName"]):
                self.assertIn(data["url"], readme)
                self.assertIn(f"`packages/{data['fileName']}`", readme)
                self.assertIn(data["sha256"], readme)

    def test_cmake_missing_sdk_guidance_mentions_setup_dependencies(self):
        cmake = (REPO_ROOT / "ThirdParty" / "CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn("SetupDependencies", cmake)
        self.assertIn("ThirdParty/FBX/sdk/<platform>", cmake)

    def test_cmake_strict_require_failure_mentions_setup_dependencies(self):
        cmake = (REPO_ROOT / "ThirdParty" / "CMakeLists.txt").read_text(encoding="utf-8")
        strict_start = cmake.index("if(NLS_REQUIRE_BUNDLED_FBX_SDK)")
        strict_end = cmake.index("endif()", strict_start)
        strict_block = cmake[strict_start:strict_end]

        self.assertIn("Run SetupDependencies", cmake)
        self.assertIn("FATAL_ERROR", strict_block)
        self.assertIn("_fbx_setup_guidance", strict_block)

    def test_cmake_debug_runtime_falls_back_to_release_runtime(self):
        cmake = (REPO_ROOT / "ThirdParty" / "CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn('if(NOT EXISTS "${_fbx_debug_runtime_library}")', cmake)
        self.assertIn('set(_fbx_debug_runtime_library "${_fbx_runtime_library}")', cmake)

    def test_posix_wrapper_uses_portable_dirname_invocation(self):
        wrapper = (REPO_ROOT / "SetupDependencies.sh").read_text(encoding="utf-8")

        self.assertNotIn("dirname --", wrapper)
        self.assertNotIn("cd --", wrapper)
        self.assertIn('dirname "$0"', wrapper)

    def test_windows_wrapper_quotes_script_dir_assignment(self):
        wrapper = (REPO_ROOT / "SetupDependencies.bat").read_text(encoding="utf-8")

        self.assertIn('set "SCRIPT_DIR=%~dp0"', wrapper)
        self.assertNotIn("set SCRIPT_DIR=%~dp0", wrapper)


if __name__ == "__main__":
    unittest.main()
