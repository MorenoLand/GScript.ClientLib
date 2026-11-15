"""MCP server over gclib: logs in to a Graal game server as a client, chats, moves, warps, requests files and reads the
world state, so behaviour can be checked against a real server from any MCP-capable assistant.

It speaks the Model Context Protocol on stdin/stdout (one JSON message per line) and needs nothing but Python and
gclib (bin/gclib.dll, .so or .dylib in the repository root).

Settings come from ~/.gc_mcp.json (or the file named by GC_MCP_CONFIG) and can be overridden by environment variables:

    {
      "host": "...",            GC_HOST        login or game server
      "port": 14900,            GC_PORT        client port of that server
      "version": "6.113",       GC_VERSION     6.037, 6.113, 6.2, android ...
      "account": "...",         GC_ACCOUNT
      "password": "...",        GC_PASSWORD
      "key_file": "",           GC_KEY_FILE    PEM private key, needed by the 6.2 (Gen6) protocol
      "platform": "win",        GC_PLATFORM    optional; sent in the login identity
      "ids": ["", "", "", ""],  GC_IDS         optional; four identity strings, comma separated in the variable.
                                               Left out, stable ids are derived from this machine.
      "cipher_seed": null       GC_CIPHER_SEED optional; overrides the built-in Gen5 seed
    }
"""

import ctypes
import hashlib
import json
import os
import platform as platform_info
import sys
import threading
import time
import uuid
from collections import deque

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
LIBRARY_NAMES = {"win32": "gclib.dll", "darwin": "gclib.dylib"}
DLL_PATH = os.environ.get("GCLIB_PATH") or os.path.join(ROOT, "bin", LIBRARY_NAMES.get(sys.platform, "gclib.so"))
CONFIG_PATH = os.environ.get("GC_MCP_CONFIG") or os.path.join(os.path.expanduser("~"), ".gc_mcp.json")
PROTOCOL = "latin-1"

V = ctypes.c_void_p
S = ctypes.c_char_p
I = ctypes.c_int
F = ctypes.c_float
CB_VOID = ctypes.CFUNCTYPE(None, V)
CB_DISCONNECTED = ctypes.CFUNCTYPE(None, S, V)
CB_CHAT_EX = ctypes.CFUNCTYPE(None, I, S, S, S, S, S, V)
CB_PM = ctypes.CFUNCTYPE(None, I, S, S, V)
CB_TEXT = ctypes.CFUNCTYPE(None, S, V)
CB_FILE = ctypes.CFUNCTYPE(None, S, V, I, I, V)
CB_PLAYER_LEFT = ctypes.CFUNCTYPE(None, I, V)
CB_STATE = ctypes.CFUNCTYPE(None, S, I, S, V)
CB_WARP = ctypes.CFUNCTYPE(None, F, F, S, V)
CB_REMOTE = ctypes.CFUNCTYPE(None, S, S, V)


def text(value):
    return value.decode(PROTOCOL, errors="replace") if value else ""


def wire(value):
    return (value or "").encode(PROTOCOL, errors="replace")


def machine_ids():
    base = "%s|%s|%s" % (platform_info.node(), uuid.getnode(), platform_info.system())
    return [hashlib.md5((base + "|%d" % index).encode()).hexdigest().upper() for index in range(4)]


