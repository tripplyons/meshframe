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

The installed app holds both programs:

- `/Applications/meshframe.app/Contents/MacOS/meshframe` (the editor)
- `/Applications/meshframe.app/Contents/MacOS/meshframe-cli`

The examples below call both programs by name. If they are not on `PATH`, add
the app's `MacOS` directory first:

```sh
export PATH="/Applications/meshframe.app/Contents/MacOS:$PATH"
```

If the app is not installed, build it from the repository root (macOS;
dependencies come from Homebrew, plus `librsvg` for the icon), then copy it
to `/Applications`:

```sh
brew install cmake ninja sdl3 luajit ffmpeg freetype nlohmann-json fmt googletest pkgconf librsvg
scripts/build-macos-app.sh
ditto build-app/meshframe.app /Applications/meshframe.app
```

## Choose a session

- If the user has the editor open, use that session. `meshframe-cli sessions`
  lists running sessions with their pid, kind (`editor` or `headless`),
  project path, and dirty state. With one session, the CLI connects to it
  automatically. With several, pass `--session PID`.
- With no editor open, start a headless session and use it for the whole
  task: `meshframe-cli serve FILE.mfproj`. It returns once the session is
  ready and prints its pid and socket. Then run `meshframe-cli --project
  FILE.mfproj CMD` as usual. While a session (headless or editor) has FILE
  open, `--project FILE` sends commands to it, so separate calls share one
  undo history. A headless session saves FILE after each change (the editor
  does not; the user saves there). When you finish, stop it with
  `meshframe-cli --project FILE.mfproj quit`. It also stops by itself after
  30 minutes without requests (`--idle-timeout SEC` before `serve` changes
  this). Running `serve` on a file that is already open prints that session.
- Without a running session, `--project FILE` runs a one-off headless
  session: the file is created if missing and saved after the command
  (`--no-save` prevents this, and always works on a private copy). GUI
  commands (`ui.*`) need a running editor.
- To start an editor yourself, run `meshframe FILE.mfproj &`, then wait until
  `meshframe-cli sessions` lists it. Stop only editors you started, with
  `meshframe-cli quit`. With unsaved changes, the editor asks
  whether to save instead of quitting, and `ui.state` shows the question in
  `dialog` with its `choices`. Answer with `ui.dialog save` or
  `ui.dialog discard`.
- An editor that starts after one crashed may ask to restore unsaved changes
  (`dialog` choices `restore`, `later`, `discard`). It blocks other input in
  the window. Ask the user before you answer `restore` or `discard`; `later`
  keeps the copy for the next launch.

Without `serve`, each headless invocation is a new session, so `undo` cannot
reach changes from an earlier invocation. You can also send many requests
through one process with `-`:

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

Use `--batch -` to make a group of edits atomic. The CLI sends all stdin
lines as one `batch` request. They become one undo step, and if any command
fails, none of them take effect. The reply holds each command's result in
order, or an error naming the command that failed. A batch cannot run
`undo`, `redo`, `new`, `open`, `save`, `export`, `export.cancel`, `play`,
`pause`, `toggle`, or `ui.*` commands. Refer to clips made in the same batch
by name.

```sh
meshframe-cli --project demo.mfproj --batch - <<'EOF'
clip.add solid V1 0 4 bg
clip.add text V2 0 4 title 'props={"text":"Hello"}'
prop.key title opacity 0 0
EOF
```

Over the socket or in JSON, send `{"cmd": "batch", "args": {"commands":
[...], "label": "..."}}`, where each item is a command line or a request
object.

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
- Times and durations are in seconds: `2.5`. They also accept frames at the
  project frame rate (`90f`), timecode (`00:00:03:12`, HH:MM:SS:FF), and clock
  time (`1:15.5` or `1:02:03.5`), and markers (`@chorus`, `@chorus+2f`,
  `@chorus-0:01`). Replies always use seconds. Keyframe times are
  clip-local, measured from the clip's start; a marker given as a keyframe
  time still means that point on the timeline.
- On success, a string result prints raw and an object prints as JSON. With
  `--json`, the full reply is printed. On failure, `error: MESSAGE` goes to
  stderr. Exit status: 0 for success, 1 when the command fails, 2 for usage
  or connection errors.
- Relative paths resolve against your working directory, not the editor's.

## Workflow

1. Inspect: `status`, then `show` (tracks and clips) or `show clips`. Read
   `help` and `schema` output instead of guessing argument or property names.
2. Edit with the commands below. Commands that set expressions or the script
   return `errors`. Check that it is empty.
