"""Bedrock players and backends for the integration tests.

The Bedrock side of each end, framing, compression, encryption and the login packets, is the C++
endgate_bedrock_peer built on bedrock-protocol. Python only carries its messages over RakNet and NetherNet,
and mints the tokens a login carries.
"""

from __future__ import annotations

import asyncio
import base64
import collections
import http.server
import itertools
import json
import os
import queue
import random
import string
import struct
import subprocess
import threading
import time
from pathlib import Path

import nethernet
import raknet
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
from harness import EXE, PLAYER_IDENTITY, REPOSITORY, free_port
from nethernet import serve_http
from nethernet.identity import IdentitySigner, generate_operator_key

DEFAULT_PEER = REPOSITORY / "build" / "RelWithDebInfo" / "tests" / "interop" / f"endgate_bedrock_peer{EXE}"
PROTOCOL = 2225
LOGIN_SUCCESS = 0
MULTIPLAYER_AUDIENCE = "api://auth-minecraft-services/multiplayer"


def noise(size: int, seed: int = 0) -> str:
    """Text that does not compress, so a message of this size is about this large on the wire."""
    return "".join(random.Random(seed).choices(string.ascii_letters + string.digits, k=size))


def peer_binary() -> Path:
    path = Path(os.environ.get("ENDGATE_BEDROCK_PEER", DEFAULT_PEER))
    if not path.exists():
        raise RuntimeError(f"endgate_bedrock_peer not found at {path}")
    return path


def base64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def spki(key: ec.EllipticCurvePrivateKey) -> str:
    der = key.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    return base64.b64encode(der).decode()


def sign_token(key: ec.EllipticCurvePrivateKey, header: dict, claims: dict) -> str:
    """A compact JWT signed with ES256 or ES384 according to the key's curve."""
    bits = key.curve.key_size
    header = {"alg": f"ES{256 if bits == 256 else 384}", **header}
    signing_input = base64url(json.dumps(header).encode()) + "." + base64url(json.dumps(claims).encode())
    digest = hashes.SHA256() if bits == 256 else hashes.SHA384()
    r, s = decode_dss_signature(key.sign(signing_input.encode(), ec.ECDSA(digest)))
    size = (bits + 7) // 8
    return signing_input + "." + base64url(r.to_bytes(size, "big") + s.to_bytes(size, "big"))


def jwk(key: ec.EllipticCurvePrivateKey, kid: str) -> dict:
    numbers = key.public_key().public_numbers()
    size = (key.curve.key_size + 7) // 8

    def coordinate(value: int) -> str:
        return base64url(value.to_bytes(size, "big"))

    return {"kty": "EC", "crv": "P-256", "kid": kid, "x": coordinate(numbers.x), "y": coordinate(numbers.y)}


class Services:
    """A stand-in for Minecraft Services' discovery, OpenID configuration and key set, on loopback."""

    KEY_ID = "endgate-test-key"
    ISSUER = "https://endgate.test/issuer"

    def __init__(self) -> None:
        self.key = ec.generate_private_key(ec.SECP256R1())
        services = self
        self.hits: list[str] = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self) -> None:
                services.hits.append(self.path)
                base = f"http://127.0.0.1:{services.port}"
                if "/discovery/" in self.path:
                    body = {"result": {"serviceEnvironments": {"auth": {"prod": {"serviceUri": base}}}}}
                elif self.path.endswith("/.well-known/openid-configuration"):
                    body = {"jwks_uri": f"{base}/keys", "issuer": services.ISSUER}
                elif self.path == "/keys":
                    body = {"keys": [jwk(services.key, services.KEY_ID)]}
                else:
                    self.send_error(404)
                    return
                payload = json.dumps(body).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def log_message(self, *args) -> None:
                pass

        self._server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self._server.server_address[1]
        threading.Thread(target=self._server.serve_forever, daemon=True).start()

    @property
    def discovery_uri(self) -> str:
        return f"http://127.0.0.1:{self.port}/api/v1.0/discovery/MinecraftDedicatedServer/builds/test"

    def token(self, identity: PlayerIdentity, audience: str = MULTIPLAYER_AUDIENCE, lifetime: int = 3600) -> str:
        claims = {
            "aud": audience,
            "iss": self.ISSUER,
            "exp": int(time.time()) + lifetime,
            "xid": identity.xuid,
            "xname": identity.name,
            "cpk": identity.cpk,
        }
        return sign_token(self.key, {"kid": self.KEY_ID}, claims)

    def close(self) -> None:
        self._server.shutdown()
        self._server.server_close()


