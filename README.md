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

meshframe uses C++23, CMake, SDL3, LuaJIT, FFmpeg, FreeType, and libxml2. On macOS
with Homebrew:

```sh
brew install cmake ninja sdl3 luajit ffmpeg freetype libxml2 nlohmann-json fmt googletest pkgconf
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/meshframe_tests
```

The build produces `build/meshframe` and `build/meshframe-cli`.

Transcription (`media.transcribe`, and the `captions` made from it) is
optional. It runs `whisper-cli` from whisper.cpp with a model file from
`~/.cache/meshframe/whisper`: `ggml-large-v3-turbo.bin` (or its `-q8_0` or
`-q5_0` version) when present, else `ggml-base.en.bin`. The large model
spells names and numbers much better, which matters for captions, and takes
about 3 times as long:

```sh
brew install whisper-cpp
mkdir -p ~/.cache/meshframe/whisper
curl -L -o ~/.cache/meshframe/whisper/ggml-large-v3-turbo-q5_0.bin \
  https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo-q5_0.bin
```

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
| Option+Left, Option+Right | Previous or next marker |
| M | Add a marker at the playhead |
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
./build/meshframe-cli schema                         # clip props, effects, eases, Lua API
./build/meshframe-cli clip.add text V2 0 4 title 'props={"text":"Hello"}'
./build/meshframe-cli effect.add title drop_shadow
./build/meshframe-cli prop.expr title y "sin(t * 2) * 20"
./build/meshframe-cli sheet frames.png count=12          # contact sheet
./build/meshframe-cli clip.remove title ripple=true       # pull later clips left
./build/meshframe-cli wait timeout=60                    # next change and its diff
./build/meshframe-cli ui.screenshot window.png
```

Without a running editor, add `--project FILE.mfproj` to edit a file
headless. `meshframe-cli serve FILE.mfproj` keeps a headless session open in
the background so separate calls share one undo history; while any session
has FILE open, `--project FILE` sends commands to it. `quit` stops it. Use `-` to read many commands from stdin in one session, or
`--batch -` to run them as one undo step that has no effect if any command fails.
`examples/demo.sh` builds, renders, and exports a demo project this way.

## Path animation

Path clips hold editable vector geometry. Create rectangles, ellipses, lines,
triangles, or custom SVG path data. In the timeline's video-track menu, add a
shape and edit its properties in the inspector. Use **Import paths** to import
an SVG or a text file containing SVG path data. Imported elements become
separate clips in paint order, with their geometry stored in the project.

The CLI exposes the same objects and undo history:

```sh
./build/meshframe-cli --project shapes.mfproj clip.add path V2 0 4 box 'props={"shape":"rectangle","width":180,"height":100,"fill":"#65d5d0","stroke":"#ffffff","strokeWidth":3}'
./build/meshframe-cli --project shapes.mfproj path.import drawing.svg V3 0 4
./build/meshframe-cli --project shapes.mfproj prop.key box trimEnd 0 0
./build/meshframe-cli --project shapes.mfproj prop.key box trimEnd 2 1 ease-in-out
```

`trimStart` and `trimEnd` reveal a fraction of the stroke by distance. The fill
appears at full reveal. Use a transparent fill (`#00000000`) for line drawings.
`targetPath` and `morph`
interpolate geometry by distance along corresponding contours. Both paths
must have the same number of contours, with matching open or closed status.
Morphing uses sampled geometry, so contour order and direction affect the
result. This is not automatic matching between unrelated illustrations.

Every visual clip can follow a `motionPath`. Animate `motionProgress` from
0 to 1, and set `motionRotate` to 1 to follow the tangent. Path coordinates
are local pixels, with the origin at the frame center and positive y down.
The usual `x`, `y`, `scale`, and `rotation` apply as well. You can use curves,
Lua expressions, and signals for the numeric animation properties:

```sh
./build/meshframe-cli --project shapes.mfproj prop.set box motionPath 'M -200 0 C -100 -150 100 150 200 0'
./build/meshframe-cli --project shapes.mfproj prop.expr box motionProgress 'progress'
./build/meshframe-cli --project shapes.mfproj prop.set box motionRotate 1
```

Path clips use the standard video effect chain, opacity, blend modes,
transitions, timeline edits, bounds inspection, contact sheets, and export.
Lua scripts can declare them with `mf.clip{kind="path", ...}`. Run
`schema clips` and `help path.import` for the current property and command
definitions. See [the agent guide](skills/meshframe/SKILL.md) for import limits
and [examples/path-animation.sh](examples/path-animation.sh) for a complete
SVG import, draw-on, motion, morph, effect, and transition example.

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
