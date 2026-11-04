#!/usr/bin/env python3
"""Run the official macOS KOReader client against an isolated loopback KOSync peer."""
import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import threading
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
LOCAL = ROOT / ".local"
ARTIFACT = 11323735856
COMMIT = "83f443ce4b3dfd2e3366b97bf3e3333dfc46718e"
DIGEST = "ec5342a53f85131cb7a903cd6ed550619e06b62899b95ba25cca3e13ff3fb258"


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def prepare_client():
    archive = LOCAL / "downloads/koreader-macos-arm64-83f443c.zip"
    if not archive.exists():
        archive.parent.mkdir(parents=True, exist_ok=True)
        with archive.open("wb") as output:
            run("gh", "api", f"repos/koreader/koreader/actions/artifacts/{ARTIFACT}/zip", stdout=output)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != DIGEST:
        raise ValueError("Official KOReader artifact digest mismatch")
    destination = LOCAL / "koreader-client"
    client = destination / "KOReader.app/Contents/koreader"
    if not (client / "luajit").exists():
        destination.mkdir(exist_ok=True)
        names = run("tar", "-tf", str(archive), capture_output=True, text=True).stdout.splitlines()
        if any(Path(name).is_absolute() or ".." in Path(name).parts for name in names):
            raise ValueError("Unsafe client archive path")
        run("tar", "-xf", str(archive), "-C", str(destination))
    return client


