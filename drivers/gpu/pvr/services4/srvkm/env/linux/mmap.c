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

#include <linux/version.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0))
#include <linux/wrapper.h>
#endif
#include <linux/slab.h>
#include <asm/io.h>
#include <asm/page.h>
#include <asm/shmparam.h>
#include <asm/pgtable.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,22))
#include <linux/sched.h>
#include <asm/current.h>
#endif
#include "img_defs.h"
#include "services.h"
#include "servicesint.h"
#include "pvrmmap.h"
#include "mmap.h"
#include "mm.h"
#include "pvr_debug.h"
#include "osfunc.h"
#include "proc.h"
#include "mutex.h"


static PKV_OFFSET_STRUCT FindOffsetStructFromLinuxMemArea(LinuxMemArea *psLinuxMemArea);
static IMG_UINT32 GetFirstFreePageAlignedNumber(void);
static PKV_OFFSET_STRUCT FindOffsetStructByKVIndexAddress(IMG_VOID *pvVirtAddress,
                                                  IMG_UINT32 ui32ByteSize);
static void DeterminUsersSizeAndByteOffset(IMG_VOID *pvKVIndexAddress,
                                            LinuxMemArea *psLinuxMemArea,
                                            IMG_UINT32 *pui32RealByteSize,
                                            IMG_UINT32 *pui32ByteOffset);
static PKV_OFFSET_STRUCT FindOffsetStructByMMapOffset(IMG_UINT32 ui32Offset);
static IMG_BOOL DoMapToUser(LinuxMemArea *psLinuxMemArea,
                            struct vm_area_struct* ps_vma,
                            IMG_UINT32 ui32ByteOffset,
                            IMG_UINT32 ui32Size);

#if defined(DEBUG_LINUX_MMAP_AREAS)
static off_t PrintMMapRegistrations(char * buffer, size_t size, off_t off);
#endif


static void MMapVOpen(struct vm_area_struct* ps_vma);
static void MMapVClose(struct vm_area_struct* ps_vma);

static struct vm_operations_struct MMapIOOps =
{
	open:		MMapVOpen,
	close:		MMapVClose
};


extern PVRSRV_LINUX_MUTEX gPVRSRVLock;

static PKV_OFFSET_STRUCT g_psKVOffsetTable = 0;
static LinuxKMemCache *g_psMemmapCache = 0;
#if defined(DEBUG_LINUX_MMAP_AREAS)
static IMG_UINT32 g_ui32RegisteredAreas = 0;
static IMG_UINT32 g_ui32TotalByteSize = 0;
#endif



IMG_VOID
PVRMMapInit(IMG_VOID)
{
    g_psKVOffsetTable = 0;

    g_psMemmapCache = KMemCacheCreateWrapper("img-mmap", sizeof(KV_OFFSET_STRUCT), 0, 0);
    if (g_psMemmapCache)
    {
#if defined(DEBUG_LINUX_MMAP_AREAS)
        CreateProcReadEntry("mmap", PrintMMapRegistrations);
#endif
    }
    else
    {
        PVR_DPF((PVR_DBG_ERROR,"%s: failed to allocate kmem_cache", __FUNCTION__));
    }
}


IMG_VOID
PVRMMapCleanup(void)
{
    PKV_OFFSET_STRUCT psOffsetStruct;

    if(!g_psMemmapCache)
        return;
    
    if(g_psKVOffsetTable)
    {
        PVR_DPF((PVR_DBG_ERROR, "%s: BUG! g_psMemmapCache isn't empty!",
                __FUNCTION__));
        
        for(psOffsetStruct=g_psKVOffsetTable; psOffsetStruct; psOffsetStruct = psOffsetStruct->psNext)
        {
            PVR_DPF((PVR_DBG_ERROR, "%s: BUG!: Un-registering mmapable area: psLinuxMemArea=0x%p, CpuPAddr=0x%08lx\n",
                    __FUNCTION__,
                    psOffsetStruct->psLinuxMemArea,
                    LinuxMemAreaToCpuPAddr(psOffsetStruct->psLinuxMemArea, 0).uiAddr
                    ));
			PVRMMapRemoveRegisteredArea(psOffsetStruct->psLinuxMemArea);
        }
    }
    
    RemoveProcEntry("mmap");
    KMemCacheDestroyWrapper(g_psMemmapCache);
    g_psMemmapCache = NULL;
    PVR_DPF((PVR_DBG_MESSAGE,"PVRMMapCleanup: KVOffsetTable deallocated"));
}


