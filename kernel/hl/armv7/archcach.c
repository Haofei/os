/*++

Copyright (c) 2013 Minoca Corp. All Rights Reserved

Module Name:

    archcach.c

Abstract:

    This module implements architecture-specific cache support for the hardware
    library.

Author:

    Chris Stevens 13-Jan-2014

Environment:

    Kernel

--*/

//
// ------------------------------------------------------------------- Includes
//

#include <minoca/kernel.h>
#include <minoca/arm.h>
#include "../hlp.h"
#include "../cache.h"

//
// ---------------------------------------------------------------- Definitions
//

//
// ------------------------------------------------------ Data Type Definitions
//

//
// ----------------------------------------------- Internal Function Prototypes
//

//
// Builtin hardware module function prototypes.
//

VOID
HlpOmap4CacheControllerModuleEntry (
    PHARDWARE_MODULE_KERNEL_SERVICES Services
    );

//
// -------------------------------------------------------------------- Globals
//

//
// Built-in hardware modules.
//

PHARDWARE_MODULE_ENTRY HlBuiltinCacheModules[] = {
    HlpOmap4CacheControllerModuleEntry,
    NULL
};

//
// ------------------------------------------------------------------ Functions
//

KSTATUS
HlpArchInitializeCacheControllers (
    VOID
    )

/*++

Routine Description:

    This routine performs architecture-specific initialization for the cache
    subsystem.

Arguments:

    None.

Return Value:

    Status code.

--*/

{

    PHARDWARE_MODULE_ENTRY ModuleEntry;
    ULONG ModuleIndex;

    //
    // On the boot processor, perform one-time initialization.
    //

    if (KeGetCurrentProcessorNumber() == 0) {

        //
        // Loop through and initialize every built in hardware module.
        //

        ModuleIndex = 0;
        while (HlBuiltinCacheModules[ModuleIndex] != NULL) {
            ModuleEntry = HlBuiltinCacheModules[ModuleIndex];
            ModuleEntry(&HlHardwareModuleServices);
            ModuleIndex += 1;
        }
    }

    return STATUS_SUCCESS;
}

//
// --------------------------------------------------------- Internal Functions
//

