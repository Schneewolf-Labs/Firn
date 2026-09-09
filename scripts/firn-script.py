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
running. Windows-only modules (_winreg) are not available. Prompts (GetString, GetNumber) are answered in the terminal, or
from the FIRN_SCRIPT_ANSWERS environment variable (one line per prompt).
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
        return item[:-1] if item.endswith("_") and item[:-1] in ("None", "True", "False", "print", "exec") else item


class _Constants:
    def __getattr__(self, item):
        if item.startswith("__"):
            raise AttributeError(item)
        return _Enum(item)


class _Document:
    """Stands in for the original's document objects."""
    def __init__(self, index, title, width=0, height=0, path=""):
        self.Index, self.Title, self.Width, self.Height, self.FileName = index, title, width, height, path
        self.Size = (width, height)
        self.Name = title
    def __repr__(self):
        return f"<Document {self.Index} {self.Title!r} {self.Width}x{self.Height}>"


class _App:
    Constants = _Constants()

    def __init__(self):
        self._sock = None

    def _state(self):
        s = self._connect()
        s.sendall(b"state\n")
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                raise ScriptError("Firn closed the connection")
            buf += chunk
        return dict(re.findall(r'(\w+)=("[^"]*"|\S+)', buf.decode("utf-8", "replace")))

    def _prompt(self, name, params):
        """Prompts happen in the terminal (or come from FIRN_SCRIPT_ANSWERS,
        one answer per line, for unattended runs)."""
        answers = os.environ.get("FIRN_SCRIPT_ANSWERS")
        prompt = params.get("Prompt") or params.get("DialogTitle") or name
        if name == "GetString":
            default = str(params.get("DefaultText", ""))
        else:
            default = str(params.get("DefaultValue", 1))
        if answers is not None:
            lines = answers.split("\n")
            answer = lines[0] if lines and lines[0] != "" else default
            os.environ["FIRN_SCRIPT_ANSWERS"] = "\n".join(lines[1:])
        elif sys.stdin.isatty():
            try:
                answer = input(f"{prompt} [{default}]: ") or default
            except EOFError:
                return {"OKButton": False}
        else:
            answer = default
        if name == "GetString":
            return {"OKButton": True, "EnteredText": answer[: int(params.get("MaxLength", 80) or 80)]}
        try:
            value = float(answer)
        except ValueError:
            return {"OKButton": False, "EnteredNumber": float(default)}
        if params.get("GetInteger", True) in (True, "True", 1):
            value = int(value)
        return {"OKButton": True, "EnteredNumber": value}

    @property
    def TargetDocument(self):
        st = self._state()
        if int(st.get("docs", "0")) == 0:
            return None
        w, h = (int(v) for v in st.get("size", "0x0").split("x"))
        return _Document(0, st.get("title", "").strip('"'), w, h)

    @property
    def Documents(self):
        st = self._state()
        return [_Document(i, "") for i in range(int(st.get("docs", "0")))]

    def _connect(self):
        if self._sock is None:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                s.connect(SOCK)
            except OSError as e:
                raise ScriptError(f"no Firn listening on {SOCK} (start one with scripts/drive.py --launch): {e}")
            self._sock = s
        return self._sock

    def Do(self, environment, name, params=None, *rest):
        params = _jsonable(params or {})
        if name in ("GetString", "GetNumber"):
            return self._prompt(name, params)
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
    src = re.sub(r"\.(None|True|False|print|exec)\b", r".\1_", src)   # Python 2 allowed these as attribute names
    try:
        from lib2to3.refactor import RefactoringTool, get_fixers_from_package  # type: ignore
        tool = RefactoringTool(get_fixers_from_package("lib2to3.fixes"))
        return str(tool.refactor_string(src if src.endswith("\n") else src + "\n", "script"))
    except Exception:
        pass
    src = re.sub(r"\.(None|True|False|print|exec)\b", r".\1_", src)   # Python 2 allowed these as attribute names
    out = []
    for line in src.splitlines():
        m = re.match(r"^(\s*)print\s+(.*)$", line)
        if m and not line.strip().startswith("print("):
            out.append(f"{m.group(1)}print({m.group(2).rstrip(',')})")
        else:
            out.append(line.replace(" <> ", " != "))
    return "\n".join(out) + "\n"


def _library_paths():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    paths = [os.path.expanduser("~/.config/firn/scripts"), os.path.join(here, "WindowsInstall", "Python Libraries")]
    if os.environ.get("FIRN_SCRIPT_DIRS"):
        paths = os.environ["FIRN_SCRIPT_DIRS"].split(":") + paths
    return [p for p in paths if os.path.isdir(p)]


class _ScriptFinder:
    """Imports the original's helper modules (JascUtils and friends) after
    converting them from Python 2."""
    def find_spec(self, name, path=None, target=None):
        import importlib.util
        for d in _library_paths():
            f = os.path.join(d, name + ".py")
            if os.path.isfile(f):
                return importlib.util.spec_from_loader(name, _ScriptLoader(f))
        return None


class _ScriptLoader:
    def __init__(self, path):
        self.path = path
    def create_module(self, spec):
        return None
    def exec_module(self, module):
        with open(self.path, encoding="utf-8", errors="replace") as f:
            src = f.read()
        module.__file__ = self.path
        exec(compile(_py2_to_py3(src), self.path, "exec"), module.__dict__)


def run_source(src, filename="<script>"):
    if not any(isinstance(f, _ScriptFinder) for f in sys.meta_path):
        sys.meta_path.append(_ScriptFinder())
    module = types.ModuleType("JascApp")
    module.App = App
    module.ScriptError = ScriptError
    sys.modules["JascApp"] = module
    code = compile(_py2_to_py3(src), filename, "exec")
    ns = {"__name__": "__main__", "App": App, "Environment": {"Environment": "Firn"}}
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
