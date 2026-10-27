#include <winsock2.h>
#include <afunix.h>
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

static HRESULT connect_locked(void)
{
    SOCKADDR_UN addr = { .sun_family = AF_UNIX };
    char desc[128];
    DWORD desc_len = sizeof(desc) - 1;
    HRESULT hr;

    if (sock != INVALID_SOCKET) return S_OK;
    if (!socket_path(addr.sun_path, sizeof(addr.sun_path)))
    {
        sfl_log("transport: no socket path (set SFL_USBD_SOCKET or XDG_RUNTIME_DIR)");
        return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
    }
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET || connect(sock, (struct sockaddr *)&addr, sizeof(addr)))
    {
        sfl_log("transport: cannot reach silverfast-usbd at %s (error %d)", addr.sun_path, WSAGetLastError());
        drop_connection();
        return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
    }
    hr = exchange(SFL_OP_HELLO, SFL_PROTOCOL_MAGIC, 4, desc, &desc_len);
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
