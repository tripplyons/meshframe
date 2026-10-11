---
name: meshframe
description: Edit video and audio in meshframe through its CLI, either in a running editor session or headless on a project file. Use when asked to build, change, inspect, render, or export a meshframe project, or to drive the meshframe GUI.
---

# meshframe

meshframe is a video editor in which every property is reactive. A property is
a constant, a keyframe curve, or a Lua expression, and expressions can read
time, signals, and other properties. The editor keeps a dependency graph of
these values and recomputes only what changed. `meshframe-cli` runs the same
commands as the GUI, so anything you do through it shows up live in an open
editor, with undo.

## Setup

Build from the repository root (macOS; dependencies come from Homebrew):

```sh
brew install cmake ninja sdl3 luajit ffmpeg freetype nlohmann-json fmt googletest pkgconf
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This produces `build/meshframe` (the editor) and `build/meshframe-cli`.

## Choose a session

- If the user has the editor open, use that session. `meshframe-cli sessions`
  lists running editors with their pid, project path, and dirty state. With
  one session, the CLI connects to it automatically. With several, pass
  `--session PID`.
- With no editor open, work headless: `meshframe-cli --project FILE.mfproj CMD`.
  The file is created if missing and saved after each change (`--no-save`
  prevents this). GUI commands (`ui.*`) need a running editor.
- To start an editor yourself, run `build/meshframe FILE.mfproj &`, then wait
  until `meshframe-cli sessions` lists it. Stop only editors you started, with
  `meshframe-cli ui.action app.quit`. Save first if the user should keep the
  changes.

Each headless invocation is a new session, so `undo` cannot reach changes
from an earlier invocation. To share one session and history, send many
requests through one process with `-`:

```sh
meshframe-cli --project demo.mfproj - <<'EOF'
clip.add text V2 0 4 title 'props={"text":"Hello"}'
prop.key title opacity 0 0
undo
EOF
```

Each stdin line is either words, split like a shell command line, or a JSON
request `{"cmd": "...", "args": {...}, "id": ...}`. Lines starting with `#`
are comments. Each reply is printed as one JSON line: `{"ok":true,"result":...}`
or `{"ok":false,"error":"..."}`, plus the `id` you sent.

## Calling commands

```sh
meshframe-cli COMMAND ARG... [name=value...]
meshframe-cli help              # every command with its usage line
meshframe-cli help clip.add     # one command's arguments
```

- Arguments are positional in usage order, or `name=value` in any order.
- Values are parsed as JSON when they look like JSON. Otherwise they are
  strings. Quote JSON objects for the shell: `'props={"text":"Hi","size":64}'`.
- Clips, tracks, and other targets accept an id or a name. Names must be
  unique. Use ids after splitting a clip, because both halves keep the name.
- Times are in seconds. Keyframe times are clip-local, measured from the
  clip's start.
- On success, a string result prints raw and an object prints as JSON. With
  `--json`, the full reply is printed. On failure, `error: MESSAGE` goes to
  stderr. Exit status: 0 for success, 1 when the command fails, 2 for usage
  or connection errors.
- Relative paths resolve against your working directory, not the editor's.

## Workflow

1. Inspect: `status`, then `show` (tracks and clips) or `show clips`. Read
   `help` output instead of guessing argument names.
2. Edit with the commands below. Commands that set expressions or the script
   return `errors`. Check that it is empty.
3. Verify what you made:
   - `frame out.png TIME [WIDTH]` renders one frame to PNG. Read the image.
   - `clip.get CLIP TIME` shows evaluated values. `prop.get TARGET NAME TIME`
     shows one property's definition and value.
   - `levels` shows audio meters at the current time. `errors` lists script,
     expression, and render errors at the current frame.
   - In a GUI session, `ui.screenshot shot.png` saves the whole window.
4. `save` writes the project in a GUI session; headless runs save it for you.
   Then `export out.mp4 wait=true` if the user wants a video.

## Model

- **Project**: settings (`width`, `height`, `fps`, `sampleRate`,
  `background`), tracks, media, clips, transitions, signals, a master audio
  effect chain, and one Lua script.
- **Tracks**: the default project has V3, V2, V1 (top to bottom) and A2, A1.
  Higher video tracks draw over lower ones. Track kinds: `video` or `audio`.
- **Clips** have a kind, track, start, duration, properties, and effect
  chains:
  - `text`: `text`, `size`, `color`, `font` (a font file path; empty means
    the default).
  - `solid`: `color`.
  - `generator`: `generator`, the name of a script generator.
  - `synth` (audio track): `synth`, the name of a script synth.
  - `media`: placed with `clip.media`. Video and audio stay linked as a pair
    until `clip.unlink`. Use `clip.speed` to change playback speed.
  - Every video clip also has `x`, `y` (pixels from the frame center; +y is
    down), `scale`, `rotation` (degrees), `opacity`, and `blend` (`normal`,
    `add`, `multiply`, `screen`).
  - Every audio clip also has `gain` (dB, -60 to 12) and `pan` (-1 to 1).