PVRSRV_ERROR
PVRMMapRegisterArea(const IMG_CHAR *pszName,
                    LinuxMemArea *psLinuxMemArea,
                    IMG_UINT32 ui32AllocFlags)
{
    PKV_OFFSET_STRUCT psOffsetStruct;
    
    PVR_DPF((PVR_DBG_MESSAGE,
             "%s(%s, psLinuxMemArea=%p, ui32AllocFlags=0x%8lx)",
             __FUNCTION__, pszName, psLinuxMemArea, ui32AllocFlags));

    
    psOffsetStruct = FindOffsetStructFromLinuxMemArea(psLinuxMemArea);
    if(psOffsetStruct)
    {
        PVR_DPF((PVR_DBG_ERROR, "PVRMMapRegisterArea: psLinuxMemArea=%p is already registered",
                psOffsetStruct->psLinuxMemArea));
        return PVRSRV_ERROR_INVALID_PARAMS;
    }

    psOffsetStruct = KMemCacheAllocWrapper(g_psMemmapCache, GFP_KERNEL);
    if(!psOffsetStruct)
    {
        PVR_DPF((PVR_DBG_ERROR,"PVRMMapRegisterArea: Couldn't alloc another mapping record from cache"));
        return PVRSRV_ERROR_OUT_OF_MEMORY;
    }
    
    
    psOffsetStruct->ui32MMapOffset = GetFirstFreePageAlignedNumber();
    psOffsetStruct->psLinuxMemArea = psLinuxMemArea;
    
    if(psLinuxMemArea->eAreaType != LINUX_MEM_AREA_SUB_ALLOC)
    {
        psOffsetStruct->ui32AllocFlags = ui32AllocFlags;
    }
    else
    {
        PKV_OFFSET_STRUCT psParentOffsetStruct;
        psParentOffsetStruct = 
            FindOffsetStructFromLinuxMemArea(psLinuxMemArea->uData.sSubAlloc.psParentLinuxMemArea);
        PVR_ASSERT(psParentOffsetStruct);
        psOffsetStruct->ui32AllocFlags = psParentOffsetStruct->ui32AllocFlags;
    }

#if defined(DEBUG_LINUX_MMAP_AREAS)
    
    psOffsetStruct->pszName					= pszName;
    psOffsetStruct->pid						= current->pid;
    psOffsetStruct->ui16Mapped				= 0;
    psOffsetStruct->ui16Faults				= 0;

    g_ui32RegisteredAreas++;
    if(psLinuxMemArea->eAreaType != LINUX_MEM_AREA_SUB_ALLOC)
    {
        g_ui32TotalByteSize+=psLinuxMemArea->ui32ByteSize;
    }
#endif
    
	
    psOffsetStruct->psNext					= g_psKVOffsetTable;
    
    g_psKVOffsetTable                         = psOffsetStruct;

    return PVRSRV_OK;
}


