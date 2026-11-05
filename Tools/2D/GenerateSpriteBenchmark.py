"""Generate a reproducible Sprite benchmark from a 2D Core project's scene."""

import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import random
import uuid


def generate(project: Path, count: int, seed: int, force: bool) -> Path:
    root = project.parent if project.suffix == ".nullus" else project
    assets = root.resolve() / "Assets"
    source = assets / "Scenes/Main.scene"
    destination = assets / "Scenes/SpriteBenchmark.scene"
    if not 1 <= count <= 100000:
        raise ValueError("count must be between 1 and 100000")
    if destination.exists() and not force:
        raise FileExistsError(f"{destination} already exists; choose --force to replace this benchmark")
    source_bytes = source.read_bytes()
    scene = json.loads(source_bytes.decode("utf-8-sig"))
    if scene.get("format") != "Nullus.ObjectGraph.Scene" or scene.get("version") != 2:
        raise ValueError("Main.scene must use the supported ObjectGraph Scene v2 format")
    if scene.get("prefabInstances"):
        raise ValueError("Use a 2D Core scene with unpacked Sprite objects for this benchmark")
    objects = {item["id"]: item for item in scene["objects"]}
    root_object = objects[scene["root"]]
    sprites = [item for item in objects.values() if item["type"].endswith("::SpriteRendererComponent")]
    if not sprites:
        raise ValueError("Main.scene does not contain a SpriteRendererComponent")
    removed = set()
    templates = []
    for sprite in sprites:
        actor = next((item for item in objects.values() if item["type"] == "NLS::Engine::GameObject" and
                      any(ref.get("$owned") == sprite["id"] for ref in item["properties"]["components"])), None)
        if actor is None:
            raise ValueError("SpriteRendererComponent has no owning GameObject")
        components = [objects[ref["$owned"]] for ref in actor["properties"]["components"]]
        transform = next((item for item in components if item["type"].endswith("::TransformComponent")), None)
        properties = actor["properties"]
        if len(components) != 2 or transform is None or properties.get("children") or (properties.get("parent") or {}).get("$ref"):
            raise ValueError("Use root Sprite objects containing only Transform and SpriteRenderer components")
        removed.update((sprite["id"], actor["id"], transform["id"]))
        templates.append((transform, sprite, actor))
    transform, sprite, actor = templates[0]

    def references_removed(value):
        if isinstance(value, dict):
            reference = value.get("$ref")
            return (isinstance(reference, str) and reference in removed) or any(references_removed(child) for child in value.values())
        return isinstance(value, list) and any(references_removed(child) for child in value)

    if any(references_removed(item["properties"]) for item in objects.values() if item["id"] not in removed):
        raise ValueError("A retained object references a Sprite being replaced; use a plain 2D Core scene")
    namespace = uuid.UUID(scene["documentId"])
    randomizer = random.Random(seed)
    columns = math.ceil(math.sqrt(count))
    rows = math.ceil(count / columns)
    spacing = 0.72
    scene["objects"] = [item for item in scene["objects"] if item["id"] not in removed]
    root_object["properties"]["gameObjects"] = [ref for ref in root_object["properties"]["gameObjects"]
                                                if ref.get("$owned") not in removed]
    first_file_id = max(int(item.get("fileID", 0)) for item in scene["objects"]) + 1
    for camera in scene["objects"]:
        if camera["type"].endswith("::CameraComponent"):
            camera["properties"]["projectionMode"] = "ORTHOGRAPHIC"
            camera["properties"]["size"] = max(columns, rows) * spacing * 0.6
    for index in range(count):
        name = f"Sprite {index:05d}"
        generated = [copy.deepcopy(item) for item in (transform, sprite, actor)]
        for offset, item in enumerate(generated):
            item["id"] = str(uuid.uuid5(namespace, f"benchmark:{seed}:{index}:{offset}"))
            item["fileID"] = first_file_id + index * 3 + offset
            item["debugName"] = name
        pose, renderer, instance = generated
        pose["properties"]["localPosition"] = {
            "x": (index % columns - (columns - 1) / 2) * spacing,
            "y": (index // columns - (rows - 1) / 2) * spacing,
            "z": 0,
        }
        renderer["properties"]["color"] = {"x": randomizer.uniform(0.65, 1),
                                             "y": randomizer.uniform(0.65, 1), "z": 1, "w": 1}
        renderer["properties"]["orderInLayer"] = 0
        instance["properties"]["name"] = name
        instance["properties"]["components"] = [{"$owned": pose["id"]}, {"$owned": renderer["id"]}]
        scene["objects"].extend(generated)
        root_object["properties"]["gameObjects"].append({"$owned": instance["id"]})
    scene["documentId"] = str(uuid.uuid5(namespace, f"benchmark-document:{seed}:{count}"))
    destination.write_text(json.dumps(scene, separators=(",", ":")), encoding="utf-8")
    destination.with_suffix(".scene.meta").write_text(
        f"GUID={uuid.uuid5(namespace, 'benchmark-asset')}\nIMPORTER_ID=scene\nIMPORTER_VERSION=1\nASSET_TYPE=scene\n",
        encoding="utf-8",
    )
    destination.with_suffix(".benchmark.json").write_text(json.dumps({
        "version": 1, "seed": seed, "count": count, "columns": columns, "rows": rows,
        "spacing": spacing, "expectedSingleAtlasDraws": 1,
        "sourceSceneSha256": hashlib.sha256(source_bytes).hexdigest(),
        "sourceSpriteCount": len(sprites),
        "validation": "Unmeasured: open in Editor/Game, record visible/culled counts and CPU/GPU quantiles.",
    }, indent=2), encoding="utf-8")
    return destination


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--count", default=10000, type=int)
    parser.add_argument("--seed", default=12345, type=int)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    print(generate(args.project, args.count, args.seed, args.force))
