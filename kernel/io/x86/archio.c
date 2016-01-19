/*++

Copyright (c) 2012 Minoca Corp. All Rights Reserved

Module Name:

    archio.c

Abstract:

    This module implements x86 architecture specific code for the I/O
    Subsystem.

Author:

    Evan Green 15-Dec-2012

Environment:

    Kernel

--*/

//
// ------------------------------------------------------------------- Includes
//

#include <minoca/kernel.h>
#include "../iop.h"

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
// -------------------------------------------------------------------- Globals
//

//
// ------------------------------------------------------------------ Functions
//

KSTATUS
IopArchInitializeKnownArbiterRegions (
    VOID
    )

/*++

Routine Description:

    This routine performs any architecture-specific initialization of the
    resource arbiters.

Arguments:

    None.

Return Value:

    Status code.

--*/

{

    KSTATUS Status;

    //
    // Allow the physical address space arbiter to dole out the first megabyte
    // of physical address space, which was not presented to MM as free because
    // it's not free. This will be swallowed up by PCI and ISA on all BIOSes.
    //

    Status = IoAddFreeSpaceToArbiter(IoRootDevice,
                                     ResourceTypePhysicalAddressSpace,
                                     0,
                                     _1MB,
                                     0,
                                     NULL,
                                     0);

    if (!KSUCCESS(Status)) {
        goto ArchInitializeKnownArbiterRegionsEnd;
    }

ArchInitializeKnownArbiterRegionsEnd:
    return Status;
}

//
// --------------------------------------------------------- Internal Functions
//

