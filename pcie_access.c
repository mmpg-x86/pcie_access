// SPDX-License-Identifier: GPL-2.0
/*
 * pcie_access — userspace CLI tool for MMIO reads/writes via the
 * pcie_stub kernel driver's ioctl interface.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "pcie_ioctl.h"

struct bdf {
	unsigned int domain;
	uint8_t bus;
	uint8_t dev;
	uint8_t fn;
};

static int fill_bdf(const char *arg, struct bdf *bdf);
static int fill_off(const char *arg, uint64_t *off);
static int fill_bar(const char *arg, unsigned int *bar);
static int fill_value(const char *arg, uint64_t *val);

static void usage(const char *progname)
{
	fprintf(stderr,
		"\nUsage:\t%s { [DDDD:]BB:DD.F } { BAR } { OFFSET } { TYPE } [ DATA ]\n"
		"\n"
		"\t[DDDD:]BB:DD.F : PCI address (domain optional, defaults to 0000)\n"
		"\tBAR            : PCI BAR number (0–5)\n"
		"\tOFFSET         : Offset into the BAR region\n"
		"\tTYPE           : Access width: [b]yte (8), [h]alfword (16), [d]word (32), [q]word (64)\n"
		"\tDATA           : Value to write (omit for read)\n"
		"\n"
		"  Numbers accept 0x (hex) or plain decimal.\n"
		"  Note: 64-bit (q) access requires a 64-bit kernel with ioread64/iowrite64 support.\n\n",
		progname);
}

int main(int argc, char **argv)
{
	struct pcie_ioctl_mmio_access access = {0};
	struct bdf bdf;
	uint64_t offset;
	unsigned int bar;
	bool write = false;
	uint64_t wval = 0;
	char path[64] = {0};
	int file_desc, ret_val = 0;

	if (argc < 5) {
		usage(argv[0]);
		return 1;
	}

	if (fill_bdf(argv[1], &bdf))
		return 1;
	if (fill_bar(argv[2], &bar))
		return 1;
	if (fill_off(argv[3], &offset))
		return 1;

	snprintf(path, sizeof(path), "/dev/pcie_ctrl-%04x:%02x:%02x.%x",
		 bdf.domain, bdf.bus, bdf.dev, bdf.fn);

	file_desc = open(path, O_RDWR);
	if (file_desc < 0) {
		perror(path);
		return 1;
	}

	/* Parse access width */
	switch (argv[4][0]) {
	case 'b':
		access.size = 1;
		break;
	case 'h':
		access.size = 2;
		break;
	case 'd':
		access.size = 4;
		break;
	case 'q':
		access.size = 8;
		break;
	default:
		fprintf(stderr, "Invalid access type: '%s' (use b/h/d/q)\n",
			argv[4]);
		close(file_desc);
		return 1;
	}

	if (argc == 6) {
		write = true;
		if (fill_value(argv[5], &wval)) {
			close(file_desc);
			return 1;
		}
	}

	access.off = offset;
	access.bar = bar;

	if (write) {
		access.val = wval;
		ret_val = ioctl(file_desc, PCIE_IOCTL_WRITE_MMIO, &access);
	} else {
		ret_val = ioctl(file_desc, PCIE_IOCTL_READ_MMIO, &access);
	}

	if (ret_val < 0) {
		perror("ioctl");
		close(file_desc);
		return 1;
	}

	if (write)
		printf("[0x%" PRIx64 "] -> BAR[%u] off:0x%" PRIx64 "\n",
		       (uint64_t)access.val, access.bar, (uint64_t)access.off);
	else
		printf("BAR[%u] off:0x%" PRIx64 " -> 0x%" PRIx64 "\n",
		       access.bar, (uint64_t)access.off, (uint64_t)access.val);

	close(file_desc);
	return 0;
}

static int fill_value(const char *arg, uint64_t *val)
{
	char *endp;

	*val = strtoull(arg, &endp, 0);
	if (endp == arg || *endp != '\0') {
		fprintf(stderr, "Invalid value: '%s'\n", arg);
		return -1;
	}
	return 0;
}

static int fill_bar(const char *arg, unsigned int *bar)
{
	char *endp;

	*bar = strtoul(arg, &endp, 0);
	if (endp == arg || *endp != '\0' || *bar > 5) {
		fprintf(stderr, "Invalid BAR number: '%s' (must be 0–5)\n",
			arg);
		return -1;
	}
	return 0;
}

static int fill_off(const char *arg, uint64_t *off)
{
	char *endp;

	*off = strtoull(arg, &endp, 0);
	if (endp == arg || *endp != '\0') {
		fprintf(stderr, "Invalid offset: '%s'\n", arg);
		return -1;
	}
	return 0;
}

static int fill_bdf(const char *arg, struct bdf *bdf)
{
	unsigned int domain, bus, dev, fn;

	/*
	 * Accept both formats:
	 *   "DDDD:BB:DD.F"  — full PCI address with domain
	 *   "BB:DD.F"       — short form (domain defaults to 0000)
	 */
	if (sscanf(arg, "%x:%x:%x.%x", &domain, &bus, &dev, &fn) == 4) {
		/* Full DDDD:BB:DD.F format */
	} else if (sscanf(arg, "%x:%x.%x", &bus, &dev, &fn) == 3) {
		domain = 0;
	} else {
		fprintf(stderr,
			"Invalid BDF: '%s' (expected [DDDD:]BB:DD.F)\n", arg);
		return -1;
	}

	if (domain > 0xffff || bus > 0xff || dev > 0x1f || fn > 0x7) {
		fprintf(stderr,
			"BDF out of range: domain=0x%x bus=0x%x dev=0x%x fn=0x%x\n",
			domain, bus, dev, fn);
		return -1;
	}

	bdf->domain = domain;
	bdf->bus = bus;
	bdf->dev = dev;
	bdf->fn = fn;
	return 0;
}