- **Effects** run in chain order.
  - Video: `brightness_contrast`, `saturation` (amount), `blur` (radius),
    `invert` (mix), `tint` (color, amount), `vignette` (amount, softness),
    `threshold` (level).
  - Audio: `gain`, `pan`, `lowpass` (cutoff, q), `highpass` (cutoff, q),
    `delay` (time, feedback, mix), `compressor` (threshold, ratio, attack,
    release, makeup).
  - Audio effects can also go on `master`. Effect parameters are properties,
    so they can be keyed or driven by expressions too.
- **Transitions** blend clip A into clip B over a window centered on B's
  start. Types: `crossfade`, `wipe` (direction, softness), `dip` (color),
  `slide` (direction). The transition's curve maps progress to mix.
- **Signals** are named project values, constant or expression, read as
  `sig.NAME`.
- **Eases** for keys and curves: `hold`, `linear`, `ease`, `ease-in`,
  `ease-out`, `ease-in-out`, `ease-in-cubic`, `ease-out-cubic`,
  `ease-in-out-cubic`, `ease-in-back`, `ease-out-back`, `ease-in-out-back`.
  A key's ease shapes the segment that starts at that key.
- Colors are `#rrggbb` or `#rrggbbaa`. In Lua, use `rgb()`, `gray()`, or `hsv()`.

## Commands

Run `meshframe-cli help NAME` for exact arguments.

| Area | Commands |
| --- | --- |
| Files | `new`, `open PATH`, `save [PATH]`, `status`, `show [WHAT]` |
| Project | `settings [width] [height] [fps] [sampleRate] [background]` |
| Tracks | `track.add KIND [NAME]`, `track.set TRACK [name] [muted] [hidden] [locked]`, `track.move TRACK INDEX`, `track.remove TRACK` |
| Media | `media.import PATH`, `media.list`, `media.remove MEDIA` |
| Clips | `clip.add KIND TRACK START DURATION [NAME] [PROPS]`, `clip.media MEDIA [START] [TRACK] [AUDIOTRACK]`, `clip.get CLIP [TIME]`, `clip.move CLIP START [TRACK]`, `clip.trim CLIP START DURATION`, `clip.split CLIP TIME`, `clip.speed`, `clip.rename`, `clip.unlink`, `clip.remove` |
| Properties | `prop.set TARGET NAME VALUE`, `prop.key TARGET NAME TIME VALUE [EASE]`, `prop.unkey TARGET NAME TIME`, `prop.expr TARGET NAME EXPR`, `prop.get TARGET NAME [TIME]` |
| Effects | `effect.add TARGET TYPE [INDEX]`, `effect.enable EFFECT [ENABLED]`, `effect.move EFFECT DELTA`, `effect.remove EFFECT` |
| Transitions | `transition.add A B [TYPE] [DURATION]`, `transition.set TR [TYPE] [DURATION]`, `transition.curve TR CURVE`, `transition.remove TR` |
| Reactive | `signal.set NAME value=V` or `expr=E`, `signal.remove NAME`, `script.get`, `script.set [SOURCE] [file=PATH]`, `eval CODE [TIME]`, `graph [TIME]`, `errors` |
| Transport | `play`, `pause`, `toggle`, `seek TIME`, `time` |
| History | `undo`, `redo`, `history` |
| Output | `frame PATH [TIME] [WIDTH] [HEIGHT]`, `export PATH [width] [height] [start] [end] [crf] [wait]`, `export.status`, `export.cancel`, `levels`, `log [SINCE]` |
| Selection | `select [clips] [track] [media] [effect] [transition] [prop]` |
| GUI | `ui.state`, `ui.space N`, `ui.swap N`, `ui.layout NAME [space]`, `ui.pane N VIEW [space]`, `ui.actions`, `ui.action ID`, `ui.bind ID [KEYS]`, `ui.theme [SLOT] [COLOR]`, `ui.settings [set]`, `ui.screenshot PATH` |

Notes:

- `prop.*` targets are clips, effects (by id), or transitions (by id).
  `prop.set` replaces keys or an expression with a constant.
- Linked media clips move, trim, split, and delete together. Pass
  `linked=false` to edit one side only.
- `transition.curve` takes an ease name or a JSON key list such as
  `'[{"t":0,"v":0},{"t":1,"v":1,"interp":"linear"}]'`.
- `export` writes MP4 (H.264 and AAC) or, for a `.wav` path, audio only. It
  runs in the background. Use `wait=true` to reply when it finishes, or poll
  `export.status`. `crf` is 0 to 51, and lower means higher quality.
- `select` changes what the GUI's inspector, curves, and effects views show.

## Expressions and scripting

Property and signal expressions are Lua expressions. They can read:

