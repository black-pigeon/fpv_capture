/* Host regression harness: exercise the real CLI through a fake device ABI.
 * cc -std=c11 -Wall -Wextra -Werror -Iapp/include -Iapp/capture \
 *   app/tests/test_capture_cli.c app/capture/fpv_frame.c -o /tmp/test-capture-cli
 */
#define _POSIX_C_SOURCE 200809L
#include "fpv_frame.h"
#include <linux/fpv_capture.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static struct fpv_capture_config_v2 applied;
static unsigned packet_ntsc, get_config_calls, configure_calls, detect_calls;
static unsigned initial_enable_mask = 15;
static int damaged, short_packet;
static size_t requested_bytes;
static int fake_open(const char *path, int flags, ...)
{
    assert(!strcmp(path, "/dev/fpv-capture"));
    assert(flags == (O_RDWR | O_CLOEXEC));
    return 123;
}
static int fake_close(int fd) { assert(fd == 123); return 0; }
static int fake_ioctl(int fd, unsigned long cmd, ...)
{
    va_list args;
    void *arg;
    assert(fd == 123);
    va_start(args, cmd); arg = va_arg(args, void *); va_end(args);
    if (cmd == FPV_CAPTURE_INFO) {
        struct fpv_capture_info *info = arg;
        memset(info, 0, sizeof(*info));
        info->lock_mask = 4;
    } else if (cmd == FPV_CAPTURE_GET_CONFIG_V2) {
        struct fpv_capture_config_v2 initial = {
            .config = {3, 6, initial_enable_mask, 900, {1, 2, 3, 4}}, .auto_mask = 10,
        };
        memcpy(arg, &initial, sizeof(initial)); get_config_calls++;
    } else if (cmd == FPV_CAPTURE_CONFIG_V2) {
        const struct fpv_capture_config_v2 *config = arg;
        if (!(config->config.enable_mask & (1U << config->config.channel))) {
            errno = EINVAL;
            return -1;
        }
        applied = *config; configure_calls++;
    } else if (cmd == FPV_CAPTURE_DETECT) {
        struct fpv_capture_detect *detect = arg;
        memset(detect, 0, sizeof(*detect));
        detect->auto_mask = 15;
        detect->channel[1].state = (42U << 8) | (3U << 2) | 2;
        detect->channel[1].field_samples = 307200;
        detect->channel[1].line_samples = 983;
        detect->channel[2].state = (43U << 8) | (3U << 2) | 3;
        detect->channel[2].field_samples = 256256;
        detect->channel[2].line_samples = 976;
        detect_calls++;
    } else {
        assert(!"CLI used legacy or unknown ioctl");
    }
    return 0;
}
static void put32(uint8_t *p, uint32_t value)
{ p[0] = value; p[1] = value >> 8; p[2] = value >> 16; p[3] = value >> 24; }
static ssize_t fake_read(int fd, void *data, size_t bytes)
{
    uint8_t *p = data;
    unsigned height = packet_ntsc ? 480 : 576;
    size_t len = 720 * height + 20;
    assert(fd == 123 && bytes >= len);
    requested_bytes = bytes;
    memset(data, 0, len);
    put32(p, 0x46505647);
    put32(p + 4, (2U << 24) | (2U << 17) | (packet_ntsc << 16) | 17);
    put32(p + 8, (720U << 16) | height); put32(p + 12, 720 * height / 4);
    put32(p + len - 4, 0x454e4400 | (damaged ? 0x40 : 0));
    return len - (short_packet ? 1 : 0);
}
#define open fake_open
#define close fake_close
#define ioctl fake_ioctl
#define read fake_read
#define main capture_main
#include "../tools/fpv-capture.c"
#undef main
#undef read
#undef ioctl
#undef close
#undef open

