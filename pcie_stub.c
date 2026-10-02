// SPDX-License-Identifier: GPL-2.0
/*
 * Generic PCI stub driver for userspace BAR access via ioctl.
 *
 * Binds to any PCI device specified at module load time (vid=/device_id=)
 * or at runtime via sysfs new_id.  Maps memory BARs and exposes a
 * character device per function for MMIO reads/writes from userspace.
 *
 * Optionally registers interrupt handlers (MSI-X, MSI, or legacy INTx)
 * to monitor and log interrupt delivery during hardware evaluation.
 *
 * Implements PCIe AER (Advanced Error Reporting) error recovery handlers
 * to validate error handling, Downstream Port Containment (DPC), and
 * hot-plug behavior of real hardware.
 *
 * This is a lab/debug tool intended for developers and embedded use.
 * It is NOT intended for production systems.
 *
 * Minimum kernel version: 5.5 (for compat_ptr_ioctl).
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/miscdevice.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/capability.h>
#include <linux/atomic.h>
#include <linux/kref.h>
#include <linux/rwsem.h>

#include "pcie_ioctl.h"

/* PCI_IRQ_LEGACY was renamed to PCI_IRQ_INTX in kernel 6.8 */
#ifndef PCI_IRQ_INTX
#define PCI_IRQ_INTX PCI_IRQ_LEGACY
#endif

#define PCIE_ACCESS_DRIVER "pcie_access"

/* --------------------------------------------------------------------
 * Module parameters
 * ------------------------------------------------------------------ */

static unsigned int vid;
module_param(vid, uint, 0444);
MODULE_PARM_DESC(vid, "PCI Vendor ID of the target device (hex, e.g. 0x1234)");

static unsigned int device_id;
module_param(device_id, uint, 0444);
MODULE_PARM_DESC(device_id, "PCI Device ID of the target device (hex, e.g. 0x5678)");

/*
 * irq_mode controls interrupt handling:
 *   0 = disabled (default)
 *   1 = auto: try MSI-X first, then MSI, then legacy INTx
 *   2 = MSI-X only
 *   3 = MSI only
 *   4 = legacy INTx only
 *
 * MSI and MSI-X require bus mastering (the device performs a DMA write
 * to signal the interrupt), so the driver automatically calls
 * pci_set_master() and sets a DMA mask when MSI/MSI-X is requested.
 *
 * Legacy INTx note: since this is a generic driver with no device-specific
 * knowledge, the interrupt handler cannot confirm interrupt ownership or
 * clear the interrupt source.  The handler returns IRQ_NONE for legacy
 * INTx, which means the kernel may eventually disable the IRQ if the
 * device continuously asserts it.  For reliable interrupt monitoring,
 * prefer MSI or MSI-X (irq_mode=1, 2, or 3).
 */
static int irq_mode;
module_param(irq_mode, int, 0444);
MODULE_PARM_DESC(irq_mode,
	"IRQ mode: 0=none, 1=auto(MSI-X>MSI>INTx), 2=MSI-X, 3=MSI, 4=legacy INTx");

static int max_vectors = 32;
module_param(max_vectors, int, 0444);
MODULE_PARM_DESC(max_vectors,
	"Max MSI-X vectors to allocate (default=32, only used in MSI-X mode)");

static int set_master;
module_param(set_master, int, 0444);
MODULE_PARM_DESC(set_master,
	"Force pci_set_master even without MSI/MSI-X (0=off, 1=on, default=0)");

/* --------------------------------------------------------------------
 * Internal definitions
 * ------------------------------------------------------------------ */

#define PCIE_NR_BARS	6

struct pcie_bar {
	resource_size_t len;
	void __iomem *mmio;
};

/* Per-vector interrupt context */
struct pcie_irq_ctx {
	struct pcie_dev *pciedev;
	int vector;
	atomic_t count;
};

struct pcie_dev {
	struct pci_dev *pdev;
	struct miscdevice misc;
	char misc_name[32];
	struct pcie_bar bars[PCIE_NR_BARS];

	/* Interrupt state */
	int nr_irqs;
	struct pcie_irq_ctx *irq_ctx;