- `t`: project time in seconds. `lt`: time since the clip's start.
  `progress`: 0 to 1 across the clip (or across the transition's window).
- `sig.NAME`: a signal. `prop("clip name", "prop")` reads another clip's
  evaluated property.
- `fps`, `w`, `h`, `sample_rate`: project settings.
- Helpers: `abs ceil floor sqrt exp log sin cos tan asin acos atan atan2 min
  max fmod pow pi huge`, `clamp(x, a=0, b=1)`, `lerp(a, b, x)`,
  `smoothstep(a, b, x)`, `round`, `ease(name, x)`, `hash(n)` (0 to 1),
  `noise(x)` (smooth, -1 to 1), `osc(freq, phase)` (a sine of `t`, -1 to 1),
  `rgb(r, g, b, a)`, `gray(v, a)`, `hsv(h, s, v, a)` (components 0 to 1).
- Functions defined in the project script.

Examples:

```sh
meshframe-cli prop.expr title x "sin(t * 3) * 100"
meshframe-cli prop.expr bg color "hsv(t * 0.1, 0.6, 0.4)"
meshframe-cli signal.set beat expr="exp(-fmod(t * 2, 1) * 6)"
meshframe-cli prop.expr logo scale "1 + sig.beat * 0.1"
meshframe-cli prop.expr shadow opacity 'prop("title", "opacity") * 0.5'
```

The project script (`script.set`) runs whenever it changes. It can declare:

```lua
-- Signals, constant or computed per frame.
mf.signal("pulse", function() return exp(-fmod(t * 2, 1) * 6) end)

-- Generators draw a clip's image. g has w, h, t, lt, and progress.
-- Methods: g:clear(color), g:rect(x, y, w, h, color), g:circle(x, y, r, color),
-- g:line(x1, y1, x2, y2, width, color), g:text(str, x, y, size, color),
-- g:poly({x1, y1, x2, y2, x3, y3, ...}, color). Coordinates are project pixels.
mf.generator("rings", function(g)
  g:clear(gray(0.05))
  for i = 1, 6 do g:circle(g.w / 2, g.h / 2, i * 40 * (1 + sig.pulse * 0.2), hsv(i / 6, 0.5, 0.9, 0.3)) end
end)

-- Synths return one sample in -1..1 for a timeline time.
mf.synth("tone", function(time) return sin(time * 2 * pi * 220) * 0.2 end)

-- Clips the script owns. They appear on the timeline as read-only. Property
-- values can be constants or functions.
for i = 0, 3 do
  mf.clip{ track = "V3", kind = "text", text = "Item " .. i, start = i, duration = 1,
           x = function() return -300 + lt * 200 end }
end

mf.log("script loaded")  -- appears in `log` and the console view
```

`script.set` replies with what the script declared. A script error is
reported with its line number, and nothing the script declares takes effect
until the error is fixed. Use `script.set file=path.lua` for longer scripts.
`eval CODE [TIME]` runs Lua in the same environment, which is useful for
checking an expression before you set it.

## Driving the GUI

These commands need a running editor. Read the result back with
`ui.screenshot` (the whole window) or `ui.state`.

- 10 spaces (`ui.space 1`–`10`), each with a layout preset: `editor`,
  `single`, `columns-2`, `columns-3`, `rows-2`, `grid-2x2`, `main-stack`,
  `code`.
- Panes are numbered from 1 in reading order. Views: `preview`, `timeline`,
  `media`, `inspector`, `video-effects`, `audio-effects`, `curves`, `script`,
  `graph`, `meters`, `scopes`, `console`, `history`, `export`, `settings`.
- `ui.actions` lists every action with its keys. `ui.action ID` runs one, for
  example `transport.toggle`, `edit.split`, `file.save`, or `app.quit`.
- `ui.bind ID "Alt+1, Ctrl+1"` rebinds an action. `ui.bind ID` with no keys
  restores its defaults. `ui.theme SLOT #rrggbb` recolors a theme slot, and
  `ui.theme` with no arguments lists the slots. These changes persist in the
  user's settings file, so make them only when asked.

## Example: a short title sequence

`examples/demo.sh` builds a complete project this way and exports it. A
smaller version:

```sh
P="--project title.mfproj"
meshframe-cli $P settings width=1280 height=720 fps=30
meshframe-cli $P clip.add solid V1 0 5 bg 'props={"color":"#101820"}'
meshframe-cli $P clip.add text V2 0.5 4 title 'props={"text":"Hello","size":120}'
meshframe-cli $P prop.key title opacity 0 0
meshframe-cli $P prop.key title opacity 0.6 1 ease-out
meshframe-cli $P prop.expr title y "sin(t * 2) * 10"
meshframe-cli $P effect.add title vignette
meshframe-cli $P frame check.png 2      # look at it
meshframe-cli $P export title.mp4 wait=true
```

In zsh, `$P` does not split into words, so spell the option out or use bash.
