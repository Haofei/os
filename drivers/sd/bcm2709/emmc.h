/*++

Copyright (c) 2014 Minoca Corp. All Rights Reserved

Module Name:

    emmc.h

Abstract:

    This header contains definitions for the BCM2709 eMMC controller.

Author:

    Chris Stevens 10-Dec-2014

--*/

//
// ------------------------------------------------------------------- Includes
//

//
// ---------------------------------------------------------------- Definitions
//

//
// ------------------------------------------------------ Data Type Definitions
//

//
// -------------------------------------------------------------------- Globals
//

//
// -------------------------------------------------------- Function Prototypes
//

KSTATUS
Bcm2709EmmcInitialize (
    VOID
    );

/*++

Routine Description:

    This routine initializes the BCM2835 SoC's Emmc controller.

Arguments:

    None.

Return Value:

    Status code.

--*/

KSTATUS
Bcm2709EmmcGetClockFrequency (
    PULONG Frequency
    );

/*++

Routine Description:

    This routine gets the eMMC's clock frequency for the BCM2835 SoC.

Arguments:

    Frequency - Supplies a pointer that receives the eMMC's clock frequency.

Return Value:

    Status code.

--*/

