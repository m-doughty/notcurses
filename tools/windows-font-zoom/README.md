# Windows physical rendering regression

Run from PowerShell with the MSYS2 UCRT compiler/dependencies and WezTerm on
PATH, against a completed local notcurses CMake build:

```powershell
./tools/windows-font-zoom/run.ps1 -BuildDir ./build -Mode text
./tools/windows-font-zoom/run.ps1 -BuildDir ./build -Mode images
./tools/windows-font-zoom/run.ps1 -BuildDir ./build -Mode images -FixedGrid
```

Each run opens an isolated WezTerm process for about 20 seconds. It never
changes your normal configuration. Unique output directories retain expected
text and the actual text from WezTerm's pane, after both ConPTY and WezTerm
have processed the output. A missing capture or any mismatch fails the run.

The text fixture reproduces resize damage without images; its final explicit
refresh demonstrates whether physical corruption can be repaired from the
logical framebuffer. The image fixture uses Selkie's explicit crop-plane
path, scrolls color bands with a distinctive final column, and decodes every
emitted Sixel pixel for exact comparison (accounting for the encoder's 99%
palette ceiling). Pixel support is required, never silently skipped.

These checks cover native output and terminal text. They cannot inspect the
terminal GPU's image composition; visual checks in the full application
remain useful, especially for occlusion, modal dialogs and rapid repeated zoom.