def prepare_bridge():
    project = LOCAL / "koreader-client-bridge"
    project.mkdir(exist_ok=True)
    project.joinpath("CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(koreader_client_bridge LANGUAGES C CXX)
add_subdirectory("{ROOT}/firmware/components/reader_core" core)
add_subdirectory("{ROOT}/firmware/components/reader_store" store)
add_subdirectory("{ROOT}/firmware/components/reader_document" document)
add_subdirectory("{ROOT}/firmware/components/reader_sync" sync)
add_executable(koreader_sync_bridge "{ROOT}/tools/koreader_sync_bridge.cpp")
target_include_directories(koreader_sync_bridge PRIVATE "{ROOT}/firmware/components/reader_store/third_party/cjson")
target_link_libraries(koreader_sync_bridge PRIVATE reader_sync)
''')
    run("cmake", "-S", str(project), "-B", str(project / "build"), "-DCMAKE_BUILD_TYPE=Release")
    run("cmake", "--build", str(project / "build"), "-j", "4")
    return project / "build/koreader_sync_bridge"


def check(client, bridge, book, font_size):
    state = LOCAL / "koreader-client-runs" / f"{book.stem}-{font_size}-{secrets.token_hex(4)}"
    state.mkdir(parents=True)
    (state / "patches").mkdir()
    shutil.copyfile(ROOT / "tools/koreader_client_check.lua", state / "patches/2-pico-validation.lua")
    # KOReader may create a sidecar next to the book. Use an unchanged isolated copy.
    isolated_book = state / book.name
    shutil.copyfile(book, isolated_book)
    username, userkey = "pico-test-" + secrets.token_hex(8), secrets.token_hex(16)
    env = os.environ.copy()
    for name in ("http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY"):
        env.pop(name, None)
    env.update(KO_HOME=str(state), SDL_VIDEODRIVER="dummy", PICO_TEST_USER=username, PICO_TEST_KEY=userkey)
    env["NO_PROXY"] = env["no_proxy"] = "127.0.0.1,localhost"
    offer = json.loads(run(str(bridge), str(book), "offer", env=env, capture_output=True, text=True).stdout)
    records, accounts, requests = {}, {}, []
    official_upload = threading.Event()

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def reply(self, code, body):
            encoded = json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(encoded)))
            self.end_headers()
            self.wfile.write(encoded)

        def handle_request(self):
            size = int(self.headers.get("Content-Length", 0))
            if size > 32768:
                return self.reply(413, {})
            body = json.loads(self.rfile.read(size)) if size else {}
            requests.append((self.command, self.path))
            if self.path == "/users/create" and self.command == "POST":
                if body != {"username": username, "password": userkey}:
                    return self.reply(400, {})
                accounts[username] = userkey
                return self.reply(201, {})
            if self.headers.get("x-auth-user") != username or accounts.get(username) != self.headers.get("x-auth-key"):
                return self.reply(401, {})
            if self.headers.get("accept") != "application/vnd.koreader.v1+json":
                return self.reply(406, {})
            if self.path == "/users/auth":
                return self.reply(200, {})
            if self.path == "/syncs/progress" and self.command == "PUT":
                if body.get("document") != offer["document"] or not isinstance(body.get("progress"), str):
                    return self.reply(400, {})
                records[body["document"]] = body | {"timestamp": int(time.time())}
                if body.get("device_id") == "official-koreader-validation":
                    official_upload.set()
                return self.reply(200, {})
            if self.path == "/syncs/progress/" + offer["document"] and self.command == "GET":
                return self.reply(200, records.get(offer["document"], {}))
            return self.reply(404, {})

        do_GET = do_POST = do_PUT = handle_request

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    endpoint = f"http://127.0.0.1:{server.server_port}"
    fixture = {key: offer[key] for key in ("document", "pointer", "end_pointer", "text")}
    fixture.update(endpoint=endpoint, username=username, userkey=userkey, font_size=font_size)
    (state / "fixture.json").write_text(json.dumps(fixture))
    (state / "fixture.json").chmod(0o600)
    (state / "settings.reader.lua").write_text(
        f'return {{ color_rendering=true, copt_font_size={font_size}, sdl_window={{width=684,height=1216}}, followed_link_marker=false }}')
    try:
        with (state / "client.log").open("w") as log:
            process = subprocess.Popen([str(client / "luajit"), "reader.lua", str(isolated_book)], cwd=client, env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 45
                while not official_upload.wait(.1):
                    if process.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError(f"Official client did not upload; inspect {state / 'client.log'}")
                uploaded = records[offer["document"]].copy()
                recovered = json.loads(run(str(bridge), str(book), "receive", input=json.dumps(uploaded), capture_output=True, text=True).stdout)
                if recovered["pointer"] != uploaded["progress"]:
                    raise ValueError("Native peer changed the official client position")
                request = urllib.request.Request(endpoint + "/syncs/progress", data=offer["body"].encode(), headers=offer["headers"], method=offer["method"])
                with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(request, timeout=5) as response:
                    if response.status != 200:
                        raise ValueError("Native request was not confirmed")
                (state / "native-ready.json").write_text(json.dumps(recovered))
                process.wait(timeout=25)
                result = json.loads((state / "result.json").read_text())
                if process.returncode != 0 or not result.get("ok") or not result.get("empty_record"):
                    raise RuntimeError(f"Official client validation failed: {result}")
                fixture["resume"] = True
                (state / "fixture.json").write_text(json.dumps(fixture))
                with (state / "restart.log").open("w") as restart_log:
                    run(str(client / "luajit"), "reader.lua", str(isolated_book), cwd=client,
                        env=env, stdout=restart_log, stderr=subprocess.STDOUT, timeout=20)
                resumed = json.loads((state / "result.json").read_text())
                if not resumed.get("ok") or resumed.get("restored") != offer["pointer"]:
                    raise RuntimeError(f"Official client restart failed: {resumed}")
                result["restart"] = resumed
                result.update(book_sha256=hashlib.sha256(book.read_bytes()).hexdigest(), client_commit=COMMIT,
                              artifact_sha256=DIGEST, native_recovered=recovered, requests=requests,
                              transport="isolated loopback HTTP fixture; no external sync service")
                (state / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2))
                print(f"PASS {book.name}, {font_size} px: official push/pull/restart + native exact recovery; {state / 'result.json'}", flush=True)
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
    finally:
        server.shutdown()
        server.server_close()
        (state / "fixture.json").unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--book", type=Path, action="append", required=True)
    parser.add_argument("--font-size", type=int, action="append")
    args = parser.parse_args()
    client, bridge = prepare_client(), prepare_bridge()
    for book in args.book:
        for size in args.font_size or (28, 38, 56):
            check(client, bridge, book.resolve(), size)


if __name__ == "__main__":
    main()