PVRSRV_ERROR
PVRMMapRemoveRegisteredArea(LinuxMemArea *psLinuxMemArea)
{
    PKV_OFFSET_STRUCT *ppsOffsetStruct, psOffsetStruct;
    
    for(ppsOffsetStruct=&g_psKVOffsetTable;
        (psOffsetStruct = *ppsOffsetStruct);
        ppsOffsetStruct=&(*ppsOffsetStruct)->psNext)
    {
        if(psOffsetStruct->psLinuxMemArea == psLinuxMemArea)
        {
            break;
        }
    }

    if(!psOffsetStruct)
    {
        PVR_DPF((PVR_DBG_ERROR, "%s: Registration for psLinuxMemArea = 0x%p not found",
                __FUNCTION__,
                psLinuxMemArea));
        return PVRSRV_ERROR_BAD_MAPPING;
    }

#if defined(DEBUG_LINUX_MMAP_AREAS)
    
    if(psOffsetStruct->ui16Mapped)
    {
        PVR_DPF((PVR_DBG_ERROR, "%s: Unregistering still-mapped area! (psLinuxMemArea=0x%p)\n",
                __FUNCTION__, psOffsetStruct->psLinuxMemArea));
        return PVRSRV_ERROR_BAD_MAPPING;
    }

    g_ui32RegisteredAreas--;

    if(psLinuxMemArea->eAreaType != LINUX_MEM_AREA_SUB_ALLOC)
    {
        g_ui32TotalByteSize -= psOffsetStruct->psLinuxMemArea->ui32ByteSize;
    }
#endif
    
    PVR_DPF((PVR_DBG_MESSAGE, "%s: Table entry: "
             "psLinuxMemArea=0x%08lX, CpuPAddr=0x%08lX", __FUNCTION__,
             psOffsetStruct->psLinuxMemArea,
             LinuxMemAreaToCpuPAddr(psOffsetStruct->psLinuxMemArea, 0)));
    
    *ppsOffsetStruct = psOffsetStruct->psNext;
 
    KMemCacheFreeWrapper(g_psMemmapCache, psOffsetStruct);

    return PVRSRV_OK;
}



static PKV_OFFSET_STRUCT
FindOffsetStructFromLinuxMemArea(LinuxMemArea *psLinuxMemArea)
{
    PKV_OFFSET_STRUCT psOffsetStruct = NULL;
    
    for(psOffsetStruct=g_psKVOffsetTable; psOffsetStruct; psOffsetStruct=psOffsetStruct->psNext)
    {
        if(psOffsetStruct->psLinuxMemArea == psLinuxMemArea)
        {
            return psOffsetStruct;
        }
    }
    return NULL;
}



static IMG_UINT32
GetFirstFreePageAlignedNumber(void)
{
    PKV_OFFSET_STRUCT psCurrentRec;
    IMG_UINT32 ui32CurrentPageOffset;
    
    if(!g_psKVOffsetTable)
    {
        return 0;
    }

    psCurrentRec = g_psKVOffsetTable;
    ui32CurrentPageOffset = (g_psKVOffsetTable->ui32MMapOffset);

    while(psCurrentRec)
    {
        if(ui32CurrentPageOffset != (psCurrentRec->ui32MMapOffset))
        {
            return ui32CurrentPageOffset;
        }
        psCurrentRec = psCurrentRec->psNext;
        ui32CurrentPageOffset += PAGE_SIZE;
    }
    
    return g_psKVOffsetTable->ui32MMapOffset + PAGE_SIZE;
}



PVRSRV_ERROR
PVRMMapKVIndexAddressToMMapData(IMG_VOID *pvKVIndexAddress,
                                IMG_UINT32 ui32Size,
                                IMG_UINT32 *pui32MMapOffset,
                                IMG_UINT32 *pui32ByteOffset,
                                IMG_UINT32 *pui32RealByteSize)
{
    PKV_OFFSET_STRUCT psOffsetStruct;
    
    psOffsetStruct = FindOffsetStructByKVIndexAddress(pvKVIndexAddress, ui32Size);
    if (!psOffsetStruct)
    {
        return PVRSRV_ERROR_BAD_MAPPING;
    }

    *pui32MMapOffset = psOffsetStruct->ui32MMapOffset;

    DeterminUsersSizeAndByteOffset(pvKVIndexAddress,
                                   psOffsetStruct->psLinuxMemArea,
                                   pui32RealByteSize,
                                   pui32ByteOffset);

    return PVRSRV_OK;
}


