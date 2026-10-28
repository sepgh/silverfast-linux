/* silverfast-usbd: gives SilverFast (running in Wine) raw USB access to the
 * scanner through a Unix socket or token-protected loopback TCP. sti.dll inside the Wine prefix is the only
 * client; see helper/src/protocol.h for the wire format and
 * docs/how-it-works.md for the overall design.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <libusb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "firmware.h"
#include "protocol.h"
#include "rxbuf.h"

#define IO_TIMEOUT_MS   30000
#define FW_TIMEOUT_MS   10000

struct scanner {
    libusb_context          *usb;
    libusb_device_handle    *handle;
    const struct sfl_model  *model;
    struct sfl_rxbuf         rx;
    unsigned int             timeout_ms;
};

static const char *firmware_dir;
static const char *socket_path;
static const char *port_file;
static const char *token;     /* from $SFL_USBD_TOKEN; required in HELLO when set */
static int verbose;
static FILE *log_file;
static volatile sig_atomic_t stop;

/* The scanner to drive, from --device/--firmware (the launcher takes them
 * from the scanner catalog in models/). Endpoints come from its USB
 * descriptors. */
struct sfl_model {
    uint16_t    vid, pid;
    const char *name;
    const char *firmware;  /* file inside firmware_dir, uploaded with Epson's protocol */
    uint8_t     ep_out, ep_in;
    uint16_t    packet_size;
};

static struct sfl_model custom_model;
static char custom_name[64];

static void logmsg(const char *fmt, ...)
{
    struct timespec ts;
    va_list ap;

    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(log_file, "%ld.%03ld ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    va_start(ap, fmt);
    vfprintf(log_file, fmt, ap);
    va_end(ap);
    fputc('\n', log_file);
    fflush(log_file);
}

static void loghex(const char *what, const uint8_t *buf, size_t len)
{
    char text[3 * 24 + 4];
    size_t i, n = len < 24 ? len : 24;

    if (!verbose) return;
    for (i = 0; i < n; i++) sprintf(text + 3 * i, "%02x ", buf[i]);
    if (len > n) strcpy(text + 3 * n, "...");
    logmsg("%s %zu: %s", what, len, n ? text : "");
}

static int usb_errno(int err)
{
    switch (err)
    {
    case LIBUSB_ERROR_NO_DEVICE: return -ENODEV;
    case LIBUSB_ERROR_TIMEOUT:   return -ETIMEDOUT;
    case LIBUSB_ERROR_OVERFLOW:  return -EOVERFLOW;
    case LIBUSB_ERROR_PIPE:      return -EPIPE;
    case LIBUSB_ERROR_ACCESS:    return -EACCES;
    case LIBUSB_ERROR_BUSY:      return -EBUSY;
    default:                     return -EIO;
    }
}

static int bulk_write(void *ctx, const uint8_t *buf, size_t len)
{
    struct scanner *s = ctx;
    int done = 0, ret;

    ret = libusb_bulk_transfer(s->handle, s->model->ep_out, (uint8_t *)buf, (int)len, &done, s->timeout_ms);
    if (ret) return usb_errno(ret);
    return (size_t)done == len ? 0 : -EIO;
}

static int bulk_read_raw(void *ctx, uint8_t *buf, size_t cap)
{
    struct scanner *s = ctx;
    int done = 0, ret;

    ret = libusb_bulk_transfer(s->handle, s->model->ep_in, buf, (int)cap, &done, s->timeout_ms);
    if (ret && !(ret == LIBUSB_ERROR_TIMEOUT && done)) return usb_errno(ret);
    return done;
}

/* Exact-length read used for firmware handshakes; goes through the packet buffer. */
static int bulk_read_exact(void *ctx, uint8_t *buf, size_t len)
{
    struct scanner *s = ctx;
    long got = sfl_rxbuf_read(&s->rx, buf, len, bulk_read_raw, s);

    if (got < 0) return (int)got;
    return (size_t)got == len ? 0 : -EIO;
}

static void scanner_close(struct scanner *s)
{
    if (!s->handle) return;
    libusb_release_interface(s->handle, 0);
    libusb_close(s->handle);
    s->handle = NULL;
    sfl_rxbuf_clear(&s->rx);
    logmsg("scanner closed");
}

static int load_firmware(struct scanner *s)
{
    struct sfl_fw_io io = { s, bulk_write, bulk_read_exact };
    char path[4096];
    uint8_t *file = NULL, *payload = NULL;
    size_t file_len = 0, payload_len = 0;
    FILE *f;
    long size;
    int ret;

    if (!s->model->firmware) return 0;
    s->timeout_ms = FW_TIMEOUT_MS;
    ret = sfl_fw_is_loaded(&io);
    if (ret < 0) { logmsg("firmware status query failed: %s", strerror(-ret)); goto out; }
    if (ret == 1) { logmsg("firmware already running"); ret = 0; goto out; }

    snprintf(path, sizeof(path), "%s/%s", firmware_dir, s->model->firmware);
    if (!(f = fopen(path, "rb"))) { ret = -errno; logmsg("cannot open firmware %s: %s", path, strerror(errno)); goto out; }
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET)) { fclose(f); ret = -EIO; goto out; }
    file_len = (size_t)size;
    if (!(file = malloc(file_len)) || fread(file, 1, file_len, f) != file_len) { fclose(f); ret = -EIO; goto out; }
    fclose(f);

    if (!(payload = sfl_fw_prepare(file, file_len, &payload_len))) { ret = -EINVAL; goto out; }
    logmsg("uploading firmware %s (%zu bytes)", path, payload_len);
    ret = sfl_fw_upload(&io, payload, payload_len);
    logmsg(ret ? "firmware upload failed: %s" : "firmware upload done%s", ret ? strerror(-ret) : "");
