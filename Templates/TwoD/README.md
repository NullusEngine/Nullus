# 2D Core template

Select **2D Core** in the Launcher. The template starts with an orthographic camera and a Sprite sample. New textures inherit Single Sprite import settings, PPU 100 and no mipmaps. Existing texture metadata is preserved.

In the Project Browser expand an imported image to see its Sprite child. Drag that child into the 2D Scene View or Hierarchy, then edit Sprite, Color, Flip and Order in Layer in the Inspector. A drop is one subtree Undo operation and its reference uses GUID plus stable child file ID.

The Sprite Editor, Atlas, Pixel Perfect, Physics2D, Tilemap and advanced animation remain later implementation phases; see Docs/Plans/2D/Progress.md for current verification status.

制作入口、属性语义和发布验证命令见 [2D Core 入门](../../Docs/zh-CN/2DGettingStarted.md)。
