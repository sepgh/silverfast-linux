#!/usr/bin/env python3
"""Stand-in for silverfast-usbd used by the sti.dll self-test.

Speaks the protocol from helper/src/protocol.h. Bytes written by the client
are queued and handed back by the next reads, so the test can check that data
survives the round trip. Serves a single client, then exits.
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
    path = sys.argv[1]
    if os.path.exists(path):
        os.unlink(path)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
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
                status = 0 if recv_all(conn, length) == b"SFL1" else -71
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
        os.unlink(path)


if __name__ == "__main__":
    main()
