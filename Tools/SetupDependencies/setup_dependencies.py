#!/usr/bin/env python3
"""Prepare source-developer dependencies for Nullus."""

import argparse
import hashlib
import json
import os
import platform
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath
from typing import Callable, Dict, List, Mapping, Optional, Sequence, TextIO
from urllib.parse import urlparse

MIN_PYTHON = (3, 8)
if sys.version_info < MIN_PYTHON:
    print(
        "error: SetupDependencies requires Python 3.8 or newer. "
        f"Detected Python {sys.version_info.major}.{sys.version_info.minor}.",
        file=sys.stderr,
    )
    raise SystemExit(2)

from dataclasses import dataclass


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_MANIFEST = SCRIPT_DIR / "dependency_manifest.json"
ACCEPTED_EULA_VALUES = {"1", "true", "yes", "on"}
DOWNLOAD_RETRY_COUNT = 3
DOWNLOAD_TIMEOUT_SECONDS = 60
INSTALLER_TIMEOUT_SECONDS = 30 * 60
OFFICIAL_AUTODESK_PACKAGE_HOST = "damassets.autodesk.net"
OFFICIAL_DOTNET_PACKAGE_HOST = "builds.dotnet.microsoft.com"
FILE_ATTRIBUTE_REPARSE_POINT = 0x0400


class SetupError(RuntimeError):
    """User-facing setup failure."""


class UnsafeSdkPathError(SetupError):
    """SDK path escapes or redirects away from the repository layout."""


@dataclass(frozen=True)
class PlatformPackage:
    dependency_id: str
    dependency_name: str
    version: str
    platform_key: str
    url: str
    file_name: str
    sha256: str
    sdk_root: Path
    archive_type: str
    installer_entry: Optional[str]
    validation: Dict
    eula: Dict
    arch: Optional[str] = None


@dataclass(frozen=True)
class DotnetPackage:
    dependency_id: str
    dependency_name: str
    version: str
    platform_key: str
    arch: str
    url: str
    file_name: str
    sha512: str
    sdk_root: Path
    archive_type: str
    validation: Dict


def find_repo_root(start: Optional[Path] = None) -> Path:
    current = (start or SCRIPT_DIR).resolve()
    for path in (current, *current.parents):
        if (path / "CMakeLists.txt").exists() and (path / "ThirdParty").is_dir():
            return path
    return SCRIPT_DIR.parents[1]


