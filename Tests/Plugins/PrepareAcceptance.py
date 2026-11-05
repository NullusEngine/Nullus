"""Create an isolated native Editor/Player acceptance project in a named root."""
import json
from pathlib import Path
import shutil
import sys

repo = Path(__file__).resolve().parents[2]
root, binary = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
marker = root / ".nullus-plugin-acceptance"
if root.exists() and any(root.iterdir()) and not marker.exists():
    raise SystemExit("Refusing to overwrite a directory not created by this fixture")
root.mkdir(parents=True, exist_ok=True); marker.write_text("Nullus plugin acceptance fixture\n")
(root / "Assets").mkdir(exist_ok=True)
runtime = root / "Plugins/RuntimeSample"; runtime.mkdir(parents=True, exist_ok=True)
source = repo / "Examples/Plugins/RuntimeSample"
shutil.copy2(source / "plugin.json", runtime / "plugin.json")
shutil.copytree(source / "Content", runtime / "Content", dirs_exist_ok=True)
# The normal BuildPipeline requires a real cooked asset in its manifest.
# Keep a small texture as well as the sample's explicitly retained raw text.
pixel = bytearray(21)
pixel[2], pixel[12], pixel[14], pixel[16], pixel[17], pixel[18] = 2, 1, 1, 24, 32, 255
(runtime / "Content/RuntimeIcon.tga").write_bytes(pixel)
manifest = json.loads((runtime / "plugin.json").read_text())
manifest["content"][0]["runtimeKeep"].append("RuntimeIcon.tga")
(runtime / "plugin.json").write_text(json.dumps(manifest, indent=2))
(runtime / "Binaries").mkdir(exist_ok=True)
shutil.copy2(binary, runtime / "Binaries/NullusRuntimeSample.dll")
shutil.copytree(repo / "Examples/Plugins/EditorTools", root / "Plugins/EditorTools", dirs_exist_ok=True)
shutil.copy2(repo / "Tests/Unit/Fixtures/ObjectGraph/simple_scene.objectgraph.json", root / "Assets/Main.scene")
(root / "Assets/Main.scene.meta").write_text("GUID=11111111-2222-4333-8444-555555555555\nASSET_TYPE=scene\nIMPORTER_ID=scene\nIMPORTER_VERSION=1\n")
(root / "PluginAcceptance.nullus").write_text("executable_name=Game\nx_resolution=1280\ny_resolution=720\nfullscreen=false\nstart_scene=Main.scene\nlast_opened_scene=Main.scene\nvsync=true\ngraphics_backend=DX12\ndev_build=true\ncsharp_debugging=false\nlua_panda_debugging=false\nenable_threaded_rendering=true\n")
(root / "Settings").mkdir(exist_ok=True)
(root / "BuildProfiles.json").write_text(json.dumps(dict(schemaVersion=1, defaultProfile="plugins-debug", profiles=[dict(
    id="plugins-debug", displayName="Plugin acceptance", productName="PluginAcceptance", executableName="Game", version="1.0.0", mode="Development",
    target=dict(platform="windows", architecture="x64", graphicsBackend="dx12", artifactPlatform="win64-dx12", shaderTarget="dxil", scriptBackends=["Lua"]),
    startScene="Main.scene", rawAssetExtensions=[".scene", ".prefab"], generateArchive=False)]), indent=2))
print(root)