	/*
	 * Lifetime management: kref ensures pciedev is not freed while
	 * any file descriptor is still open.  rw_sem + dead flag ensure
	 * no ioctl is in flight when the device is being removed.
	 *
	 * - open()    → kref_get (hold reference for this fd)
	 * - release() → kref_put (drop fd reference)
	 * - ioctl()   → down_read(rw_sem), check dead, work, up_read
	 * - remove()  → down_write(rw_sem), set dead, up_write,
	 *               misc_deregister, teardown, kref_put (drop device ref)
	 */
	struct kref refcount;
	struct rw_semaphore rw_sem;
	bool dead;
	bool in_error_recovery;	/* set during AER error recovery */
};

static void pcie_dev_free(struct kref *ref)
{
	struct pcie_dev *pciedev = container_of(ref, struct pcie_dev, refcount);

	kfree(pciedev);
}

/*
 * The id_table starts empty -- it is populated dynamically in __init
 * based on the vid/device_id module parameters.  Additional devices
 * can be added at runtime via the sysfs new_id mechanism.
 */
static struct pci_device_id pcie_id_table[] = {
	{0,},
	{0,}
};
MODULE_DEVICE_TABLE(pci, pcie_id_table);

/* --------------------------------------------------------------------
 * File operations
 * ------------------------------------------------------------------ */

static int pcie_fops_open(struct inode *inode, struct file *filp)
{
	/* misc framework sets filp->private_data to &pciedev->misc */
	struct miscdevice *misc = filp->private_data;
	struct pcie_dev *pciedev = container_of(misc, struct pcie_dev, misc);

	/*
	 * CAP_SYS_RAWIO warning: this driver is a lab/debug tool for
	 * developers and embedded use.  We warn but do not block access
	 * without the capability -- production systems should not use
	 * this driver at all.
	 */
	if (!capable(CAP_SYS_RAWIO))
		dev_warn_once(&pciedev->pdev->dev,
			      "opened by process without CAP_SYS_RAWIO -- "
			      "not suitable for production use\n");

	/* Take a reference so pciedev outlives this file descriptor */
	kref_get(&pciedev->refcount);
	filp->private_data = pciedev;

	/* Pure ioctl device -- no read/write/seek fops, mark non-seekable. */
	stream_open(inode, filp);

	return 0;
}

static int pcie_fops_release(struct inode *inode, struct file *filp)
{
	struct pcie_dev *pciedev = filp->private_data;

	kref_put(&pciedev->refcount, pcie_dev_free);
	return 0;
}

/* --------------------------------------------------------------------
 * IOCTL helpers
 * ------------------------------------------------------------------ */

static int validate_mmio_access(struct pcie_dev *pciedev,
				const struct pcie_ioctl_mmio_access *access)
{
	u8 bar = access->bar;
	u8 size = access->size;
	u64 off = access->off;

	/* Reject if reserved fields are non-zero (future ABI compat) */
	if (memchr_inv(access->reserved, 0, sizeof(access->reserved)))
		return -EINVAL;

	if (bar >= PCIE_NR_BARS)
		return -ENODEV;
	if (!pciedev->bars[bar].mmio)
		return -ENXIO;

	/*
	 * Valid access sizes: 1, 2, 4 bytes on all architectures.
	 * 8-byte access (ioread64/iowrite64) is only available on 64-bit
	 * kernels.  Note: whether 8-byte MMIO results in a single atomic
	 * PCIe TLP depends on the CPU and platform -- x86_64 guarantees
	 * atomicity, some ARM64 implementations may split into two 32-bit
	 * accesses.
	 */
	if (size != 1 && size != 2 && size != 4
#ifdef CONFIG_64BIT
	    && size != 8
#endif
	   )
		return -EINVAL;

	/* Enforce natural alignment */
	if (off % size != 0)
		return -EINVAL;

	/* Bounds check (overflow-safe: check off against remaining space) */
	if (off > pciedev->bars[bar].len - size)
		return -EFAULT;

	return 0;
}

/* --------------------------------------------------------------------
 * IOCTL handlers
 * ------------------------------------------------------------------ */

static int ioctl_read_mmio(struct pcie_dev *pciedev, void __user *argp)
{
	struct pcie_ioctl_mmio_access access = {};
	void __iomem *addr;
	int rc;

	if (copy_from_user(&access, argp, sizeof(access)))
		return -EFAULT;

	rc = validate_mmio_access(pciedev, &access);
	if (rc)
		return rc;

	addr = pciedev->bars[access.bar].mmio + access.off;

	switch (access.size) {
	case 1:
		access.val = ioread8(addr);
		break;
	case 2:
		access.val = ioread16(addr);
		break;
	case 4:
		access.val = ioread32(addr);
		break;
#ifdef CONFIG_64BIT
	case 8:
		access.val = ioread64(addr);
		break;
#endif
	}

	dev_dbg(&pciedev->pdev->dev,
		"read bar:%u size:%u off:0x%llx -> 0x%llx",
		access.bar, access.size, access.off, access.val);

	if (copy_to_user(argp, &access, sizeof(access)))
		return -EFAULT;

	return 0;
}

