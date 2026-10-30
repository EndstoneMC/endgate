from __future__ import annotations

import queue
import time
from pathlib import Path

import pytest
from bedrock import PROTOCOL, Peer, PlayerIdentity
from raw import RawClient
from wire import Reliability


class RawPlayer(RawClient):
    """A hand-written RakNet client; the test frames the datagrams and a Bedrock peer fills them."""

    def __init__(self, server: tuple[str, int]) -> None:
        super().__init__(server)
        self.outbox: queue.Queue[bytes] = queue.Queue()
        self.peer: Peer | None = None
        self.backend = None

    def sign_in(self, key_path: Path, timeout: float = 15.0) -> None:
        identity = PlayerIdentity()
        identity.write_key(key_path)
        arguments = ["client", "--raknet", "--protocol", str(PROTOCOL), "--key", str(key_path)]
        arguments += ["--request", identity.connection_request().hex()]
        self.peer = Peer(arguments, self.outbox.put)
        self.peer.command("start")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and not self.peer.pending("play-status"):
            self.pump()
        assert self.peer.pending("play-status") == ["0"], self.peer.log

    def pump(self, quiet: float = 0.05) -> None:
        """Sends what the peer has produced, takes in what arrived and hands it to the peer."""
        while True:
            try:
                self.send(self.outbox.get(timeout=0.05))
            except queue.Empty:
                break
        self.collect(quiet)
        for message in self.messages:
            self.peer.command("wire " + message.hex())
        self.messages.clear()

    def encode(self, text: str) -> bytes:
        """The message that carries a chat line, as the peer would put it on the wire."""
        self.peer.command("text " + text.encode().hex())
        return self.outbox.get(timeout=5)

    def say(self, text: str, reliability: int = Reliability.RELIABLE_ORDERED, channel: int = 0):
        return self.send(self.encode(text), reliability, channel)

    def heard(self, timeout: float = 0.5) -> list[str]:
        """The chat lines that reached this player."""
        self.pump()
        lines = []
        while (line := self.peer.wait("text", timeout)) is not None:
            lines.append(bytes.fromhex(line).decode())
            timeout = 0.05
        return lines

    def close(self) -> None:
        super().close()
        if self.peer is not None:
            self.peer.stop()


@pytest.fixture
def join_raw(backend, tmp_path):
    """Opens a hand-written RakNet connection to the proxy and logs it in; the backend's session is its `backend`."""
    clients: list[RawPlayer] = []

    def join(proxy) -> RawPlayer:
        client = RawPlayer(proxy.address)
        clients.append(client)
        client.transcript = client.connect()
        client.sign_in(tmp_path / f"raw{len(clients)}.pem")
        client.backend = backend.accept(timeout=10)
        time.sleep(0.05)
        return client

    yield join
    for client in clients:
        client.close()


@pytest.fixture
def player(proxy, join_raw):
    return join_raw(proxy)
