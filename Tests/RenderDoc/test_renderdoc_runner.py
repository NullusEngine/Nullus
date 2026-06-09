import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_DIR = REPO_ROOT / "Tools" / "RenderDoc"
sys.dont_write_bytecode = True
sys.path.insert(0, str(TOOLS_DIR))

import renderdoc_runner  # noqa: E402


class RenderDocRunnerTests(unittest.TestCase):
    def test_default_capture_dir_uses_project_logs_for_project_file(self):
        project_file = REPO_ROOT / "TestProject" / "TestProject.nullus"

        capture_dir = renderdoc_runner.resolve_capture_dir(project_file, "Editor", "dx12", None, {})

        self.assertEqual(capture_dir, REPO_ROOT / "TestProject" / "Logs" / "RenderDoc" / "Editor" / "dx12")

    def test_default_capture_dir_uses_project_logs_for_project_root(self):
        project_root = REPO_ROOT / "TestProject"

        capture_dir = renderdoc_runner.resolve_capture_dir(project_root, "Game", "vulkan", None, {})

        self.assertEqual(capture_dir, project_root / "Logs" / "RenderDoc" / "Game" / "vulkan")

    def test_explicit_capture_dir_overrides_project_log_default(self):
        project_root = REPO_ROOT / "TestProject"
        explicit_capture_dir = REPO_ROOT / "Build" / "CustomCaptures"

        capture_dir = renderdoc_runner.resolve_capture_dir(
            project_root,
            "Editor",
            "dx12",
            str(explicit_capture_dir),
            {},
        )

        self.assertEqual(capture_dir, explicit_capture_dir)

    def test_environment_capture_dir_overrides_project_log_default(self):
        project_root = REPO_ROOT / "TestProject"
        environment_capture_dir = REPO_ROOT / "Build" / "EnvironmentCaptures"

        capture_dir = renderdoc_runner.resolve_capture_dir(
            project_root,
            "Editor",
            "dx12",
            None,
            {"NLS_RENDERDOC_CAPTURE_DIR": str(environment_capture_dir)},
        )

        self.assertEqual(capture_dir, environment_capture_dir)

    def test_unknown_capture_target_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Unsupported RenderDoc target"):
            renderdoc_runner.resolve_capture_target_name("launcher")

    def test_game_without_project_does_not_default_launch_to_test_project(self):
        self.assertIsNone(renderdoc_runner.resolve_launch_project("game", None))

    def test_editor_without_project_launches_default_test_project(self):
        self.assertEqual(
            renderdoc_runner.resolve_launch_project("editor", None),
            REPO_ROOT / "TestProject" / "TestProject.nullus",
        )

    def test_game_capture_dir_can_use_environment_project_without_launch_override(self):
        project_file = REPO_ROOT / "TestProject" / "TestProject.nullus"

        capture_project = renderdoc_runner.resolve_capture_project(
            "game",
            None,
            {"NLS_PROJECT_FILE": str(project_file)},
        )

        self.assertEqual(capture_project, project_file)


if __name__ == "__main__":
    unittest.main()
