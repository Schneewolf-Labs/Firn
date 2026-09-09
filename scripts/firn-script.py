#!/usr/bin/env python3
"""Run one of the original's .PspScript files (or any Python script written
against its API) on a running Firn.

    python3 scripts/drive.py --launch [image]          # start the app with the FIRN_DRIVE socket
    python3 scripts/firn-script.py Script.PspScript    # then run scripts against it
    python3 scripts/firn-script.py -c "App.Do(Environment, 'NegativeImage', {})"

The script's `from JascApp import *` gets this module's App object:
App.Do(Environment, name, params) sends the command as JSON over the socket
and returns the result dict (with the keys the command reports, or raises
on an unsupported command); App.Constants.<Enum>.<Value> evaluates to the
value's name as a string, matching what the app expects. Scripts are
Python 2; print statements and a few other constructs are converted before
running. Windows-only modules (_winreg) are not available.
"""
import json, os, re, socket, sys, types

SOCK = os.environ.get("FIRN_DRIVE", "/tmp/firn-drive.sock")


class ScriptError(Exception):
    pass


class _Enum:
    """App.Constants.<Enum>: attributes evaluate to their own names."""
    def __init__(self, name):
        self._name = name
    def __getattr__(self, item):
        if item.startswith("__"):
            raise AttributeError(item)
        if self._name == "Boolean":
            return {"true": True, "false": False}.get(item, item)
        return item


class _Constants:
    def __getattr__(self, item):
        if item.startswith("__"):
            raise AttributeError(item)
        return _Enum(item)


class _App:
    Constants = _Constants()

    def __init__(self):
        self._sock = None

    def _connect(self):
        if self._sock is None:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                s.connect(SOCK)
            except OSError as e:
                raise ScriptError(f"no Firn listening on {SOCK} (start one with scripts/drive.py --launch): {e}")
            self._sock = s
        return self._sock

    def Do(self, environment, name, params=None):
        params = _jsonable(params or {})
        s = self._connect()
        s.sendall((f"do {name} {json.dumps(params)}\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                raise ScriptError("Firn closed the connection")
            buf += chunk
        line = buf.decode("utf-8", "replace").rstrip("\n")
        if line.startswith("ok result "):
            return json.loads(line[len("ok result "):])
        if line.startswith("ok error "):
            raise ScriptError(f"{name}: {line[len('ok error '):]}")
        raise ScriptError(f"unexpected reply: {line}")


def _jsonable(v):
    """Python values from a script into JSON: tuples become lists, colors stay [r, g, b]."""
    if isinstance(v, dict):
        return {str(k): _jsonable(x) for k, x in v.items()}
    if isinstance(v, (list, tuple)):
        return [_jsonable(x) for x in v]
    if isinstance(v, bytes):
        return v.decode("utf-8", "replace")
    if isinstance(v, (str, int, float, bool)) or v is None:
        return v
    return str(v)


App = _App()


def _py2_to_py3(src):
    """Enough of a translation for the original's scripts: print statements,
    backtick repr, <> operator, and u'' literals are already fine in 3."""
    try:
        from lib2to3.refactor import RefactoringTool, get_fixers_from_package  # type: ignore
        tool = RefactoringTool(get_fixers_from_package("lib2to3.fixes"))
        return str(tool.refactor_string(src if src.endswith("\n") else src + "\n", "script"))
    except Exception:
        pass
    out = []
    for line in src.splitlines():
        m = re.match(r"^(\s*)print\s+(.*)$", line)
        if m and not line.strip().startswith("print("):
            out.append(f"{m.group(1)}print({m.group(2).rstrip(',')})")
        else:
            out.append(line.replace(" <> ", " != "))
    return "\n".join(out) + "\n"


def run_source(src, filename="<script>"):
    module = types.ModuleType("JascApp")
    module.App = App
    module.ScriptError = ScriptError
    sys.modules["JascApp"] = module
    code = compile(_py2_to_py3(src), filename, "exec")
    ns = {"__name__": "__main__", "App": App}
    exec(code, ns)
    if "Do" in ns and callable(ns["Do"]):
        return ns["Do"]({"Environment": "Firn"})
    return None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    if sys.argv[1] == "-c":
        run_source("from JascApp import *\n" + sys.argv[2], "<command>")
        return 0
    path = sys.argv[1]
    with open(path, encoding="utf-8", errors="replace") as f:
        src = f.read()
    try:
        run_source(src, path)
    except ScriptError as e:
        print(f"firn-script: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