out:
    free(file);
    free(payload);
    s->timeout_ms = IO_TIMEOUT_MS;
    return ret;
}

/* Fills in the first bulk OUT/IN endpoints of interface 0. */
static int detect_endpoints(libusb_device_handle *h, struct sfl_model *m)
{
    struct libusb_config_descriptor *cfg;
    const struct libusb_interface_descriptor *itf;
    int i, ret;

    if ((ret = libusb_get_active_config_descriptor(libusb_get_device(h), &cfg))) return usb_errno(ret);
    if (cfg->bNumInterfaces < 1 || cfg->interface[0].num_altsetting < 1) { libusb_free_config_descriptor(cfg); return -ENODEV; }
    itf = &cfg->interface[0].altsetting[0];
    m->ep_in = m->ep_out = 0;
    for (i = 0; i < itf->bNumEndpoints; i++)
    {
        const struct libusb_endpoint_descriptor *ep = &itf->endpoint[i];
        if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK) continue;
        if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) && !m->ep_in) { m->ep_in = ep->bEndpointAddress; m->packet_size = ep->wMaxPacketSize; }
        else if (!(ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) && !m->ep_out) m->ep_out = ep->bEndpointAddress;
    }
    libusb_free_config_descriptor(cfg);
    if (!m->ep_in || !m->ep_out) return -ENODEV;
    logmsg("endpoints: out 0x%02x, in 0x%02x, packet %u", m->ep_out, m->ep_in, m->packet_size);
    return 0;
}

static int scanner_open(struct scanner *s)
{
    int ret;

    if (s->handle) return 0;
    s->model = &custom_model;
    if (!(s->handle = libusb_open_device_with_vid_pid(s->usb, custom_model.vid, custom_model.pid))) return -ENODEV;
    if ((ret = detect_endpoints(s->handle, &custom_model)))
    {
        logmsg("no bulk endpoints on interface 0");
        libusb_close(s->handle);
        s->handle = NULL;
        return ret;
    }

    libusb_set_auto_detach_kernel_driver(s->handle, 1);
    if ((ret = libusb_claim_interface(s->handle, 0)))
    {
        logmsg("cannot claim scanner interface: %s", libusb_error_name(ret));
        libusb_close(s->handle);
        s->handle = NULL;
        return usb_errno(ret);
    }
    sfl_rxbuf_init(&s->rx, s->model->packet_size);
    s->timeout_ms = IO_TIMEOUT_MS;
    logmsg("opened %s", s->model->name);

    if ((ret = load_firmware(s))) { scanner_close(s); return ret; }
    return 0;
}

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len)
    {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return -1; }
        p += n; len -= n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    while (len)
    {
        ssize_t n = recv(fd, p, len, 0);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return -1; }
        p += n; len -= n;
    }
    return 0;
}

static uint32_t get_le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static int reply(int fd, int status, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[SFL_REPLY_SIZE];

    put_le32(hdr, (uint32_t)status);
    put_le32(hdr + 4, len);
    if (send_all(fd, hdr, sizeof(hdr))) return -1;
    return len ? send_all(fd, data, len) : 0;
}

/* After a disconnect the scanner usually re-enumerates without firmware;
 * reopening re-runs the firmware check so the next session works. */
static void handle_io_error(struct scanner *s, int err)
{
    if (err != -ENODEV) return;
    scanner_close(s);
    if (scanner_open(s) == 0) logmsg("scanner reopened after disconnect");
}

/* HELLO payload: "SFL1" followed by the session token, if one is configured. */
static int hello_ok(const uint8_t *buf, uint32_t len)
{
    size_t token_len = token ? strlen(token) : 0;

    if (len != 4 + token_len || memcmp(buf, SFL_PROTOCOL_MAGIC, 4)) return 0;
    return !token_len || !memcmp(buf + 4, token, token_len);
}

