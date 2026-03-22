/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * pcie_ioctl.h — ioctl ABI shared between kernel driver and userspace tool.
 *
 * This header is included by both the kernel module (pcie_stub.c) and the
 * userspace utility (pcie_access.c).  Keep it self-contained and use only
 * __u* fixed-width types from <linux/types.h> (available to both contexts).
 */
#ifndef _PCIE_IOCTL_H
#define _PCIE_IOCTL_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define PCIE_IOCTL_READ_MMIO \
	_IOR('P', 0xF0, struct pcie_ioctl_mmio_access)
#define PCIE_IOCTL_WRITE_MMIO \
	_IOWR('P', 0xF1, struct pcie_ioctl_mmio_access)

struct pcie_ioctl_mmio_access {
	__u64 val;
	__u64 off;
	__u8  bar;
	__u8  size;
	__u8  reserved[6];	/* must be zero — reserved for future ABI use */
};

#endif /* _PCIE_IOCTL_H */
