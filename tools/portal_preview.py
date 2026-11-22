#!/usr/bin/env python3
"""Preview the web portal's connected pages without an adapter.

The portal shows almost nothing until an adapter answers, and it has no build
step or component tests, so a layout or rendering change to a connected page
cannot be looked at without hardware. This writes a scratch copy of
web/index.html whose transport is answered from the repository's own fixtures:

  contracts/management/kbm-wire-corpus.json   replies produced by the real
                                              firmware formatters
  contracts/management/protocol-v1.json       the management protocol vectors

so every page renders against real reply shapes. The copy is for looking at.
It is never deployed and proves nothing about a real adapter.

    python tools/portal_preview.py                 # write build/portal-preview/
    python tools/portal_preview.py --serve 8799    # and serve it on localhost
    python tools/portal_preview.py --shots         # and screenshot each page
                                                   # with headless Edge/Chrome

Pages and states are chosen in the URL:

    index.html#kbm                         a page (play kbm amiibo custom adapter settings)
    index.html?start=1#play                Controller Link started
    index.html?layout=kbm&tab=kbm:mouse#kbm   a KB/M layout and a tab
    index.html?tab=adapter:controllers#adapter
    index.html?pick=key:1A#kbm             the key picker open on W
"""
import argparse
import functools
import http.server
import json
import os
import shutil
import subprocess
import sys
import urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

HARNESS = """
// ---- preview harness: scratch copy only, never deployed ----
const PREVIEW_REPLIES = %s;
writeTransportLine = async text => {
  const reply = PREVIEW_REPLIES[text.trim()] || { ok: true };
  setTimeout(() => onLine(JSON.stringify(reply)), 0);
};
(async () => {
  const params = new URLSearchParams(location.search);
  transportKind = "bluetooth";
  bleClIn = { writeValueWithoutResponse: async () => {} };
  navigator.getGamepads = () => [{
    index: 0, id: "Xbox Wireless Controller (STANDARD GAMEPAD)", mapping: "standard",
    axes: [0, 0, 0, 0], buttons: Array.from({ length: 18 }, () => ({ pressed: false, value: 0 })),
  }];
  await initializeAdapter("Bluetooth");
  if (params.get("start")) await linkStart();
  if (params.get("layout")) {
    $("#kbmLayoutSel").value = params.get("layout");
    await $("#kbmLayoutSel").onchange();
  }
  for (const tab of (params.get("tab") || "").split(",").filter(Boolean)) {
    const [group, name] = tab.split(":");
    document.querySelector(`.tabs[data-tabs="${group}"] button[data-tab="${name}"]`).click();
  }
  for (let i = 0; i < 200 && (kbm.loading || !kbm.mapOk); i++) await sleep(20);
  if (params.get("pick")) kbmOpenPicker(params.get("pick"));
  document.title = "PicoSwitch2 Portal (preview)";
})();
"""

SHOTS = [
    ("play", "?start=1#play", "1366,860"),
    ("kbm-mapping", "#kbm", "1366,860"),
    ("kbm-mouse", "?layout=kbm&tab=kbm:mouse#kbm", "1366,860"),
    ("kbm-profiles", "?tab=kbm:profiles#kbm", "1366,1000"),
    ("kbm-picker", "?pick=key:1A#kbm", "1366,860"),
    ("amiibo", "#amiibo", "1366,860"),
    ("custom", "#custom", "1366,860"),
    ("adapter-console", "#adapter", "1366,860"),
    ("adapter-controllers", "?tab=adapter:controllers#adapter", "1366,860"),
    ("settings", "#settings", "1366,860"),
    ("play-narrow", "#play", "560,900"),
    ("kbm-narrow", "#kbm", "700,900"),
    # Common phone viewports, CSS px. Portrait uses the bottom bar; landscape
    # wider than 720 px uses the side rail, which must reach every page.
    ("phone-360-play", "?start=1#play", "360,780"),
    ("phone-375-kbm", "#kbm", "375,667"),
    ("phone-390-amiibo", "#amiibo", "390,844"),
    ("phone-412-adapter", "#adapter", "412,915"),
    ("phone-430-settings", "#settings", "430,932"),
    ("phone-667x375-play", "#play", "667,375"),
    ("phone-844x390-kbm", "#kbm", "844,390"),
    ("phone-915x412-settings", "#settings", "915,412"),
    ("phone-932x430-adapter", "#adapter", "932,430"),
]


def replies():
    def load(relative):
        with open(os.path.join(ROOT, relative), encoding="utf-8") as handle:
            return json.load(handle)

    corpus = load("contracts/management/kbm-wire-corpus.json")
    fixture = load("contracts/management/protocol-v1.json")
    table = {entry["command"]: entry["reply"] for entry in corpus["replies"]}
    for vector in fixture["vectors"].values():
        table.setdefault(vector["command"], vector["reply"])
    for step in fixture["pagination"]["peers"]:
        table[step["command"]] = step["reply"]
    with open(os.path.join(ROOT, "VERSION"), encoding="utf-8") as handle:
        version = handle.read().strip()
    # The fixture's info vector predates the current contract; a preview with a
    # contract mismatch would only ever show the mismatch notice.
    table["info"] = {"id": "picoswitch", "product": "PicoSwitch2", "version": version,
                     "bridge_contract": 4, "build": "preview"}
    table["mgmt status"] = {"ok": True, "enabled": True}
    table["clink start"] = {"clink": {"active": True, "att_mtu": 247, "min_att_mtu": 33, "mtu_ok": True}}
    table["clink stop"] = {"clink": {"active": False}}
    table["amiibo status"] = dict(table["amiibo status"], loaded=False, v3loaded=False)
    return table