static int run(char **args, int argc)
{
    optind = 0;
    return capture_main(argc, args);
}
static void capture_case(const char *mode, unsigned ntsc, unsigned auto_mask,
                         unsigned std_mask, unsigned enable_mask)
{
    char path[] = "/tmp/fpv-cli-test-XXXXXX";
    char *args[] = {"fpv-capture", "--channel", "2", "--output", path,
                    "--standard", (char *)mode, NULL};
    int fd = mkstemp(path);
    FILE *image;
    long file_bytes;
    char header[64];
    assert(fd >= 0); close(fd);
    packet_ntsc = ntsc;
    assert(run(args, mode ? 7 : 5) == 0);
    assert(applied.auto_mask == auto_mask && applied.config.standard_mask == std_mask);
    assert(applied.config.channel == 2 && applied.config.enable_mask == enable_mask);
    assert(applied.config.cfo[0] == 1 && applied.config.cfo[3] == 4);
    assert(requested_bytes == (auto_mask & 4 ? FPV_CAPTURE_MAX_BYTES : 720U * (ntsc ? 480 : 576) + 20));
    image = fopen(path, "rb"); assert(image);
    assert(fgets(header, sizeof(header), image) && !strcmp(header, "P5\n"));
    assert(fgets(header, sizeof(header), image));
    assert(!strcmp(header, ntsc ? "720 480\n" : "720 576\n"));
    assert(fgets(header, sizeof(header), image) && !strcmp(header, "255\n"));
    file_bytes = ftell(image); assert(!fseek(image, 0, SEEK_END));
    assert(ftell(image) - file_bytes == 720L * (ntsc ? 480 : 576));
    fclose(image); unlink(path);
}
static void status_case(void)
{
    char *args[] = {"fpv-capture", "--status", NULL};
    char text[2048];
    FILE *captured = tmpfile();
    int saved_stdout;
    size_t size;
    assert(captured);
    fflush(stdout); saved_stdout = dup(STDOUT_FILENO); assert(saved_stdout >= 0);
    assert(dup2(fileno(captured), STDOUT_FILENO) >= 0);
    assert(run(args, 2) == 0 && detect_calls == 1);
    fflush(stdout); assert(dup2(saved_stdout, STDOUT_FILENO) >= 0); close(saved_stdout);
    rewind(captured); size = fread(text, 1, sizeof(text) - 1, captured); text[size] = 0;
    assert(strstr(text, "channel 0: UNKNOWN"));
    assert(strstr(text, "channel 1: PAL mode=auto confidence=3 field_samples=307200 line_samples=983 generation=42"));
    assert(strstr(text, "channel 2: NTSC mode=auto confidence=3 field_samples=256256 line_samples=976 generation=43 frames=0 locked=1"));
    fclose(captured);
}
int main(void)
{
    char *bad[] = {"fpv-capture", "--channel", "2", "--standard", "ntsc", "--output", "/tmp/unused-fpv.pgm", NULL};
    _Static_assert(sizeof(struct fpv_capture_config) == 32, "legacy config ABI");
    _Static_assert(sizeof(struct fpv_capture_info) == 32, "legacy info ABI");
    _Static_assert(sizeof(struct fpv_capture_config_v2) == 36, "v2 config ABI");
    _Static_assert(sizeof(struct fpv_capture_detect) == 52, "detection ABI");
    _Static_assert(FPV_CAPTURE_CONFIG == 0x40204601U, "legacy CONFIG ioctl");
    _Static_assert(FPV_CAPTURE_INFO == 0x80204602U, "legacy INFO ioctl");
    _Static_assert(FPV_CAPTURE_GET_CONFIG == 0x80204603U, "legacy GET_CONFIG ioctl");
    _Static_assert(_IOC_NR(FPV_CAPTURE_CONFIG_V2) == 4, "CONFIG_V2 number");
    _Static_assert(_IOC_NR(FPV_CAPTURE_GET_CONFIG_V2) == 5, "GET_CONFIG_V2 number");
    _Static_assert(_IOC_NR(FPV_CAPTURE_DETECT) == 6, "DETECT number");
    capture_case(NULL, 0, 14, 6, 15);
    capture_case("auto", 1, 14, 6, 15);
    capture_case("pal", 0, 10, 2, 15);
    capture_case("ntsc", 1, 10, 6, 15);
    /* Enable selected channel 2 while preserving disabled channel 1. */
    initial_enable_mask = 9;
    capture_case(NULL, 0, 14, 6, 13);
    capture_case("ntsc", 1, 10, 6, 13);
    initial_enable_mask = 15;
    assert(get_config_calls == 6 && configure_calls == 6);
    damaged = 1; packet_ntsc = 1; assert(run(bad, 7) == 1); damaged = 0;
    short_packet = 1; assert(run(bad, 7) == 1); short_packet = 0;
    /* A complete PAL packet is still wrong for an explicit NTSC request. */
    bad[4] = "pal"; packet_ntsc = 1; assert(run(bad, 7) == 1);
    status_case();
    puts("PASS automatic/manual CLI, preserved channels, packet sizes/errors and detection ABI");
    return 0;
}
