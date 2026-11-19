"""The resource packs endgate serves from its config, and the backends that must offer the same ones."""

import json
import random
import zipfile

import pytest
from bedrock import Backend

PACK_UUID = "29e55501-89d9-452e-88c3-e953a915fa27"
PACK = f"{PACK_UUID}_1.0.3"
LOBBY_RUNTIME_ID = 1
SECOND_RUNTIME_ID = 7


@pytest.fixture
def pack_file(tmp_path):
    path = tmp_path / "pack.mcpack"
    manifest = {
        "format_version": 2,
        "header": {"name": "Endgate Test", "uuid": PACK_UUID, "version": [1, 0, 3], "min_engine_version": [1, 26, 0]},
        "modules": [{"type": "resources", "uuid": "0e1c0c5e-6d1c-4b4a-9f4b-1f2e3d4c5b6a", "version": [1, 0, 3]}],
    }
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("Endgate Test/manifest.json", json.dumps(manifest))
        archive.writestr("Endgate Test/textures/noise.bin", random.Random(0).randbytes(250_000), zipfile.ZIP_STORED)
    return path


def packs_setting(pack_file, required: bool = False) -> dict[str, dict[str, str]]:
    return {
        "resource-packs": {
            "packs": f'["{pack_file.as_posix()}"]',
            "texturepack-required": str(required).lower(),
        }
    }


@pytest.fixture
def lobby():
    server = Backend("raknet", play=True, runtime_id=LOBBY_RUNTIME_ID, unique_id=-1, packs=(PACK,))
    yield server
    server.close()


@pytest.fixture
def second():
    server = Backend("raknet", play=True, runtime_id=SECOND_RUNTIME_ID, unique_id=-7, packs=(PACK,))
    yield server
    server.close()


@pytest.fixture
def no_packs():
    server = Backend("raknet", play=True, runtime_id=SECOND_RUNTIME_ID, unique_id=-7)
    yield server
    server.close()


def test_the_player_downloads_the_packs_the_proxy_serves(make_proxy, lobby, pack_file, connect):
    proxy = make_proxy(lobby.port, settings=packs_setting(pack_file, required=True))
    player = connect(proxy)
    assert player.wait_login(), proxy.lines[-20:]
    lobby.accept()

    assert player.peer.wait("resource-packs", 10) == f"1 {PACK}", proxy.lines[-20:]
    assert player.peer.wait("resource-pack", 10) == f"{PACK} {pack_file.stat().st_size} 1"
    assert player.peer.wait("pack-stack", 10) == f"1 {PACK}"
    assert player.peer.wait("start-game", 10) is not None


def test_a_server_offering_other_packs_than_the_proxy_is_refused(make_proxy, no_packs, pack_file, connect):
    proxy = make_proxy(no_packs.port, settings=packs_setting(pack_file))
    player = connect(proxy)
    assert player.wait_login(), proxy.lines[-20:]
    no_packs.accept()

    disconnect = player.wait_disconnect()
    assert disconnect is not None, proxy.lines[-20:]
    assert disconnect[1].endswith("it needs other resource packs than the proxy serves"), disconnect
    assert player.peer.pending("start-game") == []


def test_a_server_offering_packs_the_proxy_does_not_serve_is_refused(make_proxy, lobby, connect):
    proxy = make_proxy(lobby.port)
    player = connect(proxy)
    assert player.wait_login(), proxy.lines[-20:]
    lobby.accept()

    disconnect = player.wait_disconnect()
    assert disconnect is not None, proxy.lines[-20:]
    assert disconnect[1].endswith("it needs other resource packs than the proxy serves"), disconnect


def test_a_switch_keeps_the_packs_the_player_joined_with(make_proxy, lobby, second, no_packs, pack_file, connect):
    proxy = make_proxy(
        lobby.port,
        servers={"second": f"127.0.0.1:{second.port}", "bare": f"127.0.0.1:{no_packs.port}"},
        console=True,
        settings=packs_setting(pack_file),
    )
    player = connect(proxy)
    assert player.wait_login(), proxy.lines[-20:]
    lobby.accept()
    assert player.peer.wait("start-game", 10) is not None, proxy.lines[-20:]

    proxy.command(f"send {player.identity.name} bare")
    no_packs.accept()
    assert player.wait_text() == "Connecting to bare…"
    refusal = "§cUnable to connect to bare: it needs other resource packs than the ones you joined with"
    assert player.wait_text() == refusal

    proxy.command(f"send {player.identity.name} second")
    downstream = second.accept()
    assert downstream.peer.wait("initialized", 10) == str(SECOND_RUNTIME_ID), proxy.lines[-20:]
    assert player.peer.pending("resource-packs") == [f"0 {PACK}"]
