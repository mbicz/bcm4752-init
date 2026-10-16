/*
 * bcm4752-init — power up and initialise a Broadcom BCM4752 GPS receiver.
 *
 * This program implements the initialisation sequence described in SPEC.md. It is an
 * independent reimplementation of the observed wire protocol: it contains no vendor
 * source code and no vendor firmware bytes. The receiver RAM patch is read at run
 * time from a container produced by tools/extract_patch.py from the user's own copy
 * of the vendor daemon.
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define VERSION "0.1.0"

/* Frame bytes, see SPEC.md sections 2 and 3. */
#define WAKE_MAX          512u     /* largest autobaud burst */
#define SEC_SOF_A        0xfeu
#define SEC_SOF_B        0xffu
#define SEC_CHANNEL      0x00u
#define SEC_MARK         0xfdu
#define SEC_END          0xfcu
#define SEC_OP_READ_REG  0x1fu
#define SEC_OP_READ      0x00u
#define SEC_OP_WRITE     0x61u
#define SEC_OP_WRITE_ALT 0x21u
#define SEC_OP_ACK       0x2fu
#define SEC_GRP_DATA     0x17u
#define SEC_GRP_CRC      0x18u
#define SEC_DATA_MARK_A  0xdeu
#define SEC_DATA_MARK_B  0xc0u

/* Transfer granularity, see SPEC.md section 5. */
#define SEC_CHUNK_MAX   1020u
#define SEC_WRITE_PIECE 160u
#define SEC_PIECE_MAX   4096u

/* GLME, see SPEC.md section 9. */
#define GLME_ESC 0xe0u
#define GLME_EOF 0x01u

#define PATCH_MAGIC "BCM4752PATCH"

/*
 * 921600 baud. glibc defines B921600; other termios implementations only
 * define a table of speeds, so fall back to the raw value.
 */
#ifndef B921600
#define B921600 921600
#endif

static const uint8_t AUTOBAUD_FRAME[] = {0xfe, 0x00, 0xfd, 0x21, 0x00, 0x01, 0x33, 0x0e, 0x10, 0xfc};

static const char *prog = "bcm4752-init";
static int verbose;

static void logv(const char *fmt, ...)
{
    va_list ap;
    if (!verbose)
        return;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", prog);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", prog);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", prog);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/* ---------------------------------------------------------------- output */

static int tty = -1;               /* serial port, or -1 in --dump-plan mode */
static int plan_only;
static uint8_t *plan;
static size_t plan_len;
static size_t plan_cap;

static void plan_add(const void *buf, size_t n)
{
    if (plan_len + n > plan_cap) {
        size_t cap = plan_cap ? plan_cap * 2 : 65536;
        while (cap < plan_len + n)
            cap *= 2;
        uint8_t *p = realloc(plan, cap);
        if (!p)
            die("out of memory");
        plan = p;
        plan_cap = cap;
    }
    memcpy(plan + plan_len, buf, n);
    plan_len += n;
}

static void print_hex(const char *prefix, const uint8_t *buf, size_t n)
{
    printf("%s", prefix);
    for (size_t i = 0; i < n; i++)
        printf("%02x", buf[i]);
    putchar('\n');
}

/* Send bytes to the receiver and record them in the plan. */
static void tx(const void *buf, size_t n, const char *what)
{
    plan_add(buf, n);
    if (plan_only) {
        print_hex("tx ", buf, n);
        logv("tx %-24s %zu bytes", what, n);
        return;
    }
    const uint8_t *p = buf;
    size_t left = n;
    while (left > 0) {
        ssize_t w = write(tty, p, left);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            die("write: %s", strerror(errno));
        }
        p += w;
        left -= (size_t)w;
    }
    logv("tx %-24s %zu bytes", what, n);
}

/* ---------------------------------------------------------------- input */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * Read one receiver->host frame: accumulate until the frame terminator 0xfc, or
 * until a GLME end marker (e0 01). Returns the frame length, 0 on timeout and
 * -1 on error.
 */
