"""steamui: run JavaScript in Steam's UI (its SharedJSContext) through Steam's CEF debugger on
127.0.0.1:8080, with nothing but the standard library (input/vrws.py's web socket client).

  evaluate(expression)   the expression's value (JSON-able), or an OSError when Steam or its
                         UI isn't there, or a RuntimeError when the script threw
"""
import json
import time
import urllib.request

from vrws import VrSocket

DEBUGGER = "http://127.0.0.1:8080"


def evaluate(expression, target="SharedJSContext", timeout=2.0):
    with urllib.request.urlopen(f"{DEBUGGER}/json", timeout=timeout) as resp:
        pages = json.load(resp)
    url = next((p.get("webSocketDebuggerUrl") for p in pages if p.get("title") == target), None)
    if not url:
        raise OSError(f"Steam's {target} isn't open")
    ws = VrSocket(timeout=timeout, url=url)
    try:
        ws.send(json.dumps({"id": 1, "method": "Runtime.evaluate",
                            "params": {"expression": expression, "returnByValue": True}}))
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            msg = ws.recv(timeout=max(left, 0)) if left > 0 else None
            if msg is None:
                raise OSError("Steam's UI didn't answer")
            if isinstance(msg, dict) and msg.get("id") == 1:
                break
    finally:
        ws.close()
    result = msg.get("result") or {}
    if "exceptionDetails" in result:
        details = result["exceptionDetails"]
        raise RuntimeError((details.get("exception") or {}).get("description") or details.get("text", "error"))
    return (result.get("result") or {}).get("value")