static int ioctl_write_mmio(struct pcie_dev *pciedev, void __user *argp)
{
	struct pcie_ioctl_mmio_access access = {};
	void __iomem *addr;
	int rc;

	if (copy_from_user(&access, argp, sizeof(access)))
		return -EFAULT;

	rc = validate_mmio_access(pciedev, &access);
	if (rc)
		return rc;

	addr = pciedev->bars[access.bar].mmio + access.off;

	switch (access.size) {
	case 1:
		iowrite8((u8)access.val, addr);
		break;
	case 2:
		iowrite16((u16)access.val, addr);
		break;
	case 4:
		iowrite32((u32)access.val, addr);
		break;
#ifdef CONFIG_64BIT
	case 8:
		iowrite64(access.val, addr);
		break;
#endif
	}

	dev_dbg(&pciedev->pdev->dev,
		"write 0x%llx -> bar:%u size:%u off:0x%llx",
		access.val, access.bar, access.size, access.off);

	return 0;
}

static long pcie_fops_ioctl(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	struct pcie_dev *pciedev = filp->private_data;
	void __user *argp = (void __user *)arg;
	int rc;

	/*
	 * Take the read side of rw_sem to ensure the device isn't being
	 * removed while we access the BARs.  Multiple ioctls can run
	 * concurrently (read lock), but remove() takes the write lock
	 * to drain all in-flight ioctls before tearing down resources.
	 */
	down_read(&pciedev->rw_sem);

	if (pciedev->dead) {
		up_read(&pciedev->rw_sem);
		return -ENODEV;
	}

	if (READ_ONCE(pciedev->in_error_recovery)) {
		up_read(&pciedev->rw_sem);
		return -EIO;
	}

	switch (cmd) {
	case PCIE_IOCTL_READ_MMIO:
		rc = ioctl_read_mmio(pciedev, argp);
		break;
	case PCIE_IOCTL_WRITE_MMIO:
		rc = ioctl_write_mmio(pciedev, argp);
		break;
	default:
		rc = -ENOTTY;
		break;
	}

	up_read(&pciedev->rw_sem);
	return rc;
}

