"""Loopback HTTPS registry and Git smart-HTTP fixture; never changes user trust."""
import hashlib
import http.server
import json
import os
from pathlib import Path
import ssl
import subprocess
import sys
import zipfile

root, git, openssl = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
root.mkdir(parents=True, exist_ok=True)
env = {key: value for key, value in os.environ.items() if not key.upper().startswith("GIT_")}
env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull, GIT_TERMINAL_PROMPT="0")

def command(args, cwd=None):
    return subprocess.check_output(args, cwd=cwd, env=env, stderr=subprocess.STDOUT).decode().strip()

cert, key = root / "cert.pem", root / "key.pem"
command([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", "/CN=localhost",
         "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1", "-keyout", str(key), "-out", str(cert)])

def manifest(plugin_id, dependencies=None):
    return dict(schemaVersion=1, id=plugin_id, name=plugin_id, version="1.0.0", engineVersion="^1.0.0",
                pluginApiVersion=1, modules=[], dependencies=dependencies or [])

packages = []
for plugin_id, deps in [("org.fixture.shared", []), ("org.fixture.root", [{"id": "org.fixture.shared", "version": "^1.0.0"}]),
                        ("org.fixture.bad", [{"id": "org.fixture.shared", "version": "^1.0.0"}])]:
    data = manifest(plugin_id, deps)
    archive = root / (plugin_id + ".zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as output:
        output.writestr("plugin.json", json.dumps(data))
        output.writestr("Content/data.txt", "real HTTPS package bytes")
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if plugin_id == "org.fixture.bad":
        digest = "0" * 64
    packages.append(dict(id=plugin_id, versions=[dict(version="1.0.0", manifest=data,
                    archive=dict(url=archive.name, sha256=digest))]))
(root / "index.json").write_text(json.dumps(dict(schemaVersion=1, packages=packages)))
with zipfile.ZipFile(root / "traversal.zip", "w") as archive:
    archive.writestr("../escaped.txt", "forbidden")
with zipfile.ZipFile(root / "case.zip", "w") as archive:
    archive.writestr("Content/Name.txt", "a")
    archive.writestr("Content/name.txt", "b")
with zipfile.ZipFile(root / "link.zip", "w") as archive:
    item = zipfile.ZipInfo("link"); item.create_system = 3; item.external_attr = 0o120777 << 16
    archive.writestr(item, "../escaped.txt")

work = root / "git-work"
command([git, "init", str(work)])
(work / "plugin.json").write_text(json.dumps(manifest("org.fixture.git")))
(work / "payload.txt").write_text("Git object bytes")
(work / ".gitattributes").write_text("* filter=must_not_execute\n")
command([git, "-C", str(work), "add", "--all"])
command([git, "-C", str(work), "-c", "user.name=Nullus fixture", "-c", "user.email=fixture@localhost",
         "-c", "commit.gpgsign=false", "commit", "-m", "fixture"])
commit = command([git, "-C", str(work), "rev-parse", "HEAD"])
command([git, "clone", "--bare", str(work), str(root / "repo.git")])

class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(root), **kwargs)

    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path.startswith("/repo.git/"):
            return self.git_request()
        if self.path.startswith("/redirect") or self.path.startswith("/loop"):
            self.send_response(302)
            self.send_header("Location", "/index.json" if self.path.startswith("/redirect") else "/loop")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        return super().do_GET()

    def do_POST(self):
        return self.git_request()

    def git_request(self):
        path, _, query = self.path.partition("?")
        child_env = dict(env, GIT_PROJECT_ROOT=str(root), GIT_HTTP_EXPORT_ALL="1", PATH_INFO=path,
                         QUERY_STRING=query, REQUEST_METHOD=self.command, REMOTE_ADDR="127.0.0.1",
                         CONTENT_TYPE=self.headers.get("Content-Type", ""),
                         CONTENT_LENGTH=self.headers.get("Content-Length", "0"))
        body = self.rfile.read(int(child_env["CONTENT_LENGTH"]))
        response = subprocess.run([git, "http-backend"], input=body, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=child_env, check=True).stdout
        header, _, payload = response.partition(b"\r\n\r\n")
        fields = [line.decode().split(":", 1) for line in header.split(b"\r\n") if b":" in line]
        status = next((int(value.strip().split()[0]) for name, value in fields if name.lower() == "status"), 200)
        self.send_response(status)
        for name, value in fields:
            if name.lower() != "status":
                self.send_header(name, value.strip())
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(cert, key)
server.socket = context.wrap_socket(server.socket, server_side=True)
(root / "ready.json").write_text(json.dumps(dict(url=f"https://localhost:{server.server_port}", commit=commit)))
server.serve_forever()
