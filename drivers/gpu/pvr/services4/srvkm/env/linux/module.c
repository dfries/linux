/**********************************************************************
 *
 * Copyright(c) 2008 Imagination Technologies Ltd. All rights reserved.
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope it will be useful but, except 
 * as otherwise stated in writing, without any warranty; without even the 
 * implied warranty of merchantability or fitness for a particular purpose. 
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 * 
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * Contact Information:
 * Imagination Technologies Ltd. <gpl-support@imgtec.com>
 * Home Park Estate, Kings Langley, Herts, WD4 8LZ, UK 
 *
 ******************************************************************************/

#ifndef AUTOCONF_INCLUDED
 #include <linux/config.h>
#endif

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>

#if defined(LDM_PLATFORM)
#include <linux/platform_device.h>
#endif 

#if defined(LDM_PCI)
#include <linux/pci.h>
#endif 

#if defined(DEBUG) && defined(PVR_MANUAL_POWER_CONTROL)
#include <asm/uaccess.h>
#endif

#include "img_defs.h"
#include "services.h"
#include "kerneldisplay.h"
#include "kernelbuffer.h"
#include "syscommon.h"
#include "pvrmmap.h"
#include "mm.h"
#include "mmap.h"
#include "mutex.h"
#include "pvr_debug.h"
#include "srvkm.h"
#include "perproc.h"
#include "handle.h"
#include "pvr_bridge_km.h"
#include "proc.h"
#include "pvrmodule.h"

#define DRVNAME		"pvrsrvkm"
#define DEVNAME		"pvrsrvkm"


MODULE_SUPPORTED_DEVICE(DEVNAME);
#ifdef DEBUG
static int debug = DBGPRIV_WARNING;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0))
#include <linux/moduleparam.h>
module_param(debug, int, 0);
#else
MODULE_PARM(debug, "i");
MODULE_PARM_DESC(debug, "Sets the level of debug output (default=0x4)");
#endif
#endif


void PVRDebugSetLevel(IMG_UINT32 uDebugLevel);

extern IMG_BOOL PVRGetDisplayClassJTable(PVRSRV_DC_DISP2SRV_KMJTABLE *psJTable);
extern IMG_BOOL PVRGetBufferClassJTable(PVRSRV_BC_BUFFER2SRV_KMJTABLE *psJTable);
EXPORT_SYMBOL(PVRGetDisplayClassJTable);
EXPORT_SYMBOL(PVRGetBufferClassJTable);


static int AssignedMajorNumber;


extern long PVRSRV_BridgeDispatchKM(struct file *file, unsigned int cmd, unsigned long arg);
static int PVRSRVOpen(struct inode* pInode, struct file* pFile);
static int PVRSRVRelease(struct inode* pInode, struct file* pFile);

PVRSRV_LINUX_MUTEX gPVRSRVLock;

static struct file_operations pvrsrv_fops = {
	owner:THIS_MODULE,
	unlocked_ioctl:PVRSRV_BridgeDispatchKM,
	open:PVRSRVOpen,
	release:PVRSRVRelease,
	mmap:PVRMMap,
};


#if defined(DEBUG) && defined(PVR_MANUAL_POWER_CONTROL)
static IMG_UINT32 gPVRPowerLevel;
#endif

#if defined(LDM_PLATFORM) || defined(LDM_PCI)

#if defined(LDM_PLATFORM)
#define	LDM_DEV	struct platform_device
#define	LDM_DRV	struct platform_driver
#if defined(LDM_PCI)
#undef	LDM_PCI
#endif 
#endif 

#if defined(LDM_PCI)
#define	LDM_DEV	struct pci_dev
#define	LDM_DRV	struct pci_driver
#endif 