def announcement(protocol: int) -> bytes:
    """A Bedrock server's unconnected pong payload: the semicolon fields behind their length."""
    fields = f"MCPE;Dedicated Server;{protocol};1.26.60;0;10;1;Bedrock level;Survival;1;19132;19133;0;"
    return len(fields).to_bytes(2, "big") + fields.encode()


PLAYER_NUMBERS = itertools.count(1)


class PlayerIdentity:
    """A player's own key and the login it signs with it, self-signed or vouched for by Services."""

    def __init__(self, name: str | None = None, xuid: str | None = None) -> None:
        number = next(PLAYER_NUMBERS)
        self.key = ec.generate_private_key(ec.SECP384R1())
        self.name = name or f"Steve{number}"
        self.xuid = xuid or str(2535400000000000 + number)
        self.cpk = spki(self.key)

    def write_key(self, path: Path) -> Path:
        path.write_bytes(
            self.key.private_bytes(
                serialization.Encoding.PEM,
                serialization.PrivateFormat.PKCS8,
                serialization.NoEncryption(),
            )
        )
        return path

    def connection_request(self, services: Services | None = None, token: str | None = None) -> bytes:
        """The login's payload: its authentication info and its client data, each behind a 32-bit length."""
        if token is not None:
            authentication = {"AuthenticationType": 0, "Token": token}
        elif services is not None:
            authentication = {"AuthenticationType": 0, "Token": services.token(self)}
        else:
            now = int(time.time())
            claims = {"cpk": self.cpk, "exp": now + 3600, "iat": now, "xname": self.name}
            authentication = {"AuthenticationType": 2, "Token": sign_token(self.key, {"x5u": self.cpk}, claims)}
        client_data = sign_token(
            self.key,
            {"x5u": self.cpk},
            {"ThirdPartyName": self.name, "GameVersion": "1.26.60", "DeviceModel": "EndgateTest"},
        )
        parts = [json.dumps(authentication).encode(), client_data.encode()]
        return b"".join(struct.pack("<I", len(part)) + part for part in parts)


class Loop:
    """An asyncio loop on its own thread, so the blocking test code can drive NetherNet."""

    def __init__(self) -> None:
        self.loop = asyncio.new_event_loop()
        threading.Thread(target=self.loop.run_forever, daemon=True).start()

    def run(self, coroutine, timeout: float | None = None):
        return asyncio.run_coroutine_threadsafe(coroutine, self.loop).result(timeout)


LOOP = Loop()


class RakNetLink:
    """A RakNet connection, carrying the messages with their 0xFE prefix."""

    prefixed = True

    def __init__(self, connection: raknet.Connection) -> None:
        self.connection = connection
        self.options: dict = {}

    def send(self, data: bytes) -> None:
        self.connection.send(data, **self.options)

    def recv(self, timeout: float) -> bytes:
        return self.connection.recv(timeout=timeout)

    def close(self) -> None:
        self.connection.close()


class NetherNetLink:
    """A NetherNet connection, carrying the batches without a prefix."""

    prefixed = False

    def __init__(self, connection: nethernet.Connection) -> None:
        self.connection = connection
        self.options: dict = {}

    def send(self, data: bytes) -> None:
        LOOP.run(self.connection.send(data, **self.options), 30)

    def recv(self, timeout: float) -> bytes:
        return LOOP.run(asyncio.wait_for(self.connection.recv(), timeout), timeout + 5)

    def close(self) -> None:
        LOOP.run(self.connection.close(), 10)


CLOSED = (raknet.ConnectionClosed, nethernet.ConnectionClosed)


