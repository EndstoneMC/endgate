import sys
import threading
import time

import raknet

if len(sys.argv) != 3 or sys.argv[1] not in ("backend", "client"):
    sys.exit("usage: echo.py backend PORT | echo.py client HOST:PORT")

if sys.argv[1] == "backend":
    server = raknet.create_server(("127.0.0.1", int(sys.argv[2])))
    print(f"backend listening on 127.0.0.1:{server.local_address[1]}", flush=True)
    while True:
        connection = server.accept()
        print(f"backend accepted {connection.remote_address}", flush=True)

        def echo(connection: raknet.Connection = connection) -> None:
            while True:
                try:
                    message = connection.recv()
                except raknet.ConnectionClosed:
                    print(f"backend lost {connection.remote_address}", flush=True)
                    return
                connection.send(message)

        threading.Thread(target=echo, daemon=True).start()

host, _, port = sys.argv[2].rpartition(":")
address = (host, int(port))
pong = raknet.ping(address, timeout=2)
print(f"pong: {pong.data[2:].decode(errors='replace')}")
connection = raknet.create_connection(address, timeout=5)
time.sleep(0.5)
for size in (1, 1400, 65535, 300_000):
    payload = b"\xfe" + bytes(i & 0xFF for i in range(size))
    start = time.monotonic()
    connection.send(payload)
    reply = connection.recv(timeout=10)
    elapsed = (time.monotonic() - start) * 1000
    print(f"{size} bytes: {'echoed' if reply == payload else 'MISMATCH'} in {elapsed:.1f} ms")
connection.close()