def load_manifest(path: Path = DEFAULT_MANIFEST) -> Dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise SetupError(f"Dependency manifest not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise SetupError(f"Dependency manifest is invalid JSON: {path}: {exc}") from exc


def detect_platform() -> str:
    system = platform.system().lower()
    if system == "windows":
        return "windows"
    if system == "linux":
        return "linux"
    if system == "darwin":
        return "macos"
    raise SetupError(f"Unsupported host platform '{platform.system()}'. Supported platforms: windows, linux, macos")


def resolve_package(
    manifest: Mapping,
    dependency_id: str,
    platform_key: Optional[str],
    arch: Optional[str] = None,
) -> PlatformPackage:
    if dependency_id == "dotnet-sdk":
        return resolve_dotnet_package(manifest, platform_key, arch)  # type: ignore[return-value]

    dependencies = manifest.get("dependencies", {})
    dependency = dependencies.get(dependency_id)
    if not dependency:
        known = ", ".join(sorted(dependencies)) or "<none>"
        raise SetupError(f"Unknown dependency '{dependency_id}'. Known dependencies: {known}")

    selected_platform = platform_key or detect_platform()
    platforms = dependency.get("platforms", {})
    data = platforms.get(selected_platform)
    if not data:
        known = ", ".join(sorted(platforms)) or "<none>"
        raise SetupError(f"Unsupported platform '{selected_platform}'. Supported platforms: {known}")

    selected_arch = None
    if selected_platform == "windows":
        selected_arch = _validate_windows_arch(arch or detect_windows_arch())
    elif arch:
        raise SetupError("--arch is only supported for the Windows FBX SDK package.")

    url = _validated_manifest_url(_required_manifest_field(data, "url", selected_platform), selected_platform)
    file_name = _validated_manifest_file_name(_required_manifest_field(data, "fileName", selected_platform), selected_platform)
    sha256 = _validated_manifest_sha256(_required_manifest_field(data, "sha256", selected_platform), selected_platform)
    installer_entry = _validated_manifest_installer_entry(data.get("installerEntry"), selected_platform)
    sdk_root = _validated_manifest_sdk_root(_required_manifest_field(data, "sdkRoot", selected_platform), selected_platform)

    return PlatformPackage(
        dependency_id=dependency_id,
        dependency_name=str(dependency.get("name", dependency_id)),
        version=str(dependency.get("version", "")),
        platform_key=selected_platform,
        url=url,
        file_name=file_name,
        sha256=sha256,
        sdk_root=sdk_root,
        archive_type=_required_manifest_field(data, "archiveType", selected_platform),
        installer_entry=installer_entry,
        validation=dict(_required_manifest_mapping(data, "validation", selected_platform)),
        eula=dict(dependency.get("eula", {})),
        arch=selected_arch,
    )


def detect_host_arch() -> str:
    machine = platform.machine().lower()
    if machine in {"arm64", "aarch64"}:
        return "arm64"
    if machine in {"x86_64", "amd64", "x64", "i686", "i386", "x86"}:
        return "x64"
    raise SetupError(
        f"Unsupported host architecture '{platform.machine()}'. Supported architectures: x64, arm64"
    )


def _validated_manifest_sha512(value: str, platform_key: str) -> str:
    normalized = value.lower()
    if len(normalized) != 128 or any(character not in "0123456789abcdef" for character in normalized):
        raise SetupError(f"Manifest SHA512 for {platform_key} must be a 128-character hexadecimal digest.")
    return normalized


def _validated_dotnet_sdk_root(value: str, platform_key: str, arch: str) -> Path:
    normalized = value.replace("\\", "/")
    path = PurePosixPath(normalized)
    expected = PurePosixPath("Tools") / "Dotnet" / platform_key / arch
    if path.is_absolute() or ".." in path.parts or path != expected:
        raise SetupError(
            f"Manifest sdkRoot for {platform_key}/{arch} must be exactly {expected}; got '{value}'."
        )
    return Path(*path.parts)


def _validate_dotnet_install_root(value: object) -> None:
    if not isinstance(value, str):
        raise SetupError("Manifest installRoot for dotnet-sdk must be Tools/Dotnet.")
    normalized = value.replace("\\", "/")
    if PurePosixPath(normalized) != PurePosixPath("Tools") / "Dotnet":
        raise SetupError(f"Manifest installRoot for dotnet-sdk must be Tools/Dotnet; got '{value}'.")


def resolve_dotnet_package(
    manifest: Mapping,
    platform_key: Optional[str],
    arch: Optional[str] = None,
) -> DotnetPackage:
    dependency = manifest.get("dependencies", {}).get("dotnet-sdk")
    if not dependency:
        raise SetupError("Dependency manifest does not contain the dotnet-sdk package.")
    _validate_dotnet_install_root(dependency.get("installRoot"))

    selected_platform = platform_key or detect_platform()
    platforms = dependency.get("platforms", {})
    platform_data = platforms.get(selected_platform)
    if not isinstance(platform_data, Mapping):
        known = ", ".join(sorted(platforms)) or "<none>"
        raise SetupError(f"Unsupported platform '{selected_platform}'. Supported platforms: {known}")

    selected_arch = (arch or detect_host_arch()).lower()
    if selected_arch not in {"x64", "arm64"}:
        raise SetupError(".NET SDK architecture must be x64 or arm64.")
    data = platform_data.get(selected_arch)
    if not isinstance(data, Mapping):
        known = ", ".join(sorted(str(key) for key in platform_data)) or "<none>"
        raise SetupError(
            f"Unsupported .NET SDK architecture '{selected_arch}' on {selected_platform}. Supported: {known}"
        )

    url = _required_manifest_field(data, "url", f"{selected_platform}/{selected_arch}")
    parsed = urlparse(url)
    if parsed.scheme != "https" or parsed.hostname != OFFICIAL_DOTNET_PACKAGE_HOST:
        raise SetupError(
            f"Manifest url for {selected_platform}/{selected_arch} must use https://{OFFICIAL_DOTNET_PACKAGE_HOST}; got '{url}'."
        )
    file_name = _validated_manifest_file_name(
        _required_manifest_field(data, "fileName", f"{selected_platform}/{selected_arch}"),
        f"{selected_platform}/{selected_arch}",
    )
    sha512 = _validated_manifest_sha512(
        _required_manifest_field(data, "sha512", f"{selected_platform}/{selected_arch}"),
        f"{selected_platform}/{selected_arch}",
    )
    sdk_root = _validated_dotnet_sdk_root(
        _required_manifest_field(data, "sdkRoot", f"{selected_platform}/{selected_arch}"),
        selected_platform,
        selected_arch,
    )
    archive_type = _required_manifest_field(data, "archiveType", f"{selected_platform}/{selected_arch}")
    if archive_type not in {"zip", "tar.gz"}:
        raise SetupError(f"Unsupported .NET SDK archive type '{archive_type}'.")
    validation = dict(_required_manifest_mapping(data, "validation", f"{selected_platform}/{selected_arch}"))
    executable = str(validation.get("executable", ""))
    if executable not in {"dotnet", "dotnet.exe"}:
        raise SetupError(".NET SDK validation.executable must be dotnet or dotnet.exe.")

    return DotnetPackage(
        dependency_id="dotnet-sdk",
        dependency_name=str(dependency.get("name", ".NET SDK")),
        version=str(dependency.get("version", "")),
        platform_key=selected_platform,
        arch=selected_arch,
        url=url,
        file_name=file_name,
        sha512=sha512,
        sdk_root=sdk_root,
        archive_type=archive_type,
        validation=validation,
    )


def detect_windows_arch() -> str:
    machine = platform.machine().lower()
    if machine in {"arm64", "aarch64"}:
        return "arm64"
    return "x64"


def _validate_windows_arch(arch: str) -> str:
    normalized = arch.lower()
    if normalized not in {"x64", "arm64"}:
        raise SetupError("Windows FBX SDK architecture must be x64 or arm64.")
    return normalized


def _validated_manifest_sdk_root(value: str, platform_key: str) -> Path:
    normalized = value.replace("\\", "/")
    path = PurePosixPath(normalized)
    expected = PurePosixPath("ThirdParty") / "FBX" / "sdk" / platform_key
    if path.is_absolute() or ".." in path.parts or path != expected:
        raise SetupError(
            f"Manifest sdkRoot for {platform_key} must be exactly {expected}; got '{value}'."
        )
    return Path(*path.parts)


def _required_manifest_field(data: Mapping, field: str, platform_key: str) -> str:
    value = data.get(field)
    if value is None or str(value) == "":
        raise SetupError(f"Manifest field '{field}' is required for {platform_key}.")
    return str(value)


def _required_manifest_mapping(data: Mapping, field: str, platform_key: str) -> Mapping:
    value = data.get(field)
    if not isinstance(value, Mapping):
        raise SetupError(f"Manifest field '{field}' is required for {platform_key}.")
    return value


def _validated_manifest_url(value: str, platform_key: str) -> str:
    parsed = urlparse(value)
    if parsed.scheme != "https" or parsed.hostname != OFFICIAL_AUTODESK_PACKAGE_HOST:
        raise SetupError(
            f"Manifest url for {platform_key} must use https://{OFFICIAL_AUTODESK_PACKAGE_HOST}; got '{value}'."
        )
    if not parsed.path or parsed.path.endswith("/"):
        raise SetupError(f"Manifest url for {platform_key} must point to a package file; got '{value}'.")
    return value


def _validated_manifest_file_name(value: str, platform_key: str) -> str:
    path = PurePosixPath(value.replace("\\", "/"))
    if path.is_absolute() or len(path.parts) != 1 or path.name in {"", ".", ".."}:
        raise SetupError(f"Manifest fileName for {platform_key} must be a plain package file name; got '{value}'.")
    return path.name


def _validated_manifest_sha256(value: str, platform_key: str) -> str:
    normalized = value.upper()
    if len(normalized) != 64 or any(character not in "0123456789ABCDEF" for character in normalized):
        raise SetupError(f"Manifest SHA256 for {platform_key} must be a 64-character hexadecimal digest.")
    return normalized


def _validated_manifest_installer_entry(value: Optional[str], platform_key: str) -> Optional[str]:
    if value is None:
        return None
    path = PurePosixPath(str(value).replace("\\", "/"))
    if path.is_absolute() or ".." in path.parts or path.name in {"", ".", ".."}:
        raise SetupError(f"Manifest installerEntry for {platform_key} must be a relative package entry; got '{value}'.")
    return str(path)


def env_accepts_eula(environ: Mapping[str, str]) -> bool:
    value = environ.get("NLS_ACCEPT_AUTODESK_FBX_EULA", "")
    return value.strip().lower() in ACCEPTED_EULA_VALUES


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def verify_package_hash(path: Path, expected_sha256: str) -> str:
    actual = hash_file(path)
    expected = expected_sha256.upper()
    if actual != expected:
        raise SetupError(
            f"Package hash mismatch for {path}. Expected SHA256 {expected}, actual SHA256 {actual}."
        )
    return actual


def hash_file_sha512(path: Path) -> str:
    digest = hashlib.sha512()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().lower()


def verify_package_sha512(path: Path, expected_sha512: str) -> str:
    actual = hash_file_sha512(path)
    expected = expected_sha512.lower()
    if actual != expected:
        raise SetupError(
            f"Package SHA512 mismatch for {path}. Expected SHA512 {expected}, actual SHA512 {actual}."
        )
    return actual


def _read_required_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="ignore")
    except FileNotFoundError as exc:
        raise SetupError(f"Required SDK file is missing: {path}") from exc