class Peer:
    """One endgate_bedrock_peer process, its events collected by kind."""

    def __init__(self, arguments: list[str], send) -> None:
        self.process = subprocess.Popen(
            [str(peer_binary()), *arguments],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self.send = send
        self.log: list[str] = []
        self.wires = 0
        self._events: dict[str, collections.deque[str]] = collections.defaultdict(collections.deque)
        self._condition = threading.Condition()
        self._lock = threading.Lock()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self) -> None:
        for line in self.process.stdout:
            kind, _, rest = line.rstrip("\n").partition(" ")
            if kind == "wire":
                try:
                    self.send(bytes.fromhex(rest))
                except Exception:
                    pass
                with self._condition:
                    self.wires += 1
                    self._condition.notify_all()
                continue
            with self._condition:
                self.log.append(line.rstrip("\n"))
                self._events[kind].append(rest)
                self._condition.notify_all()

    def command(self, line: str) -> None:
        with self._lock:
            try:
                self.process.stdin.write(line + "\n")
                self.process.stdin.flush()
            except (BrokenPipeError, ValueError):
                pass

    def wait(self, kind: str, timeout: float = 10.0) -> str | None:
        """The oldest event of this kind not yet taken, or None once the time is up."""
        deadline = time.monotonic() + timeout
        with self._condition:
            while not self._events[kind]:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not self._condition.wait(remaining):
                    return None
            return self._events[kind].popleft()

    def wait_wire(self, count: int, timeout: float = 10.0) -> bool:
        """Whether the peer has put `count` messages on the wire by now."""
        deadline = time.monotonic() + timeout
        with self._condition:
            while self.wires < count:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not self._condition.wait(remaining):
                    return False
            return True

    def pending(self, kind: str) -> list[str]:
        with self._condition:
            return list(self._events[kind])

    def stop(self) -> None:
        try:
            self.process.stdin.close()
        except (BrokenPipeError, ValueError):
            pass
        try:
            self.process.wait(5)
        except subprocess.TimeoutExpired:
            self.process.kill()


class Session:
    """A transport link joined to a peer: what arrives on the link goes to the peer, what the peer sends goes out."""

    def __init__(self, link, arguments: list[str]) -> None:
        self.link = link
        self.peer = Peer(arguments + (["--raknet"] if link.prefixed else []), link.send)
        self.closed = threading.Event()
        self.outcome = "open"
        self.thread = threading.Thread(target=self._pump, daemon=True)
        self.thread.start()

    def _pump(self) -> None:
        while not self.closed.is_set():
            try:
                data = self.link.recv(0.5)
            except TimeoutError:
                continue
            except CLOSED as error:
                self.outcome = type(error).__name__
                break
            except Exception as error:
                self.outcome = type(error).__name__
                break
            self.peer.command("wire " + data.hex())
        self.closed.set()

    def text(self, message: str) -> None:
        self.peer.command("text " + message.encode().hex())

    def say(self, message: str, **options) -> None:
        """Sends a chat line with the transport's own send options, and waits until it has left."""
        sent = self.peer.wires
        self.link.options = options
        self.text(message)
        self.peer.wait_wire(sent + 1)
        self.link.options = {}

    def wait_text(self, timeout: float = 10.0) -> str | None:
        received = self.peer.wait("text", timeout)
        return None if received is None else bytes.fromhex(received).decode()

    def texts(self, timeout: float = 0.5) -> list[str]:
        """The chat lines that arrive until a gap of `timeout` passes."""
        lines = []
        while (line := self.wait_text(timeout)) is not None:
            lines.append(line)
            timeout = 0.05
        return lines

    def wait_disconnect(self, timeout: float = 10.0) -> tuple[int, str] | None:
        received = self.peer.wait("disconnect", timeout)
        if received is None:
            return None
        reason, _, message = received.partition(" ")
        return int(reason), bytes.fromhex(message).decode()

    def wait_closed(self, timeout: float = 10.0) -> bool:
        return self.closed.wait(timeout)

    def close(self) -> None:
        self.closed.set()
        try:
            self.link.close()
        except Exception:
            pass
        self.peer.stop()


