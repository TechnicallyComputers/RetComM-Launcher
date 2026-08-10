# imgui_freetype

Vendored from [ocornut/imgui](https://github.com/ocornut/imgui) `v1.91.9b`
(`misc/freetype/`), MIT license — see upstream `LICENSE.txt`.

Path kept as `third_party/misc/freetype/` so `#include "misc/freetype/imgui_freetype.h"`
matches stock Dear ImGui (`imgui_draw.cpp`).

Used by `retcomm-hub` when FreeType is available so CBDT/CBLC color emoji fonts
(e.g. Noto Color Emoji) can be merged into the UI atlas via
`ImGuiFreeTypeBuilderFlags_LoadColor`.