def validate_sdk_root(repo_root: Path, package: PlatformPackage) -> Path:
    sdk_root = resolve_sdk_root(repo_root, package)
    missing = []  # type: List[str]

    for relative in package.validation.get("headers", []):
        if not (sdk_root / relative).exists():
            missing.append(relative)

    version_header = sdk_root / str(package.validation.get("versionHeader", ""))
    if version_header.exists():
        text = _read_required_text(version_header)
        version = package.validation.get("version", {})
        expected_macros = {
            "FBXSDK_VERSION_MAJOR": str(version.get("major", "")),
            "FBXSDK_VERSION_MINOR": str(version.get("minor", "")),
            "FBXSDK_VERSION_POINT": str(version.get("point", "")),
        }
        for macro, expected in expected_macros.items():
            if expected and f"{macro} {expected}" not in text:
                missing.append(f"{package.validation.get('versionHeader')} ({macro} {expected})")
    elif package.validation.get("versionHeader"):
        missing.append(str(package.validation["versionHeader"]))

    missing.extend(_missing_libraries(sdk_root, package, "linkLibraries"))
    missing.extend(_missing_libraries(sdk_root, package, "runtimeLibraries"))

    if missing:
        details = ", ".join(missing)
        raise SetupError(f"SDK root '{sdk_root}' is incomplete or invalid; missing {details}.")
    return sdk_root