#if defined(LDM_PLATFORM)
static int PVRSRVDriverRemove(LDM_DEV *device);
static int PVRSRVDriverProbe(LDM_DEV *device);
#endif
#if defined(LDM_PCI)
static void PVRSRVDriverRemove(LDM_DEV *device);
static int PVRSRVDriverProbe(LDM_DEV *device, const struct pci_device_id *id);
#endif
static int PVRSRVDriverSuspend(LDM_DEV *device, pm_message_t state);
static void PVRSRVDriverShutdown(LDM_DEV *device);
static int PVRSRVDriverResume(LDM_DEV *device);

#if defined(LDM_PCI)
struct pci_device_id powervr_id_table[] __devinitdata = {
	{ PCI_DEVICE(SYS_SGX_DEV_VENDOR_ID, SYS_SGX_DEV_DEVICE_ID) },
	{ 0 }
};

MODULE_DEVICE_TABLE(pci, powervr_id_table);
#endif

static LDM_DRV powervr_driver = {
#if defined(LDM_PLATFORM)
	.driver = {
		.name		= DRVNAME,
	},
#endif
#if defined(LDM_PCI)
	.name		= DRVNAME,
	.id_table = powervr_id_table,
#endif
	.probe		= PVRSRVDriverProbe,
#if defined(LDM_PLATFORM)
	.remove		= PVRSRVDriverRemove,
#endif
#if defined(LDM_PCI)
	.remove		= __devexit_p(PVRSRVDriverRemove),
#endif
	.suspend	= PVRSRVDriverSuspend,
	.resume		= PVRSRVDriverResume,
	.shutdown	= PVRSRVDriverShutdown,
};

LDM_DEV *gpsPVRLDMDev;

#if defined(LDM_PLATFORM)
static void PVRSRVDeviceRelease(struct device *device);

static struct platform_device powervr_device = {
	.name			= DEVNAME,
	.id				= -1,
	.dev 			= {
		.release		= PVRSRVDeviceRelease
	}
};
#endif 

#if defined(LDM_PLATFORM)
static int PVRSRVDriverProbe(LDM_DEV *pDevice)
#endif
#if defined(LDM_PCI)
static int __devinit PVRSRVDriverProbe(LDM_DEV *pDevice, const struct pci_device_id *id)
#endif
{
	SYS_DATA *psSysData;

	PVR_TRACE(("PVRSRVDriverProbe(pDevice=%p)", pDevice));

	pDevice->dev.driver_data = NULL;

#if 0
	
	if (PerDeviceSysInitialise((IMG_PVOID)pDevice) != PVRSRV_OK)
	{
		return -EINVAL;
	}
#endif	
	
	if (SysAcquireData(&psSysData) != PVRSRV_OK)
	{
		gpsPVRLDMDev = pDevice;

		if (SysInitialise() != PVRSRV_OK)
		{
			return -ENODEV;
		}
	}

	return 0;
}


#if defined (LDM_PLATFORM)
static int PVRSRVDriverRemove(LDM_DEV *pDevice)
#endif
#if defined(LDM_PCI)
static void __devexit PVRSRVDriverRemove(LDM_DEV *pDevice)
#endif
{
	SYS_DATA *psSysData;

	PVR_TRACE(("PVRSRVDriverRemove(pDevice=%p)", pDevice));

	if (SysAcquireData(&psSysData) == PVRSRV_OK)
	{
#if defined(DEBUG) && defined(PVR_MANUAL_POWER_CONTROL)
		if (gPVRPowerLevel != 0)
		{
			if (PVRSRVSetPowerStateKM(PVRSRV_POWER_STATE_D0) == PVRSRV_OK)
			{
				gPVRPowerLevel = 0;
			}
		}
#endif
		SysDeinitialise(psSysData);

		gpsPVRLDMDev = IMG_NULL;
	}

#if 0
	if (PerDeviceSysDeInitialise((IMG_PVOID)pDevice) != PVRSRV_OK)
	{
		return -EINVAL;
	}
#endif

#if defined (LDM_PLATFORM)
	return 0;
#endif
#if defined (LDM_PCI)
	return;
#endif
}