static ssize_t rx_frame(uint8_t *buf, size_t cap, int timeout_ms)
{
    size_t n = 0;
    int64_t deadline = now_ms() + timeout_ms;
    while (1) {
        int64_t left = deadline - now_ms();
        if (left <= 0)
            return 0;
        struct pollfd pfd = {.fd = tty, .events = POLLIN};
        int pr = poll(&pfd, 1, (int)left);
        if (pr == 0)
            return 0;
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            die("poll: %s", strerror(errno));
        }
        ssize_t r = read(tty, buf + n, cap - n);
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            die("read: %s", strerror(errno));
        }
        if (r == 0)
            return 0;
        n += (size_t)r;
        if (n >= 1 && buf[n - 1] == SEC_END)
            return (ssize_t)n;
        if (n >= 2 && buf[n - 2] == GLME_ESC && buf[n - 1] == GLME_EOF)
            return (ssize_t)n;
        if (n == cap)
            die("response frame longer than %zu bytes", cap);
    }
}

/*
 * Wait for the expected reply. In --dump-plan mode the expectation is only
 * recorded. Returns 0 on success/timeout-not-required, -1 on mismatch.
 */
static int expect(const uint8_t *want, size_t wantl, const char *what, int timeout_ms)
{
    if (plan_only) {
        print_hex("rx ", want, wantl);
        return 0;
    }
    uint8_t buf[4096];
    ssize_t n = rx_frame(buf, sizeof(buf), timeout_ms);
    if (n == 0) {
        warn("timeout waiting for %s", what);
        return -1;
    }
    if ((size_t)n != wantl || memcmp(buf, want, wantl) != 0) {
        char got[2 * sizeof(buf) + 1];
        char exp[2 * sizeof(buf) + 1];
        for (ssize_t i = 0; i < n; i++)
            sprintf(got + 2 * i, "%02x", buf[i]);
        for (size_t i = 0; i < wantl; i++)
            sprintf(exp + 2 * i, "%02x", want[i]);
        warn("%s: got %s, expected %s", what, got, exp);
        return -1;
    }
    logv("rx %-24s %s", what, "ok");
    return 0;
}

/* Wait for a patch acknowledgement: fe 00 fd 2f 15 <4 bytes> fc. */
static int expect_ack(const char *what, int timeout_ms)
{
    uint8_t buf[4096];
    if (plan_only) {
        /* The 4-byte value is produced by the receiver; only the frame shape is
         * deterministic, so it is recorded separately from exact expectations. */
        static const uint8_t shape[] = {0xfe, 0x00, 0xfd, 0x2f, 0x15, 0x00, 0x00, 0x00, 0x00, 0xfc};
        print_hex("ack ", shape, sizeof(shape));
        return 0;
    }
    ssize_t n = rx_frame(buf, sizeof(buf), timeout_ms);
    if (n == 0) {
        warn("timeout waiting for %s", what);
        return -1;
    }
    if (n != 10 || buf[0] != SEC_SOF_A || buf[1] != SEC_CHANNEL || buf[2] != SEC_MARK ||
        buf[3] != SEC_OP_ACK || buf[4] != 0x15 || buf[9] != SEC_END) {
        warn("%s: unexpected acknowledgement shape", what);
        return -1;
    }
    logv("rx %-24s ack crc=%02x%02x%02x%02x", what, buf[5], buf[6], buf[7], buf[8]);
    return 0;
}

/* ---------------------------------------------------------- patch upload */

struct piece {
    uint32_t addr;
    uint32_t len;
    uint8_t *data;
};

static struct piece *pieces;
static size_t npieces;