def build(out):
    with open(os.path.join(ROOT, "web", "index.html"), encoding="utf-8") as handle:
        html = handle.read().replace("\r\n", "\n")
    marker = "</script>\n</body>"
    if html.count(marker) != 1:
        raise SystemExit("web/index.html no longer ends its script the way this tool expects")
    html = html.replace(marker, HARNESS % json.dumps(replies()) + marker)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "index.html"), "w", encoding="utf-8", newline="\n") as handle:
        handle.write(html)
    # Headless Chromium will not make a window narrower than about 500 px, so
    # phone-width shots load the page in an iframe of the exact size: media
    # queries inside a frame follow the frame's width.
    with open(os.path.join(out, "frame.html"), "w", encoding="utf-8", newline="\n") as handle:
        handle.write(FRAME)
    return out


FRAME = """<!doctype html><meta charset="utf-8"><title>frame</title>
<style>html,body{margin:0;background:#000}iframe{border:0;display:block}</style>
<iframe id="f"></iframe>
<script>
const q = new URLSearchParams(location.search), f = document.getElementById("f");
f.width = q.get("w"); f.height = q.get("h"); f.src = "index.html" + q.get("page");
</script>
"""


def shot_url(port, query, size):
    """The URL to screenshot; phone widths go through frame.html."""
    width, height = (int(part) for part in size.split(","))
    if width >= 500:
        return f"http://127.0.0.1:{port}/index.html{query}", size
    page = urllib.parse.quote(query, safe="")
    return f"http://127.0.0.1:{port}/frame.html?w={width}&h={height}&page={page}", f"500,{height}"


def browser():
    candidates = [
        os.path.join(os.environ.get("ProgramFiles(x86)", ""), "Microsoft", "Edge", "Application", "msedge.exe"),
        os.path.join(os.environ.get("ProgramFiles", ""), "Microsoft", "Edge", "Application", "msedge.exe"),
        os.path.join(os.environ.get("ProgramFiles", ""), "Google", "Chrome", "Application", "chrome.exe"),
    ]
    for name in ("msedge", "google-chrome", "chromium", "chromium-browser", "chrome"):
        found = shutil.which(name)
        if found:
            candidates.append(found)
    return next((path for path in candidates if path and os.path.isfile(path)), None)


class _Server(http.server.ThreadingHTTPServer):
    # On Windows the default lets a second server bind a port that is already
    # being served, and the browser then reaches whichever one answers: the
    # screenshots silently show some other directory. Fail to bind instead.
    allow_reuse_address = False


class _Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def serve(out, port):
    """Serve `out` on 127.0.0.1. Port 0 asks the system for a free one."""
    return _Server(("127.0.0.1", port), functools.partial(_Quiet, directory=out))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=os.path.join(ROOT, "build", "portal-preview"))
    parser.add_argument("--serve", type=int, metavar="PORT", help="serve the preview on 127.0.0.1")
    parser.add_argument("--shots", action="store_true", help="screenshot every page into <out>/shots")
    args = parser.parse_args()

    out = build(os.path.abspath(args.out))
    print(f"preview written to {os.path.join(out, 'index.html')}")

    if args.shots:
        exe = browser()
        if not exe:
            raise SystemExit("no Edge or Chrome found for screenshots")
        import threading
        server = serve(out, args.serve or 0)
        port = server.server_address[1]
        threading.Thread(target=server.serve_forever, daemon=True).start()
        shots = os.path.join(out, "shots")
        os.makedirs(shots, exist_ok=True)
        import time
        failed = 0
        for name, query, wanted in SHOTS:
            url, size = shot_url(port, query, wanted)
            target = os.path.join(shots, name + ".png")
            if os.path.isfile(target):
                os.remove(target)
            # The launcher process returns before the browser has written the
            # file, and two launches sharing a profile collide. So: one profile
            # per shot, and wait for the file rather than for the process.
            profile = os.path.join(out, "browser-profile", name)
            subprocess.run([exe, "--headless=new", "--disable-gpu", "--no-first-run",
                            f"--user-data-dir={profile}", f"--window-size={size}",
                            "--virtual-time-budget=8000", f"--screenshot={target}",
                            url],
                           check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            deadline = time.monotonic() + 40
            while not os.path.isfile(target) and time.monotonic() < deadline:
                time.sleep(0.25)
            ok = os.path.isfile(target)
            failed += 0 if ok else 1
            print(("wrote " if ok else "FAILED ") + target)
        server.shutdown()
        shutil.rmtree(os.path.join(out, "browser-profile"), ignore_errors=True)
        return 1 if failed else 0
        server.shutdown()
        return 0

    if args.serve:
        server = serve(out, args.serve)
        print(f"serving http://127.0.0.1:{args.serve}/index.html  (Ctrl+C to stop)")
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