static void serve_client(struct scanner *s, int fd)
{
    uint8_t hdr[SFL_HEADER_SIZE], *buf = NULL;
    size_t bufcap = 0;

    int greeted = 0;

    while (!stop && !recv_all(fd, hdr, sizeof(hdr)))
    {
        uint32_t len = get_le32(hdr + 1);
        int status = 0;
        uint32_t out_len = 0;

        if (len > SFL_MAX_TRANSFER) { logmsg("request too large (%u bytes)", len); break; }
        if (len > bufcap || bufcap < 256) /* 256: room for the HELLO reply */
        {
            size_t want = len > 256 ? len : 256;
            uint8_t *p = realloc(buf, want);
            if (!p) break;
            buf = p; bufcap = want;
        }
        if (hdr[0] != SFL_OP_READ && len && recv_all(fd, buf, len)) break;
        if (!greeted && hdr[0] != SFL_OP_HELLO) { logmsg("client did not say hello"); break; }

        switch (hdr[0])
        {
        case SFL_OP_HELLO:
            if (!hello_ok(buf, len)) { status = -EACCES; logmsg("client rejected: bad hello"); break; }
            if ((status = scanner_open(s))) break;
            greeted = 1;
            out_len = (uint32_t)strlen(s->model->name);
            memcpy(buf, s->model->name, out_len);
            break;
        case SFL_OP_WRITE:
            if ((status = scanner_open(s))) break;
            loghex("W", buf, len);
            if ((status = bulk_write(s, buf, len))) handle_io_error(s, status);
            break;
        case SFL_OP_READ:
        {
            long got;
            if ((status = scanner_open(s))) break;
            got = sfl_rxbuf_read(&s->rx, buf, len, bulk_read_raw, s);
            if (got < 0) { status = (int)got; handle_io_error(s, status); break; }
            out_len = (uint32_t)got;
            loghex("R", buf, out_len);
            break;
        }
        default:
            status = -EPROTO;
        }
        if (status) logmsg("op '%c' failed: %s", hdr[0], strerror(-status));
        if (reply(fd, status, buf, out_len)) break;
    }
    free(buf);
}