static void load_patch(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        die("%s: %s", path, strerror(errno));

    uint8_t hdr[20];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
        die("%s: truncated header", path);
    if (memcmp(hdr, PATCH_MAGIC, strlen(PATCH_MAGIC)) != 0)
        die("%s: not a %s container", path, PATCH_MAGIC);
    uint32_t count = (uint32_t)hdr[12] | ((uint32_t)hdr[13] << 8) |
                     ((uint32_t)hdr[14] << 16) | ((uint32_t)hdr[15] << 24);
    uint32_t reserved = (uint32_t)hdr[16] | ((uint32_t)hdr[17] << 8) |
                        ((uint32_t)hdr[18] << 16) | ((uint32_t)hdr[19] << 24);
    if (reserved != 0)
        die("%s: nonzero reserved field in the header", path);
    if (count == 0 || count > 64)
        die("%s: implausible piece count %u", path, count);

    pieces = calloc(count, sizeof(*pieces));
    if (!pieces)
        die("out of memory");
    for (uint32_t i = 0; i < count; i++) {
        uint8_t rec[12];
        if (fread(rec, 1, sizeof(rec), f) != sizeof(rec))
            die("%s: truncated piece header", path);
        uint32_t addr = (uint32_t)rec[0] | ((uint32_t)rec[1] << 8) |
                        ((uint32_t)rec[2] << 16) | ((uint32_t)rec[3] << 24);
        uint32_t len = (uint32_t)rec[4] | ((uint32_t)rec[5] << 8) |
                       ((uint32_t)rec[6] << 16) | ((uint32_t)rec[7] << 24);
        uint32_t reserved = (uint32_t)rec[8] | ((uint32_t)rec[9] << 8) |
                            ((uint32_t)rec[10] << 16) | ((uint32_t)rec[11] << 24);
        if (reserved != 0)
            die("%s: piece %u has nonzero reserved field", path, i);
        if (len == 0 || len > 65535)
            die("%s: piece %u has implausible length %u", path, i, len);
        uint8_t *data = malloc(len);
        if (!data)
            die("out of memory");
        if (fread(data, 1, len, f) != len)
            die("%s: truncated piece %u data", path, i);
        pieces[i].addr = addr;
        pieces[i].len = len;
        pieces[i].data = data;
        logv("patch piece %u: addr 0x%04x length %u", i, addr, len);
    }
    uint8_t extra;
    if (fread(&extra, 1, 1, f) == 1)
        warn("%s: trailing bytes ignored", path);
    fclose(f);
    npieces = count;
}

/*
 * Transfer one piece (SPEC.md section 5): chunks of at most SEC_CHUNK_MAX, the
 * aligned part written in SEC_WRITE_PIECE-sized pieces, a 4-byte frame for a
 * partial final word, then the 10-byte check frame.
 */
static void send_piece(const struct piece *pc, const char *what)
{
    uint32_t remaining = pc->len;
    uint32_t sent = 0;

    while (remaining > 0) {
        uint32_t chunk = remaining < SEC_CHUNK_MAX ? remaining : SEC_CHUNK_MAX;
        uint32_t aligned = chunk & ~3u;
        uint32_t rem = chunk & 3u;
        uint32_t addr = pc->addr + sent;

        if (aligned > 0) {
            uint8_t hdr[9];
            hdr[0] = SEC_SOF_A;
            hdr[1] = (uint8_t)((aligned + 3u) >> 2);
            hdr[2] = SEC_MARK;
            hdr[3] = SEC_OP_WRITE;
            hdr[4] = SEC_GRP_DATA;
            hdr[5] = SEC_DATA_MARK_A;
            hdr[6] = SEC_DATA_MARK_B;
            hdr[7] = (uint8_t)(addr & 0xffu);
            hdr[8] = (uint8_t)((addr >> 8) & 0xffu);
            tx(hdr, sizeof(hdr), what);

            uint32_t left = aligned;
            while (left > 0) {
                uint32_t piece = left < SEC_WRITE_PIECE ? left : SEC_WRITE_PIECE;
                tx(pc->data + sent, piece, what);
                sent += piece;
                left -= piece;
            }
            uint8_t end = SEC_END;
            tx(&end, 1, what);
        }

        if (rem > 0) {
            uint32_t words = (rem + 3u) >> 2;
            uint32_t tail = (addr + aligned + rem - 4u) & 0xffffu;
            uint8_t hdr[9];
            hdr[0] = SEC_SOF_A;
            hdr[1] = (uint8_t)words;
            hdr[2] = SEC_MARK;
            hdr[3] = SEC_OP_WRITE;
            hdr[4] = SEC_GRP_DATA;
            hdr[5] = SEC_DATA_MARK_A;
            hdr[6] = SEC_DATA_MARK_B;
            hdr[7] = (uint8_t)(tail & 0xffu);
            hdr[8] = (uint8_t)((tail >> 8) & 0xffu);
            tx(hdr, sizeof(hdr), what);
            tx(pc->data + sent + rem - 4u, 4, what);
            uint8_t end = SEC_END;
            tx(&end, 1, what);
        }

        sent += rem;
        remaining -= chunk;
    }
    uint8_t check[10];
    check[0] = SEC_SOF_A;
    check[1] = SEC_CHANNEL;
    check[2] = SEC_MARK;
    check[3] = SEC_OP_WRITE_ALT;
    check[4] = SEC_GRP_CRC;
    check[5] = (uint8_t)(pc->addr & 0xffu);
    check[6] = (uint8_t)((pc->addr >> 8) & 0xffu);
    check[7] = (uint8_t)(pc->len & 0xffu);
    check[8] = (uint8_t)((pc->len >> 8) & 0xffu);
    check[9] = SEC_END;
    tx(check, sizeof(check), what);
}