static const struct file_operations pcie_fops = {
	.owner		= THIS_MODULE,
	.open		= pcie_fops_open,
	.release	= pcie_fops_release,
	.unlocked_ioctl	= pcie_fops_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

/* --------------------------------------------------------------------
 * Interrupt handling (MSI-X / MSI / legacy INTx)
 *
 * These are stub handlers for testing/debugging interrupt delivery.
 * Each handler logs the vector number, Linux IRQ, and a running count.
 * Logging is rate-limited to prevent soft lockups under high IRQ rates.
 *
 * For MSI/MSI-X: returns IRQ_HANDLED (the interrupt is exclusively ours).
 *
 * For legacy INTx: returns IRQ_NONE because this generic driver has no
 * device-specific way to confirm interrupt ownership or clear the
 * interrupt source.  The kernel may disable the IRQ if the device
 * asserts it continuously.  For reliable interrupt monitoring, prefer
 * MSI or MSI-X.
 *
 * Monitor with: dmesg -w | grep -E 'pcie_access|pcie_access_drv'
 * ------------------------------------------------------------------ */

static const char *irq_type_str(struct pci_dev *pdev)
{
	if (pdev->msix_enabled)
		return "MSI-X";
	if (pdev->msi_enabled)
		return "MSI";
	return "INTx";
}

static irqreturn_t pcie_irq_handler(int irq, void *data)
{
	struct pcie_irq_ctx *ctx = data;
	struct pcie_dev *pciedev = ctx->pciedev;
	struct pci_dev *pdev = pciedev->pdev;
	unsigned int count;

	count = atomic_inc_return(&ctx->count);
	dev_info_ratelimited(&pdev->dev,
			     "%s vec[%d] irq=%d count=%u\n",
			     irq_type_str(pdev),
			     ctx->vector, irq, count);

	/*
	 * MSI/MSI-X interrupts are never shared -- they're always ours.
	 * Legacy INTx is shared and we have no device-specific way to
	 * confirm ownership or acknowledge the interrupt, so return
	 * IRQ_NONE to let other handlers on the shared line run.
	 */
	if (pdev->msi_enabled || pdev->msix_enabled)
		return IRQ_HANDLED;

	return IRQ_NONE;
}

static void pcie_teardown_irqs(struct pcie_dev *pciedev)
{
	struct pci_dev *pdev = pciedev->pdev;
	int i;

	if (!pciedev->nr_irqs)
		return;

	for (i = 0; i < pciedev->nr_irqs; i++)
		free_irq(pci_irq_vector(pdev, i), &pciedev->irq_ctx[i]);

	pci_free_irq_vectors(pdev);

	dev_info(&pdev->dev, "Freed %d IRQ vector(s)\n", pciedev->nr_irqs);

	kfree(pciedev->irq_ctx);
	pciedev->irq_ctx = NULL;
	pciedev->nr_irqs = 0;
}

static int pcie_setup_irqs(struct pcie_dev *pciedev)
{
	struct pci_dev *pdev = pciedev->pdev;
	unsigned int alloc_flags = 0;
	unsigned long irqflags;
	bool did_set_master = false;
	int max_vecs, nr_vecs, i, rc;

	if (!irq_mode)
		return 0;

	if (max_vectors < 1) {
		dev_err(&pdev->dev, "Invalid max_vectors=%d (must be >= 1)\n",
			max_vectors);
		return -EINVAL;
	}

	switch (irq_mode) {
	case 1: /* auto: try MSI-X → MSI → legacy */
		alloc_flags = PCI_IRQ_ALL_TYPES;
		max_vecs = max_vectors;
		break;
	case 2: /* MSI-X only */
		alloc_flags = PCI_IRQ_MSIX;
		max_vecs = max_vectors;
		break;
	case 3: /* MSI only (single vector) */
		alloc_flags = PCI_IRQ_MSI;
		max_vecs = 1;
		break;
	case 4: /* legacy INTx only */
		alloc_flags = PCI_IRQ_INTX;
		max_vecs = 1;
		break;
	default:
		dev_err(&pdev->dev, "Invalid irq_mode=%d\n", irq_mode);
		return -EINVAL;
	}

	/*
	 * MSI/MSI-X requires bus mastering: the device performs a DMA write
	 * (to a platform-specific MSI target address) to signal the interrupt.
	 * We also set a DMA mask as good practice.  Legacy INTx uses sideband
	 * signaling and does not need bus mastering.
	 *
	 * Track whether we enabled bus mastering so we can undo it on failure.
	 */
	if (alloc_flags & (PCI_IRQ_MSIX | PCI_IRQ_MSI)) {
		pci_set_master(pdev);
		did_set_master = true;
		rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
		if (rc) {
			rc = dma_set_mask_and_coherent(&pdev->dev,
						       DMA_BIT_MASK(32));
			if (rc) {
				dev_err(&pdev->dev,
					"Cannot set DMA mask: %d\n", rc);
				goto err_master;
			}
		}
	}

	nr_vecs = pci_alloc_irq_vectors(pdev, 1, max_vecs, alloc_flags);
	if (nr_vecs < 0) {
		dev_info(&pdev->dev,
			 "Failed to allocate IRQ vectors (mode=%d, rc=%d) "
			 "-- continuing without interrupts\n",
			 irq_mode, nr_vecs);
		rc = nr_vecs;
		goto err_master;
	}

	pciedev->irq_ctx = kcalloc(nr_vecs, sizeof(*pciedev->irq_ctx),
				   GFP_KERNEL);
	if (!pciedev->irq_ctx) {
		pci_free_irq_vectors(pdev);
		rc = -ENOMEM;
		goto err_master;
	}

	/*
	 * Legacy INTx uses a shared interrupt line -- IRQF_SHARED is required
	 * so request_irq doesn't fail when another driver already owns the
	 * IRQ.  MSI/MSI-X are never shared, so flags = 0.
	 */
	irqflags = (pdev->msi_enabled || pdev->msix_enabled) ? 0 : IRQF_SHARED;

	for (i = 0; i < nr_vecs; i++) {
		int irq = pci_irq_vector(pdev, i);

		pciedev->irq_ctx[i].pciedev = pciedev;
		pciedev->irq_ctx[i].vector = i;
		atomic_set(&pciedev->irq_ctx[i].count, 0);

		rc = request_irq(irq, pcie_irq_handler, irqflags,
				 PCIE_ACCESS_DRIVER, &pciedev->irq_ctx[i]);
		if (rc) {
			dev_err(&pdev->dev,
				"request_irq vec[%d] irq=%d failed: %d\n",
				i, irq, rc);
			while (--i >= 0)
				free_irq(pci_irq_vector(pdev, i),
					 &pciedev->irq_ctx[i]);
			kfree(pciedev->irq_ctx);
			pciedev->irq_ctx = NULL;
			pci_free_irq_vectors(pdev);
			goto err_master;
		}
	}

	pciedev->nr_irqs = nr_vecs;

	dev_info(&pdev->dev,
		 "Registered %d %s vector(s)%s -- monitor with: dmesg -w\n",
		 nr_vecs, irq_type_str(pdev),
		 irqflags ? " (shared)" : "");

	return 0;

err_master:
	if (did_set_master)
		pci_clear_master(pdev);
	return rc;
}

/* --------------------------------------------------------------------
 * PCIe AER (Advanced Error Reporting) error recovery
 *
 * These callbacks participate in the kernel's PCI error recovery flow.
 * They allow validating that a device (FPGA, endpoint, etc.) correctly
 * handles AER errors, DPC (Downstream Port Containment), and link
 * resets during hardware evaluation.
 *
 * The recovery flow is:
 *   1. error_detected()  -- error reported, I/O may be frozen
 *   2. slot_reset()      -- link has been reset (optional, if needed)
 *   3. resume()          -- device can resume normal operation
 *
 * All events are logged to dmesg for validation.
 * During recovery, ioctls return -EIO (in_error_recovery flag).
 *
 * Monitor with: dmesg -w | grep -E 'pcie_access|pcie_access_drv'
 * ------------------------------------------------------------------ */

static const char *pci_channel_state_str(pci_channel_state_t state)
{
	switch (state) {
	case pci_channel_io_normal:
		return "io_normal";
	case pci_channel_io_frozen:
		return "io_frozen";
	case pci_channel_io_perm_failure:
		return "io_perm_failure";
	default:
		return "unknown";
	}
}

/*
 * error_detected -- called when the PCI core detects an AER error.
 *
 * state:
 *   pci_channel_io_normal     -- correctable error, device still works
 *   pci_channel_io_frozen     -- non-fatal uncorrectable, I/O suspended
 *   pci_channel_io_perm_failure -- fatal, device is gone
 */
static pci_ers_result_t pcie_err_detected(struct pci_dev *pdev,
					  pci_channel_state_t state)
{
	struct pcie_dev *pciedev = pci_get_drvdata(pdev);

	dev_err(&pdev->dev,
		"AER: error_detected -- state=%s (%d)\n",
		pci_channel_state_str(state), state);

	switch (state) {
	case pci_channel_io_normal:
		/* Correctable error -- no action needed, just log */
		return PCI_ERS_RESULT_CAN_RECOVER;

	case pci_channel_io_frozen:
		/*
		 * Non-fatal uncorrectable error -- I/O is frozen.
		 * Block ioctls and request a slot reset to recover.
		 */
		if (pciedev)
			WRITE_ONCE(pciedev->in_error_recovery, true);
		return PCI_ERS_RESULT_NEED_RESET;

	case pci_channel_io_perm_failure:
		/*
		 * Fatal error -- device is permanently broken.
		 * Mark as dead so ioctls return -ENODEV.
		 */
		if (pciedev) {
			down_write(&pciedev->rw_sem);
			pciedev->dead = true;
			up_write(&pciedev->rw_sem);
		}
		return PCI_ERS_RESULT_DISCONNECT;

	default:
		return PCI_ERS_RESULT_DISCONNECT;
	}
}

/*
 * slot_reset -- called after the PCIe link has been reset.
 *
 * This is called after a secondary bus reset (SBR) or function-level
 * reset (FLR).  The device should be re-initialized.  For this generic
 * driver, we just re-enable the device and log the event.
 */
static pci_ers_result_t pcie_err_slot_reset(struct pci_dev *pdev)
{
	int rc;

	dev_info(&pdev->dev, "AER: slot_reset -- re-enabling device\n");

	/* Restore config space first, then re-enable based on restored state */
	pci_restore_state(pdev);

	rc = pci_reenable_device(pdev);
	if (rc) {
		dev_err(&pdev->dev,
			"AER: pci_reenable_device failed: %d\n", rc);
		return PCI_ERS_RESULT_DISCONNECT;
	}

	if (set_master)
		pci_set_master(pdev);

	return PCI_ERS_RESULT_RECOVERED;
}

/*
 * resume -- called when the device can resume normal operation after
 * successful error recovery.
 */
static void pcie_err_resume(struct pci_dev *pdev)
{
	struct pcie_dev *pciedev = pci_get_drvdata(pdev);

	dev_info(&pdev->dev, "AER: resume -- device recovered\n");

	if (pciedev)
		WRITE_ONCE(pciedev->in_error_recovery, false);
}

static const struct pci_error_handlers pcie_err_handlers = {
	.error_detected	= pcie_err_detected,
	.slot_reset	= pcie_err_slot_reset,
	.resume		= pcie_err_resume,
};

/* --------------------------------------------------------------------
 * PCI initialisation & BAR mapping
 * ------------------------------------------------------------------ */

static int pcie_init_pci(struct pcie_dev *pciedev, struct pci_dev *pdev)
{
	int rc, i, mapped = 0;

	rc = pcim_enable_device(pdev);
	if (rc) {
		dev_err(&pdev->dev, "Cannot enable device: %d\n", rc);
		return rc;
	}

	/* Bus mastering can be forced via param, or is auto-enabled for MSI */
	if (set_master)
		pci_set_master(pdev);

	/*
	 * Map all memory BARs.  Note: 64-bit BARs consume two consecutive
	 * BAR indices (e.g., BAR0+BAR1).  pci_resource_len() returns the
	 * full size for the first index and 0 for the second, so the loop
	 * naturally skips the "shadow" half of 64-bit BARs.
	 */
	for (i = 0; i < PCIE_NR_BARS; ++i) {
		unsigned long res_len;

		res_len = pci_resource_len(pdev, i);
		if (!res_len)
			continue;

		if (!(pci_resource_flags(pdev, i) & IORESOURCE_MEM)) {
			dev_info(&pdev->dev,
				 "BAR[%d] is I/O -- skipping (len:0x%lx)\n",
				 i, res_len);
			continue;
		}

		/* Claim the region before mapping -- provides exclusion */
		rc = pci_request_region(pdev, i, KBUILD_MODNAME);
		if (rc) {
			dev_err(&pdev->dev,
				"Cannot request BAR[%d] region (len:0x%lx): %d\n",
				i, res_len, rc);
			continue;
		}

		pciedev->bars[i].mmio = pcim_iomap(pdev, i, 0);
		if (!pciedev->bars[i].mmio) {
			pci_release_region(pdev, i);
			dev_err(&pdev->dev,
				"Cannot map BAR[%d] (len:0x%lx)\n",
				i, res_len);
			continue;
		}

		pciedev->bars[i].len = res_len;
		mapped++;

		dev_info(&pdev->dev,
			 "BAR[%d] mapped: start:0x%llx len:0x%lx\n",
			 i, (u64)pci_resource_start(pdev, i), res_len);
	}

	if (!mapped) {
		dev_err(&pdev->dev, "No memory BARs could be mapped\n");
		return -ENODEV;
	}

	pci_set_drvdata(pdev, pciedev);

	return 0;
}

/* --------------------------------------------------------------------
 * Probe / Remove
 * ------------------------------------------------------------------ */

static int pcie_driver_probe(struct pci_dev *pdev,
			     const struct pci_device_id *ent)
{
	struct pcie_dev *pciedev;
	int rc;

	dev_info(&pdev->dev, "Probing device %04x:%04x\n",
		 pdev->vendor, pdev->device);

	pciedev = kzalloc_node(sizeof(*pciedev), GFP_KERNEL,
			       dev_to_node(&pdev->dev));
	if (!pciedev)
		return -ENOMEM;

	pciedev->pdev = pdev;
	kref_init(&pciedev->refcount);
	init_rwsem(&pciedev->rw_sem);

	rc = pcie_init_pci(pciedev, pdev);
	if (rc)
		goto err_free;

	/* Set up interrupt handlers if requested */
	rc = pcie_setup_irqs(pciedev);
	if (rc && rc != -ENOSPC) {
		/* Non-fatal: log and continue without interrupts */
		dev_info(&pdev->dev,
			 "Continuing without interrupts (rc=%d)\n", rc);
	}

	/* Register misc device */
	snprintf(pciedev->misc_name, sizeof(pciedev->misc_name),
		 "pcie_ctrl-%04x:%02x:%02x.%x",
		 pci_domain_nr(pdev->bus),
		 pdev->bus->number,
		 PCI_SLOT(pdev->devfn),
		 PCI_FUNC(pdev->devfn));

	pciedev->misc.minor = MISC_DYNAMIC_MINOR;
	pciedev->misc.name = pciedev->misc_name;
	pciedev->misc.fops = &pcie_fops;

	rc = misc_register(&pciedev->misc);
	if (rc) {
		dev_err(&pdev->dev, "misc_register failed: %d\n", rc);
		goto err_irqs;
	}

	/*
	 * Save the "known good" PCI config state after ALL setup is complete
	 * (BARs mapped, bus mastering set, IRQs configured).  This is what
	 * pci_restore_state() restores during AER error recovery.
	 */
	pci_save_state(pdev);

	dev_info(&pdev->dev, "Registered as /dev/%s\n", pciedev->misc_name);

	return 0;

err_irqs:
	pcie_teardown_irqs(pciedev);
err_free:
	kfree(pciedev);
	return rc;
}

static void pcie_driver_remove(struct pci_dev *pdev)
{
	struct pcie_dev *pciedev = pci_get_drvdata(pdev);
	int i;

	/*
	 * Mark the device as dead under the write lock.  This waits for
	 * all in-flight ioctls (which hold the read lock) to complete,
	 * then prevents any new ioctls from proceeding.
	 */
	down_write(&pciedev->rw_sem);
	pciedev->dead = true;
	up_write(&pciedev->rw_sem);

	/* New opens are blocked after misc_deregister */
	misc_deregister(&pciedev->misc);
	pcie_teardown_irqs(pciedev);

	/* Release BAR regions claimed during probe */
	for (i = 0; i < PCIE_NR_BARS; i++) {
		if (pciedev->bars[i].mmio)
			pci_release_region(pdev, i);
	}

	pci_set_drvdata(pdev, NULL);

	dev_info(&pdev->dev, "Unregistered /dev/%s\n", pciedev->misc_name);

	/*
	 * Drop the "device exists" reference.  If userspace still has
	 * open file descriptors, pciedev will be freed when the last
	 * fd is closed (via kref_put in release).
	 */
	kref_put(&pciedev->refcount, pcie_dev_free);
}

static struct pci_driver pcie_access_pci_driver = {
	.name		= PCIE_ACCESS_DRIVER,
	.id_table	= pcie_id_table,
	.probe		= pcie_driver_probe,
	.remove		= pcie_driver_remove,
	.err_handler	= &pcie_err_handlers,
	/*
	 * Note: no .suspend/.resume callbacks.  After system suspend (S3/S4),
	 * MMIO mappings are stale and ioctls will read garbage or fault.
	 * This is acceptable for a lab/debug tool -- reboot or rmmod/insmod
	 * to re-initialize after resume.
	 */
};

/* --------------------------------------------------------------------
 * Module init / exit
 * ------------------------------------------------------------------ */

static int __init pcie_driver_init(void)
{
	int rc;

	if (vid && device_id) {
		pcie_id_table[0].vendor = vid;
		pcie_id_table[0].device = device_id;
		pcie_id_table[0].subvendor = PCI_ANY_ID;
		pcie_id_table[0].subdevice = PCI_ANY_ID;
		pr_info(KBUILD_MODNAME
			": targeting PCI device %04x:%04x\n", vid, device_id);
	} else {
		pr_info(KBUILD_MODNAME
			": no vid/device_id specified -- use sysfs new_id to bind:\n"
			"  echo \"VVVV DDDD\" > /sys/bus/pci/drivers/"
			PCIE_ACCESS_DRIVER "/new_id\n");
	}

	rc = pci_register_driver(&pcie_access_pci_driver);
	if (rc)
		return rc;

	pr_info(KBUILD_MODNAME ": loaded.\n");
	return 0;
}

static void __exit pcie_driver_exit(void)
{
	pci_unregister_driver(&pcie_access_pci_driver);
	pr_info(KBUILD_MODNAME ": unloaded.\n");
}

module_init(pcie_driver_init);
module_exit(pcie_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Maciej Grochowski <maciej.grochowski@pm.me>");
MODULE_DESCRIPTION("Generic PCI stub driver for userspace BAR access via ioctl");
