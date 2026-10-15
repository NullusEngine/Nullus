#!/usr/bin/env python3
"""Nullus documentation toolkit.

Docs/ is the single source of truth for the handbook. This tool validates the
bilingual source tree and generates the GitHub Wiki from it.

Commands:
    check                         validate the manifest, pages, links, anchors
    export --out DIR [--prune]    render the Wiki tree into DIR
    diff --wiki DIR [--clone URL] report what a sync would change
    publish [--remote URL]        export into a wiki clone and push

The exporter only ever deletes files it generated before, tracked through
``nullus-docs-manifest.json`` in the wiki repository.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from urllib.parse import quote, unquote

TOOL_DIR = Path(__file__).resolve().parent
ROOT = TOOL_DIR.parents[1]
DOCS_DIR = ROOT / "Docs"
MANIFEST_PATH = DOCS_DIR / "nav.json"
GENERATED_MANIFEST_NAME = "nullus-docs-manifest.json"

LINK_RE = re.compile(r'(!?)\[([^\]\n]*)\]\((<[^>\n]+>|(?:[^()\s]|\([^()\n]*\))+)(\s+"[^"\n]*")?\)')
HEADING_RE = re.compile(r"^(#{1,6})\s+(.*?)\s*$", re.MULTILINE)
EXTERNAL_RE = re.compile(r"^([a-zA-Z][a-zA-Z0-9+.\-]*:|//)")
IDENTIFIER_RE = re.compile(r"[a-zA-Z0-9]+(?:-[a-zA-Z0-9]+)*\Z")

# Wiki pages that existed before the handbook rewrite. Each keeps a short
# migration note plus links into the new navigation.
LEGACY_WIKI_PAGES = {
    "快速开始": ["getting-started/build", "getting-started/first-project"],
    "编辑器与资产工作流": ["editor/workspace", "assets/pipeline", "prefab/workflow"],
    "材质与ShaderLab": ["rendering/materials"],
    "脚本系统": ["scripting/overview", "scripting/api"],
    "渲染架构": ["rendering/overview", "rendering/backends", "architecture/jobs-threading"],
    "反射与序列化": ["architecture/reflection-serialization"],
    "任务系统与性能分析": ["architecture/jobs-threading", "quality/profiling"],
    "构建、测试与贡献": ["reference/build-options", "quality/debugging"],
    "zh-CN-scene-prefabs": ["prefab/workflow"],
    "en-scene-prefabs": ["prefab/workflow"],
}


class DocError(Exception):
    """Raised for a validation failure that should stop the command."""


# --------------------------------------------------------------------------
# Manifest
# --------------------------------------------------------------------------


def load_manifest() -> dict:
    if not MANIFEST_PATH.is_file():
        raise DocError(f"missing manifest: {MANIFEST_PATH.relative_to(ROOT)}")
    try:
        manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise DocError(f"{MANIFEST_PATH.relative_to(ROOT)}: {exc}") from exc
    validate_manifest_shape(manifest)
    return manifest


def validate_manifest_shape(manifest: dict) -> None:
    if not isinstance(manifest, dict):
        raise DocError("manifest must be a JSON object")
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", manifest.get("repository", "")):
        raise DocError("manifest repository must be owner/name")
    languages = manifest.get("languages") or []
    if len(languages) < 2:
        raise DocError("manifest must declare at least two languages")
    language_ids = set()
    language_dirs = set()
    for language in languages:
        for key in ("id", "label", "dir", "wikiSuffix"):
            if not language.get(key):
                raise DocError(f"language entry is missing '{key}'")
        if language["id"] in language_ids:
            raise DocError(f"duplicate language id: {language['id']}")
        for key in ("id", "dir", "wikiSuffix"):
            if not IDENTIFIER_RE.fullmatch(language[key]):
                raise DocError(f"invalid language {key}: {language[key]}")
        if language["dir"].casefold() in language_dirs:
            raise DocError(f"duplicate language directory: {language['dir']}")
        language_dirs.add(language["dir"].casefold())
        language_ids.add(language["id"])

    entry = manifest.get("entryPage")
    if not entry:
        raise DocError("manifest is missing 'entryPage'")

    chapters = manifest.get("chapters") or []
    if not chapters:
        raise DocError("manifest declares no chapters")
    chapter_ids = set()
    page_keys = set()
    for chapter in chapters:
        chapter_id = chapter.get("id")
        if not chapter_id:
            raise DocError("chapter is missing 'id'")
        if not IDENTIFIER_RE.fullmatch(chapter_id):
            raise DocError(f"invalid chapter id: {chapter_id}")
        if chapter_id in chapter_ids:
            raise DocError(f"duplicate chapter id: {chapter_id}")
        chapter_ids.add(chapter_id)
        for language_id in language_ids:
            if not (chapter.get("title") or {}).get(language_id):
                raise DocError(f"chapter '{chapter_id}' has no title for {language_id}")
        pages = chapter.get("pages") or []
        if not pages:
            raise DocError(f"chapter '{chapter_id}' declares no pages")
        page_ids = set()
        for page in pages:
            page_id = page.get("id")
            if not page_id:
                raise DocError(f"chapter '{chapter_id}' has a page without 'id'")
            if not IDENTIFIER_RE.fullmatch(page_id):
                raise DocError(f"invalid page id: {page_id}")
            if page_id in page_ids:
                raise DocError(f"duplicate page id in chapter '{chapter_id}': {page_id}")
            page_ids.add(page_id)
            page_keys.add(f"{chapter_id}/{page_id}")
            for language_id in language_ids:
                if not (page.get("title") or {}).get(language_id):
                    raise DocError(f"page '{chapter_id}/{page_id}' has no title for {language_id}")

    if entry not in page_keys:
        raise DocError(f"entryPage '{entry}' is not declared in the manifest")

    names = set()
    for language in languages:
        for chapter, page in iter_pages(manifest):
            name = wiki_page_name(language, chapter, page).casefold()
            if name in names:
                raise DocError(f"duplicate wiki page name: {name}")
            names.add(name)


def iter_pages(manifest: dict):
    for chapter in manifest["chapters"]:
        for page in chapter["pages"]:
            yield chapter, page


def language_by_id(manifest: dict, language_id: str) -> dict:
    for language in manifest["languages"]:
        if language["id"] == language_id:
            return language
    raise DocError(f"unknown language: {language_id}")


def page_rel_path(language: dict, chapter: dict, page: dict) -> str:
    return f"Docs/{language['dir']}/{chapter['id']}/{page['id']}.md"


def wiki_page_name(language: dict, chapter: dict, page: dict) -> str:
    return f"{language['wikiSuffix']}-{chapter['id']}-{page['id']}"


def github_url(manifest: dict, rel_path: str, raw: bool) -> str:
    repo = manifest["repository"]
    branch = manifest.get("defaultBranch", "main")
    prefix = "https://raw.githubusercontent.com" if raw else "https://github.com"
    tail = "" if raw else "/blob"
    return f"{prefix}/{repo}{tail}/{branch}/{quote(rel_path, safe='/')}"


# --------------------------------------------------------------------------
# Shared helpers
# --------------------------------------------------------------------------


def slugify(text: str) -> str:
    text = re.sub(r"<[^>]+>", "", text)
    text = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", text)
    lowered = text.strip().lower()
    kept = []
    for char in lowered:
        if char.isalnum() or char in "-_ ":
            kept.append(char)
    slug = "".join(kept).replace(" ", "-")
    return slug.strip("-")


def split_anchor(target: str) -> tuple[str, str]:
    path, _, anchor = target.partition("#")
    return path, anchor


def is_external(target: str) -> bool:
    return bool(EXTERNAL_RE.match(target))


def repo_relative(path: Path) -> str:
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return ""


def read_page_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def first_heading(text: str) -> str:
    match = re.search(r"^#\s+(.*?)\s*$", strip_code_blocks(text), re.MULTILINE)
    return match.group(1).strip() if match else ""


def heading_list(text: str) -> list[str]:
    return [match.group(2).strip() for match in HEADING_RE.finditer(strip_code_blocks(text))]


def strip_code_blocks(text: str, inline_code: bool = False) -> str:
    """Blank out fenced code so link scanning ignores code samples."""
    lines = text.split("\n")
    output = []
    fence = ""
    for line in lines:
        match = re.match(r"^\s{0,3}(`{3,}|~{3,})(.*)$", line)
        if match and not fence:
            fence = match.group(1)
            output.append("")
            continue
        if match and fence and match.group(1)[0] == fence[0] and len(match.group(1)) >= len(fence) and not match.group(2).strip():
            fence = ""
            output.append("")
            continue
        if fence:
            output.append("")
        elif inline_code:
            output.append(re.sub(r"(`+).*?\1", lambda match: " " * len(match.group(0)), line))
        else:
            output.append(line)
    return "\n".join(output)


# --------------------------------------------------------------------------
# check
# --------------------------------------------------------------------------


class Report:
    def __init__(self) -> None:
        self.errors: list[str] = []
        self.warnings: list[str] = []
        self.infos: list[str] = []

    def error(self, message: str) -> None:
        self.errors.append(message)

    def warn(self, message: str) -> None:
        self.warnings.append(message)

    def info(self, message: str) -> None:
        self.infos.append(message)

    def print(self) -> None:
        for message in self.infos:
            print(f"[info] {message}")
        for message in self.warnings:
            print(f"[warn] {message}")
        for message in self.errors:
            print(f"[error] {message}", file=sys.stderr)


def build_page_lookup(manifest: dict) -> dict[str, tuple[dict, dict, dict]]:
    lookup: dict[str, tuple[dict, dict, dict]] = {}
    for language in manifest["languages"]:
        for chapter, page in iter_pages(manifest):
            rel = page_rel_path(language, chapter, page)
            lookup[rel] = (language, chapter, page)
    return lookup


def check_relative_target(
    report: Report,
    manifest: dict,
    language: dict,
    source_path: Path,
    target: str,
    page_lookup: dict[str, tuple[dict, dict, dict]],
    line_hint: str,
) -> None:
    target = target.removeprefix("<").removesuffix(">")
    if is_external(target):
        return

    path_part, anchor = split_anchor(target)
    resolved = (source_path.parent / unquote(path_part)).resolve() if path_part else source_path
    if not resolved.exists():
        report.error(f"{line_hint}: link target does not exist: {target}")
        return

    rel = repo_relative(resolved)
    if not rel:
        report.error(f"{line_hint}: link escapes the repository: {target}")
        return

    if resolved.suffix != ".md":
        return

    if rel in page_lookup and rel != repo_relative(source_path):
        target_language = page_lookup[rel][0]
        if target_language["id"] != language["id"]:
            report.error(
                f"{line_hint}: cross-language link {target}; the exporter adds the language switch"
            )
    elif rel.startswith("Docs/") and rel not in page_lookup and rel != "Docs/README.md":
        report.error(f"{line_hint}: markdown file is not part of the navigation manifest: {rel}")
        return

    if anchor:
        text = read_page_text(resolved)
        anchors = {slugify(heading) for heading in heading_list(text)}
        anchors.update(re.findall(r'<a\s+(?:name|id)=["\x27]([^"\x27]+)', text))
        if unquote(anchor) not in anchors:
            report.error(f"{line_hint}: heading anchor does not exist: {target}")


def check_page(report: Report, manifest: dict, language: dict, chapter: dict, page: dict,
               page_lookup: dict[str, tuple[dict, dict, dict]]) -> int:
    rel = page_rel_path(language, chapter, page)
    path = ROOT / rel
    label = f"{rel}"
    if not path.is_file():
        report.error(f"{label}: page file is missing")
        return 0

    text = read_page_text(path)
    expected_title = page["title"][language["id"]]
    actual_title = first_heading(text)
    if actual_title != expected_title:
        report.error(
            f"{label}: first heading is '{actual_title}' but the manifest title is '{expected_title}'"
        )

    headings = heading_list(text)
    seen: dict[str, int] = {}
    for heading in headings:
        slug = slugify(heading)
        seen[slug] = seen.get(slug, 0) + 1
    for slug, count in seen.items():
        if count > 1:
            report.error(f"{label}: duplicate heading anchor '#{slug}' appears {count} times")

    if "\ufffd" in text:
        report.error(f"{label}: contains replacement characters, re-save the file as UTF-8")

    if re.search(r"^\s*(TODO|TBD|FIXME|待补充|待定)\b", text, re.MULTILINE):
        report.error(f"{label}: contains a placeholder marker")

    content = strip_code_blocks(text)
    if len(re.sub(r"^#.*$", "", content, flags=re.MULTILINE).strip()) < 80:
        report.error(f"{label}: page has no substantive content")

    scan = strip_code_blocks(text, inline_code=True)
    for line_number, line in enumerate(scan.split("\n"), start=1):
        for match in LINK_RE.finditer(line):
            check_relative_target(
                report,
                manifest,
                language,
                path,
                match.group(3),
                page_lookup,
                f"{rel}:{line_number}",
            )

    return len(text.split("\n"))


def run_check(manifest: dict, strict: bool) -> Report:
    report = Report()
    page_lookup = build_page_lookup(manifest)

    expected_files: set[Path] = set()

    for language in manifest["languages"]:
        language_dir = DOCS_DIR / language["dir"]
        if not language_dir.is_dir():
            report.error(f"missing language directory: {language_dir.relative_to(ROOT)}")
            continue

    total_pages = 0
    total_lines = 0
    for language in manifest["languages"]:
        for chapter, page in iter_pages(manifest):
            rel = page_rel_path(language, chapter, page)
            expected_files.add((ROOT / rel).resolve())
            lines = check_page(report, manifest, language, chapter, page, page_lookup)
            total_pages += 1
            total_lines += lines

    for language in manifest["languages"]:
        language_dir = DOCS_DIR / language["dir"]
        if not language_dir.is_dir():
            continue
        for path in sorted(language_dir.rglob("*.md")):
            if path.resolve() not in expected_files:
                report.error(
                    f"{path.relative_to(ROOT)}: markdown file is not declared in the navigation manifest"
                )

    if not (DOCS_DIR / "README.md").is_file():
        report.error("Docs/README.md is missing")
    else:
        index = DOCS_DIR / "README.md"
        for line_number, line in enumerate(strip_code_blocks(read_page_text(index), inline_code=True).splitlines(), 1):
            for match in LINK_RE.finditer(line):
                target = match.group(3).strip("<>")
                path_part = split_anchor(target)[0]
                resolved = (index.parent / unquote(path_part)).resolve()
                entry = page_lookup.get(repo_relative(resolved))
                language = entry[0] if entry else manifest["languages"][0]
                check_relative_target(report, manifest, language, index, target, page_lookup,
                                      f"Docs/README.md:{line_number}")

    report.info(f"{len(manifest['chapters'])} chapters, {total_pages} pages, {total_lines} lines")

    if strict and report.warnings:
        report.errors.extend(report.warnings)
        report.warnings = []

    if not report.errors:
        print("documentation check passed")

    return report


# --------------------------------------------------------------------------
# export
# --------------------------------------------------------------------------


def rewrite_links(manifest: dict, language: dict, source_path: Path, text: str,
                  page_lookup: dict[str, tuple[dict, dict, dict]]) -> str:
    def replace(match: re.Match) -> str:
        is_image = match.group(1) == "!"
        label = match.group(2)
        target = match.group(3).strip("<>")
        suffix = match.group(4) or ""

        if is_external(target) or target.startswith("#"):
            return match.group(0)

        path_part, anchor = split_anchor(target)
        if not path_part:
            return match.group(0)

        resolved = (source_path.parent / unquote(path_part)).resolve()
        rel = repo_relative(resolved)
        if not rel:
            return match.group(0)

        if is_image:
            return f"![{label}]({github_url(manifest, rel, raw=True)}{suffix})"

        entry = page_lookup.get(rel)
        if entry is not None:
            target_language, target_chapter, target_page = entry
            name = wiki_page_name(target_language, target_chapter, target_page)
            return f"[{label}]({name}{'#' + anchor if anchor else ''}{suffix})"

        if rel == "Docs/README.md":
            return f"[{label}](Home{'#' + anchor if anchor else ''}{suffix})"

        url = github_url(manifest, rel, raw=False)
        if resolved.is_dir():
            url = url.replace("/blob/", "/tree/", 1)
        return f"[{label}]({url}{'#' + anchor if anchor else ''}{suffix})"

    scan = strip_code_blocks(text, inline_code=True)
    offsets = []
    offset = 0
    for original, visible in zip(text.splitlines(keepends=True), scan.splitlines(keepends=True)):
        for match in LINK_RE.finditer(visible):
            offsets.append((offset + match.start(), offset + match.end(), replace(match)))
        offset += len(original)
    for start, end, replacement in reversed(offsets):
        text = text[:start] + replacement + text[end:]
    return text


def render_language_banner(manifest: dict, language: dict, chapter: dict, page: dict) -> str:
    parts = []
    for other in manifest["languages"]:
        name = wiki_page_name(other, chapter, page)
        if other["id"] == language["id"]:
            parts.append(f"**{other['label']}**")
        else:
            parts.append(f"[{other['label']}]({name})")
    repo = manifest["repository"]
    parts.append(f"[Docs](Home)")
    parts.append(f"[Repository](https://github.com/{repo})")
    return "> " + " | ".join(parts)


def render_page(manifest: dict, language: dict, chapter: dict, page: dict,
                page_lookup: dict[str, tuple[dict, dict, dict]]) -> str:
    source = ROOT / page_rel_path(language, chapter, page)
    text = read_page_text(source)
    lines = text.split("\n")
    if lines and lines[0].startswith("# "):
        heading = lines[0]
        body = "\n".join(lines[1:]).lstrip("\n")
    else:
        heading = ""
        body = text

    # Rewrite before adding the banner: the banner already contains wiki page
    # names, which are not repository-relative paths and must not be rewritten.
    body = rewrite_links(manifest, language, source, body, page_lookup)

    banner = render_language_banner(manifest, language, chapter, page)
    if heading:
        rendered = f"{heading}\n\n{banner}\n\n{body}"
    else:
        rendered = f"{banner}\n\n{body}"
    if not rendered.endswith("\n"):
        rendered += "\n"
    return rendered


def render_home(manifest: dict) -> str:
    entry_key = manifest["entryPage"]
    chapter_id, page_id = entry_key.split("/")
    lines = ["# Nullus Engine", ""]
    lines.append("Nullus documentation is maintained in the repository and published here as a read-only mirror.")
    lines.append("")
    for language in manifest["languages"]:
        lines.append(f"## {language['label']}")
        lines.append("")
        lines.append(f"**{_chapter_title(manifest, chapter_id, language)}**")
        lines.append("")
        for chapter, page in iter_pages(manifest):
            title = f"{chapter['title'][language['id']]} - {page['title'][language['id']]}"
            name = wiki_page_name(language, chapter, page)
            marker = " **Start here**" if chapter["id"] == chapter_id and page["id"] == page_id else ""
            lines.append(f"- [{title}]({name}){marker}")
        lines.append("")
    repo = manifest["repository"]
    lines.append("## Repository")
    lines.append("")
    lines.append(f"- [Source code](https://github.com/{repo})")
    lines.append(f"- [README](https://github.com/{repo}/blob/{manifest.get('defaultBranch', 'main')}/README.md)")
    lines.append(f"- [Contributing](https://github.com/{repo}/blob/{manifest.get('defaultBranch', 'main')}/CONTRIBUTING.md)")
    lines.append(f"- [License](https://github.com/{repo}/blob/{manifest.get('defaultBranch', 'main')}/LICENSE)")
    lines.append("")
    lines.append("Documentation source lives in `Docs/` in the repository. Edit it there; this wiki is regenerated automatically.")
    return "\n".join(lines) + "\n"


def _chapter_title(manifest: dict, chapter_id: str, language: dict) -> str:
    for chapter in manifest["chapters"]:
        if chapter["id"] == chapter_id:
            return chapter["title"][language["id"]]
    return chapter_id


def render_sidebar(manifest: dict) -> str:
    lines = ["**Nullus Engine**", "", "[Home](Home)", ""]
    for language in manifest["languages"]:
        lines.append(f"**{language['label']}**")
        lines.append("")
        current_chapter = None
        for chapter, page in iter_pages(manifest):
            if chapter["id"] != current_chapter:
                current_chapter = chapter["id"]
                lines.append(f"- **{chapter['title'][language['id']]}**")
            name = wiki_page_name(language, chapter, page)
            lines.append(f"  - [{page['title'][language['id']]}]({name})")
        lines.append("")
    return "\n".join(lines) + "\n"


def render_legacy_stub(page_name: str, keys: list[str], manifest: dict) -> str:
    lines = [f"# {page_name}", ""]
    lines.append("This page moved into the restructured handbook. The wiki is now generated from `Docs/` in the repository.")
    lines.append("")
    for key in keys:
        chapter_id, page_id = key.split("/")
        for chapter, page in iter_pages(manifest):
            if chapter["id"] == chapter_id and page["id"] == page_id:
                for language in manifest["languages"]:
                    title = f"{language['label']}: {page['title'][language['id']]}"
                    name = wiki_page_name(language, chapter, page)
                    lines.append(f"- [{title}]({name})")
                break
    lines.append("")
    lines.append("[Home](Home) · [首页](Home)")
    return "\n".join(lines) + "\n"


def export_wiki(manifest: dict, out_dir: Path, prune: bool) -> list[str]:
    validate_manifest_shape(manifest)
    previous_files = managed_files(out_dir) if prune else set()
    out_dir.mkdir(parents=True, exist_ok=True)
    page_lookup = build_page_lookup(manifest)
    generated: set[str] = set()

    def write(name: str, text: str) -> None:
        if (out_dir / name).is_symlink():
            raise DocError(f"refusing to overwrite a symbolic link: {name}")
        (out_dir / name).write_text(text, encoding="utf-8", newline="\n")
        generated.add(name)

    for language in manifest["languages"]:
        for chapter, page in iter_pages(manifest):
            name = wiki_page_name(language, chapter, page) + ".md"
            write(name, render_page(manifest, language, chapter, page, page_lookup))

    write("Home.md", render_home(manifest))
    write("_Sidebar.md", render_sidebar(manifest))

    for legacy_name, keys in LEGACY_WIKI_PAGES.items():
        write(f"{legacy_name}.md", render_legacy_stub(legacy_name, keys, manifest))

    for stale in previous_files - generated:
        stale_path = out_dir / stale
        if stale_path.is_file():
            stale_path.unlink()

    validate_wiki_links(out_dir, generated)

    write(GENERATED_MANIFEST_NAME, json.dumps(
        {
            "generator": "Tools/Docs/nullus_docs.py",
            "repository": manifest["repository"],
            "files": sorted(generated),
        },
        indent=2,
        ensure_ascii=False,
    ) + "\n")

    return sorted(generated)


def managed_files(out_dir: Path) -> set[str]:
    path = out_dir / GENERATED_MANIFEST_NAME
    if not path.is_file():
        return set()
    try:
        previous = json.loads(path.read_text(encoding="utf-8"))
        files = previous["files"]
        if previous.get("generator") != "Tools/Docs/nullus_docs.py" or not isinstance(files, list):
            raise ValueError("unrecognized generator or file list")
        for name in files:
            if not isinstance(name, str) or not re.fullmatch(r"[^/\\:]+\.md", name) or Path(name).is_absolute():
                raise ValueError(f"unsafe generated file name: {name}")
            if (out_dir / name).is_symlink():
                raise ValueError(f"generated file is a symbolic link: {name}")
        return set(files)
    except (json.JSONDecodeError, KeyError, TypeError, ValueError) as exc:
        raise DocError(f"invalid generated manifest {path}: {exc}") from exc


def validate_wiki_links(out_dir: Path, generated: set[str]) -> None:
    for name in sorted(generated):
        if not name.endswith(".md"):
            continue
        text = strip_code_blocks(read_page_text(out_dir / name), inline_code=True)
        for match in LINK_RE.finditer(text):
            target = match.group(3).strip("<>")
            if is_external(target):
                continue
            page_name, anchor = split_anchor(target)
            destination = (page_name + ".md") if page_name else name
            if destination not in generated:
                raise DocError(f"{name}: broken exported wiki link: {target}")
            if anchor:
                anchors = {slugify(heading) for heading in heading_list(read_page_text(out_dir / destination))}
                if unquote(anchor) not in anchors:
                    raise DocError(f"{name}: broken exported wiki anchor: {target}")


# --------------------------------------------------------------------------
# publish
# --------------------------------------------------------------------------


def wiki_remote_url(manifest: dict) -> str:
    return f"https://github.com/{manifest['repository']}.wiki.git"


def redact_url(url: str) -> str:
    """Strip credentials from a remote URL before printing it."""
    return re.sub(r"://[^/@]+@", "://***@", url)


def detect_wiki_branch(url: str) -> str:
    try:
        result = subprocess.run(
            ["git", "ls-remote", "--symref", url, "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "master"
    for line in result.stdout.splitlines():
        if line.startswith("ref:") and "refs/heads/" in line:
            # Output looks like "ref: refs/heads/master\tHEAD"; keep only the
            # branch name and drop the symref target that follows the tab.
            remainder = line.split("refs/heads/", 1)[1]
            branch = remainder.split()[0].strip() if remainder.split() else ""
            if branch:
                return branch
    return "master"


def git(args: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess:
    result = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        message = (result.stderr or result.stdout or "").strip() or f"exit code {result.returncode}"
        raise DocError(redact_url(f"git {' '.join(args[:2])} failed: {message}"))
    return result


def publish_wiki(manifest: dict, remote: str, branch: str | None, dry_run: bool) -> int:
    work_dir = Path(tempfile.mkdtemp(prefix="nullus-wiki-"))
    try:
        print(f"cloning {redact_url(remote)}")
        clone = subprocess.run(
            ["git", "clone", "--quiet", remote, str(work_dir)],
            capture_output=True,
            text=True,
        )
        if clone.returncode != 0:
            raise DocError(redact_url(f"wiki clone failed: {clone.stderr.strip()}"))
        else:
            actual = git(["symbolic-ref", "--short", "HEAD"], cwd=work_dir).stdout.strip()
            if branch and branch != actual:
                # An explicit override must select an existing remote branch;
                # never silently publish to the default instead.
                git(["checkout", "--quiet", branch], cwd=work_dir)
            branch = branch or actual
            print(f"branch: {branch}")

        export_wiki(manifest, work_dir, prune=True)
        # Exported files use LF on every host. Ignore a user's autocrlf policy
        # when staging so a Windows publish and a Linux CI publish agree, even
        # when the previous wiki revision stored CRLF blobs.
        git(["-c", "core.autocrlf=false", "add", "-A"], cwd=work_dir)
        staged = git(["-c", "core.quotepath=false", "diff", "--cached", "--name-only"],
                     cwd=work_dir).stdout.splitlines()
        if not staged:
            print("wiki is already up to date; nothing to publish")
            return 0

        print(f"{len(staged)} wiki file(s) changed")
        for name in staged:
            print(f"  {name}")

        if dry_run:
            print("dry run: stopping before commit and push")
            return 0

        git(["config", "user.name", "nullus-docs-bot"], cwd=work_dir)
        git(["config", "user.email", "nullus-docs-bot@users.noreply.github.com"], cwd=work_dir)
        git(["commit", "--quiet", "-m", "docs: regenerate wiki from Docs/"], cwd=work_dir)
        git(["push", "origin", branch], cwd=work_dir)
        print(f"published to {redact_url(remote)} ({branch})")
        return 0
    finally:
        shutil.rmtree(work_dir, ignore_errors=True)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def command_check(args: argparse.Namespace) -> int:
    report = run_check(load_manifest(), strict=args.strict)
    report.print()
    return 1 if report.errors else 0


def command_export(args: argparse.Namespace) -> int:
    manifest = load_manifest()
    require_valid(manifest)
    out_dir = Path(args.out).resolve()
    files = export_wiki(manifest, out_dir, prune=args.prune)
    print(f"exported {len(files)} file(s) to {out_dir}")
    return 0


def command_diff(args: argparse.Namespace) -> int:
    manifest = load_manifest()
    require_valid(manifest)
    wiki_dir = Path(args.wiki).resolve() if args.wiki else None
    with tempfile.TemporaryDirectory(prefix="nullus-wiki-diff-") as temp:
        target = Path(temp) / "wiki"
        if wiki_dir is None:
            remote = args.clone or wiki_remote_url(manifest)
            print(f"cloning {redact_url(remote)}")
            clone = subprocess.run(["git", "clone", "--quiet", remote, str(target)],
                                   capture_output=True, text=True)
            if clone.returncode != 0:
                raise DocError(redact_url(f"wiki clone failed: {clone.stderr.strip()}"))
        else:
            if not wiki_dir.is_dir():
                raise DocError(f"wiki directory does not exist: {wiki_dir}")
            target.mkdir()
            for source in wiki_dir.iterdir():
                if source.is_file() and (source.suffix == ".md" or source.name == GENERATED_MANIFEST_NAME):
                    shutil.copy2(source, target / source.name)
        before = {path.name: path.read_text(encoding="utf-8") for path in target.iterdir()
                  if path.is_file() and (path.suffix == ".md" or path.name == GENERATED_MANIFEST_NAME)}
        export_wiki(manifest, target, prune=True)
        after = {path.name: path.read_text(encoding="utf-8") for path in target.iterdir()
                 if path.is_file() and (path.suffix == ".md" or path.name == GENERATED_MANIFEST_NAME)}
        changes = [(name, "added" if name not in before else "deleted" if name not in after else "modified")
                   for name in sorted(before.keys() | after.keys()) if before.get(name) != after.get(name)]
        if not changes:
            print("wiki is already up to date")
            return 0
        print("changes that a sync would publish:")
        for name, kind in changes:
            print(f"  {kind}: {name}")
    return 0


def command_publish(args: argparse.Namespace) -> int:
    manifest = load_manifest()
    require_valid(manifest)
    remote = args.remote or wiki_remote_url(manifest)
    print(f"remote: {redact_url(remote)}")
    return publish_wiki(manifest, remote, args.branch, dry_run=args.dry_run)


def require_valid(manifest: dict) -> None:
    report = run_check(manifest, strict=True)
    if report.errors:
        report.print()
        raise DocError("documentation validation failed; export/publish cancelled")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    subparsers = parser.add_subparsers(dest="command", required=True)

    check_parser = subparsers.add_parser("check", help="validate the bilingual documentation tree")
    check_parser.add_argument("--strict", action="store_true", help="treat warnings as errors")
    check_parser.set_defaults(func=command_check)

    export_parser = subparsers.add_parser("export", help="render the wiki tree")
    export_parser.add_argument("--out", required=True, help="destination directory")
    export_parser.add_argument("--prune", action="store_true",
                               help="delete previously generated pages that are gone")
    export_parser.set_defaults(func=command_export)

    diff_parser = subparsers.add_parser("diff", help="report what a wiki sync would change")
    diff_parser.add_argument("--wiki", help="existing wiki checkout")
    diff_parser.add_argument("--clone", help="wiki remote to clone when --wiki is omitted")
    diff_parser.set_defaults(func=command_diff)

    publish_parser = subparsers.add_parser("publish", help="export and push the wiki")
    publish_parser.add_argument("--remote", help="wiki remote URL")
    publish_parser.add_argument("--branch", help="wiki branch; detected from the remote by default")
    publish_parser.add_argument("--dry-run", action="store_true",
                                help="export and report changes without pushing")
    publish_parser.set_defaults(func=command_publish)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except DocError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    except subprocess.CalledProcessError as exc:
        detail = (exc.stderr or exc.stdout or "").strip().splitlines()
        message = detail[-1] if detail else f"exit code {exc.returncode}"
        print(redact_url(f"error: {' '.join(exc.cmd)} failed: {message}"), file=sys.stderr)
        return 2
    except OSError as exc:
        print(redact_url(f"error: {exc}"), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