/* ------------------------------------------------------------ power on */

static char rfkill_path[512];

static int opt_delay_ms = 1000;
static int opt_timeout_ms = 1000;
static int opt_hold_ms = 0;
static int opt_burst = 16;
static int opt_skip_autobaud = 0;
static int opt_no_power = 0;

static int find_gps_rfkill(void)
{
    DIR *d = opendir("/sys/class/rfkill");
    if (!d)
        return -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "rfkill", 6) != 0)
            continue;
        char path[512], buf[64];
        snprintf(path, sizeof(path), "/sys/class/rfkill/%s/type", de->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0)
            continue;
        buf[n] = '\0';
        if (strncmp(buf, "gps", 3) != 0)
            continue;
        if (snprintf(rfkill_path, sizeof(rfkill_path), "/sys/class/rfkill/%s/state",
                     de->d_name) >= (int)sizeof(rfkill_path))
            continue;
        closedir(d);
        return 0;
    }
    closedir(d);
    return -1;
}

static void write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        die("%s: %s", path, strerror(errno));
    if (write(fd, text, strlen(text)) < 0)
        die("%s: %s", path, strerror(errno));
    close(fd);
}

static void power_on(const char *tty_path, int delay_ms)
{
    char pm[512];
    snprintf(pm, sizeof(pm), "/sys/class/tty/%s/device/power/control",
             strrchr(tty_path, '/') ? strrchr(tty_path, '/') + 1 : tty_path);
    if (find_gps_rfkill() < 0)
        die("no rfkill switch of type gps found");
    logv("power: %s = 0, %s = on, wait %d ms", rfkill_path, pm, delay_ms);
    write_text(pm, "on");
    write_text(rfkill_path, "0");
    struct timespec ts = {.tv_sec = delay_ms / 1000, .tv_nsec = (long)(delay_ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* --------------------------------------------------------------- UART */

static void configure_tty(int fd)
{
    struct termios tio;
    if (tcgetattr(fd, &tio) < 0)
        die("tcgetattr: %s", strerror(errno));
    tio.c_iflag = 0;
    tio.c_oflag = 0;                 /* binary payload, no output translation */
    tio.c_lflag = 0;
    tio.c_cflag = CS8 | CREAD | CLOCAL | CRTSCTS;
    cfsetispeed(&tio, B921600);
    cfsetospeed(&tio, B921600);
    if (tcsetattr(fd, TCSANOW, &tio) < 0)
        die("tcsetattr: %s", strerror(errno));
    tcflush(fd, TCIOFLUSH);
}

/* ------------------------------------------------------------- sequence */

static int open_tty(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        die("%s: %s", path, strerror(errno));
    configure_tty(fd);
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    return fd;
}

static int initialise(void)
{
    int rc = 0;

    uint8_t wake[WAKE_MAX];
    size_t burst = (size_t)opt_burst;
    memset(wake, 0x80, burst);
    tx(wake, burst, "autobaud");
    if (!opt_skip_autobaud) {
        tx(AUTOBAUD_FRAME, sizeof(AUTOBAUD_FRAME), "autobaud sync");
        if (expect(AUTOBAUD_FRAME, sizeof(AUTOBAUD_FRAME), "autobaud echo", opt_timeout_ms) < 0)
            rc = -1;
    }

    /* Query 0x0000. */
    static const uint8_t q00[] = {0xff, 0x00, 0xfd, 0x00, 0x00, 0xfc};
    static const uint8_t r00[] = {0xfe, 0x00, 0xfd, 0x00, 0x00, 0x05, 0x00, 0x02, 0x30, 0xfc};
    tx(q00, sizeof(q00), "query word 7");
    if (expect(r00, sizeof(r00), "query word 7 reply", opt_timeout_ms) < 0)
        rc = -1;

    /* Query item 0x95. */
    static const uint8_t q95[] = {0xff, 0x00, 0xfd, 0x1f, 0x95, 0xfc};
    static const uint8_t r95[] = {0xfe, 0x00, 0xfd, 0x1f, 0x95, 0x00, 0x00, 0x00, 0x00, 0xfc};
    tx(q95, sizeof(q95), "query 0x95");
    if (expect(r95, sizeof(r95), "query 0x95 reply", opt_timeout_ms) < 0)
        rc = -1;

    /* Write item 0x16 = 0. */
    static const uint8_t w16_0[] = {0xfe, 0x00, 0xfd, 0x61, 0x16, 0x00, 0x00, 0x00, 0x00, 0xfc};
    tx(w16_0, sizeof(w16_0), "write 0x16 = 0");

    /* Query item 0x94. */
    static const uint8_t q94[] = {0xff, 0x00, 0xfd, 0x1f, 0x94, 0xfc};
    static const uint8_t r94_0[] = {0xfe, 0x00, 0xfd, 0x1f, 0x94, 0x00, 0x00, 0x00, 0x00, 0xfc};
    tx(q94, sizeof(q94), "query 0x94");
    if (expect(r94_0, sizeof(r94_0), "query 0x94 reply", opt_timeout_ms) < 0)
        rc = -1;

    /* RAM patch pieces 1 and 2. */
    for (size_t i = 0; i < 2 && i < npieces; i++) {
        char what[64];
        snprintf(what, sizeof(what), "patch piece %zu (0x%04x/%u)", i, pieces[i].addr, pieces[i].len);
        send_piece(&pieces[i], what);
        if (expect_ack(what, opt_timeout_ms) < 0)
            rc = -1;
    }

    /* Write item 0x16 = 1, then query item 0x94. */
    static const uint8_t w16_1[] = {0xfe, 0x00, 0xfd, 0x61, 0x16, 0x01, 0x00, 0x00, 0x00, 0xfc};
    static const uint8_t r94_1[] = {0xfe, 0x00, 0xfd, 0x1f, 0x94, 0x01, 0x00, 0x00, 0x00, 0xfc};
    tx(w16_1, sizeof(w16_1), "write 0x16 = 1");
    tx(q94, sizeof(q94), "query 0x94");
    if (expect(r94_1, sizeof(r94_1), "query 0x94 reply", opt_timeout_ms) < 0)
        rc = -1;

    /* RAM patch pieces 3 and 4. */
    for (size_t i = 2; i < npieces; i++) {
        char what[64];
        snprintf(what, sizeof(what), "patch piece %zu (0x%04x/%u)", i, pieces[i].addr, pieces[i].len);
        send_piece(&pieces[i], what);
        if (expect_ack(what, opt_timeout_ms) < 0)
            rc = -1;
    }

    /* Switch the receiver from boot mode to GLME mode. */
    static const uint8_t sw[] = {0xfe, 0x00, 0xfd, 0x00, 0x00, 0x55, 0xaa, 0x00, 0xff, 0xfc};
    tx(sw, sizeof(sw), "switch to GLME");

    uint8_t glme[4096];
    static const uint8_t glme_handshake[] = {
        0xe0, 0x00, 0x00, 0x00, 0x07, 0x10, 0x02, 0x01, 0x7e,
        0x06, 0x30, 0x02, 0x00, 0x05, 0x50, 0xe0, 0x01,
    };
    if (plan_only) {
        print_hex("rx ", glme_handshake, sizeof(glme_handshake));
    } else {
        ssize_t n = rx_frame(glme, sizeof(glme), opt_timeout_ms);
        if (n <= 0) {
            warn("timeout waiting for the GLME handshake frame");
            rc = -1;
        } else if ((size_t)n != sizeof(glme_handshake) ||
                   memcmp(glme, glme_handshake, sizeof(glme_handshake)) != 0) {
            warn("unexpected GLME handshake frame (%zd bytes)", n);
            print_hex("glme ", glme, (size_t)n);
        } else {
            logv("rx GLME handshake ok");
        }
    }
    return rc;
}

/* ------------------------------------------------------------------ main */

static void usage(void)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  --tty PATH        serial port (default /dev/ttyS2)\n"
            "  --patch FILE      RAM patch container (required to talk to hardware)\n"
            "  --rfkill PATH     rfkill state file (default: auto-detect type gps)\n"
            "  --no-power        do not power the receiver on\n"
            "  --delay MS        delay after power-on (default 1000)\n"
            "  --timeout MS      per-step reply timeout (default 1000)\n"
            "  --hold MS         keep reading after the mode switch\n"
            "  --burst N         bytes of 0x80 in the autobaud burst (default 16)\n"
            "  --skip-autobaud   do not send the autobaud sync frame\n"
            "  --dump-plan       print the frames and exit; touch no hardware\n"
            "  --verbose         log every frame\n"
            "  --version         print version\n",
            prog);
    exit(2);
}

int main(int argc, char **argv)
{
    const char *tty_path = "/dev/ttyS2";
    const char *patch_path = NULL;
    const char *rfkill = NULL;

    static const struct {
        const char *name;
        int *value;
    } int_opts[] = {
        {"--delay", &opt_delay_ms},
        {"--timeout", &opt_timeout_ms},
        {"--hold", &opt_hold_ms},
        {"--burst", &opt_burst},
    };

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0) {
            usage();
        } else if (strcmp(a, "--version") == 0) {
            printf("%s %s\n", prog, VERSION);
            return 0;
        } else if (strcmp(a, "--tty") == 0 && i + 1 < argc) {
            tty_path = argv[++i];
        } else if (strcmp(a, "--patch") == 0 && i + 1 < argc) {
            patch_path = argv[++i];
        } else if (strcmp(a, "--rfkill") == 0 && i + 1 < argc) {
            rfkill = argv[++i];
        } else if (strcmp(a, "--no-power") == 0) {
            opt_no_power = 1;
        } else if (strcmp(a, "--skip-autobaud") == 0) {
            opt_skip_autobaud = 1;
        } else if (strcmp(a, "--dump-plan") == 0) {
            plan_only = 1;
        } else if (strcmp(a, "--verbose") == 0) {
            verbose = 1;
        } else {
            int *v = NULL;
            for (size_t j = 0; j < sizeof(int_opts) / sizeof(int_opts[0]); j++)
                if (strcmp(a, int_opts[j].name) == 0)
                    v = int_opts[j].value;
            if (!v || i + 1 >= argc)
                usage();
            char *end = NULL;
            long n = strtol(argv[++i], &end, 10);
            if (!end || *end != '\0' || n < 0)
                usage();
            *v = (int)n;
        }
    }

    if (!patch_path)
        die("--patch is required");

    if (opt_burst <= 0 || opt_burst > (int)WAKE_MAX)
        die("--burst must be between 1 and 512");

    load_patch(patch_path);

    if (plan_only) {
        verbose = 0;
        (void)initialise();
        return 0;
    }

    if (rfkill)
        snprintf(rfkill_path, sizeof(rfkill_path), "%s", rfkill);
    if (!opt_no_power)
        power_on(tty_path, opt_delay_ms);

    tty = open_tty(tty_path);
    int rc = initialise();

    if (opt_hold_ms > 0) {
        uint8_t buf[4096];
        int64_t deadline = now_ms() + opt_hold_ms;
        while (now_ms() < deadline) {
            int left = (int)(deadline - now_ms());
            ssize_t n = rx_frame(buf, sizeof(buf), left);
            if (n <= 0)
                break;
            print_hex("glme ", buf, (size_t)n);
        }
    }

    if (rc == 0)
        fprintf(stderr, "%s: initialisation complete\n", prog);
    else
        fprintf(stderr, "%s: initialisation incomplete\n", prog);
    return rc == 0 ? 0 : 1;
}
