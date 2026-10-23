/**
  sapphire: start our XNU boot shim the way the phone's ABL starts it.

  BootShim copies an "XNUP" container that follows the UEFI FD to PAYLOAD_BASE before the UEFI
  runs (the memory map reserves that range). When BDS finds nothing else to boot, this takes the
  shim (an arm64 Image with the XNU payload behind it, 4K aligned) and the phone's device tree
  out of it, leaves the boot services, cleans the caches and turns the MMU off, and enters the
  shim at EL1 with x0 = the device tree: what ABL does after "fastboot boot". The shim then loads
  XNU as it always has.

  Container: u32 'XNUP', u32 size (bytes after these 8), u32 shim offset, u32 shim size,
  u32 dtb offset, u32 dtb size (offsets from the container's start). Without it nothing happens.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define SAPPHIRE_PAYLOAD_BASE   0xA0000000ULL
#define SAPPHIRE_PAYLOAD_MAX    0x04000000ULL
#define SAPPHIRE_XNUP_MAGIC     0x50554E58U     /* "XNUP" */

typedef struct {
  UINT32  Magic;
  UINT32  Size;
  UINT32  ShimOffset;
  UINT32  ShimSize;
  UINT32  DtbOffset;
  UINT32  DtbSize;
} SAPPHIRE_XNUP;

typedef VOID (*SAPPHIRE_SHIM_ENTRY)(UINT64 Dtb);

STATIC
UINTN
SapphireCurrentEl (
  VOID
  )
{
  UINTN  El;

  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (El));
  return (El >> 2) & 3;
}

/*
  Clean and invalidate the whole data cache by set/way (this ArmLib has no such call). Dirty lines
  the UEFI left could otherwise be written back over what the shim copies with the caches off (the
  kernel's memory among it). The classic CCSIDR layout: Cortex-A73/A53 have no FEAT_CCIDX.
*/
STATIC
VOID
SapphireCleanInvalidateDcacheAll (
  VOID
  )
{
  UINT64  Clidr, Ccsidr;
  UINTN   Level, Loc;

  __asm__ volatile ("mrs %0, clidr_el1" : "=r" (Clidr));
  Loc = (Clidr >> 24) & 7;
  for (Level = 0; Level < Loc; Level++) {
    UINTN  Ctype = (Clidr >> (3 * Level)) & 7;
    UINT32 LineShift, Ways, Sets, WayShift;

    if (Ctype < 2) {
      continue;                           /* no data cache at this level */
    }
    __asm__ volatile ("msr csselr_el1, %0; isb" : : "r" ((UINT64)(Level << 1)) : "memory");
    __asm__ volatile ("mrs %0, ccsidr_el1" : "=r" (Ccsidr));
    LineShift = (UINT32)(Ccsidr & 7) + 4;
    Ways      = (UINT32)((Ccsidr >> 3) & 0x3FF);
    Sets      = (UINT32)((Ccsidr >> 13) & 0x7FFF);
    WayShift  = Ways != 0 ? (UINT32)__builtin_clz (Ways) : 0;
    for (UINT32 Way = 0; Way <= Ways; Way++) {
      for (UINT32 Set = 0; Set <= Sets; Set++) {
        UINT64  SetWay = ((UINT64)Way << WayShift) | ((UINT64)Set << LineShift) | (Level << 1);
        __asm__ volatile ("dc cisw, %0" : : "r" (SetWay) : "memory");
      }
    }
  }
  __asm__ volatile ("dsb sy; isb" : : : "memory");
}

/* Leave the boot services: the memory map key must be the latest. */
STATIC
EFI_STATUS
SapphireExitBootServices (
  VOID
  )
{
  EFI_STATUS             Status;
  EFI_MEMORY_DESCRIPTOR  *Map = NULL;
  UINTN                  MapSize = 0, MapKey, DescSize;
  UINT32                 DescVersion;

  for (UINTN Try = 0; Try < 8; Try++) {
    Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVersion);
    if (Status == EFI_BUFFER_TOO_SMALL) {
      if (Map != NULL) {
        FreePool (Map);
      }
      MapSize += 8 * DescSize;
      Map = AllocatePool (MapSize);
      if (Map == NULL) {
        return EFI_OUT_OF_RESOURCES;
      }
      continue;
    }
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Status = gBS->ExitBootServices (gImageHandle, MapKey);
    if (!EFI_ERROR (Status)) {
      return EFI_SUCCESS;
    }
  }
  return Status;
}

/**
  Boot the sapphire XNU shim from the container, if there is one. Returns only when there is
  none (or it is malformed); otherwise it never returns.
**/
VOID
EFIAPI
SapphireXnuBoot (
  VOID
  )
{
  SAPPHIRE_XNUP        *Xnup = (SAPPHIRE_XNUP *)(UINTN)SAPPHIRE_PAYLOAD_BASE;
  UINT64               Shim, Dtb;
  EFI_STATUS           Status;

  if (Xnup->Magic != SAPPHIRE_XNUP_MAGIC) {
    DEBUG ((EFI_D_WARN, "sapphire: no XNUP container at 0x%lx\n", SAPPHIRE_PAYLOAD_BASE));
    return;
  }
  if ((Xnup->Size + 8ULL) > SAPPHIRE_PAYLOAD_MAX || (Xnup->ShimOffset & 0xFFF) != 0 ||
      (Xnup->DtbOffset & 7) != 0 || (UINT64)Xnup->ShimOffset + Xnup->ShimSize > Xnup->Size + 8ULL ||
      (UINT64)Xnup->DtbOffset + Xnup->DtbSize > Xnup->Size + 8ULL)
  {
    DEBUG ((EFI_D_ERROR, "sapphire: bad XNUP container (size 0x%x shim 0x%x+0x%x dtb 0x%x+0x%x)\n",
      Xnup->Size, Xnup->ShimOffset, Xnup->ShimSize, Xnup->DtbOffset, Xnup->DtbSize));
    return;
  }

  Shim = SAPPHIRE_PAYLOAD_BASE + Xnup->ShimOffset;
  Dtb  = SAPPHIRE_PAYLOAD_BASE + Xnup->DtbOffset;
  DEBUG ((EFI_D_WARN, "sapphire: XNU shim at 0x%lx (0x%x bytes), device tree at 0x%lx (0x%x bytes), EL%u\n",
    Shim, Xnup->ShimSize, Dtb, Xnup->DtbSize, SapphireCurrentEl ()));

  Status = SapphireExitBootServices ();
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "sapphire: ExitBootServices failed: %r\n", Status));
    return;
  }

  /* As ABL leaves it for a kernel: interrupts masked, caches clean, MMU and caches off. */
  ArmDisableInterrupts ();
  ArmDisableDataCache ();
  SapphireCleanInvalidateDcacheAll ();
  ArmDisableMmu ();
  ArmDisableInstructionCache ();
  ArmInvalidateInstructionCache ();

  ((SAPPHIRE_SHIM_ENTRY)(UINTN)Shim)(Dtb);
  CpuDeadLoop ();
}