class Player(Session):
    """A Bedrock client logged in through the proxy."""

    def __init__(
        self, link, identity: PlayerIdentity, request: bytes, key_path: Path, protocol: int = PROTOCOL
    ) -> None:
        self.identity = identity
        super().__init__(
            link,
            ["client", "--protocol", str(protocol), "--key", str(key_path), "--request", request.hex()],
        )
        self.peer.command("start")

    def wait_login(self, timeout: float = 15.0) -> bool:
        status = self.peer.wait("play-status", timeout)
        return status is not None and int(status) == LOGIN_SUCCESS


def connect_raknet(proxy, timeout: float = 5.0) -> RakNetLink:
    return RakNetLink(raknet.create_connection(proxy.address, timeout=timeout))


def connect_nethernet(proxy) -> NetherNetLink:
    return NetherNetLink(LOOP.run(nethernet.connect_http(proxy.url, timeout=20, identity_signer=PLAYER_IDENTITY), 30))


def make_player(
    proxy, tmp_path: Path, transport: str = "raknet", services: Services | None = None, **options
) -> Player:
    """Connects to the proxy and starts the login; wait_login says whether it succeeded."""
    identity = options.pop("identity", None) or PlayerIdentity()
    request = options.pop("request", None) or identity.connection_request(services)
    key_path = identity.write_key(tmp_path / f"{identity.name}-{id(identity)}.pem")
    link = connect_raknet(proxy) if transport == "raknet" else connect_nethernet(proxy)
    return Player(link, identity, request, key_path, **options)


class Backend:
    """A Bedrock server on RakNet or NetherNet that logs players in and hands each one's session to the test."""

    def __init__(
        self,
        transport: str = "raknet",
        encrypt: bool = False,
        threshold: int = 256,
        delay: int = 0,
        protocol: int = 2225,
        play: bool = False,
        runtime_id: int = 1,
        unique_id: int = -1,
        checksum: int = 0,
        dimension: int = 0,
    ) -> None:
        self.transport = transport
        self.protocol = protocol
        self.arguments = [
            "server",
            "--protocol",
            str(protocol or PROTOCOL),
            "--threshold",
            str(threshold),
            "--delay",
            str(delay),
        ]
        self.arguments += ["--encrypt"] if encrypt else []
        if play:
            self.arguments += ["--play", "--runtime-id", str(runtime_id), "--unique-id", str(unique_id)]
            self.arguments += ["--checksum", str(checksum), "--dimension", str(dimension)]
        self.sessions: queue.Queue[Session] = queue.Queue()
        self.all: list[Session] = []
        if transport == "raknet":
            self.server = raknet.create_server(
                ("127.0.0.1", free_port()), max_connections=32, offline_ping_response=announcement(protocol)
            )
            self.port = self.server.local_address[1]
            threading.Thread(target=self.server.serve_forever, args=(self._serve,), daemon=True).start()
        else:
            self._start_nethernet()

    def _start_nethernet(self) -> None:
        signer = IdentitySigner(generate_operator_key(), domain="endgate-test")

        async def handler(connection) -> None:
            session = Session(NetherNetLink(connection), self.arguments)
            self._publish(session)
            await connection.wait_closed()

        async def start():
            server = serve_http(handler, host="127.0.0.1", port=0, identity_signer=signer)
            await server.start()
            return server

        self.server = LOOP.run(start(), 20)
        self.port = self.server.bound_port

    def _serve(self, connection) -> None:
        session = Session(RakNetLink(connection), self.arguments)
        self._publish(session)
        session.wait_closed(3600)

    def _publish(self, session: Session) -> None:
        self.all.append(session)
        self.sessions.put(session)

    def accept(self, timeout: float = 10.0) -> Session:
        """The next player the proxy sends here, once the login has reached the backend."""
        return self.sessions.get(timeout=timeout)

    def close(self) -> None:
        for session in self.all:
            session.close()
        if self.transport == "raknet":
            self.server.close(0)
        else:
            LOOP.run(self.server.aclose(), 10)
