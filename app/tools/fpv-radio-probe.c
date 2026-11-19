#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ad9361_api.h"
#include "fpv_radio.h"
#include "axi_dac_core.h"
#include "e200_ad9361_config.h"
#include "linux_gpio.h"
#include "no_os_delay.h"
#include "no_os_gpio.h"

#define DEFAULT_RX_GAIN_DB      20
#define DEFAULT_TX_ATTEN_MDB    10000U

static struct fpv_radio radio;

static uint64_t parse_u64(const char *arg, const char *name)
{
	char *end = NULL;
	unsigned long long value;

	errno = 0;
	value = strtoull(arg, &end, 0);
	if (errno || !end || *end != '\0') {
		fprintf(stderr, "invalid %s: %s\n", name, arg);
		exit(2);
	}

	return (uint64_t)value;
}

static uint32_t parse_u32(const char *arg, const char *name)
{
	uint64_t value = parse_u64(arg, name);

	if (value > UINT32_MAX) {
		fprintf(stderr, "%s out of range: %" PRIu64 "\n", name, value);
		exit(2);
	}

	return (uint32_t)value;
}

static void usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("  --rx-lo HZ          RX LO frequency (default 2400000000)\n");
	printf("  --tx-lo HZ          TX LO frequency (default follows RX LO)\n");
	printf("  --sample-rate HZ    RX/TX sample rate (default 30720000)\n");
	printf("  --bandwidth HZ      RX/TX RF bandwidth (default 18000000)\n");
	printf("  --rx-gain DB        Manual RX gain for RX1/RX2 (default %u)\n",
	       DEFAULT_RX_GAIN_DB);
	printf("  --tx-atten MDB      TX attenuation mdB for TX1/TX2 (default %u)\n",
	       DEFAULT_TX_ATTEN_MDB);
	printf("  --enable-fir        Load and enable the bundled FIR filters\n");
	printf("  --keep-running      Keep the process alive after init\n");
	printf("  --help              Show this help\n");
}

int main(int argc, char **argv)
{
	uint64_t rx_lo = 2400000000ULL;
	uint64_t tx_lo = 0;
	uint32_t sample_rate = 30720000U;
	uint32_t bandwidth = 18000000U;
	uint32_t rx_gain = DEFAULT_RX_GAIN_DB;
	uint32_t tx_atten_mdb = DEFAULT_TX_ATTEN_MDB;
	bool enable_fir = false;
	bool keep_running = false;
	int opt;

	static const struct option long_opts[] = {
		{"rx-lo", required_argument, NULL, 'r'},
		{"tx-lo", required_argument, NULL, 't'},
		{"sample-rate", required_argument, NULL, 's'},
		{"bandwidth", required_argument, NULL, 'b'},
		{"rx-gain", required_argument, NULL, 'g'},
		{"tx-atten", required_argument, NULL, 'a'},
		{"enable-fir", no_argument, NULL, 'f'},
		{"keep-running", no_argument, NULL, 'k'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, argv, "r:t:s:b:g:a:fkh", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'r':
			rx_lo = parse_u64(optarg, "rx-lo");
			break;
		case 't':
			tx_lo = parse_u64(optarg, "tx-lo");
			break;
		case 's':
			sample_rate = parse_u32(optarg, "sample-rate");
			break;
		case 'b':
			bandwidth = parse_u32(optarg, "bandwidth");
			break;
		case 'g':
			rx_gain = parse_u32(optarg, "rx-gain");
			break;
		case 'a':
			tx_atten_mdb = parse_u32(optarg, "tx-atten");
			break;
		case 'f':
			enable_fir = true;
			break;
		case 'k':
			keep_running = true;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 2;
		}
	}

	if (!tx_lo)
		tx_lo = rx_lo;

	setenv("NO_OS_GPIOCHIP", "/dev/gpiochip0", 0);

	printf("%s AD9361 init: rx_lo=%" PRIu64 " tx_lo=%" PRIu64
	       " sample_rate=%" PRIu32 " bandwidth=%" PRIu32
	       " rx_gain=%" PRIu32 " tx_atten=%" PRIu32 "\n",
	       fpv_board_current()->name, rx_lo, tx_lo, sample_rate, bandwidth, rx_gain, tx_atten_mdb);

	if (fpv_radio_init(&radio, fpv_board_current(), rx_lo, tx_lo, sample_rate, bandwidth, rx_gain,
			tx_atten_mdb, enable_fir) < 0) {
		fpv_radio_close(&radio);
		return 1;
	}

	printf("%s AD9361 init done; RX port %c\n", fpv_board_current()->name, 'A'+radio.port);

	if (keep_running) {
		for (;;)
			no_os_mdelay(1000);
	}

	fpv_radio_close(&radio);
	return 0;
}