static int make_socket(const char *path)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    int fd;

    if (strlen(path) >= sizeof(addr.sun_path)) { fprintf(stderr, "socket path too long: %s\n", path); return -1; }
    strcpy(addr.sun_path, path);

    /* Refuse to start twice; a stale socket file from a crash is replaced. */
    if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0) return -1;
    if (!connect(fd, (struct sockaddr *)&addr, sizeof(addr)))
    {
        fprintf(stderr, "silverfast-usbd is already running on %s\n", path);
        close(fd);
        return -1;
    }
    close(fd);
    unlink(path);

    if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0) return -1;
    umask(077);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, 1))
    {
        fprintf(stderr, "cannot listen on %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* Loopback TCP listener for Wine builds without AF_UNIX support (mainline
 * Wine; AF_UNIX is a Wine Staging feature). The kernel picks the port, which
 * is written to port_file for the launcher. Clients must know the token. */
static int make_tcp_socket(void)
{
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t len = sizeof(addr);
    FILE *f;
    int fd;

    if ((fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0) return -1;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, 1) ||
        getsockname(fd, (struct sockaddr *)&addr, &len))
    {
        fprintf(stderr, "cannot listen on loopback TCP: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    if (!(f = fopen(port_file, "w"))) { perror(port_file); close(fd); return -1; }
    fprintf(f, "%u\n", ntohs(addr.sin_port));
    fclose(f);
    logmsg("listening on 127.0.0.1:%u", ntohs(addr.sin_port));
    return fd;
}

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s --device VID:PID [--firmware FILE --firmware-dir DIR] [--socket PATH]\n"
            "       [--tcp-port-file FILE] [--log FILE] [--verbose]\n"
            "  --socket PATH       Unix socket to serve (default $XDG_RUNTIME_DIR/silverfast-linux/usbd.sock)\n"
            "  --tcp-port-file FILE also listen on 127.0.0.1 (random port, written to FILE);\n"
            "                      requires $SFL_USBD_TOKEN so other local users cannot connect\n"
            "  --device VID:PID    the scanner to drive\n"
            "  --firmware FILE     firmware to upload when the scanner has none running\n"
            "  --firmware-dir DIR  directory holding FILE\n"
            "  --name NAME         scanner name for logs and the client greeting\n"
            "  --log FILE          append log lines to FILE instead of stderr\n"
            "  --verbose           log every USB transfer\n", argv0);
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "firmware-dir", required_argument, NULL, 'f' },
        { "socket",       required_argument, NULL, 's' },
        { "log",          required_argument, NULL, 'l' },
        { "tcp-port-file", required_argument, NULL, 'p' },
        { "verbose",      no_argument,       NULL, 'v' },
        { "device",       required_argument, NULL, 'd' },
        { "firmware",     required_argument, NULL, 'F' },
        { "name",         required_argument, NULL, 'n' },
        { "help",         no_argument,       NULL, 'h' },
        { 0 }
    };
    struct sigaction sa = { .sa_handler = on_signal };
    struct scanner s = { 0 };
    char default_socket[4096];
    struct pollfd fds[2];
    int c, nfds = 1, listen_fd, ret;

    log_file = stderr;
    while ((c = getopt_long(argc, argv, "f:s:l:p:vd:F:n:h", opts, NULL)) != -1)
    {
        switch (c)
        {
        case 'f': firmware_dir = optarg; break;
        case 's': socket_path = optarg; break;
        case 'l':
            if (!(log_file = fopen(optarg, "a"))) { perror(optarg); return 1; }
            break;
        case 'p': port_file = optarg; break;
        case 'v': verbose = 1; break;
        case 'd':
        {
            unsigned int vid, pid;
            if (sscanf(optarg, "%4x:%4x", &vid, &pid) != 2) { fprintf(stderr, "--device expects VID:PID\n"); return 1; }
            custom_model.vid = vid;
            custom_model.pid = pid;
            if (!custom_name[0]) snprintf(custom_name, sizeof(custom_name), "USB device %04x:%04x", vid, pid);
            custom_model.name = custom_name;
            break;
        }
        case 'F': custom_model.firmware = optarg; break;
        case 'n': snprintf(custom_name, sizeof(custom_name), "%s", optarg); break;
        default: usage(argv[0]); return c == 'h' ? 0 : 1;
        }
    }
    if (!custom_model.vid) { usage(argv[0]); return 1; }
    if (custom_model.firmware && !firmware_dir) { fprintf(stderr, "--firmware needs --firmware-dir\n"); return 1; }
    token = getenv("SFL_USBD_TOKEN");
    if (token && !*token) token = NULL;
    if (port_file && !token) { fprintf(stderr, "--tcp-port-file requires SFL_USBD_TOKEN\n"); return 1; }
    if (!socket_path)
    {
        const char *rt = getenv("XDG_RUNTIME_DIR");
        if (!rt) { fprintf(stderr, "XDG_RUNTIME_DIR is not set; pass --socket\n"); return 1; }
        snprintf(default_socket, sizeof(default_socket), "%s/silverfast-linux", rt);
        mkdir(default_socket, 0700);
        strncat(default_socket, "/usbd.sock", sizeof(default_socket) - strlen(default_socket) - 1);
        socket_path = default_socket;
    }

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if ((ret = libusb_init(&s.usb))) { fprintf(stderr, "libusb_init failed: %s\n", libusb_error_name(ret)); return 1; }
    if ((listen_fd = make_socket(socket_path)) < 0) { libusb_exit(s.usb); return 1; }
    logmsg("listening on %s", socket_path);
    fds[0] = (struct pollfd){ .fd = listen_fd, .events = POLLIN };
    if (port_file)
    {
        int tcp_fd = make_tcp_socket();
        if (tcp_fd < 0) { close(listen_fd); unlink(socket_path); libusb_exit(s.usb); return 1; }
        fds[nfds++] = (struct pollfd){ .fd = tcp_fd, .events = POLLIN };
    }

    /* Open early so the firmware is loaded before SilverFast asks; a missing
     * scanner is not fatal, the next client request retries. */
    if ((ret = scanner_open(&s))) logmsg("scanner not ready yet: %s", strerror(-ret));

    while (!stop)
    {
        int i;

        if (poll(fds, nfds, -1) < 0) { if (errno == EINTR) continue; logmsg("poll failed: %s", strerror(errno)); break; }
        for (i = 0; i < nfds && !stop; i++)
        {
            int fd;
            if (!(fds[i].revents & POLLIN)) continue;
            if ((fd = accept4(fds[i].fd, NULL, NULL, SOCK_CLOEXEC)) < 0) continue;
            logmsg("client connected (%s)", i ? "tcp" : "unix");
            serve_client(&s, fd);
            close(fd);
            logmsg("client disconnected");
        }
    }

    for (c = 0; c < nfds; c++) close(fds[c].fd);
    if (port_file) unlink(port_file);
    unlink(socket_path);
    scanner_close(&s);
    sfl_rxbuf_free(&s.rx);
    libusb_exit(s.usb);
    logmsg("stopped");
    return 0;
}
