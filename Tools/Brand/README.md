# Nullus branding

- `NullusLogo.svg` at the repository root is the complete logo used by both READMEs.
- `App/Assets/Engine/Brand/NullusLogoMark.svg` is the application icon source.
- `App/Assets/Editor/Brand/NullusLogoMark.svg` exposes the same mark to editor resources.

These SVG files contain filled vector paths, including the wordmark. They have
transparent backgrounds and do not require an installed font or a raster image.
`NLS::Image` decodes SVG files and encoded SVG buffers into RGBA pixels for
window icons and texture uploads.

Windows executable resources require ICO data. CMake builds `NullusBrandIconBaker`
and generates `build/brand/NullusAppIcon.ico` from the engine SVG before compiling
the Editor, Game and Launcher resource files. The resource is named `GLFW_ICON`,
which GLFW uses for native window and detached viewport icons. The generated ICO contains
16, 24, 32, 48, 64, 128 and 256 pixel images. It is a build artifact; do not add
hand-edited PNG or ICO logo sources to the repository.

To regenerate only the Windows resource icon:

```powershell
cmake --build build --config Debug --target NullusBrandAssets
```

The runtime decoder and the icon baker share `Runtime/Base/Detail/SvgRasterizer.cpp`
and the pinned NanoSVG headers in `ThirdParty/nanosvg`. No dependency download is
needed during configuration or compilation.