static PKV_OFFSET_STRUCT
FindOffsetStructByKVIndexAddress(IMG_VOID *pvKVIndexAddress,
                                 IMG_UINT32 ui32ByteSize)
{
    PKV_OFFSET_STRUCT psOffsetStruct;
    IMG_UINT8 *pui8CpuVAddr;
    IMG_UINT8 *pui8IndexCpuVAddr = (IMG_UINT8 *)pvKVIndexAddress;

    for(psOffsetStruct=g_psKVOffsetTable; psOffsetStruct; psOffsetStruct=psOffsetStruct->psNext)
    {
        LinuxMemArea *psLinuxMemArea = psOffsetStruct->psLinuxMemArea;
        
		switch(psLinuxMemArea->eAreaType)
		{
			case LINUX_MEM_AREA_IOREMAP:
				pui8CpuVAddr = psLinuxMemArea->uData.sIORemap.pvIORemapCookie;
				break;
			case LINUX_MEM_AREA_VMALLOC:
				pui8CpuVAddr = psLinuxMemArea->uData.sVmalloc.pvVmallocAddress;
				break;
			case LINUX_MEM_AREA_EXTERNAL_KV:
				pui8CpuVAddr = psLinuxMemArea->uData.sExternalKV.pvExternalKV;
				break;
			default:
				pui8CpuVAddr = IMG_NULL;
				break;
		}
        
        
        if(pui8CpuVAddr)
        {
            if(pui8IndexCpuVAddr >= pui8CpuVAddr
               && (pui8IndexCpuVAddr + ui32ByteSize) <= (pui8CpuVAddr + psLinuxMemArea->ui32ByteSize))
            {
                return psOffsetStruct;
            }
            else
            {
                pui8CpuVAddr = NULL;
            }
        }
        
        if(pvKVIndexAddress == psOffsetStruct->psLinuxMemArea)
        {
            if(psLinuxMemArea->eAreaType == LINUX_MEM_AREA_SUB_ALLOC)
            {
                PVR_ASSERT(psLinuxMemArea->uData.sSubAlloc.psParentLinuxMemArea->eAreaType
                              != LINUX_MEM_AREA_SUB_ALLOC);
            }
            return psOffsetStruct;
        }
    }
    printk(KERN_ERR "%s: Failed to find offset struct (KVAddress=%p)\n", __FUNCTION__, pvKVIndexAddress);
    return NULL;
}


static void
DeterminUsersSizeAndByteOffset(IMG_VOID *pvKVIndexAddress,
                               LinuxMemArea *psLinuxMemArea,
                               IMG_UINT32 *pui32RealByteSize,
                               IMG_UINT32 *pui32ByteOffset)
{
    IMG_UINT8 *pui8StartVAddr = NULL;
    IMG_UINT8 *pui8IndexCpuVAddr = (IMG_UINT8 *)pvKVIndexAddress;
    IMG_UINT32 ui32PageAlignmentOffset=0;
    IMG_CPU_PHYADDR CpuPAddr;
    
    CpuPAddr=LinuxMemAreaToCpuPAddr(psLinuxMemArea, 0);
    ui32PageAlignmentOffset = ADDR_TO_PAGE_OFFSET(CpuPAddr.uiAddr);
    
    if(pvKVIndexAddress != psLinuxMemArea &&
       (psLinuxMemArea->eAreaType == LINUX_MEM_AREA_IOREMAP
       || psLinuxMemArea->eAreaType == LINUX_MEM_AREA_VMALLOC
       || psLinuxMemArea->eAreaType == LINUX_MEM_AREA_EXTERNAL_KV))
    {
        pui8StartVAddr = LinuxMemAreaToCpuVAddr(psLinuxMemArea);
        *pui32ByteOffset = (pui8IndexCpuVAddr - pui8StartVAddr) + ui32PageAlignmentOffset;
    }
    else
    {
        *pui32ByteOffset = ui32PageAlignmentOffset;
    }

    *pui32RealByteSize = PAGE_ALIGN(psLinuxMemArea->ui32ByteSize + ui32PageAlignmentOffset);
}


