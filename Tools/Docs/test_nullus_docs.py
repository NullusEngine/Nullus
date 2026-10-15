"""Regression tests for documentation checks and wiki publishing.

Run with: python -m unittest discover -s Tools/Docs -p "test_*.py" -v
All Git remotes are disposable local bare repositories.
"""

import argparse
import contextlib
import copy
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import nullus_docs as docs


class DocumentationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="nullus-docs-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.manifest = {
            "repository": "example/engine",
            "entryPage": "start/overview",
            "languages": [
                {"id": "zh-CN", "dir": "zh-CN", "label": "中文", "wikiSuffix": "zh-CN"},
                {"id": "en", "dir": "en", "label": "English", "wikiSuffix": "en"},
            ],
            "chapters": [{"id": "start", "title": {"zh-CN": "入门", "en": "Start"}, "pages": [
                {"id": "overview", "title": {"zh-CN": "概览", "en": "Overview"}},
                {"id": "details", "title": {"zh-CN": "详情", "en": "Detail Reference"}},
            ]}],
        }
        for language in self.manifest["languages"]:
            for chapter, page in docs.iter_pages(self.manifest):
                path = self.root / docs.page_rel_path(language, chapter, page)
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(f"# {page['title'][language['id']]}\n\n"
                                "This page explains the prerequisites, steps, expected results, "
                                "and limitations of the current workflow.\n\n## Details\n\n"
                                "Follow the linked reference for implementation details.\n", encoding="utf-8")
        (self.root / "Docs/README.md").write_text("# Index\n", encoding="utf-8")
        (self.root / "Docs/nav.json").write_text(json.dumps(self.manifest), encoding="utf-8")
        self.source = self.root / "Docs/en/start/overview.md"
        self.wiki = self.root / "wiki"
        patches = patch.multiple(docs, ROOT=self.root, DOCS_DIR=self.root / "Docs",
                                 MANIFEST_PATH=self.root / "Docs/nav.json")
        patches.start()
        self.addCleanup(patches.stop)
        # Do not inherit a user's credentials, hooks, or line-ending policy.
        environment = patch.dict(os.environ, {
            "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_TERMINAL_PROMPT": "0",
        })
        environment.start()
        self.addCleanup(environment.stop)

    def check(self):
        with contextlib.redirect_stdout(io.StringIO()):
            return docs.run_check(self.manifest, strict=True)

    def append(self, text):
        self.source.write_text(self.source.read_text(encoding="utf-8") + text, encoding="utf-8")

    def rewrite(self, text):
        return docs.rewrite_links(self.manifest, self.manifest["languages"][1], self.source,
                                  text, docs.build_page_lookup(self.manifest))

    def export(self):
        return docs.export_wiki(self.manifest, self.wiki, prune=True)

    def test_current_fixture_passes(self):
        self.assertEqual(self.check().errors, [])

    def test_duplicate_page_id_is_rejected(self):
        pages = self.manifest["chapters"][0]["pages"]
        pages.append(copy.deepcopy(pages[0]))
        with self.assertRaisesRegex(docs.DocError, "duplicate page id"):
            docs.validate_manifest_shape(self.manifest)

    def test_missing_language_title_is_rejected(self):
        del self.manifest["chapters"][0]["pages"][0]["title"]["en"]
        with self.assertRaisesRegex(docs.DocError, "no title for en"):
            docs.validate_manifest_shape(self.manifest)

    def test_colliding_wiki_suffix_is_rejected(self):
        self.manifest["languages"][1]["wikiSuffix"] = "ZH-cn"
        with self.assertRaisesRegex(docs.DocError, "duplicate wiki page name"):
            docs.validate_manifest_shape(self.manifest)

    def test_hyphenated_chapter_page_name_collision_is_rejected(self):
        chapter = copy.deepcopy(self.manifest["chapters"][0])
        chapter["id"] = "start-overview"
        chapter["pages"] = [{"id": "details", "title": {"zh-CN": "详情", "en": "Details"}}]
        self.manifest["chapters"][0]["pages"].append(
            {"id": "overview-details", "title": {"zh-CN": "详情", "en": "Details"}})
        self.manifest["chapters"].append(chapter)
        with self.assertRaisesRegex(docs.DocError, "duplicate wiki page name"):
            docs.validate_manifest_shape(self.manifest)

    def test_path_traversal_in_manifest_is_rejected(self):
        self.manifest["languages"][0]["dir"] = "../outside"
        with self.assertRaisesRegex(docs.DocError, "invalid language dir"):
            docs.validate_manifest_shape(self.manifest)

    def test_missing_translation_and_empty_page_are_rejected(self):
        self.source.unlink()
        self.assertTrue(any("page file is missing" in error for error in self.check().errors))
        self.source.write_text("# Overview\n", encoding="utf-8")
        self.assertTrue(any("no substantive content" in error for error in self.check().errors))

    def test_duplicate_anchor_is_rejected_but_fenced_headings_are_ignored(self):
        self.append("\n````markdown\n## Details\n```\n## Details\n````\n")
        self.assertEqual(self.check().errors, [])
        self.append("\n## Details\n")
        self.assertTrue(any("duplicate heading anchor" in error for error in self.check().errors))

    def test_local_and_cross_page_anchors_are_checked(self):
        self.append("\n[local](#missing) [other](details.md#absent)\n")
        errors = self.check().errors
        self.assertEqual(sum("heading anchor does not exist" in error for error in errors), 2)

    def test_valid_anchors_and_inline_code_are_supported(self):
        self.append("\n[local](#details) [other](details.md#details) `[example](missing.md)`\n")
        self.assertEqual(self.check().errors, [])

    def test_missing_images_and_source_files_are_rejected(self):
        self.append("\n![image](../../Screenshots/missing.png) [source](../../../missing.cpp)\n")
        self.assertEqual(sum("link target does not exist" in error for error in self.check().errors), 2)

    def test_readme_links_are_checked(self):
        (self.root / "Docs/README.md").write_text("# Index\n[bad](en/start/overview.md#missing)", encoding="utf-8")
        self.assertTrue(any("Docs/README.md:2" in error for error in self.check().errors))

    def test_link_titles_images_unicode_source_and_home_are_rewritten(self):
        image = self.root / "Docs/Screenshots/测试 image.png"
        image.parent.mkdir()
        image.write_bytes(b"image")
        source = self.root / "Runtime/source.cpp"
        source.parent.mkdir()
        source.write_text("// source\n", encoding="utf-8")
        text = ('[details](details.md#details "tip")\n'
                '![image](<../../Screenshots/测试 image.png> "preview")\n'
                '[source](../../../Runtime/source.cpp)\n[index](../../README.md)\n')
        rendered = self.rewrite(text)
        self.assertIn('[details](en-start-details#details "tip")', rendered)
        self.assertIn('%E6%B5%8B%E8%AF%95%20image.png "preview")', rendered)
        self.assertIn('https://github.com/example/engine/blob/main/Runtime/source.cpp', rendered)
        self.assertIn('[index](Home)', rendered)

    def test_code_examples_are_never_rewritten(self):
        text = ('```markdown\n[details](details.md)\n```\n'
                '~~~markdown\n[details](details.md)\n~~~\n'
                '`[details](details.md)` [details](details.md)\n')
        rendered = self.rewrite(text)
        self.assertEqual(rendered.count('[details](details.md)'), 3)
        self.assertEqual(rendered.count('[details](en-start-details)'), 1)

    def test_export_has_bilingual_navigation_and_language_switch(self):
        files = self.export()
        self.assertIn("en-start-overview.md", files)
        page = (self.wiki / "en-start-overview.md").read_text(encoding="utf-8")
        self.assertIn('[中文](zh-CN-start-overview)', page)
        for name in ("Home.md", "_Sidebar.md"):
            navigation = (self.wiki / name).read_text(encoding="utf-8")
            self.assertIn("en-start-overview", navigation)
            self.assertIn("zh-CN-start-overview", navigation)

    def test_export_is_idempotent_and_prunes_only_managed_files(self):
        self.export()
        snapshot = {path.name: path.read_bytes() for path in self.wiki.iterdir()}
        self.export()
        self.assertEqual(snapshot, {path.name: path.read_bytes() for path in self.wiki.iterdir()})
        (self.wiki / "Manual.md").write_text("manual", encoding="utf-8")
        (self.wiki / "Stale.md").write_text("stale", encoding="utf-8")
        manifest_path = self.wiki / docs.GENERATED_MANIFEST_NAME
        previous = json.loads(manifest_path.read_text(encoding="utf-8"))
        previous["files"].append("Stale.md")
        manifest_path.write_text(json.dumps(previous), encoding="utf-8")
        self.export()
        self.assertFalse((self.wiki / "Stale.md").exists())
        self.assertEqual((self.wiki / "Manual.md").read_text(), "manual")

    def test_corrupt_and_escaping_generated_manifest_fail_before_writing(self):
        self.wiki.mkdir()
        manifest_path = self.wiki / docs.GENERATED_MANIFEST_NAME
        for content in ("{invalid", json.dumps({"generator": "Tools/Docs/nullus_docs.py", "files": ["../outside.md"]})):
            manifest_path.write_text(content, encoding="utf-8")
            with self.assertRaisesRegex(docs.DocError, "invalid generated manifest"):
                self.export()
            self.assertEqual(len(list(self.wiki.iterdir())), 1)

    def test_diff_is_read_only_and_reports_deletions(self):
        self.export()
        page = self.wiki / "en-start-overview.md"
        page.write_text("local unsaved edit\n", encoding="utf-8")
        manifest_path = self.wiki / docs.GENERATED_MANIFEST_NAME
        previous = json.loads(manifest_path.read_text(encoding="utf-8"))
        previous["files"].append("Stale.md")
        manifest_path.write_text(json.dumps(previous), encoding="utf-8")
        (self.wiki / "Stale.md").write_text("old page", encoding="utf-8")
        before = {path.name: path.read_bytes() for path in self.wiki.iterdir()}
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(docs.command_diff(argparse.Namespace(wiki=str(self.wiki), clone=None)), 0)
        self.assertIn("modified: en-start-overview.md", output.getvalue())
        self.assertIn("deleted: Stale.md", output.getvalue())
        self.assertEqual(before, {path.name: path.read_bytes() for path in self.wiki.iterdir()})

    def test_invalid_source_blocks_export(self):
        self.append("\n[bad](missing.md)\n")
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaisesRegex(docs.DocError, "validation failed"):
                docs.command_export(argparse.Namespace(out=str(self.wiki), prune=True))
        self.assertFalse(self.wiki.exists())

    def run_git(self, *args, cwd=None):
        result = subprocess.run(["git", *map(str, args)], cwd=cwd or self.root,
                                capture_output=True, text=True, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.strip()

    def init_remote(self, branch="trunk"):
        remote = self.root / "remote.git"
        self.run_git("init", "--bare", f"--initial-branch={branch}", remote)
        seed = self.root / "seed"
        self.run_git("clone", remote, seed)
        self.run_git("config", "user.name", "Test", cwd=seed)
        self.run_git("config", "user.email", "test@example.invalid", cwd=seed)
        (seed / "Manual.md").write_text("manual page\n", encoding="utf-8")
        self.run_git("add", "Manual.md", cwd=seed)
        self.run_git("commit", "-m", "initial wiki", cwd=seed)
        self.run_git("push", "origin", branch, cwd=seed)
        return remote, seed

    def publish(self, remote, dry_run=False):
        with contextlib.redirect_stdout(io.StringIO()):
            return docs.publish_wiki(self.manifest, str(remote), docs.detect_wiki_branch(str(remote)), dry_run)

    def test_publish_preserves_history_and_second_sync_creates_no_commit(self):
        remote, seed = self.init_remote()
        before = self.run_git("rev-parse", "HEAD", cwd=remote)
        self.assertEqual(docs.detect_wiki_branch(str(remote)), "trunk")
        self.assertEqual(self.publish(remote), 0)
        after = self.run_git("rev-parse", "HEAD", cwd=remote)
        self.assertNotEqual(before, after)
        self.assertEqual(self.run_git("rev-parse", "HEAD^", cwd=remote), before)
        self.assertEqual(self.run_git("show", "HEAD:Manual.md", cwd=remote), "manual page")
        self.assertEqual(self.publish(remote), 0)
        self.assertEqual(self.run_git("rev-parse", "HEAD", cwd=remote), after)

    def test_master_branch_and_dry_run(self):
        remote, seed = self.init_remote("master")
        before = self.run_git("rev-parse", "HEAD", cwd=remote)
        self.assertEqual(docs.detect_wiki_branch(str(remote)), "master")
        self.assertEqual(self.publish(remote, dry_run=True), 0)
        self.assertEqual(self.run_git("rev-parse", "HEAD", cwd=remote), before)

    def test_publish_normalizes_legacy_crlf_and_is_stable_across_host_policies(self):
        remote, seed = self.init_remote()
        generated = docs.export_wiki(self.manifest, seed, prune=True)
        for name in generated:
            path = seed / name
            path.write_bytes(path.read_bytes().replace(b"\n", b"\r\n"))
        self.run_git("-c", "core.autocrlf=false", "add", "-A", cwd=seed)
        self.run_git("commit", "-m", "legacy CRLF wiki", cwd=seed)
        self.run_git("push", "origin", "trunk", cwd=seed)
        legacy_head = self.run_git("rev-parse", "HEAD", cwd=remote)

        with patch.dict(os.environ, {"GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "core.autocrlf",
                                     "GIT_CONFIG_VALUE_0": "true"}):
            self.assertEqual(self.publish(remote), 0)
            normalized_head = self.run_git("rev-parse", "HEAD", cwd=remote)
            self.assertNotEqual(normalized_head, legacy_head)
            for name in generated:
                blob = subprocess.run(["git", "show", f"HEAD:{name}"], cwd=remote,
                                      capture_output=True, check=True).stdout
                self.assertNotIn(b"\r", blob, name)
            self.assertEqual(self.publish(remote), 0)
            self.assertEqual(self.run_git("rev-parse", "HEAD", cwd=remote), normalized_head)

        self.assertEqual(self.publish(remote), 0)
        self.assertEqual(self.run_git("rev-parse", "HEAD", cwd=remote), normalized_head)

    def test_clone_failure_is_an_error(self):
        with self.assertRaisesRegex(docs.DocError, "clone failed"):
            self.publish(self.root / "missing.git")

    def test_explicit_branch_is_selected_and_missing_branch_fails(self):
        remote, seed = self.init_remote()
        default_head = self.run_git("rev-parse", "HEAD", cwd=remote)
        self.run_git("checkout", "-b", "release", cwd=seed)
        self.run_git("push", "origin", "release", cwd=seed)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(docs.publish_wiki(self.manifest, str(remote), "release", False), 0)
        self.assertEqual(self.run_git("rev-parse", "HEAD", cwd=remote), default_head)
        self.assertNotEqual(self.run_git("rev-parse", "release", cwd=remote), default_head)
        with self.assertRaisesRegex(docs.DocError, "checkout"):
            docs.publish_wiki(self.manifest, str(remote), "missing", False)

    def test_default_branch_is_used_when_override_is_absent(self):
        remote, seed = self.init_remote("master")
        before = self.run_git("rev-parse", "HEAD", cwd=remote)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(docs.publish_wiki(self.manifest, str(remote), None, False), 0)
        self.assertNotEqual(self.run_git("rev-parse", "HEAD", cwd=remote), before)

    def test_rejected_push_is_an_error_and_remote_is_unchanged(self):
        remote, seed = self.init_remote()
        before = self.run_git("rev-parse", "HEAD", cwd=remote)
        (remote / "hooks/pre-receive").write_text("#!/bin/sh\necho 'acceptance rejection' >&2\nexit 1\n", encoding="utf-8")
        (remote / "hooks/pre-receive").chmod(0o755)
        with self.assertRaisesRegex(docs.DocError, "acceptance rejection"):
            self.publish(remote)
        self.assertEqual(self.run_git("rev-parse", "HEAD", cwd=remote), before)

    def test_concurrent_remote_update_is_not_overwritten(self):
        remote, seed = self.init_remote()
        original_git = docs.git
        competitor_head = []

        def race(args, cwd=None):
            if args[0] == "push":
                (seed / "Concurrent.md").write_text("concurrent change\n", encoding="utf-8")
                self.run_git("add", "Concurrent.md", cwd=seed)
                self.run_git("commit", "-m", "concurrent update", cwd=seed)
                self.run_git("push", "origin", "trunk", cwd=seed)
                competitor_head.append(self.run_git("rev-parse", "HEAD", cwd=seed))
            return original_git(args, cwd=cwd)

        with patch.object(docs, "git", side_effect=race):
            with self.assertRaisesRegex(docs.DocError, "push"):
                self.publish(remote)
        self.assertEqual(self.run_git("rev-parse", "HEAD", cwd=remote), competitor_head[0])
        self.assertEqual(self.run_git("show", "HEAD:Concurrent.md", cwd=remote), "concurrent change")
        self.assertEqual(self.publish(remote), 0)

    def test_errors_redact_url_credentials(self):
        secret_url = "https://x-access-token:do-not-print@github.com/example/wiki.git"
        result = subprocess.CompletedProcess([], 1, "", f"fatal: cannot push to {secret_url}")
        with patch.object(docs.subprocess, "run", return_value=result):
            with self.assertRaises(docs.DocError) as caught:
                docs.git(["push", "origin", "main"])
        self.assertNotIn("do-not-print", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
