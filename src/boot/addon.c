/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "addon-util.h"
#include "efi.h"
#include "pe.h"
#include "version.h"

/* Magic string for recognizing our own binaries */
DECLARE_NOALLOC_SECTION(".sdmagic", "#### LoaderInfo: systemd-addon " GIT_VERSION " ####");
_used_ _section_(".binrel") static const struct pe_metadata metadata = {
        .fname = ADDON_FILENAME
};


/* This is intended to carry data, not to be executed */

EFIAPI EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system_table);
EFIAPI EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system_table) {
        return EFI_UNSUPPORTED;
}
