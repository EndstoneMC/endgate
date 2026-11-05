import os
import re
import subprocess

from harness import REPOSITORY, free_port


def exchange(proxy, connect, backend) -> None:
    player = connect(proxy)
    assert player.wait_login()
    upstream = backend.accept()
    player.text("hello")
    assert upstream.wait_text() == "hello"
    player.close()


def test_player_connections_are_logged(make_proxy, backend, connect):
    proxy = make_proxy(backend.port)
    exchange(proxy, connect, backend)
    assert proxy.wait_for(r"INFO \[connected player\] .* has disconnected", 10), proxy.lines[-20:]
    assert proxy.events(r"INFO \[connected player\] .* has connected")
    assert proxy.events(r"INFO \[server connection\] .* -> lobby has connected")
    assert proxy.events(r"INFO \[server connection\] .* -> lobby has disconnected")


def test_log_player_connections_off_keeps_them_quiet(make_proxy, backend, connect):
    proxy = make_proxy(backend.port, debug=True, log_player_connections=False)
    exchange(proxy, connect, backend)
    assert proxy.wait_for(r"DEBUG \d+\.\d+\.\d+\.\d+:\d+: connection ended", 10), proxy.lines[-20:]
    assert not proxy.events("has connected|has disconnected")


def test_piped_output_ends_each_line_once(binary, tmp_path):
    config = (REPOSITORY / "resources" / "endgate.default.toml").read_text()
    config = re.sub(r"(?m)^bind = .*$", f'bind = "[::]:{free_port()}"', config)
    config = re.sub(r"(?m)^player-info-forwarding-mode = .*$", 'player-info-forwarding-mode = "none"', config)
    (tmp_path / "endgate.toml").write_text(config)
    result = subprocess.run(
        [str(binary)], cwd=tmp_path, input=b"shutdown\r\n", capture_output=True, timeout=30, check=False
    )
    eol = b"\r\n" if os.name == "nt" else b"\n"
    assert result.stdout.endswith(eol)
    assert b"\r\r" not in result.stdout
    assert all(line.endswith(eol) for line in result.stdout.splitlines(keepends=True))
    log = (tmp_path / "logs" / "latest.log").read_bytes()
    assert b"\r\r" not in log
    assert log.endswith(eol)
