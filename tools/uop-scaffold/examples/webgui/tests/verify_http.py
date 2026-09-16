#!/usr/bin/env python3
"""HTTP fallback verification for the webgui demo.

Used when Chromium's Local Network Access checks block Playwright from
navigating to the local GUI (environmental, Chromium >= 142). Exercises
the same assertions as tests/test_webgui.py via the bridge's HTTP API
+ SSE stream: page serves, /api/state reflects storage seed, Apply
mutates live params, and a subsequent TSS result uses the new params.

Usage: GUI_URL=http://127.0.0.1:PORT/ python3 verify_http.py
"""
import json
import os
import sys
import time
import urllib.request

GUI_URL = os.environ.get("GUI_URL", "http://127.0.0.1:5255/")
if not GUI_URL.endswith("/"):
    GUI_URL += "/"


def get(path):
    with urllib.request.urlopen(GUI_URL + path.lstrip("/"), timeout=10) as r:
        return r.status, r.read().decode()


def post_apply(mult, add):
    data = json.dumps({"mult": mult, "add": add}).encode()
    req = urllib.request.Request(
        GUI_URL + "api/apply", data=data,
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.status, json.loads(r.read().decode())


def main():
    # 1. GUI page serves.
    status, html = get("/")
    assert status == 200 and "<title>" in html, "GUI page did not serve"
    print("PASS: GUI page serves (HTTP 200)")

    # 2. Seeded storage state (7 3) is live.
    status, body = get("api/state")
    state = json.loads(body)
    assert (state["mult"], state["add"]) == (7, 3), f"seed state wrong: {state}"
    print("PASS: /api/state shows seeded storage state (mult=7 add=3)")

    # 3. Apply 5 1 via the same endpoint the GUI button uses.
    status, applied = post_apply(5, 1)
    assert status == 200, f"apply failed: {applied}"
    status, body = get("api/state")
    state = json.loads(body)
    assert (state["mult"], state["add"]) == (5, 1), f"apply not live: {state}"
    print("PASS: Apply mult=5 add=1 updates live state")

    # 4. Invalid apply is rejected.
    try:
        post_apply("x", 1)
        print("FAIL: invalid apply accepted")
        return 1
    except urllib.error.HTTPError as e:
        assert e.code == 400, f"wrong code: {e.code}"
        print("PASS: invalid Apply rejected (HTTP 400)")

    # 5. Wait for a TSS result that used (5, 1) before the second Apply,
    #    so the feeder's result log proves the first Apply flowed through.
    results_path = os.environ.get("FEEDER_RESULTS", "")
    if results_path:
        deadline = time.time() + 30
        seen = False
        while time.time() < deadline:
            try:
                with open(results_path) as f:
                    content = f.read()
                if "(mult=5 add=1)" in content:
                    seen = True
                    break
            except FileNotFoundError:
                pass
            time.sleep(0.5)
        assert seen, "no TSS result used (mult=5 add=1) within 30s"
        print("PASS: TSS result used first Apply params (mult=5 add=1)")

    # 6. Apply 9 -3 (mirrors the second Playwright Apply); the feeder
    #    verification in demo.sh confirms a later TSS result uses it.
    status, _ = post_apply(9, -3)
    assert status == 200
    print("PASS: Apply mult=9 add=-3 accepted (feeder will confirm on wire)")

    print("HTTP verification: ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