static void PVRSRVDriverShutdown(LDM_DEV *pDevice)
{
	PVR_TRACE(("PVRSRVDriverShutdown(pDevice=%p)", pDevice));

	(void) PVRSRVSetPowerStateKM(PVRSRV_POWER_STATE_D3);
}


static int PVRSRVDriverSuspend(LDM_DEV *pDevice, pm_message_t state)
{
#if !(defined(DEBUG) && defined(PVR_MANUAL_POWER_CONTROL))
	PVR_TRACE(( "PVRSRVDriverSuspend(pDevice=%p)", pDevice));

	if (PVRSRVSetPowerStateKM(PVRSRV_POWER_STATE_D3) != PVRSRV_OK)
	{
		return -EINVAL;
	}
#endif
	return 0;
}


static int PVRSRVDriverResume(LDM_DEV *pDevice)
{
#if !(defined(DEBUG) && defined(PVR_MANUAL_POWER_CONTROL))
	PVR_TRACE(("PVRSRVDriverResume(pDevice=%p)", pDevice));

	if (PVRSRVSetPowerStateKM(PVRSRV_POWER_STATE_D0) != PVRSRV_OK)
	{
		return -EINVAL;
	}
#endif
	return 0;
}


#if defined(LDM_PLATFORM)
static void PVRSRVDeviceRelease(struct device *pDevice)
{
	PVR_DPF((PVR_DBG_WARNING, "PVRSRVDeviceRelease(pDevice=%p)", pDevice));
}
#endif 
#endif 


#if defined(DEBUG) && defined(PVR_MANUAL_POWER_CONTROL)
int PVRProcSetPowerLevel(struct file *file, const char *buffer, unsigned long count, void *data)
{
	char data_buffer[2];
	IMG_UINT32 PVRPowerLevel;

	if (count != sizeof(data_buffer))
	{
		return -EINVAL;
	}
	else
	{
		if (copy_from_user(data_buffer, buffer, count))
			return -EINVAL;
		if (data_buffer[count - 1] != '\n')
			return -EINVAL;
		PVRPowerLevel = data_buffer[0] - '0';
		if (PVRPowerLevel != gPVRPowerLevel)
		{
			if (PVRPowerLevel != 0)
			{
				if (PVRSRVSetPowerStateKM(PVRSRV_POWER_STATE_D3) != PVRSRV_OK)
				{
					return -EINVAL;
				}
			}
			else
			{
				if (PVRSRVSetPowerStateKM(PVRSRV_POWER_STATE_D0) != PVRSRV_OK)
				{
					return -EINVAL;
				}
			}

			gPVRPowerLevel = PVRPowerLevel;
		}
	}
	return (count);
}

int PVRProcGetPowerLevel(char *page, char **start, off_t off, int count, int *eof, void *data)
{
	if (off == 0) {
		*start = (char *)1;
		return printAppend(page, count, 0, "%lu\n", gPVRPowerLevel);
	}
	*eof = 1;
	return 0;
}
#endif

static int PVRSRVOpen(struct inode unref__ * pInode, struct file unref__ * pFile)
{
	int Ret = 0;

	LinuxLockMutex(&gPVRSRVLock);

	if (PVRSRVProcessConnect(OSGetCurrentProcessIDKM()) != PVRSRV_OK)
	{
		Ret = -ENOMEM;
	}
	
	LinuxUnLockMutex(&gPVRSRVLock);

	return Ret;
}


static int PVRSRVRelease(struct inode unref__ * pInode, struct file unref__ * pFile)
{
	int Ret = 0;
	
	LinuxLockMutex(&gPVRSRVLock);
	
	PVRSRVProcessDisconnect(OSGetCurrentProcessIDKM());
	
	LinuxUnLockMutex(&gPVRSRVLock);
	
	return Ret;
}