def resolve_sdk_root(repo_root: Path, package: PlatformPackage) -> Path:
    root = repo_root.resolve()
    sdk_root = root / package.sdk_root
    allowed_root = root / "ThirdParty" / "FBX" / "sdk" / package.platform_key
    if sdk_root != allowed_root:
        raise SetupError(f"Resolved SDK root must be {allowed_root}; got {sdk_root}.")
    _reject_symlinked_sdk_path(root, sdk_root)
    return sdk_root


def _reject_symlinked_sdk_path(repo_root: Path, sdk_root: Path) -> None:
    check_root = repo_root
    for path in _paths_between(check_root, sdk_root):
        if path == check_root:
            continue
        if path.is_symlink():
            raise UnsafeSdkPathError(f"Refusing to use SDK root through symbolic link: {path}")
        if _is_windows_reparse_point(path):
            raise UnsafeSdkPathError(f"Refusing to use SDK root through Windows reparse point: {path}")
        if _is_mount_point(path):
            raise UnsafeSdkPathError(f"Refusing to use SDK root through mount point: {path}")


def _paths_between(start: Path, end: Path) -> List[Path]:
    paths = []  # type: List[Path]
    current = end
    while True:
        paths.append(current)
        if current == start:
            break
        if current.parent == current:
            raise UnsafeSdkPathError(f"SDK root '{end}' is not under '{start}'.")
        current = current.parent
    paths.reverse()
    return paths


def _is_windows_reparse_point(path: Path) -> bool:
    try:
        attributes = path.stat(follow_symlinks=False).st_file_attributes
    except (AttributeError, FileNotFoundError, OSError):
        return False
    return bool(attributes & FILE_ATTRIBUTE_REPARSE_POINT)


def _is_mount_point(path: Path) -> bool:
    try:
        return path.exists() and path.is_mount()
    except (NotImplementedError, OSError):
        return False


def _missing_libraries(sdk_root: Path, package: PlatformPackage, field: str) -> List[str]:
    candidates = list(package.validation.get(field, []))
    if package.platform_key == "windows":
        required = [
            candidate
            for candidate in candidates
            if f"/{package.arch}/release/" in candidate.replace("\\", "/")
        ]
        if not _any_candidate_exists(sdk_root, required):
            return [f"one of {field}: {', '.join(required)}"]
        return []
    if candidates and not _any_candidate_exists(sdk_root, candidates):
        return [f"one of {field}"]
    return []


def _any_candidate_exists(root: Path, candidates: Sequence[str]) -> bool:
    return any((root / candidate).exists() for candidate in candidates)


def resolve_cache_dir(repo_root: Path, args: argparse.Namespace, environ: Mapping[str, str]) -> Path:
    if args.cache_dir:
        return Path(args.cache_dir)
    if environ.get("NLS_DEPENDENCY_CACHE"):
        return Path(environ["NLS_DEPENDENCY_CACHE"])
    return repo_root / "ThirdParty" / "FBX" / "packages"


def download_package(package: PlatformPackage, destination: Path, stdout: TextIO) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    print(f"Downloading {package.dependency_name} from {package.url}", file=stdout)
    last_error = None  # type: Optional[BaseException]
    for attempt in range(1, DOWNLOAD_RETRY_COUNT + 1):
        fd, temporary_name = tempfile.mkstemp(
            prefix=destination.name + ".",
            suffix=".download",
            dir=str(destination.parent),
        )
        os.close(fd)
        temporary = Path(temporary_name)
        try:
            with urllib.request.urlopen(package.url, timeout=DOWNLOAD_TIMEOUT_SECONDS) as response, temporary.open("wb") as output:
                shutil.copyfileobj(response, output)
            temporary.replace(destination)
            return
        except Exception as exc:  # noqa: BLE001 - convert network/tool errors to user-facing setup errors
            last_error = exc
            temporary.unlink(missing_ok=True)
            if attempt < DOWNLOAD_RETRY_COUNT:
                print(f"Download attempt {attempt} failed: {exc}. Retrying...", file=stdout)
                continue
    raise SetupError(f"Failed to download {package.url}: {last_error}") from last_error


