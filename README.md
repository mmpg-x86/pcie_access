# pcie_access

[![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg)](LICENSE)
[![Kernel](https://img.shields.io/badge/Linux_Kernel-≥5.5-green.svg)](https://kernel.org)
[![Platform](https://img.shields.io/badge/Arch-x86__64%20|%20ARM64%20|%20RISC--V-orange.svg)](#)

**Generic PCIe BAR register access tool** — read/write any PCI device's memory-mapped registers from the command line.

> **⚠️ Lab/debug tool only.** This driver grants direct MMIO access to PCI
> device BARs from userspace. It is intended for developers and embedded
> systems — **not for production use.** Production systems should use
> [UIO](https://www.kernel.org/doc/html/latest/driver-api/uio-howto.html)
> or [VFIO](https://www.kernel.org/doc/html/latest/driver-api/vfio.html).

### Quick start

```bash
make
sudo insmod pcie_access_drv.ko vid=0x1234 device_id=0x5678
./pcie_access 03:00.0 0 0x0 d       # read device ID register
```

## Overview

This project provides:

1. **Kernel driver** (`pcie_access_drv.ko`) — a generic PCI driver that binds to any
   device (specified at load time or via sysfs), maps memory BARs using
   `pcim_iomap()`, and exposes a `miscdevice` per PCI function for MMIO
   reads/writes via ioctl. Optionally monitors MSI-X, MSI, or legacy INTx
   interrupt delivery.

2. **Userspace tool** (`pcie_access`) — a CLI utility that communicates with the
   driver to perform MMIO reads/writes on mapped BARs. Supports full PCI
   domain addressing (`DDDD:BB:DD.F`), 64-bit offsets, and byte/half/dword
   access widths.

**Minimum kernel version:** 5.5 (for `compat_ptr_ioctl` support).

## Project structure

```
pcie_access/
├── pcie_stub.c              # Kernel driver (~500 lines)
├── pcie_access.c            # Userspace CLI tool
├── pcie_ioctl.h             # Shared ioctl ABI header (kernel + userspace)
├── Makefile                 # Builds kernel module, userspace tool, and .deb packages
├── dkms.conf                # DKMS config for manual persistent installation
├── debian/                  # Debian packaging
│   ├── control              # Package metadata (pcie-stub-dkms + pcie-access)
│   ├── rules                # Build rules
│   ├── changelog            # Version history
│   ├── copyright            # License (GPL-2.0)
│   ├── pcie-stub-dkms.dkms  # DKMS config template
│   ├── pcie-stub-dkms.install
│   ├── pcie-access.install
│   ├── compat               # Debhelper compat level
│   └── source/format
├── scripts/
│   └── pcie_bind.sh         # Helper script to bind/unbind PCI devices
├── LICENSE                  # GPL-2.0
└── README.md
```

## Building

Requires Linux kernel headers for the running kernel.

```bash
# Build both kernel module and userspace tool
make

# Or build individually
make kbuild   # kernel module only
make user     # userspace tool only

make clean    # clean all build artifacts
```

## Kernel Driver

### Loading the driver

Specify the target PCI device by Vendor ID and Device ID at module load time:

```bash
# Basic: MMIO access only, no interrupts
sudo insmod pcie_access_drv.ko vid=0x1234 device_id=0x5678

# With interrupt monitoring (auto-detect best IRQ type)
sudo insmod pcie_access_drv.ko vid=0x1234 device_id=0x5678 irq_mode=1

# Force MSI-X with up to 8 vectors
sudo insmod pcie_access_drv.ko vid=0x1234 device_id=0x5678 irq_mode=2 max_vectors=8

# Force MSI (single vector)
sudo insmod pcie_access_drv.ko vid=0x1234 device_id=0x5678 irq_mode=3

# Force legacy INTx (see limitations below)
sudo insmod pcie_access_drv.ko vid=0x1234 device_id=0x5678 irq_mode=4

# Force bus mastering without interrupts (for DMA-capable devices)
sudo insmod pcie_access_drv.ko vid=0x1234 device_id=0x5678 set_master=1
```

### Dynamic binding (no vid/device_id at load time)

```bash
sudo insmod pcie_access_drv.ko

# Add a device ID at runtime
echo "1234 5678" | sudo tee /sys/bus/pci/drivers/pcie_access/new_id

# Or unbind from current driver and bind to this one
echo "0000:03:00.0" | sudo tee /sys/bus/pci/devices/0000:03:00.0/driver/unbind
echo "0000:03:00.0" | sudo tee /sys/bus/pci/drivers/pcie_access/bind
```

### What happens on probe

When the driver binds to a device, it:

1. Enables the PCI device (managed via `pcim_enable_device`)
2. Maps all available **memory** BARs (up to 6) using `pcim_iomap()` — I/O BARs are skipped
3. **Fails probe if zero memory BARs could be mapped**
4. Optionally sets up interrupt handlers (MSI-X / MSI / legacy INTx)
5. Registers a misc character device at `/dev/pcie_ctrl-DDDD:BB:DD.F`

### Interrupt monitoring

The driver can hook into device interrupts to observe interrupt delivery
during hardware evaluation. Each interrupt is logged to the kernel log
with the IRQ type, vector number, Linux IRQ number, and a running count.

**Logging is rate-limited** (`dev_info_ratelimited`) to prevent kernel log
flooding and soft lockups under high interrupt rates.

```
pcie_stub 0000:03:00.0: MSI-X vec[0] irq=45 count=1
pcie_stub 0000:03:00.0: MSI-X vec[0] irq=45 count=2
pcie_stub 0000:03:00.0: MSI-X vec[2] irq=47 count=1
```

**IRQ modes:**

| `irq_mode` | Type | Vectors | Shared | Handler returns |
|------------|------|---------|--------|-----------------|
| `0` | Disabled (default) | — | — | — |
| `1` | Auto (MSI-X → MSI → INTx) | up to `max_vectors` | depends | depends on type |
| `2` | MSI-X only | up to `max_vectors` | No | `IRQ_HANDLED` |
| `3` | MSI only | 1 | No | `IRQ_HANDLED` |
| `4` | Legacy INTx only | 1 | Yes (`IRQF_SHARED`) | `IRQ_NONE` |

**Bus mastering & DMA mask:** When MSI or MSI-X is requested (modes 1–3),
the driver automatically calls `pci_set_master()` and sets a DMA mask
(64-bit with 32-bit fallback). This is required because MSI/MSI-X signaling
is a DMA write from the device to a platform-specific target address.
If IRQ setup fails, bus mastering is reverted via `pci_clear_master()`.

**Legacy INTx limitations:** Since this is a generic driver with no
device-specific knowledge, the interrupt handler **cannot confirm interrupt
ownership** or clear the interrupt source on legacy INTx. The handler
returns `IRQ_NONE` (allowing other handlers on the shared line to run),
and the kernel may eventually disable the IRQ if the device continuously
asserts it. **For reliable interrupt monitoring, prefer MSI or MSI-X**
(irq_mode 1, 2, or 3).

**Monitor in real time:**

```bash
dmesg -w | grep pcie_stub
```

### AER error handling & DPC

The driver implements PCIe **AER (Advanced Error Reporting)** error recovery
handlers, enabling validation of error handling, **DPC (Downstream Port
Containment)**, and hot-plug recovery on real hardware.

When the kernel's AER/DPC subsystem detects an error on the PCIe link, it
calls the driver's error recovery callbacks. All events are logged to dmesg:

```
pcie_stub 0000:03:00.0: AER: error_detected — state=io_frozen (2)
pcie_stub 0000:03:00.0: AER: slot_reset — re-enabling device
pcie_stub 0000:03:00.0: AER: resume — device recovered
```

**Recovery flow:**

| Callback | When called | Driver action |
|----------|-------------|---------------|
| `error_detected` (io_normal) | Correctable error | Log only, return `CAN_RECOVER` |
| `error_detected` (io_frozen) | Non-fatal uncorrectable | Set `in_error_recovery`, ioctls return `-EIO`, request slot reset |
| `error_detected` (io_perm_failure) | Fatal error | Mark device `dead`, ioctls return `-ENODEV` |
| `slot_reset` | After link/FLR reset | Re-enable device, restore PCI state |
| `resume` | Recovery complete | Clear `in_error_recovery`, ioctls resume |

**Testing AER with `aer-inject`:**

```bash
# Install aer-inject (available in most distros)
sudo apt install aer-inject    # Debian
sudo dnf install aer-inject    # Fedora

# Inject a correctable error
echo "AER 0000:03:00.0 cor_status RCVR" | sudo tee /dev/aer_inject

# Inject a non-fatal uncorrectable error (triggers io_frozen → slot_reset → resume)
echo "AER 0000:03:00.0 uncor_status POISON_TLP" | sudo tee /dev/aer_inject

# Monitor recovery
dmesg -w | grep -E "pcie_stub|AER|DPC"
```

**DPC (Downstream Port Containment):** DPC is handled by the kernel's
`pciehp`/`dpc` drivers at the port level. When DPC triggers (e.g., due to
a fatal error on the link), the kernel freezes I/O and calls our
`error_detected(io_frozen)` callback. After the link recovers, `slot_reset`
and `resume` are called. This allows validating that your device's DPC
implementation works end-to-end.

**Kernel requirements for AER/DPC:**
- `CONFIG_PCIEAER` — PCIe Advanced Error Reporting
- `CONFIG_PCIE_DPC` — Downstream Port Containment (optional)
- `CONFIG_PCIEPORTBUS` — PCIe Port Bus driver
- BIOS/firmware must not disable AER (check with `dmesg | grep AER`)

### Security

The driver checks for `CAP_SYS_RAWIO` on `open()`. If the calling process
lacks the capability, a kernel warning is logged via `dev_warn_once()` but
access is **not blocked** — this is intentional for lab/embedded use where
restrictive capabilities may not be configured.

**Do not deploy this driver on production systems.**

### Safe hot-unplug / rmmod

The driver handles device removal safely even while userspace has open
file descriptors:

- **Reference counting** (`kref`): the `pcie_dev` structure is not freed
  until all open file descriptors are closed
- **Read-write semaphore** (`rw_sem`): the `remove()` path takes a write
  lock, waiting for all in-flight ioctls to complete before marking the
  device as dead
- After removal, any ioctl on an existing fd returns `-ENODEV`

### Unloading

```bash
sudo rmmod pcie_access_drv
```

## Userspace Tool

### Usage

```
./pcie_access { [DDDD:]BB:DD.F } { BAR } { OFFSET } { TYPE } [ DATA ]
```

| Argument | Description |
|----------|-------------|
| `[DDDD:]BB:DD.F` | PCI address — domain is optional (defaults to `0000`) |
| `BAR` | BAR number (`0`–`5`) |
| `OFFSET` | Byte offset into the BAR region (64-bit, `0x` prefix for hex) |
| `TYPE` | Access width: `b` (byte/8), `h` (halfword/16), `d` (dword/32), `q` (qword/64) |
| `DATA` | Value to write — omit for read (`0x` prefix for hex) |

**Constraints enforced by the kernel driver:**
- Natural alignment (e.g., dword reads must be at 4-byte-aligned offsets)
- Bounds checking (offset + size must not exceed BAR length)
- Reserved fields in ioctl struct must be zero

### Examples

```bash
# Read a 32-bit dword from BAR 0, offset 0x100
./pcie_access 03:00.0 0 0x100 d

# Same with explicit domain
./pcie_access 0000:03:00.0 0 0x100 d

# Write 0xDEADBEEF to BAR 0, offset 0x200
./pcie_access 03:00.0 0 0x200 d 0xDEADBEEF

# Read a single byte from BAR 2, offset 0x0
./pcie_access 05:00.0 2 0x0 b

# Read a 16-bit halfword from BAR 0, offset 0x42
./pcie_access 03:00.0 0 0x42 h

# Read a 64-bit qword from BAR 0, offset 0x100 (requires 64-bit kernel)
./pcie_access 03:00.0 0 0x100 q
```

## Module Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `vid` | uint | 0 | PCI Vendor ID of target device |
| `device_id` | uint | 0 | PCI Device ID of target device |
| `irq_mode` | int | 0 | `0`=none, `1`=auto, `2`=MSI-X, `3`=MSI, `4`=legacy INTx |
| `max_vectors` | int | 32 | Max MSI-X vectors to allocate (modes 1 and 2) |
| `set_master` | int | 0 | Force `pci_set_master()` even without MSI/MSI-X |

## Ioctl ABI

The kernel and userspace communicate via two ioctl commands defined in
`pcie_ioctl.h`:

| Command | Direction | Description |
|---------|-----------|-------------|
| `PCIE_IOCTL_READ_MMIO` | kernel → user | Read 1/2/4/8 bytes from a BAR |
| `PCIE_IOCTL_WRITE_MMIO` | user → kernel | Write 1/2/4/8 bytes to a BAR |

**Struct layout** (24 bytes, naturally aligned):

```c
struct pcie_ioctl_mmio_access {
    __u64 val;          /* value read/written */
    __u64 off;          /* byte offset into BAR (64-bit capable) */
    __u8  bar;          /* BAR number (0–5) */
    __u8  size;         /* access width: 1, 2, 4, or 8 */
    __u8  reserved[6];  /* must be zero — reserved for future ABI use */
};
```

**64-bit (qword) access:** 8-byte reads/writes using `ioread64()`/`iowrite64()`
are only available on **64-bit kernels** (`CONFIG_64BIT`). On 32-bit kernels,
requesting `size=8` returns `-EINVAL`. The userspace tool always accepts
the `q` type — the kernel will reject it if unsupported.

> **Atomicity caveat:** `CONFIG_64BIT` guarantees the `ioread64`/`iowrite64`
> API is available, but **not** that the hardware performs a single atomic
> 64-bit PCIe TLP. On **x86_64**, 64-bit MMIO is a true atomic bus
> transaction. On some **ARM64** implementations, the CPU or interconnect
> may split it into two 32-bit accesses. If your device requires atomic
> 64-bit register access, verify your platform's behavior.

The `reserved` field is validated by the kernel — non-zero values return
`-EINVAL`. This allows future ABI extensions without breaking backward
compatibility.

## Architecture

```
  ┌──────────────────┐
  │  pcie_access     │  userspace CLI tool
  │  (pcie_access.c) │
  └────────┬─────────┘
           │ ioctl(PCIE_IOCTL_READ/WRITE_MMIO)
           │
  ─────────┼──────────────────────────────────── user/kernel boundary
           │
  ┌────────▼─────────────────────────────────┐
  │  /dev/pcie_ctrl-DDDD:BB:DD.F             │  misc char device
  │  (miscdevice, one per PCI function)      │
  ├──────────────────────────────────────────┤
  │  pcie_access_drv.ko                      │
  │                                          │
  │  ┌─ Lifecycle ────────────────────────┐  │
  │  │ kref refcount (safe hot-unplug)    │  │
  │  │ rw_sem + dead flag (ioctl drain)   │  │
  │  └───────────────────────────────────-┘  │
  │                                          │
  │  ┌─ MMIO ────────────────────────────┐   │
  │  │ BAR[0..5] via pcim_iomap()        │   │
  │  │ ioread/iowrite 8/16/32/64*        │   │
  │  │ alignment + bounds checked        │   │
  │  │ *64-bit on CONFIG_64BIT only      │   │
  │  └──────────────────────────────────-┘   │
  │                                          │
  │  ┌─ IRQ (optional) ─────────────────┐   │
  │  │ MSI-X / MSI / legacy INTx        │   │
  │  │ pci_alloc_irq_vectors()           │   │
  │  │ per-vector atomic counter         │   │
  │  │ dev_info_ratelimited() logging    │   │
  │  └──────────────────────────────────-┘   │
  │                                          │
  └────────────────────┬─────────────────────┘
                       │
  ┌────────────────────▼─────────────────────┐
  │            PCI/PCIe Device               │
  │  (FPGA, NIC, accelerator, etc.)          │
  └──────────────────────────────────────────┘
```

## Why not pcimem, UIO, or VFIO?

A fair question. There are existing tools and frameworks for userspace
device access. Here's why this driver takes a different approach, and
when each tool is the right choice.

### Why not pcimem?

[pcimem](https://github.com/billfarrow/pcimem) is a popular lightweight
tool that `mmap()`s `/sys/bus/pci/devices/.../resourceN` directly from
userspace — no kernel driver needed. It's elegant in its simplicity, but
the mmap approach has fundamental trade-offs for hardware validation:

| | `pcie_stub` + `pcie_access` (this) | `pcimem` (mmap-based) |
|---|---|---|
| **Kernel driver** | Yes (out-of-tree module) | No (uses sysfs resourceN) |
| **Access mechanism** | ioctl per access (syscall) | mmap + pointer dereference |
| **Access width control** | **Kernel-enforced** 1/2/4/8 bytes | Userspace cast — compiler/CPU decides |
| **Deterministic behavior** | Each access is a single `ioread`/`iowrite` call with defined semantics | Compiler may optimize, reorder, or split accesses |
| **Alignment enforcement** | Kernel rejects unaligned accesses | Silent undefined behavior |
| **Bounds checking** | Kernel checks offset vs BAR length | Can access beyond intended range |
| **Audit trail** | `dev_dbg` per access (dyndbg) | None |
| **Interrupt monitoring** | MSI-X/MSI/INTx with logging | Not possible |
| **Hot-unplug safety** | kref + rw_semaphore | Process crash / SIGBUS |
| **Performance** | ~1µs per access (syscall overhead) | ~10ns per access (direct memory) |
| **Setup complexity** | `insmod` + bind device | Zero (sysfs always available) |

**The key design decision:** For hardware validation and FPGA bring-up,
**deterministic, auditable, single-access behavior** matters more than raw
speed. When you're debugging a register that returns different values on
consecutive reads, you need to know that each read is exactly the width
you requested, naturally aligned, and logged — not subject to compiler
optimizations, speculative execution, or store-buffer coalescing.

The ioctl approach guarantees:
- **One `ioread*()` call per request** — the kernel's MMIO accessors use
  `volatile` semantics and appropriate memory barriers
- **Exact access width** — if you ask for a byte read, you get `ioread8()`,
  not a dword read with a byte extract
- **No compiler interference** — `ioread`/`iowrite` are opaque to the
  compiler; it cannot optimize, reorder, or eliminate them
- **Kernel-enforced validation** — alignment, bounds, and reserved-field
  checks happen before any hardware access

**When to use pcimem instead:** If you need maximum throughput (millions of
accesses per second), don't care about per-access logging, and trust your
C code to do the right `volatile` casts — pcimem with mmap is faster and
requires no kernel module. It's a great tool for a different use case.

### Why not UIO or VFIO?

The kernel also provides `uio_pci_generic` and `vfio-pci` for
userspace device access. Here's why this stub driver is the better fit for
**hardware evaluation and early bring-up**:

### Comparison

| | `pcie_stub` (this) | `uio_pci_generic` | `vfio-pci` |
|---|---|---|---|
| **Purpose** | Quick HW eval & register poking | Userspace driver framework | VM passthrough & DPDK |
| **Kernel config needed** | Basic PCI (`CONFIG_PCI`) | `CONFIG_UIO` + `CONFIG_UIO_PCI_GENERIC` | `CONFIG_VFIO` + `CONFIG_VFIO_PCI` + IOMMU |
| **Userspace complexity** | One CLI command | Write C code: `open()`, `mmap()`, pointer math | Container/group/device API, IOMMU groups |
| **Access width control** | Kernel-enforced byte/half/dword | None — raw pointer dereference | None at this level |
| **Alignment checking** | Kernel-enforced | None | None |
| **Per-access logging** | `dev_dbg` on every r/w | None built-in | None built-in |
| **Interrupt monitoring** | MSI-X/MSI/INTx with logging | Basic (read blocks until IRQ) | VFIO eventfd |
| **Bounds protection** | Kernel checks offset vs BAR length | Userspace can access entire mmap | IOMMU-level only |
| **Hot-unplug safety** | kref + rwsem (safe) | fd remains valid | VFIO group lifecycle |
| **Time to first register read** | `insmod` + one CLI command | Write mmap tool, compile, run | Configure IOMMU, write client, run |
| **Performance** | Syscall per access (ioctl) | Direct memory access (mmap) | Direct memory access (mmap) |
| **Works on minimal embedded** | ✓ | Only if UIO is configured | Rarely (needs IOMMU) |

### When to use what

**Use `pcie_stub` (this driver) when:**
- You're doing initial hardware bring-up or FPGA register exploration
- You want to poke a few registers from the command line without writing code
- You need to monitor interrupt delivery (MSI-X/MSI/INTx) during bring-up
- You need audit-quality per-access kernel logs (`dyndbg`)
- Your embedded kernel doesn't have UIO/VFIO compiled in
- You want kernel-enforced access width, alignment, and bounds checking

**Use `uio_pci_generic` when:**
- You need high-throughput register access (mmap eliminates syscall overhead)
- You're writing a proper userspace driver that will do sustained I/O
- UIO is available in your kernel config

**Use `vfio-pci` when:**
- You need IOMMU isolation (security boundary between device and host)
- You're doing DMA from userspace
- You're doing device passthrough to a VM
- You're building a DPDK-class application

### The practical reality

With `uio_pci_generic`:
```bash
# 1. Ensure CONFIG_UIO + CONFIG_UIO_PCI_GENERIC (rebuild kernel if not)
# 2. modprobe uio_pci_generic
# 3. Unbind existing driver
# 4. Bind to UIO
# 5. Write a C program: open /dev/uioN, mmap BAR, volatile read/write
# 6. Compile and run
```

With `pcie_access`:
```bash
make
sudo insmod pcie_access_drv.ko vid=0x1234 device_id=0x5678
./pcie_access 03:00.0 0 0x100 d
```

When you're staring at a new FPGA board on the bench and need to verify
that BAR 0 register 0x0 returns the expected device ID, the three-command
workflow wins.

## Helper Script

A convenience script is included to simplify the bind/unbind workflow:

```bash
# Bind a device (auto-unbinds from current driver, adds VID:PID, binds)
sudo ./scripts/pcie_bind.sh bind 0000:03:00.0

# Check which driver owns a device
sudo ./scripts/pcie_bind.sh status 0000:03:00.0

# List all devices bound to pcie_access
sudo ./scripts/pcie_bind.sh list

# Unbind a device
sudo ./scripts/pcie_bind.sh unbind 0000:03:00.0
```

## DKMS Installation

For persistent installation across kernel updates using
[DKMS](https://github.com/dell/dkms):

```bash
# Copy source to DKMS tree
sudo mkdir -p /usr/src/pcie_access_drv-0.2
sudo cp pcie_stub.c pcie_ioctl.h Makefile dkms.conf /usr/src/pcie_access_drv-0.2/

# Add, build, and install
sudo dkms add pcie_access_drv/0.2
sudo dkms build pcie_access_drv/0.2
sudo dkms install pcie_access_drv/0.2

# Module is now auto-built on kernel updates
modinfo pcie_access_drv
```

## Debian Packages

Build `.deb` packages for distribution via your own Debian repository:

```bash
# Install build dependencies
sudo apt install debhelper dkms dpkg-dev

# Build packages (output in parent directory)
make deb

# Or equivalently:
dpkg-buildpackage -us -uc -b
```

This produces **two packages**:

| Package | Arch | Contents |
|---------|------|----------|
| `pcie-stub-dkms_0.2_all.deb` | `all` | Kernel module source + DKMS config. Auto-builds for any kernel. |
| `pcie-access_0.2_amd64.deb` | `amd64` | Userspace CLI tool + helper script in `/usr/bin/` |

### Installing

```bash
# Install both packages
sudo dpkg -i ../pcie-stub-dkms_0.2_all.deb ../pcie-access_0.2_amd64.deb

# DKMS automatically builds the module for your current kernel:
#   dkms: running auto-install for pcie_stub/0.2
#   Module pcie_stub/0.2 built for kernel 6.12.54

# Verify
modinfo pcie_stub
which pcie_access
```

### Custom kernel support

The DKMS package works with **any kernel** (stock Debian or custom-compiled)
as long as the matching kernel headers are installed:

```bash
# For stock Debian kernel:
sudo apt install linux-headers-$(uname -r)

# For custom kernel: ensure /lib/modules/$(uname -r)/build points to
# your kernel source/build tree. DKMS uses this to compile the module.
```

### Adding to your Debian repository

```bash
# Sign and add to your repo (example with reprepro)
dpkg-sig --sign builder ../pcie-stub-dkms_0.2_all.deb
dpkg-sig --sign builder ../pcie-access_0.2_amd64.deb
reprepro -b /path/to/repo includedeb bookworm ../pcie-stub-dkms_0.2_all.deb
reprepro -b /path/to/repo includedeb bookworm ../pcie-access_0.2_amd64.deb

# Users can then install with:
sudo apt install pcie-stub-dkms pcie-access
```

## Contributing

Contributions are welcome! This is a lab tool — if you have a use case
that needs a new feature (e.g., PCI config space access, DMA buffer
allocation, mmap support), open an issue to discuss the approach first.

## License

GPL-2.0
