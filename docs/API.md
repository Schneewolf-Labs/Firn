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