int
PVRMMap(struct file* pFile, struct vm_area_struct* ps_vma)
{
	unsigned long ulBytes;
	PKV_OFFSET_STRUCT psCurrentRec = NULL;
    int iRetVal=0;

    LinuxLockMutex(&gPVRSRVLock);
    
	ulBytes = ps_vma->vm_end - ps_vma->vm_start;
    
    PVR_DPF((PVR_DBG_MESSAGE, "%s: Recieved mmap(2) request with a ui32MMapOffset=0x%08lx,"
                              " and ui32ByteSize=%ld(0x%08lx)\n",
            __FUNCTION__,
            PFN_TO_PHYS(ps_vma->vm_pgoff),
            ulBytes, ulBytes));
   
	
    if(
       (ps_vma->vm_flags & VM_WRITE) &&
       !(ps_vma->vm_flags & VM_SHARED)
      )
    {
        PVR_DPF((PVR_DBG_ERROR,"PVRMMap: Error - Cannot mmap non-shareable writable areas."));
        iRetVal = -EINVAL;
        goto unlock_and_return;
    }
   
    psCurrentRec=FindOffsetStructByMMapOffset(PFN_TO_PHYS(ps_vma->vm_pgoff));
    if (!psCurrentRec)
    {
        PVR_DPF((PVR_DBG_ERROR,
                 "PVRMMap: Error - Attempted to mmap unregistered area at vm_pgoff=%ld",
                 ps_vma->vm_pgoff));
        iRetVal = -EINVAL;
        goto unlock_and_return;
    }
    PVR_DPF((PVR_DBG_MESSAGE, "%s: > psCurrentRec->psLinuxMemArea=%p\n",
             __FUNCTION__, psCurrentRec->psLinuxMemArea));
    
    ps_vma->vm_flags |= VM_RESERVED;
    ps_vma->vm_flags |= VM_IO;
    
    ps_vma->vm_flags |= VM_DONTEXPAND;
    
    ps_vma->vm_private_data = (void *)psCurrentRec;
    
    
    
    switch(psCurrentRec->ui32AllocFlags & PVRSRV_HAP_CACHETYPE_MASK)
    {
        case PVRSRV_HAP_CACHED:
            
            break;
        case PVRSRV_HAP_WRITECOMBINE:
#if defined(__arm__) || defined(__sh__)
            ps_vma->vm_page_prot = pgprot_writecombine(ps_vma->vm_page_prot);
#else
#if defined(__i386__)
            ps_vma->vm_page_prot = pgprot_noncached(ps_vma->vm_page_prot);

#if defined(SUPPORT_LINUX_X86_WRITECOMBINE)
            
            if(psCurrentRec->psLinuxMemArea->eAreaType == LINUX_MEM_AREA_IOREMAP
               || psCurrentRec->psLinuxMemArea->eAreaType == LINUX_MEM_AREA_IO)
            {
                ps_vma->vm_page_prot = __pgprot(pgprot_val(ps_vma->vm_page_prot) &= ~_PAGE_PWT);
            }
#endif

#else
            ps_vma->vm_page_prot = pgprot_noncached(ps_vma->vm_page_prot);
    #error  Unsupported architecture!
#endif
#endif
            break;
        case PVRSRV_HAP_UNCACHED:
            ps_vma->vm_page_prot = pgprot_noncached(ps_vma->vm_page_prot);
            break;
        default:
            PVR_DPF((PVR_DBG_ERROR, "%s: unknown cache type", __FUNCTION__));
    }
    
    
    
    ps_vma->vm_ops = &MMapIOOps;
    
    if(!DoMapToUser(psCurrentRec->psLinuxMemArea, ps_vma, 0, ulBytes))
    {
        iRetVal = -EAGAIN;
        goto unlock_and_return;
    }
    
    
    MMapVOpen(ps_vma);
    
    PVR_DPF((PVR_DBG_MESSAGE, "%s: Mapped area at offset 0x%08lx\n",
             __FUNCTION__, ps_vma->vm_pgoff));
    
unlock_and_return:
    
    LinuxUnLockMutex(&gPVRSRVLock);
    
    return iRetVal;
}