def prepare_package(
    package: PlatformPackage,
    package_path: Path,
    stdout: TextIO,
    downloader: Callable[[PlatformPackage, Path, TextIO], None] = download_package,
) -> Path:
    if package_path.exists():
        verify_package_hash(package_path, package.sha256)
        print(f"Using verified cached package: {package_path}", file=stdout)
        return package_path

    downloader(package, package_path, stdout)
    try:
        verify_package_hash(package_path, package.sha256)
    except SetupError:
        package_path.unlink(missing_ok=True)
        raise
    print(f"Verified downloaded package: {package_path}", file=stdout)
    return package_path


def resolve_dotnet_sdk_root(repo_root: Path, package: DotnetPackage) -> Path:
    root = repo_root.resolve()
    sdk_root = root / package.sdk_root
    allowed_root = root / "Tools" / "Dotnet" / package.platform_key / package.arch
    if sdk_root != allowed_root:
        raise SetupError(f"Resolved .NET SDK root must be {allowed_root}; got {sdk_root}.")
    _reject_symlinked_sdk_path(root, sdk_root)
    return sdk_root


def validate_dotnet_sdk_root(repo_root: Path, package: DotnetPackage) -> Path:
    sdk_root = resolve_dotnet_sdk_root(repo_root, package)
    executable = sdk_root / str(package.validation["executable"])
    if not executable.is_file():
        raise SetupError(f".NET SDK root '{sdk_root}' is incomplete; missing {package.validation['executable']}.")

    environment = os.environ.copy()
    environment["DOTNET_ROOT"] = str(sdk_root)
    environment["DOTNET_MULTILEVEL_LOOKUP"] = "0"
    try:
        result = subprocess.run(
            [str(executable), "--version"],
            cwd=str(sdk_root),
            env=environment,
            check=True,
            capture_output=True,
            text=True,
            timeout=DOWNLOAD_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        raise SetupError(f"Unable to execute the .NET SDK at {executable}.") from exc

    version = result.stdout.strip().splitlines()[-1] if result.stdout.strip() else ""
    if version != package.version:
        raise SetupError(
            f".NET SDK at {sdk_root} reports version {version or '<unknown>'}; expected {package.version}."
        )
    return executable


def resolve_dotnet_cache_dir(
    repo_root: Path,
    environ: Mapping[str, str],
    args: Optional[argparse.Namespace] = None,
) -> Path:
    if args is not None and args.cache_dir:
        return Path(args.cache_dir)
    if environ.get("NLS_DOTNET_CACHE"):
        return Path(environ["NLS_DOTNET_CACHE"])
    return repo_root / "Tools" / "Dotnet" / "packages"


def prepare_dotnet_package(
    package: DotnetPackage,
    package_path: Path,
    stdout: TextIO,
    downloader: Callable[[DotnetPackage, Path, TextIO], None] = download_package,
) -> Path:
    if package_path.exists():
        verify_package_sha512(package_path, package.sha512)
        print(f"Using verified cached package: {package_path}", file=stdout)
        return package_path

    downloader(package, package_path, stdout)  # type: ignore[arg-type]
    try:
        verify_package_sha512(package_path, package.sha512)
    except SetupError:
        package_path.unlink(missing_ok=True)
        raise
    print(f"Verified downloaded package: {package_path}", file=stdout)
    return package_path


def prompt_accepts_eula(package: PlatformPackage, input_func: Callable[[str], str], stdout: TextIO) -> bool:
    eula_url = package.eula.get("url", package.url)
    print(f"{package.dependency_name} requires accepting Autodesk's EULA before download/install.", file=stdout)
    print(f"EULA/source page: {eula_url}", file=stdout)
    answer = input_func("Type 'yes' to accept and continue: ")
    return answer.strip().lower() in {"y", "yes"}


def ensure_eula_accepted(
    package: PlatformPackage,
    args: argparse.Namespace,
    environ: Mapping[str, str],
    input_func: Callable[[str], str],
    stdout: TextIO,
) -> None:
    if args.accept_autodesk_eula or env_accepts_eula(environ):
        return
    if args.non_interactive:
        raise SetupError(
            "Autodesk FBX SDK EULA acceptance is required before download/install. "
            "Pass --accept-autodesk-eula or set NLS_ACCEPT_AUTODESK_FBX_EULA=1."
        )
    if not prompt_accepts_eula(package, input_func, stdout):
        raise SetupError("Autodesk EULA was not accepted; no files were downloaded or installed.")


def install_package(package: PlatformPackage, package_path: Path, repo_root: Path, stdout: TextIO) -> None:
    sdk_root = resolve_sdk_root(repo_root, package)
    if package.archive_type == "windows-installer":
        _install_windows(package_path, sdk_root, stdout)
    elif package.archive_type == "linux-installer-tar":
        _install_linux(package, package_path, sdk_root, stdout)
    elif package.archive_type == "macos-pkg-tar":
        _install_macos(package, package_path, sdk_root, stdout)
    else:
        raise SetupError(f"Unsupported package archive type '{package.archive_type}'.")


def _install_windows(package_path: Path, sdk_root: Path, stdout: TextIO) -> None:
    sdk_root.parent.mkdir(parents=True, exist_ok=True)
    print(f"Running Windows FBX SDK installer for {sdk_root}", file=stdout)
    command = [str(package_path), "/S", f"/D={sdk_root}"]
    try:
        subprocess.run(command, check=True, timeout=INSTALLER_TIMEOUT_SECONDS)
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        raise SetupError(
            "Failed to run the Windows FBX SDK installer. "
            "If the Autodesk installer UI appears, install to ThirdParty/FBX/sdk/windows and rerun validation."
        ) from exc


def _install_linux(package: PlatformPackage, package_path: Path, sdk_root: Path, stdout: TextIO) -> None:
    sdk_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="nullus-fbx-linux-") as temp_dir:
        temp = Path(temp_dir)
        with tarfile.open(package_path, "r:gz") as archive:
            safe_extract_tar(archive, temp)
        installer = temp / str(package.installer_entry or "")
        if not installer.exists():
            raise SetupError(f"Linux FBX SDK installer entry was not found: {package.installer_entry}")
        installer.chmod(installer.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        print(f"Running Linux FBX SDK installer for {sdk_root}", file=stdout)
        try:
            subprocess.run(
                [str(installer), str(sdk_root)],
                input="yes\n",
                text=True,
                check=True,
                timeout=INSTALLER_TIMEOUT_SECONDS,
            )
        except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
            raise SetupError(f"Failed to run the Linux FBX SDK installer for {sdk_root}.") from exc


def _install_macos(package: PlatformPackage, package_path: Path, sdk_root: Path, stdout: TextIO) -> None:
    with tempfile.TemporaryDirectory(prefix="nullus-fbx-macos-") as temp_dir:
        temp = Path(temp_dir)
        with tarfile.open(package_path, "r:gz") as archive:
            safe_extract_tar(archive, temp)
        pkg = temp / str(package.installer_entry or "")
        if not pkg.exists():
            raise SetupError(f"macOS FBX SDK package entry was not found: {package.installer_entry}")

        expanded = temp / "expanded"
        command = ["pkgutil", "--expand-full", str(pkg), str(expanded)]
        try:
            subprocess.run(command, check=True, timeout=INSTALLER_TIMEOUT_SECONDS)
        except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
            raise SetupError(
                "Failed to expand the macOS FBX SDK package with pkgutil --expand-full. "
                "Install the package manually to ThirdParty/FBX/sdk/macos or run setup on a macOS version with pkgutil --expand-full support."
            ) from exc

        source_root = _find_extracted_sdk_root(expanded)
        if not source_root:
            raise SetupError("Could not locate include/fbxsdk.h in the expanded macOS package.")
        print(f"Copying macOS FBX SDK files to {sdk_root}", file=stdout)
        if sdk_root.exists():
            shutil.rmtree(sdk_root)
        shutil.copytree(source_root, sdk_root)


def _find_extracted_sdk_root(root: Path) -> Optional[Path]:
    for header in root.rglob("include/fbxsdk.h"):
        return header.parent.parent
    return None


def safe_extract_tar(archive: tarfile.TarFile, destination: Path) -> None:
    destination_root = destination.resolve()
    for member in archive.getmembers():
        if member.issym() or member.islnk() or member.isdev():
            raise SetupError(f"Refusing to extract unsafe archive member type: {member.name}")
        target = (destination_root / member.name).resolve()
        if target != destination_root and destination_root not in target.parents:
            raise SetupError(f"Refusing to extract unsafe archive member: {member.name}")
    archive.extractall(destination_root)


def safe_extract_zip(archive: zipfile.ZipFile, destination: Path) -> None:
    destination_root = destination.resolve()
    for member in archive.infolist():
        member_path = PurePosixPath(member.filename.replace("\\", "/"))
        if (
            member_path.is_absolute()
            or ".." in member_path.parts
            or (member_path.parts and ":" in member_path.parts[0])
        ):
            raise SetupError(f"Refusing to extract unsafe archive member: {member.filename}")
        target = (destination_root / Path(*member_path.parts)).resolve()
        if target != destination_root and destination_root not in target.parents:
            raise SetupError(f"Refusing to extract unsafe archive member: {member.filename}")
        unix_mode = (member.external_attr >> 16) & 0o170000
        if unix_mode == stat.S_IFLNK:
            raise SetupError(f"Refusing to extract unsafe archive member type: {member.filename}")
    archive.extractall(destination_root)


def _find_dotnet_payload_root(root: Path, executable_name: str) -> Path:
    direct = root / executable_name
    if direct.is_file():
        return root
    matches = [path for path in root.rglob(executable_name) if path.is_file()]
    if len(matches) != 1:
        raise SetupError(
            f"Expected exactly one {executable_name} in the .NET SDK archive; found {len(matches)}."
        )
    return matches[0].parent


def install_dotnet_package(
    package: DotnetPackage,
    package_path: Path,
    repo_root: Path,
    stdout: TextIO,
) -> None:
    sdk_root = resolve_dotnet_sdk_root(repo_root, package)
    sdk_root.parent.mkdir(parents=True, exist_ok=True)
    executable_name = str(package.validation["executable"])
    with tempfile.TemporaryDirectory(prefix="nullus-dotnet-", dir=str(sdk_root.parent)) as temp_dir:
        temp_root = Path(temp_dir)
        payload_root = temp_root / "payload"
        payload_root.mkdir()
        if package.archive_type == "zip":
            with zipfile.ZipFile(package_path, "r") as archive:
                safe_extract_zip(archive, payload_root)
        elif package.archive_type == "tar.gz":
            with tarfile.open(package_path, "r:gz") as archive:
                safe_extract_tar(archive, payload_root)
        else:
            raise SetupError(f"Unsupported .NET SDK archive type '{package.archive_type}'.")

        source_root = _find_dotnet_payload_root(payload_root, executable_name)
        backup_root: Optional[Path] = None
        if sdk_root.exists():
            if sdk_root.is_symlink() or _is_windows_reparse_point(sdk_root):
                raise UnsafeSdkPathError(f"Refusing to replace .NET SDK through a link: {sdk_root}")
            if not sdk_root.is_dir():
                raise SetupError(f"Refusing to replace non-directory .NET SDK path: {sdk_root}")
            backup_root = sdk_root.parent / f"{sdk_root.name}.backup"
            if backup_root.exists():
                raise SetupError(f"Refusing to overwrite an existing SDK backup directory: {backup_root}")
            sdk_root.replace(backup_root)
        try:
            shutil.move(str(source_root), str(sdk_root))
        except Exception:
            if backup_root is not None and backup_root.exists() and not sdk_root.exists():
                backup_root.replace(sdk_root)
            raise
        if backup_root is not None:
            shutil.rmtree(backup_root)

    if not sdk_root.joinpath(executable_name).is_file():
        raise SetupError(f"Installed .NET SDK is missing {executable_name}: {sdk_root}")
    if package.platform_key != "windows":
        executable = sdk_root / executable_name
        executable.chmod(executable.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    print(f"Installed .NET SDK {package.version} to {sdk_root}", file=stdout)


def write_dotnet_wrapper(repo_root: Path, package: DotnetPackage) -> Path:
    wrapper_root = repo_root / "Tools" / "Dotnet"
    wrapper_root.mkdir(parents=True, exist_ok=True)
    relative_root = Path(package.platform_key) / package.arch
    if package.platform_key == "windows":
        wrapper = wrapper_root / "dotnet.cmd"
        wrapper.write_text(
            "@echo off\r\n"
            f"set \"NLS_DOTNET_ROOT=%~dp0{relative_root.as_posix().replace('/', chr(92))}\"\r\n"
            f"\"%NLS_DOTNET_ROOT%\\dotnet.exe\" %*\r\n",
            encoding="ascii",
        )
    else:
        wrapper = wrapper_root / "dotnet"
        wrapper.write_text(
            "#!/usr/bin/env sh\n"
            "set -eu\n"
            f"exec \"$(CDPATH= cd \"$(dirname \"$0\")\" && pwd)/{relative_root.as_posix()}/dotnet\" \"$@\"\n",
            encoding="ascii",
        )
        wrapper.chmod(wrapper.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return wrapper


def run_dotnet_setup(
    args: argparse.Namespace,
    environ: Mapping[str, str],
    stdout: TextIO,
) -> int:
    repo_root = Path(args.repo_root).resolve() if args.repo_root else find_repo_root()
    manifest = load_manifest(Path(args.manifest))
    package = resolve_dotnet_package(manifest, args.platform, args.arch)

    try:
        executable = validate_dotnet_sdk_root(repo_root, package)
        if not args.force:
            if not args.validate_only:
                write_dotnet_wrapper(repo_root, package)
            print(f"{package.dependency_name} {package.version} is already valid: {executable}", file=stdout)
            return 0
    except UnsafeSdkPathError:
        raise
    except SetupError:
        if args.validate_only:
            raise

    if args.validate_only:
        print(f"{package.dependency_name} SDK root is valid.", file=stdout)
        return 0

    cache_dir = resolve_dotnet_cache_dir(repo_root, environ, args)
    package_path = cache_dir / package.file_name
    if args.dry_run:
        print(
            f"Dry run: would prepare {package.dependency_name} {package.version} "
            f"for {package.platform_key} ({package.arch}) at {repo_root / package.sdk_root}",
            file=stdout,
        )
        print(f"Dry run: would use package cache {cache_dir}", file=stdout)
        print("Dry run: No files were modified.", file=stdout)
        return 0

    cache_dir.mkdir(parents=True, exist_ok=True)
    prepare_dotnet_package(package, package_path, stdout)
    try:
        install_dotnet_package(package, package_path, repo_root, stdout)
    except (OSError, tarfile.TarError, zipfile.BadZipFile) as exc:
        raise SetupError(f"Failed to extract or install the .NET SDK package: {package_path}") from exc
    executable = validate_dotnet_sdk_root(repo_root, package)
    wrapper = write_dotnet_wrapper(repo_root, package)
    print(f"{package.dependency_name} {package.version} installed and validated: {executable}", file=stdout)
    print(f"Use the repository-local launcher: {wrapper}", file=stdout)
    return 0


def run_setup(
    args: argparse.Namespace,
    environ: Mapping[str, str],
    input_func: Callable[[str], str],
    stdout: TextIO,
) -> int:
    repo_root = Path(args.repo_root).resolve() if args.repo_root else find_repo_root()
    if getattr(args, "setup_dotnet", False):
        args.dependency = "dotnet-sdk"
    if args.dependency == "dotnet-sdk":
        return run_dotnet_setup(args, environ, stdout)

    manifest = load_manifest(Path(args.manifest))
    package = resolve_package(manifest, args.dependency, args.platform, args.arch)

    try:
        sdk_root = validate_sdk_root(repo_root, package)
        if not args.force:
            print(f"{package.dependency_name} {package.version} SDK root is already valid: {sdk_root}", file=stdout)
            return 0
    except UnsafeSdkPathError:
        raise
    except SetupError:
        if args.validate_only:
            raise

    if args.validate_only:
        print(f"{package.dependency_name} SDK root is valid.", file=stdout)
        return 0

    ensure_eula_accepted(package, args, environ, input_func, stdout)

    cache_dir = resolve_cache_dir(repo_root, args, environ)
    package_path = cache_dir / package.file_name

    if args.dry_run:
        arch_suffix = f" ({package.arch})" if package.arch else ""
        print(
            f"Dry run: would prepare {package.dependency_name} {package.version} "
            f"for {package.platform_key}{arch_suffix} at {repo_root / package.sdk_root}",
            file=stdout,
        )
        print(f"Dry run: would use package cache {cache_dir}", file=stdout)
        print("Dry run: No files were modified.", file=stdout)
        return 0

    cache_dir.mkdir(parents=True, exist_ok=True)
    prepare_package(package, package_path, stdout)

    install_package(package, package_path, repo_root, stdout)
    sdk_root = validate_sdk_root(repo_root, package)
    print(f"{package.dependency_name} {package.version} installed and validated: {sdk_root}", file=stdout)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Prepare Nullus source dependencies.")
    parser.add_argument("--dependency", default="autodesk-fbx-sdk")
    parser.add_argument(
        "--setup-dotnet",
        action="store_true",
        help="Install the pinned repository-local .NET 8 SDK (equivalent to --dependency dotnet-sdk).",
    )
    parser.add_argument("--platform", choices=("windows", "linux", "macos"))
    parser.add_argument("--arch", type=str.lower, choices=("x64", "arm64"), help="Windows SDK architecture to validate/install")
    parser.add_argument("--accept-autodesk-eula", action="store_true")
    parser.add_argument("--non-interactive", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--cache-dir")
    parser.add_argument("--repo-root")
    parser.add_argument("--validate-only", action="store_true")
    parser.add_argument("--manifest", default=str(DEFAULT_MANIFEST))
    return parser


def main(
    argv: Optional[Sequence[str]] = None,
    environ: Optional[Mapping[str, str]] = None,
    input_func: Callable[[str], str] = input,
    stdout: Optional[TextIO] = None,
    stderr: Optional[TextIO] = None,
) -> int:
    out = stdout or sys.stdout
    err = stderr or sys.stderr
    parser = build_parser()
    try:
        args = parser.parse_args(argv)
        environment = os.environ if environ is None else environ
        return run_setup(args, environment, input_func, out)
    except SetupError as exc:
        print(f"error: {exc}", file=err)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