class Gc:
    def __init__(self):
        self.lib = None
        self.handle = None
        self.events = deque(maxlen=20000)
        self.sequence = 0
        self.read_cursor = 0
        self.lock = threading.Lock()
        self.callbacks = []
        self.started = time.time()
        self.files = {}
        self.save_dir = None

    def note(self, event_type, content, **extra):
        with self.lock:
            self.sequence += 1
            self.events.append({"n": self.sequence, "t": round(time.time() - self.started, 2), "type": event_type, "text": content, **extra})

    def events_after(self, sequence):
        with self.lock:
            return [event for event in self.events if event["n"] > sequence]

    def mark(self):
        with self.lock:
            return self.sequence

    def load(self):
        if self.lib:
            return
        if not os.path.exists(DLL_PATH):
            raise RuntimeError("gclib is not built: %s is missing (cmake --build build --config Release)" % DLL_PATH)
        if hasattr(os, "add_dll_directory"):
            os.add_dll_directory(os.path.dirname(DLL_PATH))
        lib = ctypes.CDLL(DLL_PATH)
        sig = {
            "gc_create": (V, [S, I, S]),
            "gc_destroy": (None, [V]),
            "gc_set_client_identity": (None, [V, S, S, S, S, S]),
            "gc_set_cipher_seed": (None, [V, ctypes.c_uint]),
            "gc_set_encryption_certificate_file": (I, [V, S]),
            "gc_connect": (I, [V]),
            "gc_login": (I, [V, S, S]),
            "gc_disconnect": (None, [V]),
            "gc_is_connected": (I, [V]),
            "gc_is_authenticated": (I, [V]),
            "gc_get_last_error": (S, [V]),
            "gc_get_last_login_error": (S, [V]),
            "gc_get_state_json": (V, [V, S]),
            "gc_free_string": (None, [V]),
            "gc_send_language": (I, [V, S]),
            "gc_request_base_files": (I, [V, V, I]),
            "gc_send_level_chat": (I, [V, S]),
            "gc_send_private_message": (I, [V, ctypes.POINTER(I), I, S]),
            "gc_move": (I, [V, F, F, I, I]),
            "gc_level_warp": (I, [V, F, F, S]),
            "gc_request_file": (I, [V, S]),
            "gc_set_flag": (I, [V, S]),
            "gc_unset_flag": (I, [V, S]),
            "gc_set_nickname": (I, [V, S]),
            "gc_set_keepalive": (None, [V, I]),
        }
        for name, (restype, argtypes) in sig.items():
            function = getattr(lib, name, None)
            if function is not None:
                function.restype = restype
                function.argtypes = argtypes
        self.lib = lib

    def register(self):
        lib, handle = self.lib, self.handle

        def add(register_name, callback_type, function):
            callback = callback_type(function)
            self.callbacks.append(callback)
            register = getattr(lib, register_name, None)
            if register is not None:
                register.argtypes = [V, callback_type, V]
                register.restype = None
                register(handle, callback, None)

        def on_file(name, data, length, mod_time, user):
            content = ctypes.string_at(data, length) if data and length > 0 else b""
            self.files[text(name)] = content
            saved = ""
            if self.save_dir:
                target = os.path.join(self.save_dir, os.path.basename(text(name)))
                with open(target, "wb") as out:
                    out.write(content)
                saved = target
            self.note("file", text(name), bytes=length, saved=saved)

        add("gc_on_connected", CB_VOID, lambda u: self.note("connected", "socket connected"))
        add("gc_on_authenticated", CB_VOID, lambda u: self.note("authenticated", "login accepted"))
        add("gc_on_disconnected", CB_DISCONNECTED, lambda r, u: self.note("disconnected", text(r)))
        add("gc_on_chat_ex", CB_CHAT_EX, lambda i, a, n, c, l, m, u: self.note("chat", text(m), player=i, account=text(a), nick=text(n), level=text(l)))
        add("gc_on_private_message", CB_PM, lambda i, t, m, u: self.note("pm", text(m), player=i, kind=text(t)))
        add("gc_on_level_name", CB_TEXT, lambda l, u: self.note("level", text(l)))
        add("gc_on_server_text", CB_TEXT, lambda t, u: self.note("server_text", text(t)))
        add("gc_on_file", CB_FILE, on_file)
        add("gc_on_file_failed", CB_TEXT, lambda n, u: self.note("file_failed", text(n)))
        add("gc_on_player_left", CB_PLAYER_LEFT, lambda i, u: self.note("player_left", str(i), player=i))
        add("gc_on_remote_event", CB_REMOTE, lambda k, j, u: self.note("remote:" + text(k), text(j)))
        add("gc_on_connection_state", CB_STATE, lambda s, a, r, u: self.note("connection_state", text(s), attempt=a, reason=text(r)))

    def call(self, function, *args):
        if not self.handle:
            raise RuntimeError("not connected: call gc_connect first")
        return getattr(self.lib, function)(self.handle, *args)

    def error(self):
        if not self.handle:
            return ""
        return "%s %s" % (text(self.lib.gc_get_last_error(self.handle)), text(self.lib.gc_get_last_login_error(self.handle))).strip()

    def wait_for(self, condition, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if condition():
                return True
            time.sleep(0.05)
        return condition()

    def status(self):
        if not self.handle:
            return {"connected": False}
        return {"connected": bool(self.lib.gc_is_connected(self.handle)), "authenticated": bool(self.lib.gc_is_authenticated(self.handle)), "last_error": self.error()}


gc = Gc()


def load_config():
    config = {}
    if os.path.exists(CONFIG_PATH):
        with open(CONFIG_PATH, "r", encoding="utf-8") as handle:
            config = json.load(handle)
    for key, variable in (("host", "GC_HOST"), ("port", "GC_PORT"), ("version", "GC_VERSION"), ("account", "GC_ACCOUNT"),
                          ("password", "GC_PASSWORD"), ("key_file", "GC_KEY_FILE"), ("platform", "GC_PLATFORM"),
                          ("cipher_seed", "GC_CIPHER_SEED")):
        if os.environ.get(variable):
            config[key] = os.environ[variable]
    if os.environ.get("GC_IDS"):
        config["ids"] = os.environ["GC_IDS"].split(",")
    return config


def render(events):
    lines = []
    for event in events:
        extra = {k: v for k, v in event.items() if k not in ("n", "t", "type", "text")}
        suffix = (" " + json.dumps(extra, ensure_ascii=False)) if extra else ""
        lines.append("[%s] %s: %s%s" % (event["t"], event["type"], event["text"], suffix))
    return "\n".join(lines) if lines else "(nothing came back)"


def after(mark, wait):
    time.sleep(wait)
    events = gc.events_after(mark)
    if events:
        gc.read_cursor = max(gc.read_cursor, events[-1]["n"])
    return render(events)


def sent(ok):
    return "sent" if ok else "NOT sent (" + gc.error() + ")"


# ----- tools -------------------------------------------------------------------------------------------------------

def tool_status(args):
    config = load_config()
    shown = {key: ("***" if key == "password" and config.get(key) else config.get(key)) for key in ("host", "port", "version", "account", "password", "key_file", "platform")}
    return json.dumps({"config_file": CONFIG_PATH, "settings": shown, "library": DLL_PATH, "library_present": os.path.exists(DLL_PATH), "state": gc.status()}, indent=2)


def tool_connect(args):
    config = load_config()
    missing = [key for key in ("host", "port", "account", "password") if not config.get(key)]
    if missing:
        return "Missing settings: %s. Put them in %s (see the top of gc_mcp.py)." % (", ".join(missing), CONFIG_PATH)
    if gc.handle:
        tool_disconnect({})
    gc.load()
    gc.started = time.time()
    handle = gc.lib.gc_create(wire(str(config["host"])), int(config["port"]), wire(config.get("version") or "6.113"))
    if not handle:
        return "gc_create failed: unknown version %s" % config.get("version")
    gc.handle = handle
    ids = (list(config.get("ids") or []) + [""] * 4)[:4] if config.get("ids") else machine_ids()
    gc.lib.gc_set_client_identity(handle, wire(config.get("platform") or "win"), *[wire(value) for value in ids])
    if config.get("cipher_seed"):
        gc.lib.gc_set_cipher_seed(handle, int(str(config["cipher_seed"]), 0))
    if config.get("key_file"):
        gc.lib.gc_set_encryption_certificate_file(handle, wire(os.path.expanduser(config["key_file"])))
    gc.register()
    if not gc.lib.gc_connect(handle):
        return "gc_connect failed: %s" % gc.error()
    gc.lib.gc_login(handle, wire(config["account"]), wire(config["password"]))
    # what the real client sends right after the login, before the server answers
    gc.lib.gc_request_base_files(handle, None, 0)
    gc.lib.gc_send_language(handle, b"English")
    if not gc.wait_for(lambda: gc.status().get("authenticated"), 30):
        return "Connected but not authenticated in 30s. %s\n%s" % (gc.error(), render(gc.events_after(0)))
    time.sleep(2)
    return "Logged in as %s. %s" % (config["account"], json.dumps(gc.status()))


def tool_disconnect(args):
    if gc.handle:
        gc.lib.gc_destroy(gc.handle)
        gc.handle = None
    gc.callbacks = []
    return "disconnected"


def tool_events(args):
    wait = float(args.get("wait", 0) or 0)
    mark = gc.read_cursor
    time.sleep(wait)
    events = gc.events_after(mark)
    if events:
        gc.read_cursor = events[-1]["n"]
    return render(events)


def tool_state(args):
    kind = args.get("kind", "session")
    pointer = gc.call("gc_get_state_json", wire(kind))
    if not pointer:
        return "no state for kind '%s'" % kind
    try:
        return ctypes.string_at(pointer).decode(PROTOCOL, errors="replace")
    finally:
        gc.lib.gc_free_string(pointer)


def tool_chat(args):
    mark = gc.mark()
    return sent(gc.call("gc_send_level_chat", wire(args.get("message", "")))) + "\n" + after(mark, float(args.get("wait", 2) or 2))


def tool_pm(args):
    mark = gc.mark()
    ids = (I * 1)(int(args["player"]))
    return sent(gc.call("gc_send_private_message", ids, 1, wire(args.get("message", "")))) + "\n" + after(mark, float(args.get("wait", 2) or 2))


def tool_move(args):
    mark = gc.mark()
    ok = gc.call("gc_move", float(args["x"]), float(args["y"]), int(args.get("direction", 2)), 0)
    return sent(ok) + "\n" + after(mark, float(args.get("wait", 1) or 1))


def tool_warp(args):
    mark = gc.mark()
    ok = gc.call("gc_level_warp", float(args.get("x", 30)), float(args.get("y", 30)), wire(args["level"]))
    return sent(ok) + "\n" + after(mark, float(args.get("wait", 4) or 4))


def tool_request_file(args):
    name = args["name"]
    gc.save_dir = os.path.expanduser(args["save_dir"]) if args.get("save_dir") else None
    if gc.save_dir:
        os.makedirs(gc.save_dir, exist_ok=True)
    mark = gc.mark()
    ok = gc.call("gc_request_file", wire(name))
    gc.wait_for(lambda: any(e["type"] in ("file", "file_failed") and e["text"].lower() == name.lower() for e in gc.events_after(mark)), float(args.get("wait", 10) or 10))
    return sent(ok) + "\n" + render(gc.events_after(mark))


def tool_flag(args):
    mark = gc.mark()
    if args.get("unset"):
        ok = gc.call("gc_unset_flag", wire(args["flag"]))
    else:
        ok = gc.call("gc_set_flag", wire(args["flag"]))
    return sent(ok) + "\n" + after(mark, float(args.get("wait", 1) or 1))


def tool_nickname(args):
    mark = gc.mark()
    return sent(gc.call("gc_set_nickname", wire(args.get("nickname", "")))) + "\n" + after(mark, 1)


def schema(properties, required=()):
    return {"type": "object", "properties": properties, "required": list(required)}


WAIT = {"type": "number", "description": "seconds to listen for the server's answer"}
TOOLS = [
    ("gc_status", "Settings file, library and connection state.", schema({}), tool_status),
    ("gc_connect", "Log in to the configured server as a client. Needs ~/.gc_mcp.json (host, port, account, password; version and key_file as needed).", schema({}), tool_connect),
    ("gc_disconnect", "Log out and free the connection.", schema({}), tool_disconnect),
    ("gc_events", "Everything the server sent since the last call (chat, PMs, level changes, files, errors).", schema({"wait": {"type": "number", "description": "seconds to wait first"}}), tool_events),
    ("gc_state", "JSON copy of cached world state. kind: players, level, npcs, flags, weapons, horses, baddies, items, bombs, chests, signs, links, session.", schema({"kind": {"type": "string"}}, ["kind"]), tool_state),
    ("gc_chat", "Say something in the current level.", schema({"message": {"type": "string"}, "wait": WAIT}, ["message"]), tool_chat),
    ("gc_pm", "Send a private message to a player id.", schema({"player": {"type": "integer"}, "message": {"type": "string"}, "wait": WAIT}, ["player", "message"]), tool_pm),
    ("gc_move", "Move the player to x, y (tiles) facing direction 0-3 (up, left, down, right).", schema({"x": {"type": "number"}, "y": {"type": "number"}, "direction": {"type": "integer"}, "wait": WAIT}, ["x", "y"]), tool_move),
    ("gc_warp", "Warp to a level at x, y.", schema({"level": {"type": "string"}, "x": {"type": "number"}, "y": {"type": "number"}, "wait": WAIT}, ["level"]), tool_warp),
    ("gc_request_file", "Request a public file from the server; optionally save it into a directory.", schema({"name": {"type": "string"}, "save_dir": {"type": "string"}, "wait": WAIT}, ["name"]), tool_request_file),
    ("gc_flag", "Set (or with unset=true delete) a client flag, e.g. 'name=value'.", schema({"flag": {"type": "string"}, "unset": {"type": "boolean"}, "wait": WAIT}, ["flag"]), tool_flag),
    ("gc_nickname", "Change the player nickname.", schema({"nickname": {"type": "string"}}, ["nickname"]), tool_nickname),
]
HANDLERS = {name: handler for name, _, _, handler in TOOLS}


# ----- MCP over stdio ----------------------------------------------------------------------------------------------

def send(message):
    sys.stdout.write(json.dumps(message, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def reply(request_id, result=None, error=None):
    if request_id is None:
        return
    message = {"jsonrpc": "2.0", "id": request_id}
    if error is not None:
        message["error"] = error
    else:
        message["result"] = result
    send(message)


def handle(message):
    method, request_id, params = message.get("method"), message.get("id"), message.get("params") or {}
    if method == "initialize":
        reply(request_id, {"protocolVersion": params.get("protocolVersion", "2024-11-05"), "capabilities": {"tools": {}},
                           "serverInfo": {"name": "gc-mcp", "version": "1.0.0"}})
    elif method == "ping":
        reply(request_id, {})
    elif method == "tools/list":
        reply(request_id, {"tools": [{"name": n, "description": d, "inputSchema": s} for n, d, s, _ in TOOLS]})
    elif method == "tools/call":
        name, arguments = params.get("name"), params.get("arguments") or {}
        handler = HANDLERS.get(name)
        if handler is None:
            reply(request_id, error={"code": -32602, "message": "unknown tool " + str(name)})
            return
        try:
            content, failed = handler(arguments), False
        except Exception as problem:  # the tool failed; the model reads why
            content, failed = "%s: %s" % (type(problem).__name__, problem), True
        reply(request_id, {"content": [{"type": "text", "text": content}], "isError": failed})
    elif request_id is not None:
        reply(request_id, error={"code": -32601, "message": "method not found: " + str(method)})


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            continue
        handle(message)
    tool_disconnect({})


if __name__ == "__main__":
    main()