static int __init PVRCore_Init(void)
{
	int error;
#if !(defined(LDM_PLATFORM) || defined(LDM_PCI))
	PVRSRV_ERROR eError;
#endif

	PVR_TRACE(("PVRCore_Init"));

	
	AssignedMajorNumber = register_chrdev(0, DEVNAME, &pvrsrv_fops);

	if (AssignedMajorNumber <= 0)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRCore_Init: unable to get major number"));

		return -EBUSY;
	}

	PVR_TRACE(("PVRCore_Init: major device %d", AssignedMajorNumber));

	
	if (CreateProcEntries ())
	{
		unregister_chrdev(AssignedMajorNumber, DRVNAME);

		return -ENOMEM;
	}

    LinuxInitMutex(&gPVRSRVLock);

#ifdef DEBUG
	PVRDebugSetLevel(debug);
#endif

	if(LinuxMMInit() != PVRSRV_OK)
    {
        error = -ENOMEM;
        goto init_failed;
    }

	LinuxBridgeInit();

	PVRMMapInit();

#if defined(LDM_PLATFORM) || defined(LDM_PCI)

#if defined(LDM_PLATFORM)
	if ((error = platform_driver_register(&powervr_driver)) != 0)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRCore_Init: unable to register platform driver (%d)", error));

		goto init_failed;
	}

	powervr_device.dev.devt = MKDEV(AssignedMajorNumber, 0);

	if ((error = platform_device_register(&powervr_device)) != 0)
	{
		platform_driver_unregister(&powervr_driver);

		PVR_DPF((PVR_DBG_ERROR, "PVRCore_Init: unable to register platform device (%d)", error));

		goto init_failed;
	}
#endif 

#if defined(LDM_PCI)
	if ((error = pci_register_driver(&powervr_driver)) != 0)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRCore_Init: unable to register PCI driver (%d)", error));

		goto init_failed;
	}
#endif 

#else 
	
	if ((eError = SysInitialise()) != PVRSRV_OK)
	{
		error = -ENODEV;
#if defined(TCF_REV) && (TCF_REV == 110)
		if(eError == PVRSRV_ERROR_NOT_SUPPORTED)
		{
			printk("\nAtlas wrapper (FPGA image) version mismatch");
			error = -ENODEV;
		}
#endif
		goto init_failed;
	}
#endif 

	return 0;

init_failed:

	PVRMMapCleanup();
	LinuxMMCleanup();
	RemoveProcEntries();
	unregister_chrdev(AssignedMajorNumber, DRVNAME);

	return error;

} 


static void __exit PVRCore_Cleanup(void)
{
	SYS_DATA *psSysData;

	PVR_TRACE(("PVRCore_Cleanup"));

	SysAcquireData(&psSysData);
	
#if (LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,22))
	if (
#endif	
		unregister_chrdev(AssignedMajorNumber, DRVNAME)
#if !(LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,22))
								;
#else	
								)
	{
		PVR_DPF((PVR_DBG_ERROR," can't unregister device major %d", AssignedMajorNumber));
	}
#endif	

#if defined(LDM_PLATFORM) || defined(LDM_PCI)

#if defined(LDM_PCI)
	pci_unregister_driver(&powervr_driver);
#endif

#if defined (LDM_PLATFORM)
	platform_device_unregister(&powervr_device);
	platform_driver_unregister(&powervr_driver);
#endif

#else 
#if defined(DEBUG) && defined(PVR_MANUAL_POWER_CONTROL)
	if (gPVRPowerLevel != 0)
	{
		if (PVRSRVSetPowerStateKM(PVRSRV_POWER_STATE_D0) == PVRSRV_OK)
		{
			gPVRPowerLevel = 0;
		}
	}
#endif
	
	SysDeinitialise(psSysData);
#endif 

	PVRMMapCleanup();

	LinuxMMCleanup();

	LinuxBridgeDeInit();

	RemoveProcEntries();

	PVR_TRACE(("PVRCore_Cleanup: unloading"));
}

module_init(PVRCore_Init);
module_exit(PVRCore_Cleanup);