3. Verify what you made:
   - `sheet out.png` renders a contact sheet: a grid of frames, each labeled
     with its timecode and any marker on that frame. Read the image to check
     motion, cuts, and timing in one look. By default it shows 12 frames
     across the project. `clip=NAME` limits it to one clip, `start` and `end`
     set the range, `count=N` or `every=15f` set the sampling, and `width`
     and `columns` set the layout. The reply lists each cell's time.
   - `frame out.png TIME [WIDTH]` renders one frame to PNG. Read the image.
   - `clip.bounds CLIP TIME` shows where a video clip actually draws: the box
     of covered pixels after effects (`left`, `top`, `right`, `bottom` from
     the frame's top-left), its `center` in clip `x`, `y` terms, `inFrame`
     (the fraction of covered pixels inside the frame), the frame edges it
     `crosses`, and whether it is `titleSafe` (inside the central 90%). To
     center a clip, subtract `center` from its `x` and `y`. A fully
     transparent clip reports `visible: false`, so pick a time after any
     fade-in.
   - `text.measure TEXT [size] [font]` gives a text clip's block size before
     scale and rotation, so you can choose a size that fits.
   - `clip.get CLIP TIME` shows evaluated values. `prop.get TARGET NAME TIME`
     shows one property's definition and value.
   - `audio.analyze` measures the mix, since you cannot listen to it:
     integrated loudness (`integratedLufs`, BS.1770), loudness range (needs
     3 s), true and sample peak, RMS, silent ranges (below `silence=-50`
     dBFS for 0.3 s), clipping ranges (samples at full scale, which clip on
     export), and loudness per `step` (default 0.5 s). Use `clip=NAME` or
     `start` and `end` for part of the mix, or `media=NAME` to measure an
     imported file before you place it (times are then source times).
     `waveform=PATH` also draws a PNG with silence shaded, clipping in red,
     and markers labeled. Values in dB are null for silence. Common targets:
     about -14 LUFS for online video, true peak below -1 dBTP.
   - `levels` shows audio meters at the current time. `errors` lists script,
     expression, and render errors at the current frame.
   - In a GUI session, `ui.screenshot shot.png` saves the whole window.
4. Run `check` before you hand the work back or export. It lists issues,
   each with `severity` (`error` or `warning`), `kind`, `start` and `end`,
   the clip it concerns, and a message: script errors and errors at any
   sampled time (`script`, `expression`), gaps with no video, clips past the
   end of their media, missing and unused media, clips hidden under an opaque
   full-frame clip, black frames, text off frame or outside title-safe,
   overlapping text, clipping, and loudness or true peak off target
   (`lufs=-14`, `truePeak=-1`). It samples every `step` seconds (default 0.5)
   and at clip edges. Fix errors; judge warnings against what the user asked
   for (a fade to black is intentional).
5. `save` writes the project in a GUI session; headless runs save it for you.
   Then `export out.mp4 wait=true` if the user wants a video.

## Model

`meshframe-cli schema` lists every clip kind with its properties (type,
default, range, unit, choices), every video and audio effect with its
parameters, transition types, eases, blend modes, and the Lua API. It comes
from the same specs the editor uses, so read it instead of guessing names.
`schema SECTION` shows one part, for example `schema clips` or `schema lua`.

- **Project**: settings (`width`, `height`, `fps`, `sampleRate`,
  `background`), tracks, media, clips, transitions, signals, a master audio
  effect chain, and one Lua script.
- **Tracks**: the default project has V3, V2, V1 (top to bottom) and A2, A1.
  Higher video tracks draw over lower ones. Track kinds: `video` or `audio`.
- **Clips** have a kind, track, start, duration, properties, and effect
  chains. Video tracks hold `text`, `solid`, `generator` (drawn by a script
  generator), and `media` clips. Audio tracks hold `synth` (a script synth)
  and `media` clips.
  - `media` clips are placed with `clip.media`. `media.import` names media by
    file name, extension included (`clip.mp4`). Video and audio stay linked as
    a pair until `clip.unlink`. Use `clip.speed` to change playback speed.
  - Video clips share `x`, `y` (pixels from the frame center; +y is down),
    `scale`, `rotation` (degrees), `opacity`, and `blend`. Audio clips share
    `gain` (dB) and `pan`.
- **Effects** run in chain order. Audio effects can also go on `master`.
  Effect parameters are properties, so they can be keyed or driven by
  expressions too.
- **Transitions** blend clip A into clip B over a window centered on B's
  start. The transition's curve maps progress to mix.
- **Signals** are named project values, constant or expression, read as
  `sig.NAME`.
- **Markers** are named timeline times. Names use letters, digits, and `_`
  and are unique. Add markers for beats, sections, and cue points, then
  place clips and keys at `@name` and read `marker.NAME` in expressions and
  the script. Moving a marker updates everything that reads it through
  `marker.NAME`; clips placed with `@name` keep their time.
- **Eases** shape keyframe segments and transition curves. A key's ease
  shapes the segment that starts at that key.
- Colors are `#rrggbb` or `#rrggbbaa`. In Lua, use `rgb()`, `gray()`, or `hsv()`.

## Commands

Run `meshframe-cli help NAME` for exact arguments.

| Area | Commands |
| --- | --- |
| Files | `new`, `open PATH`, `save [PATH]`, `status`, `show [WHAT]`, `schema [SECTION]`, `quit` |
| Project | `settings [width] [height] [fps] [sampleRate] [background]` |
| Tracks | `track.add KIND [NAME]`, `track.set TRACK [name] [muted] [hidden] [locked]`, `track.move TRACK INDEX`, `track.remove TRACK` |
| Media | `media.import PATH`, `media.list`, `media.remove MEDIA` |
| Clips | `clip.bounds CLIP [TIME]`, `text.measure TEXT [size] [font]`, `clip.add KIND TRACK START DURATION [NAME] [PROPS]`, `clip.media MEDIA [START] [TRACK] [AUDIOTRACK]`, `clip.get CLIP [TIME]`, `clip.move CLIP START [TRACK]`, `clip.trim CLIP START DURATION`, `clip.split CLIP TIME`, `clip.speed`, `clip.rename`, `clip.unlink`, `clip.remove` |
| Properties | `prop.set TARGET NAME VALUE`, `prop.key TARGET NAME TIME VALUE [EASE]`, `prop.unkey TARGET NAME TIME`, `prop.expr TARGET NAME EXPR`, `prop.get TARGET NAME [TIME]` |
| Effects | `effect.add TARGET TYPE [INDEX]`, `effect.enable EFFECT [ENABLED]`, `effect.move EFFECT DELTA`, `effect.remove EFFECT` |
| Transitions | `transition.add A B [TYPE] [DURATION]`, `transition.set TR [TYPE] [DURATION]`, `transition.curve TR CURVE`, `transition.remove TR` |
| Markers | `marker.add [NAME] [TIME]`, `marker.move MARKER TIME`, `marker.rename MARKER NAME`, `marker.remove MARKER`, `show markers` |
| Reactive | `signal.set NAME value=V` or `expr=E`, `signal.remove NAME`, `script.get`, `script.set [SOURCE] [file=PATH]`, `eval CODE [TIME]`, `graph [TIME]`, `errors` |
| Transport | `play`, `pause`, `toggle`, `seek TIME`, `time` |
| History | `undo`, `redo`, `history`, `batch COMMANDS [LABEL]` |
| Output | `check [step] [lufs] [truePeak]`, `audio.analyze [start] [end] [clip] [media] [step] [silence] [waveform] [width]`, `frame PATH [TIME] [WIDTH] [HEIGHT]`, `sheet PATH [start] [end] [count] [every] [clip] [width] [columns]`, `export PATH [width] [height] [start] [end] [crf] [wait]`, `export.status`, `export.cancel`, `levels`, `log [SINCE]` |
| Selection | `select [clips] [track] [media] [effect] [transition] [prop]` |
| GUI | `ui.state`, `ui.space N`, `ui.swap N`, `ui.layout NAME [space]`, `ui.pane N VIEW [space]`, `ui.actions`, `ui.action ID`, `ui.dialog CHOICE`, `ui.bind ID [KEYS]`, `ui.theme [SLOT] [COLOR]`, `ui.settings [set]`, `ui.screenshot PATH` |

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

Property and signal expressions are Lua expressions. They can read `t`
(project time), `lt` (time since the clip's start), `progress` (0 to 1 across
the clip or transition), `sig.NAME`, `marker.NAME`, `prop("clip name", "prop")`, project
settings, math and color helpers, and functions defined in the project script.
`schema lua` lists every variable and helper with its signature.

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

-- Generators draw a clip's image. g has w, h, t, lt, and progress, and the
-- drawing methods in `schema lua` (canvas). Coordinates are project pixels.
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
- `ui.state` also shows the status bar `notice`, such as an export result.
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
meshframe-cli $P sheet check.png       # look at it
meshframe-cli $P export title.mp4 wait=true
```

In zsh, `$P` does not split into words, so spell the option out or use bash.
