<img src="assets/icon.svg" width="64" height="64" alt="">

# meshframe

meshframe is a video editor built on reactive values. Every clip, effect, and
transition property is a constant, a keyframe curve, or a Lua expression that
can read time, named signals, and other properties. The editor tracks the
dependencies between these values and recomputes only what changes. Lua
scripts can also declare signals, generator clips that draw images, and synth
clips that produce audio.

You can drive it two ways. The GUI is a keyboard-driven, tiling editor. The
CLI, `meshframe-cli`, runs the same commands against a running editor or
headless against a project file, so agents and scripts can edit alongside you.

Created in [T3 Code](https://t3.codes).

## Build

meshframe uses C++23, CMake, SDL3, LuaJIT, FFmpeg, and FreeType. On macOS
with Homebrew:

```sh
brew install cmake ninja sdl3 luajit ffmpeg freetype nlohmann-json fmt googletest pkgconf
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/meshframe_tests
```

The build produces `build/meshframe` and `build/meshframe-cli`.

To build a self-contained `meshframe.app` that runs without Homebrew:

```sh
brew install librsvg
scripts/build-macos-app.sh
```

The script writes `build-app/meshframe.app`. The bundle holds the editor,
`Contents/MacOS/meshframe-cli`, the icon, and copies of the libraries both
programs load. It registers itself as the editor for `.mfproj` files. It has an
ad-hoc signature, so other Macs need a Developer ID signature and notarization
before Gatekeeper will open it.

## Run

```sh
./build/meshframe [--settings FILE] [--socket PATH] [PROJECT.mfproj]
```

The window has 10 spaces, like i3 workspaces. Each space has a layout preset
and remembers which view each pane shows. The status bar at the bottom has
the space buttons, the layout picker, the menu, the project name, and the
time. Each pane header has a view picker. Drag the borders between panes to
resize them. Cmd+K opens the command palette, which finds any action by
name and shows its keys. The menu lists recent projects, and an empty
timeline or media view offers buttons to import media or open a project.

While a project has unsaved changes, the editor keeps a copy of it in
`recovery/` next to the settings file, at most every 10 seconds. If the editor
does not quit normally, the next launch offers to restore the copy.

Views: preview, timeline, media, inspector, video effects, audio effects,
transition curves, script, reactive graph, audio meters, scopes, console,
history, export, and settings.

The UI is dark and grayscale by default. You can change every color, the
fonts, the scale, and every key binding in the settings view (Cmd+,). Settings
are saved to `settings.json` in the SDL preference directory
(`~/Library/Application Support/meshframe/meshframe/` on macOS) or to the path
in `MESHFRAME_SETTINGS`.

### Default keys

| Keys | Action |
| --- | --- |
| Option+1 … Option+0 | Switch to space 1–10 |
| Option+Shift+1 … Option+Shift+0 | Swap the current space with that space and follow it |
| Space | Play or pause |
| Left, Right | Previous or next frame |
| Shift+Left, Shift+Right | Back or forward one second |
| Home, End | Go to start or end |
| Cmd+L | Toggle loop |
| Cmd+Z, Cmd+Shift+Z | Undo, redo |
| Cmd+B | Split at the playhead |
| Backspace, Delete | Delete the selection |
| Cmd+A, Escape | Select all clips, clear the selection |
| Cmd+N, Cmd+O, Cmd+S, Cmd+Shift+S | New, open, save, save as |
| Cmd+I | Import media |
| Cmd+E | Show export |
| Cmd+=, Cmd+-, Cmd+0 | Zoom the timeline in, out, or to fit |
| Cmd+Enter | Apply the script |
| Cmd+K | Command palette |
| Cmd+, | Show settings |
| Cmd+Q | Quit |

## CLI

The running editor listens on a Unix socket in `$MESHFRAME_RUNTIME_DIR` or
`/tmp/meshframe-<uid>`. The CLI finds it automatically:

```sh
./build/meshframe-cli sessions                       # running editors
./build/meshframe-cli help                           # every command
./build/meshframe-cli clip.add text V2 0 4 title 'props={"text":"Hello"}'
./build/meshframe-cli prop.expr title y "sin(t * 2) * 20"
./build/meshframe-cli ui.screenshot window.png
```

Without a running editor, add `--project FILE.mfproj` to edit a file
headless. Use `-` to read many commands from stdin in one session.
`examples/demo.sh` builds, renders, and exports a demo project this way.

## Agent skill

`skills/meshframe/SKILL.md` teaches an agent to use the CLI: finding a
session, the command reference, the project model, the expression and
scripting API, and how to check its work by rendering frames. To make it
available to Claude Code in every project, link it into your skills
directory:

```sh
ln -s "$PWD/skills/meshframe" ~/.claude/skills/meshframe
```

## Source layout

| Directory | Contents |
| --- | --- |
| `src/core` | Values, curves, and the reactive graph |
| `src/model` | Project data, schema, edits, undo history, and file format |
| `src/script` | LuaJIT runtime and scripting API |
| `src/engine` | Evaluator, frame renderer, and player |
| `src/render`, `src/audio`, `src/media`, `src/exporter` | Compositing, mixing, FFmpeg decoding, and export |
| `src/session` | Session state, commands, and the socket server |
| `src/ui` | Immediate-mode UI, workspace, shell, and views |
| `src/app`, `src/cli` | The editor and CLI executables |
