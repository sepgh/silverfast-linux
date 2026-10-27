#include <winsock2.h>
#include <afunix.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "transport.h"
#include "log.h"
#include "../../helper/src/protocol.h"

static SOCKET sock = INVALID_SOCKET;
static CRITICAL_SECTION lock;
static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_lock(INIT_ONCE *once, void *param, void **ctx)
{
    WSADATA wsa;
    InitializeCriticalSection(&lock);
    WSAStartup(MAKEWORD(2, 2), &wsa);
    return TRUE;
}

/* Socket path: $SFL_USBD_SOCKET, else $XDG_RUNTIME_DIR/silverfast-linux/usbd.sock.
 * Wine passes the Unix environment through, and Wine's AF_UNIX accepts Unix paths. */
static BOOL socket_path(char *out, size_t size)
{
    DWORD n = GetEnvironmentVariableA("SFL_USBD_SOCKET", out, (DWORD)size);
    char rt[MAX_PATH];

    if (n && n < size) return TRUE;
    n = GetEnvironmentVariableA("XDG_RUNTIME_DIR", rt, sizeof(rt));
    if (!n || n >= sizeof(rt)) return FALSE;
    return _snprintf(out, size, "%s/silverfast-linux/usbd.sock", rt) < (int)size;
}

static BOOL send_all(const void *buf, DWORD len)
{
    const char *p = buf;
    while (len)
    {
        int n = send(sock, p, len > 0x7fffffff ? 0x7fffffff : (int)len, 0);
        if (n <= 0) return FALSE;
        p += n; len -= n;
    }
    return TRUE;
}

static BOOL recv_all(void *buf, DWORD len)
{
    char *p = buf;
    while (len)
    {
        int n = recv(sock, p, len > 0x7fffffff ? 0x7fffffff : (int)len, 0);
        if (n <= 0) return FALSE;
        p += n; len -= n;
    }
    return TRUE;
}

static void drop_connection(void)
{
    if (sock != INVALID_SOCKET) closesocket(sock);
    sock = INVALID_SOCKET;
}

static HRESULT errno_to_hresult(INT32 status)
{
    switch (-status)
    {
    case 19 /* ENODEV */:    return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
    case 110 /* ETIMEDOUT */: return HRESULT_FROM_WIN32(ERROR_SEM_TIMEOUT);
    case 13 /* EACCES */:    return E_ACCESSDENIED;
    default:                 return E_FAIL;
    }
}

/* One request/response exchange; caller holds the lock. */
static HRESULT exchange(char op, const void *payload, DWORD len, void *out, DWORD *out_len)
{
    unsigned char hdr[SFL_HEADER_SIZE], rep[SFL_REPLY_SIZE];
    INT32 status;
    UINT32 n;

    hdr[0] = op;
    memcpy(hdr + 1, &len, 4); /* x86 is little-endian, matching the wire format */
    if (!send_all(hdr, sizeof(hdr)) || (payload && len && !send_all(payload, len)) || !recv_all(rep, sizeof(rep)))
    {
        sfl_log("transport: connection to silverfast-usbd lost");
        drop_connection();
        return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
    }
    memcpy(&status, rep, 4);
    memcpy(&n, rep + 4, 4);
    if (n)
    {
        DWORD cap = out_len ? *out_len : 0;
        if (n > cap || !recv_all(out, n)) { drop_connection(); return E_FAIL; }
    }
    if (out_len) *out_len = n;
    return status ? errno_to_hresult(status) : S_OK;
}

/* AF_UNIX sockets only exist in Wine Staging; fall through to TCP elsewhere. */
static BOOL connect_unix(void)
{
    SOCKADDR_UN addr = { .sun_family = AF_UNIX };

    if (!socket_path(addr.sun_path, sizeof(addr.sun_path))) return FALSE;
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock != INVALID_SOCKET && !connect(sock, (struct sockaddr *)&addr, sizeof(addr))) return TRUE;
    sfl_log("transport: unix socket %s unavailable (error %d)", addr.sun_path, WSAGetLastError());
    drop_connection();
    return FALSE;
}

/* $SFL_USBD_TCP holds the loopback port the helper listens on. */
static BOOL connect_tcp(void)
{
    struct sockaddr_in addr = { .sin_family = AF_INET };
    char port[16];
    DWORD n = GetEnvironmentVariableA("SFL_USBD_TCP", port, sizeof(port));

    if (!n || n >= sizeof(port) || !(addr.sin_port = htons((u_short)atoi(port)))) return FALSE;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock != INVALID_SOCKET && !connect(sock, (struct sockaddr *)&addr, sizeof(addr))) return TRUE;
    sfl_log("transport: 127.0.0.1:%s unavailable (error %d)", port, WSAGetLastError());
    drop_connection();
    return FALSE;
}

static HRESULT connect_locked(void)
{
    char desc[128], hello[4 + 128];
    DWORD desc_len = sizeof(desc) - 1, token_len;
    HRESULT hr;

    if (sock != INVALID_SOCKET) return S_OK;
    if (!connect_unix() && !connect_tcp())
    {
        sfl_log("transport: cannot reach silverfast-usbd; is it running?");
        return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
    }
    memcpy(hello, SFL_PROTOCOL_MAGIC, 4);
    token_len = GetEnvironmentVariableA("SFL_USBD_TOKEN", hello + 4, sizeof(hello) - 4);
    if (token_len >= sizeof(hello) - 4) token_len = 0;
    hr = exchange(SFL_OP_HELLO, hello, 4 + token_len, desc, &desc_len);
    if (FAILED(hr)) { sfl_log("transport: helper reports no usable scanner (%08lx)", hr); drop_connection(); return hr; }
    desc[desc_len] = 0;
    sfl_log("transport: connected to %s", desc);
    return S_OK;
}

HRESULT transport_connect(void)
{
    HRESULT hr;
    InitOnceExecuteOnce(&init_once, init_lock, NULL, NULL);
    EnterCriticalSection(&lock);
    hr = connect_locked();
    LeaveCriticalSection(&lock);
    return hr;
}

HRESULT transport_write(const void *buf, DWORD len)
{
    HRESULT hr;
    InitOnceExecuteOnce(&init_once, init_lock, NULL, NULL);
    EnterCriticalSection(&lock);
    if (SUCCEEDED(hr = connect_locked())) hr = exchange(SFL_OP_WRITE, buf, len, NULL, NULL);
    LeaveCriticalSection(&lock);
    return hr;
}

HRESULT transport_read(void *buf, DWORD *len)
{
    HRESULT hr;
    InitOnceExecuteOnce(&init_once, init_lock, NULL, NULL);
    EnterCriticalSection(&lock);
    if (SUCCEEDED(hr = connect_locked())) hr = exchange(SFL_OP_READ, NULL, *len, buf, len);
    else *len = 0;
    LeaveCriticalSection(&lock);
    return hr;
}

void transport_disconnect(void)
{
    InitOnceExecuteOnce(&init_once, init_lock, NULL, NULL);
    EnterCriticalSection(&lock);
    drop_connection();
    LeaveCriticalSection(&lock);
}
