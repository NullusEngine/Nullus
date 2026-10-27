"""Run with installed UE 5.7.4's Python commandlet; never edits source assets."""
import json
from pathlib import Path
import unreal

repo = Path(__file__).resolve().parents[2]
output = repo / "build" / "niagara-reference" / "installed-metadata.json"
output.parent.mkdir(parents=True, exist_ok=True)
registry = unreal.AssetRegistryHelpers.get_asset_registry()
registry.search_all_assets(True)
assets = registry.get_assets_by_path("/Niagara", recursive=True)
rows = []
properties = (
    "usage", "library_visibility", "category", "description", "keywords",
    "provided_dependencies", "required_dependencies", "exposed_version",
    "numeric_output_type_selection_mode", "module_usage_bitmask",
)
for data in sorted(assets, key=lambda item: str(item.package_name)):
    package = str(data.package_name)
    if not any(package.startswith("/Niagara/" + folder + "/") for folder in
               ("Modules", "DynamicInputs", "Functions", "DefaultAssets", "ParameterDefinitions")):
        continue
    row = {"package": package, "class": str(data.asset_class_path.package_name)+"."+str(data.asset_class_path.asset_name),
           "properties": {}, "registryTags": {}, "unavailableProperties": [], "status": "captured-metadata-only"}
    for tag in ("Category", "Description", "Keywords", "ProvidedDependencies", "LibraryVisibility", "ModuleUsageBitmask", "bDeprecated", "bSuggested"):
        try:
            value = data.get_tag_value(tag)
            if value is not None:
                row["registryTags"][tag] = str(value)
        except Exception:
            pass
    asset = data.get_asset()
    if asset is None:
        row["error"] = "Asset could not be loaded"
    else:
        for name in properties:
            try:
                row["properties"][name] = str(asset.get_editor_property(name))
            except Exception as error:
                row["unavailableProperties"].append(name)
    rows.append(row)
    if len(rows) % 50 == 0:
        unreal.log("Nullus reference: captured {} Niagara assets".format(len(rows)))
result = {
    "engineVersion": unreal.SystemLibrary.get_engine_version(),
    "scope": "Niagara core; metadata is not behavioral acceptance",
    "assets": rows,
}
output.write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
unreal.log("Nullus reference: wrote {} assets to {}".format(len(rows), output))
