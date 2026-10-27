# Niagara 5.7.4 reference

Open `Niagara57.uproject` with installed UE 5.7.4 CL 51494982. The source checkout
with changelist 0 is explanatory reference, not the executable acceptance build.
Use the default dark theme, English labels, a reset Niagara workspace and the
same window dimensions for each capture. Record OS scale and UE application
scale separately. Capture 100%, 150% and 200% DPI; an ImGui draw test alone is
not screenshot or interaction acceptance.

Reference fixtures use Y-up and native Nullus world units at the comparison
boundary. Keep the conversion with each fixture; do not change Nullus engine
coordinate conventions or silently compare UE centimeters to native values.

`Tools/VFX/Export-NiagaraParity.ps1` freezes the installed content, editor commands,
panels and native capability declarations into `Docs/Plans/NiagaraParity.json`.
This is the starting inventory for the acceptance matrix. Individual properties,
defaults, data-interface function signatures, visibility predicates and execution
results still require reflection/behavior capture. No inventory entry is marked
verified automatically. `Tools/VFX/Test-NiagaraParity.ps1` intentionally returns
failure while any requirement is unverified and forbids default cutover then.

The reference project and its generated `Saved/`, `Intermediate/` and
`DerivedDataCache/` are independent of user projects. Screenshots and traces
belong in build artifacts with their exact reference case and test identifier.
