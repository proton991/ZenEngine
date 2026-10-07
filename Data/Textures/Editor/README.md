# Editor icon

`zen_engine.png` is the supplied source artwork, including its transparency.
`zen_engine_menu.png` is its 64 × 64 UI copy. `zen_engine.ico` contains 16, 20,
24, 32, 40, 48, 64, 128 and 256 pixel versions for Windows.

The menu texture uses the engine texture path. The Windows resource embeds the
ICO in `zen_editor.exe` for Explorer, the taskbar and native windows, with the
resource name recognized by both SDL and GLFW.

On macOS, `ZenEditor/App/ZenEditor.icns` is generated from the same source artwork
and includes standard and Retina representations from 16 to 1024 pixels. CMake
copies it to `ZenEditor.app/Contents/Resources`, and the app's `Info.plist` selects
it for Finder and the Dock. The editor also applies this artwork to the running
application so direct IDE and terminal launches show the same Dock icon.