static PKV_OFFSET_STRUCT
FindOffsetStructByMMapOffset(IMG_UINT32 ui32MMapOffset)
{
    PKV_OFFSET_STRUCT psOffsetStruct;
    
    for(psOffsetStruct = g_psKVOffsetTable; psOffsetStruct; psOffsetStruct = psOffsetStruct->psNext)
    {
        if(psOffsetStruct->ui32MMapOffset == ui32MMapOffset)
        {
            return psOffsetStruct;
        }
    }
    return NULL;
}


static IMG_BOOL
DoMapToUser(LinuxMemArea *psLinuxMemArea,
            struct vm_area_struct* ps_vma,
            IMG_UINT32 ui32ByteOffset,
            IMG_UINT32 ui32ByteSize)
{
    if (psLinuxMemArea->eAreaType == LINUX_MEM_AREA_SUB_ALLOC)
    {
        return DoMapToUser(psLinuxMemArea->uData.sSubAlloc.psParentLinuxMemArea,
                    ps_vma,
                    psLinuxMemArea->uData.sSubAlloc.ui32ByteOffset + ui32ByteOffset,
                    ui32ByteSize);
    }

    PVR_ASSERT(ADDR_TO_PAGE_OFFSET(ui32ByteSize) == 0);

#if defined (__sparc__)
    
#error "SPARC not supported"
#endif

    if (LinuxMemAreaPhysIsContig(psLinuxMemArea))
    {
        
        unsigned long pfn = LinuxMemAreaToCpuPFN(psLinuxMemArea, ui32ByteOffset);

	int result= IO_REMAP_PFN_RANGE(ps_vma, ps_vma->vm_start, pfn, ui32ByteSize, ps_vma->vm_page_prot);
        if(result != 0)
        {
            PVR_DPF((PVR_DBG_ERROR, "%s: Error - Failed to map contiguous physical address range (%d)", __FUNCTION__, result));
            return IMG_FALSE;
        }
    }
    else
    {
        
        unsigned long ulVMAPos=ps_vma->vm_start;
	IMG_UINT32 ui32ByteEnd = ui32ByteOffset + ui32ByteSize;
	IMG_UINT32 ui32PA;

	for(ui32PA = ui32ByteOffset; ui32PA < ui32ByteEnd; ui32PA += PAGE_SIZE)
	{
	    unsigned long pfn =  LinuxMemAreaToCpuPFN(psLinuxMemArea, ui32PA);

	    int result = REMAP_PFN_RANGE(ps_vma, ulVMAPos, pfn, PAGE_SIZE, ps_vma->vm_page_prot);
            if(result != 0)
            {
                PVR_DPF((PVR_DBG_ERROR,"%s: Error - Failed to map discontiguous physical address range (%d)", __FUNCTION__, result));
                return IMG_FALSE;
            }
            ulVMAPos += PAGE_SIZE;
        }
    }

    return IMG_TRUE;
}


static void
MMapVOpen(struct vm_area_struct* ps_vma)
{
#if defined(DEBUG_LINUX_MMAP_AREAS)
    PKV_OFFSET_STRUCT psOffsetStruct = (PKV_OFFSET_STRUCT)ps_vma->vm_private_data;
    PVR_ASSERT(psOffsetStruct != IMG_NULL)
    psOffsetStruct->ui16Mapped++;

    PVR_DPF((PVR_DBG_MESSAGE,
             "%s: psLinuxMemArea=%p, KVAddress=%p MMapOffset=%ld, ui16Mapped=%d",
             __FUNCTION__,
             psOffsetStruct->psLinuxMemArea,
             LinuxMemAreaToCpuVAddr(psOffsetStruct->psLinuxMemArea),
             psOffsetStruct->ui32MMapOffset,
             psOffsetStruct->ui16Mapped));
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0))
    MOD_INC_USE_COUNT;
#endif
}


