#!/usr/bin/env python3
"""Stand-in for silverfast-usbd used by the sti.dll self-test.

Speaks the protocol from helper/src/protocol.h. Bytes written by the client
are queued and handed back by the next reads, so the test can check that data
survives the round trip. Serves a single client, then exits.

usage: mock_usbd.py unix PATH       listen on a Unix socket
       mock_usbd.py tcp PORT_FILE   listen on 127.0.0.1, write the port to PORT_FILE
The HELLO token must match $SFL_USBD_TOKEN.
"""
import os
import socket
import struct
import sys


def recv_all(conn, n):
    data = b""
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            raise EOFError
        data += chunk
    return data


def main():
    mode, path = sys.argv[1], sys.argv[2]
    token = os.environ.get("SFL_USBD_TOKEN", "").encode()
    if mode == "unix":
        if os.path.exists(path):
            os.unlink(path)
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(path)
    else:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.bind(("127.0.0.1", 0))
        with open(path + ".tmp", "w") as f:
            f.write(str(srv.getsockname()[1]))
        os.rename(path + ".tmp", path)
    srv.listen(1)
    srv.settimeout(60)
    print("mock_usbd: listening", flush=True)
    conn, _ = srv.accept()
    queued = b""
    try:
        while True:
            op, length = struct.unpack("<cI", recv_all(conn, 5))
            status, out = 0, b""
            if op == b"H":
                status = 0 if recv_all(conn, length) == b"SFL1" + token else -13
                out = b"Mock Scanner" if status == 0 else b""
            elif op == b"W":
                queued += recv_all(conn, length)
            elif op == b"R":
                out, queued = queued[:length], queued[length:]
            else:
                status = -71
            conn.sendall(struct.pack("<iI", status, len(out)) + out)
    except EOFError:
        pass
    finally:
        conn.close()
        srv.close()
        if os.path.exists(path):
            os.unlink(path)


if __name__ == "__main__":
    main()
