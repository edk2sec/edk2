/** @file
  Implements APIs to load PE/COFF debug information.

  Copyright (c) 2020 - 2021, Marvin Häuser. All rights reserved.<BR>
  Copyright (c) 2020, Vitaly Cheptsov. All rights reserved.<BR>
  Copyright (c) 2020, ISP RAS. All rights reserved.<BR>
  Portions copyright (c) 2006 - 2019, Intel Corporation. All rights reserved.<BR>
  Portions copyright (c) 2008 - 2010, Apple Inc. All rights reserved.<BR>
  Portions copyright (c) 2020, Hewlett Packard Enterprise Development LP. All rights reserved.<BR>

  SPDX-License-Identifier: BSD-3-Clause
**/

#include <Base.h>
#include <Uefi/UefiBaseType.h>

#include <IndustryStandard/PeImage2.h>

#include <Library/BaseMemoryLib.h>
#include <Library/BaseOverflowLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/PeCoffLib2.h>

#include "BasePeCoffLib2Internals.h"

RETURN_STATUS
PeCoffGetPdbPath (
  IN OUT PE_COFF_LOADER_IMAGE_CONTEXT  *Context,
  OUT    CONST CHAR8                   **PdbPath,
  OUT    UINT32                        *PdbPathSize
  )
{
  BOOLEAN  Overflow;

  CONST EFI_IMAGE_DATA_DIRECTORY  *DebugDir;
  CONST EFI_TE_IMAGE_HEADER       *TeHdr;
  CONST EFI_IMAGE_NT_HEADERS32    *Pe32Hdr;
  CONST EFI_IMAGE_NT_HEADERS64    *Pe32PlusHdr;

  CONST EFI_IMAGE_DEBUG_DIRECTORY_ENTRY  *DebugEntries;
  UINT32                                 NumDebugEntries;
  UINT32                                 DebugIndex;
  CONST EFI_IMAGE_DEBUG_DIRECTORY_ENTRY  *CodeViewEntry;

  UINT32                          DebugDirTop;
  UINT32                          DebugDirFileOffset;
  UINT32                          DebugDirSectionOffset;
  UINT32                          DebugDirSectionRawTop;
  UINT32                          DebugEntryFileOffset;
  UINT32                          DebugEntryFileOffsetTop;
  CONST EFI_IMAGE_SECTION_HEADER  *Sections;
  UINT16                          SectionIndex;

  CONST CHAR8  *CodeView;
  UINT32       PdbOffset;
  CONST CHAR8  *PdbName;
  UINT32       PdbNameSize;

  ASSERT (Context != NULL);
  ASSERT (PdbPath != NULL);
  ASSERT (PdbPathSize != NULL);

  if (!PcdGetBool (PcdImageLoaderDebugSupport)) {
    return RETURN_NOT_FOUND;
  }

  //
  // Retrieve the Debug Directory of the Image.
  //
  switch (Context->ImageType) {
    case PeCoffLoaderTypeTe:
      if (PcdGetBool (PcdImageLoaderProhibitTe)) {
        ASSERT (FALSE);
        return RETURN_UNSUPPORTED;
      }

      TeHdr = (CONST EFI_TE_IMAGE_HEADER *)(CONST VOID *)(
                                                          (CONST CHAR8 *)Context->FileBuffer
                                                          );

      DebugDir = &TeHdr->DataDirectory[1];
      break;

    case PeCoffLoaderTypePe32:
      Pe32Hdr = (CONST EFI_IMAGE_NT_HEADERS32 *)(CONST VOID *)(
                                                               (CONST CHAR8 *)Context->FileBuffer + Context->ExeHdrOffset
                                                               );

      if (Pe32Hdr->NumberOfRvaAndSizes <= EFI_IMAGE_DIRECTORY_ENTRY_DEBUG) {
        return RETURN_NOT_FOUND;
      }

      DebugDir = Pe32Hdr->DataDirectory + EFI_IMAGE_DIRECTORY_ENTRY_DEBUG;
      break;

    case PeCoffLoaderTypePe32Plus:
      Pe32PlusHdr = (CONST EFI_IMAGE_NT_HEADERS64 *)(CONST VOID *)(
                                                                   (CONST CHAR8 *)Context->FileBuffer + Context->ExeHdrOffset
                                                                   );

      if (Pe32PlusHdr->NumberOfRvaAndSizes <= EFI_IMAGE_DIRECTORY_ENTRY_DEBUG) {
        return RETURN_NOT_FOUND;
      }

      DebugDir = Pe32PlusHdr->DataDirectory + EFI_IMAGE_DIRECTORY_ENTRY_DEBUG;
      break;

    default:
      ASSERT (FALSE);
      return RETURN_UNSUPPORTED;
  }

  //
  // Verify the Debug Directory is not empty.
  //
  if (DebugDir->Size == 0) {
    return RETURN_NOT_FOUND;
  }

  //
  // Verify the Debug Directory has a well-formed size.
  //
  if (DebugDir->Size % sizeof (*DebugEntries) != 0) {
    DEBUG_RAISE ();
    return RETURN_UNSUPPORTED;
  }

  //
  // Verify the Debug Directory is in bounds of the Image buffer.
  //
  Overflow = BaseOverflowAddU32 (
               DebugDir->VirtualAddress,
               DebugDir->Size,
               &DebugDirTop
               );
  if (Overflow || (DebugDirTop > Context->SizeOfImage)) {
    DEBUG_RAISE ();
    return RETURN_UNSUPPORTED;
  }

  //
  // Determine the raw file offset of the Debug Directory.
  //
  Sections = (CONST EFI_IMAGE_SECTION_HEADER *)(CONST VOID *)(
                                                              (CONST CHAR8 *)Context->FileBuffer + Context->SectionsOffset
                                                              );

  for (SectionIndex = 0; SectionIndex < Context->NumberOfSections; ++SectionIndex) {
    if (  (DebugDir->VirtualAddress >= Sections[SectionIndex].VirtualAddress)
       && (DebugDirTop <= Sections[SectionIndex].VirtualAddress + Sections[SectionIndex].VirtualSize))
    {
      break;
    }
  }

  //
  // Verify the Debug Directory was found among the Image sections.
  //
  if (SectionIndex == Context->NumberOfSections) {
    DEBUG_RAISE ();
    return RETURN_UNSUPPORTED;
  }

  //
  // Verify the Debug Directory data is in bounds of the Image section.
  //
  // This arithmetic cannot overflow because we know
  //   1) DebugDir->VirtualAddress + DebugDir->Size <= MAX_UINT32
  //   2) Sections[SectionIndex].VirtualAddress <= DebugDir->VirtualAddress.
  //
  DebugDirSectionOffset = DebugDir->VirtualAddress - Sections[SectionIndex].VirtualAddress;
  DebugDirSectionRawTop = DebugDirSectionOffset + DebugDir->Size;
  if (DebugDirSectionRawTop > Sections[SectionIndex].SizeOfRawData) {
    DEBUG_RAISE ();
    return RETURN_UNSUPPORTED;
  }

  //
  // Verify the Debug Directory raw file offset is sufficiently aligned.
  //
  DebugDirFileOffset = Sections[SectionIndex].PointerToRawData + DebugDirSectionOffset;

  if (!PcdGetBool (PcdImageLoaderProhibitTe)) {
    //
    // This subtraction is safe because we know it holds that:
    //   Context->TeStrippedOffset <= Sections[SectionIndex].PointerToRawData.
    //
    ASSERT (Context->TeStrippedOffset <= Sections[SectionIndex].PointerToRawData);
    DebugDirFileOffset -= Context->TeStrippedOffset;
  } else {
    ASSERT (Context->TeStrippedOffset == 0);
  }

  if (!IS_ALIGNED (DebugDirFileOffset, ALIGNOF (EFI_IMAGE_DEBUG_DIRECTORY_ENTRY))) {
    DEBUG_RAISE ();
    return RETURN_UNSUPPORTED;
  }

  DebugEntries = (CONST EFI_IMAGE_DEBUG_DIRECTORY_ENTRY *)(CONST VOID *)(
                                                                         (CONST CHAR8 *)Context->FileBuffer + DebugDirFileOffset
                                                                         );

  NumDebugEntries = DebugDir->Size / sizeof (*DebugEntries);

  for (DebugIndex = 0; DebugIndex < NumDebugEntries; ++DebugIndex) {
    if (DebugEntries[DebugIndex].Type == EFI_IMAGE_DEBUG_TYPE_CODEVIEW) {
      break;
    }
  }

  //
  // Verify the CodeView entry has been found in the Debug Directory.
  //
  if (DebugIndex == NumDebugEntries) {
    return RETURN_NOT_FOUND;
  }

  //
  // Verify the CodeView entry has sufficient space for the signature.
  //
  CodeViewEntry = &DebugEntries[DebugIndex];

  if (CodeViewEntry->SizeOfData < sizeof (UINT32)) {
    DEBUG_RAISE ();
    return RETURN_UNSUPPORTED;
  }

  DebugEntryFileOffset = CodeViewEntry->FileOffset;

  if (!PcdGetBool (PcdImageLoaderProhibitTe)) {
    Overflow = BaseOverflowSubU32 (
                 DebugEntryFileOffset,
                 Context->TeStrippedOffset,
                 &DebugEntryFileOffset
                 );
    if (Overflow) {
      DEBUG_RAISE ();
      return RETURN_UNSUPPORTED;
    }
  } else {
    ASSERT (Context->TeStrippedOffset == 0);
  }

  //
  // Verify the CodeView entry is in bounds of the Image buffer, and the
  // CodeView entry RVA is sufficiently aligned.
  //
  Overflow = BaseOverflowAddU32 (
               DebugEntryFileOffset,
               CodeViewEntry->SizeOfData,
               &DebugEntryFileOffsetTop
               );
  if (  Overflow || (DebugEntryFileOffsetTop > Context->FileSize)
     || !IS_ALIGNED (DebugEntryFileOffset, ALIGNOF (UINT32)))
  {
    DEBUG_RAISE ();
    return RETURN_UNSUPPORTED;
  }

  CodeView = (CONST CHAR8 *)Context->FileBuffer + DebugEntryFileOffset;
  //
  // This memory access is safe because we know that
  //   1) IS_ALIGNED (DebugEntryFileOffset, ALIGNOF (UINT32))
  //   2) sizeof (UINT32) <= CodeViewEntry->SizeOfData.
  //
  switch (*(CONST UINT32 *)(CONST VOID *)CodeView) {
    case CODEVIEW_SIGNATURE_NB10:
      PdbOffset = sizeof (EFI_IMAGE_DEBUG_CODEVIEW_NB10_ENTRY);

      STATIC_ASSERT (
        ALIGNOF (EFI_IMAGE_DEBUG_CODEVIEW_NB10_ENTRY) <= ALIGNOF (UINT32),
        "The structure may be misalignedd."
        );
      break;

    case CODEVIEW_SIGNATURE_RSDS:
      PdbOffset = sizeof (EFI_IMAGE_DEBUG_CODEVIEW_RSDS_ENTRY);

      STATIC_ASSERT (
        ALIGNOF (EFI_IMAGE_DEBUG_CODEVIEW_RSDS_ENTRY) <= ALIGNOF (UINT32),
        "The structure may be misalignedd."
        );
      break;

    case CODEVIEW_SIGNATURE_MTOC:
      PdbOffset = sizeof (EFI_IMAGE_DEBUG_CODEVIEW_MTOC_ENTRY);

      STATIC_ASSERT (
        ALIGNOF (EFI_IMAGE_DEBUG_CODEVIEW_MTOC_ENTRY) <= ALIGNOF (UINT32),
        "The structure may be misalignedd."
        );
      break;

    default:
      DEBUG_RAISE ();
      return RETURN_UNSUPPORTED;
  }

  //
  // Verify the PDB path exists and is in bounds of the Image buffer.
  //
  Overflow = BaseOverflowSubU32 (
               CodeViewEntry->SizeOfData,
               PdbOffset,
               &PdbNameSize
               );
  if (Overflow || (PdbNameSize == 0)) {
    DEBUG_RAISE ();
    return RETURN_UNSUPPORTED;
  }

  //
  // Verify the PDB path is correctly terminated.
  //
  PdbName = CodeView + PdbOffset;
  if (PdbName[PdbNameSize - 1] != 0) {
    DEBUG_RAISE ();
    return RETURN_UNSUPPORTED;
  }

  *PdbPath     = PdbName;
  *PdbPathSize = PdbNameSize;

  return RETURN_SUCCESS;
}