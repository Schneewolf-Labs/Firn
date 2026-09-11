# Driving Firn from outside

Everything the menus and tool options do is also an **action**: a name, a set
of parameters, and the same App function the menu item calls. Actions are
registered in `app/src/Actions.cpp`, so the two cannot drift apart, and the
program can be asked what it offers rather than being told out of band.

Three ways in, one surface:

- **`firn-cli`**, the command line client (`tools/cli.cpp`).
- **The driver socket** directly: `$FIRN_DRIVE` (default `/tmp/firn-drive.sock`),
  one command per line, one reply per command, sent once the frames for that
  command have run. `scripts/drive.py` is the Python client.
- **`.PspScript` files**, through `scripts/firn-script.py`, which also reach
  the original's own commands (`docs/COMMANDS.md`).

## The command line

```sh
firn-cli --launch                       # start the program and wait for it
firn-cli describe                       # every action and its parameters, as JSON
firn-cli do file.new '{"width":800,"height":600,"color":"#204080"}'
firn-cli do layer.new
firn-cli do select.ellipse '{"x0":80,"y0":60,"x1":320,"y1":240,"feather":6}'
firn-cli do edit.content_aware_fill
firn-cli state                          # what is open, as JSON
firn-cli shot preview.png               # what the window is showing
firn-cli do file.save_as '{"path":"out.ora"}'
firn-cli quit
```

`--socket PATH` picks a different socket, so several instances can be driven
at once. `raw` sends the driver's own wire steps for replaying input
(`raw 'click 100 200'`, `raw 'key z ctrl'`), which is how the UI itself is
tested.

## The manual

`docs/API-reference.md` is every action with its parameters, and
`docs/api.json` is the same thing as JSON Schema, one schema per action,
which is the shape a tool-calling client already reads. Both are generated
from the running program by `scripts/gen_api_docs.py`, and CI fails if what
is committed no longer matches what the program reports, so neither can
fall behind the code.

```sh
python3 scripts/gen_api_docs.py          # rewrite both after changing an action
python3 scripts/gen_api_docs.py --check  # what CI runs
```

## Discovery

`describe` returns the action list with a summary and typed parameters for
each, the tool names `tool.select` accepts, and a note that the original's
`App.Do` commands are accepted under their own names too. A client needs
nothing else to work out what it can do:

```json
{"firn":"action api","version":1,
 "actions":[{"name":"image.resize","summary":"Resize the image",
             "params":[{"name":"width","type":"number","summary":"pixels"},
                       {"name":"height","type":"number","summary":"pixels"},
                       {"name":"filter","type":"string","summary":"smart, lanczos, ..."}]}],
 "tools":["Pan","Zoom","Selection","Paint Brush", "..."]}
```

## Replies

Every action replies with JSON. Success is `{"ok":true}` plus whatever the
action returns; a failure comes back as a message on stderr and a non-zero
exit from `firn-cli`, and as `error <message>` on the socket. `state`
reports the tool, the open documents, and for the current image its size,
zoom, history position, selection and full layer list.

## Adding an action

Register it in `app/src/Actions.cpp` next to its neighbours, calling the same
`App` method the menu item calls. Give it a dotted name, a one-line summary
and a parameter for anything the dialog asks for. It then appears in
`describe`, works from `firn-cli`, from the socket and from a script, with no
further wiring.

## Drawing without mouse gestures

All geometry uses image pixels, with (0, 0) at the top left. `draw.rectangle`
and `draw.ellipse` take `x`, `y`, `width`, and `height`. `draw.polygon` takes
`points: [[x,y], ...]`. `draw.path` takes Bezier nodes with `x`, `y`, and
optional absolute `in`/`out` control points. Shapes accept `fill`, `stroke`,
`stroke_width`, `antialias`, and an undo/object `name`. Colors are `#RRGGBB`
or `#RRGGBBAA`; shape paints also accept `none`.

The default `target: "raster"` paints the active raster layer through the
current selection. Like the brush tools, raster drawing reduces a deep
layer to 8 bits, undoably. `target: "vector"` adds an editable object to an
active vector layer; create one with `layer.new_vector`. Vector drawing
rejects an active raster selection rather than silently ignoring it.
Open paths require `fill: "none"` and a stroke. `draw.stroke` paints ordered
points with `color`, `size`, `hardness` and per-stroke `opacity` (0 to 1).
`edit.fill` replaces pixels through the selection without changing materials.
API drawing rejects mask/selection edit mode; exit that mode first.

## One transaction, one undo

`app.batch` takes a `name` and 1–256 `actions`, each containing `action` and
optional `params`. The request is prevalidated, executed in order, and
recorded as one undo entry. A failure restores the document and its original
undo/redo history; the error identifies the zero-based child index.
Successful replies include `count` and ordered `results`.

Only actions advertised with `batch_safe: true` are accepted. Currently
these are the drawing and fill actions, raster/vector layer creation,
layer properties/selection, and the basic selection actions. File writes,
clipboard access, tool settings, history actions, legacy commands and nested
batches are rejected before any edits happen. Batches require an existing
image. Intermediate states are never rendered; file saves belong after the
successful batch. Undo memory accounts for the boundary document snapshots.

A complete editable kitten is included as `samples/api-cat.json`:

```sh
build/tools/firn-cli --launch do file.new '{"width":500,"height":460,"color":"#FFF4E9"}'
build/tools/firn-cli do app.batch --file samples/api-cat.json
build/tools/firn-cli do file.save_as '{"path":"kitten.ora"}'
build/tools/firn-cli do file.save_as '{"path":"kitten.png"}'
```

`--file` accepts formatted JSON and sends one compact protocol line. Direct
JSON arguments work as before. `firn-cli describe draw.path` (or
`app.describe` with `name`) returns just that action. Discovery version 2
includes nested schemas, correctly typed literal defaults, examples, and
transaction eligibility. Computed defaults use `x-default-description`.
Modern action calls enforce required fields, types, enums and advertised
constraints; unknown fields are errors. This intentionally rejects inputs
such as numeric strings that were previously coerced. Legacy commands keep
their original permissive parsing, are explicitly marked as unschematized,
and common drawing-workflow replacements appear in `legacy_replacements`.

## Isolated instances

Use `--socket PATH` to connect to a particular instance. `--launch` reuses
an existing listener at that address, and can locate the sibling app binary
on Linux and macOS. Starting a second app at an occupied address fails
without unlinking the original listener. `scripts/drive.py --kill` shuts down
only its selected socket; it no longer kills other Firn processes. The test
and documentation scripts use private sockets and configuration directories.

On macOS, a normal build also creates `build/app/Firn.app`, which Finder and
computer-use clients can discover directly. This is a development bundle;
SDL2 still needs to be installed as described in `BUILDING.md`.

The typed action schemas do not yet cover all inherited effect commands.
Their names remain discoverable for compatibility; do not infer schemas for
those commands from the typed replacements. Full ImGui control accessibility
is also separate work from the macOS pointer and keyboard fixes.