static void
MMapVClose(struct vm_area_struct* ps_vma)
{
#if defined(DEBUG_LINUX_MMAP_AREAS)
    PKV_OFFSET_STRUCT psOffsetStruct = (PKV_OFFSET_STRUCT)ps_vma->vm_private_data;
    PVR_ASSERT(psOffsetStruct != IMG_NULL)
    psOffsetStruct->ui16Mapped--;

    PVR_DPF((PVR_DBG_MESSAGE,
             "%s: psLinuxMemArea=%p, CpuVAddr=%p ui32MMapOffset=%ld, ui16Mapped=%d",
             __FUNCTION__,
             psOffsetStruct->psLinuxMemArea,
             LinuxMemAreaToCpuVAddr(psOffsetStruct->psLinuxMemArea),
             psOffsetStruct->ui32MMapOffset,
             psOffsetStruct->ui16Mapped));
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0))
    MOD_DEC_USE_COUNT;
#endif
}



#if defined(DEBUG_LINUX_MMAP_AREAS)
static off_t
PrintMMapRegistrations(char * buffer, size_t size, off_t off)
{
	PKV_OFFSET_STRUCT psOffsetStruct;
    off_t Ret;
	
    LinuxLockMutex(&gPVRSRVLock);

	if(!off)
    {
		Ret = printAppend(buffer, size, 0,
#if !defined(DEBUG_LINUX_XML_PROC_FILES)
						  "Allocations registered for mmap: %lu\n"
                          "In total these areas correspond to %lu bytes (excluding SUB areas)\n"
                          "psLinuxMemArea "
						  "CpuVAddr "
						  "CpuPAddr "
                          "MMapOffset "
                          "ByteLength "
                          "LinuxMemType             "
						  "Pid   Name     Mapped Flags\n",
#else
                          "<mmap_header>\n"
                          "\t<count>%lu</count>\n"
                          "\t<bytes>%lu</bytes>\n" 
                          "</mmap_header>\n",
#endif
						  g_ui32RegisteredAreas,
                          g_ui32TotalByteSize
                          );

        goto unlock_and_return;
    }

	if (size < 135) 
    {
		Ret = 0;
        goto unlock_and_return;
    }
	
	for(psOffsetStruct=g_psKVOffsetTable; --off && psOffsetStruct; psOffsetStruct=psOffsetStruct->psNext)
		;
	if(!psOffsetStruct)
    {
		Ret = END_OF_FILE;
        goto unlock_and_return;
    }

	Ret =  printAppend (buffer, size, 0,
#if !defined(DEBUG_LINUX_XML_PROC_FILES)
						"%-8p       %-8p %08lx %08lx   %-8ld   %-24s %-5d %-8s %-5u  %08lx(%s)\n",
#else
                        "<mmap_record>\n"
						"\t<pointer>%-8p</pointer>\n"
                        "\t<cpu_virtual>%-8p</cpu_virtual>\n"
                        "\t<cpu_physical>%08lx</cpu_physical>\n"
                        "\t<mmap_offset>%08lx</mmap_offset>\n"
                        "\t<bytes>%-8ld</bytes>\n"
                        "\t<linux_mem_area_type>%-24s</linux_mem_area_type>\n"
                        "\t<pid>%-5d</pid>\n"
                        "\t<name>%-8s</name>\n"
                        "\t<mapping_count>%-5u</mapping_count>\n"
                        "\t<flags>%08lx</flags>\n"
                        "\t<flags_string>%s</flags_string>\n"
                        "</mmap_record>\n",
#endif
                        psOffsetStruct->psLinuxMemArea,
						LinuxMemAreaToCpuVAddr(psOffsetStruct->psLinuxMemArea),
                        LinuxMemAreaToCpuPAddr(psOffsetStruct->psLinuxMemArea,0).uiAddr,
						psOffsetStruct->ui32MMapOffset,
						psOffsetStruct->psLinuxMemArea->ui32ByteSize,
                        LinuxMemAreaTypeToString(psOffsetStruct->psLinuxMemArea->eAreaType),
						psOffsetStruct->pid,
						psOffsetStruct->pszName,
						psOffsetStruct->ui16Mapped,
						psOffsetStruct->ui32AllocFlags,
                        HAPFlagsToString(psOffsetStruct->ui32AllocFlags));

unlock_and_return:

    LinuxUnLockMutex(&gPVRSRVLock);
    return Ret;
}
#endif


