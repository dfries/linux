/*
 * Copyright 2005 Stephane Marchesin.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "drmP.h"
#include "drm.h"
#include "nouveau_drv.h"

#include "drm_pciids.h"

static struct pci_device_id pciidlist[] = {
	{
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID),
		.class = PCI_BASE_CLASS_DISPLAY << 16,
		.class_mask  = 0xff << 16,
	},
	{
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA_SGS, PCI_ANY_ID),
		.class = PCI_BASE_CLASS_DISPLAY << 16,
		.class_mask  = 0xff << 16,
	}
};

extern struct drm_ioctl_desc nouveau_ioctls[];
extern int nouveau_max_ioctl;

static int probe(struct pci_dev *pdev, const struct pci_device_id *ent);
static struct drm_driver driver = {
	.driver_features =
		DRIVER_USE_AGP | DRIVER_PCI_DMA | DRIVER_SG |
		DRIVER_HAVE_IRQ | DRIVER_IRQ_SHARED,
	.load = nouveau_load,
	.firstopen = nouveau_firstopen,
	.lastclose = nouveau_lastclose,
	.unload = nouveau_unload,
	.preclose = nouveau_preclose,
	.irq_preinstall = nouveau_irq_preinstall,
	.irq_postinstall = nouveau_irq_postinstall,
	.irq_uninstall = nouveau_irq_uninstall,
	.irq_handler = nouveau_irq_handler,
	.reclaim_buffers = drm_core_reclaim_buffers,
	.get_map_ofs = drm_core_get_map_ofs,
	.get_reg_ofs = drm_core_get_reg_ofs,
	.ioctls = nouveau_ioctls,
	.fops = {
		.owner = THIS_MODULE,
		.open = drm_open,
		.release = drm_release,
		.ioctl = drm_ioctl,
		.mmap = drm_mmap,
		.poll = drm_poll,
		.fasync = drm_fasync,
#if defined(CONFIG_COMPAT) && LINUX_VERSION_CODE > KERNEL_VERSION(2,6,9)
		.compat_ioctl = nouveau_compat_ioctl,
#endif
	},
	.pci_driver = {
		.name = DRIVER_NAME,
		.id_table = pciidlist,
		.probe = probe,
		.remove = __devexit_p(drm_cleanup_pci),
	},

        .bo_driver = &nouveau_bo_driver,
        .fence_driver = &nouveau_fence_driver,

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
#ifdef GIT_REVISION
	.date = GIT_REVISION,
#else
	.date = DRIVER_DATE,
#endif
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,
};

static int probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	return drm_get_dev(pdev, ent, &driver);
}

static int __init nouveau_init(void)
{
	driver.num_ioctls = nouveau_max_ioctl;
	return drm_init(&driver, pciidlist);
}

static void __exit nouveau_exit(void)
{
	drm_exit(&driver);
}

module_init(nouveau_init);
module_exit(nouveau_exit);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL and additional rights");
