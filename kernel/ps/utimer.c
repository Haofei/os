/*++

Copyright (c) 2013 Minoca Corp. All Rights Reserved

Module Name:

    utimer.c

Abstract:

    This module implements user mode timer support.

Author:

    Evan Green 11-Aug-2013

Environment:

    Kernel

--*/

//
// ------------------------------------------------------------------- Includes
//

#include <minoca/kernel.h>
#include "processp.h"

//
// ---------------------------------------------------------------- Definitions
//

#define PROCESS_TIMER_ALLOCATION_TAG 0x6D547350 // 'mTsP'

//
// ------------------------------------------------------ Data Type Definitions
//

/*++

Structure Description:

    This structure defines a user mode timer.

Members:

    ListEntry - Stores pointers to the next and previous timers in the process
        list.

    ReferenceCount - Stores the reference count on the timer.

    Process - Stores a pointer to the process that owns this timer.

    TimerNumber - Stores the timer's identifying number.

    DueTime - Stores the due time of the timer.

    Interval - Stores the periodic interval of the timer.

    ExpirationCount - Stores the number of timer expirations that have occurred
        since the last work item ran.

    OverflowCount - Stores the number of overflows that have occurred since the
        last time the caller asked.

    Timer - Stores a pointer to the timer backing this user mode timer.

    Dpc - Stores a pointer to the DPC that runs when the timer fires.

    WorkItem - Stores a pointer to the work item that's queued when the DPC
        runs.

    SignalQueueEntry - Stores the signal queue entry that gets queued when the
        timer expires.

--*/

typedef struct _PROCESS_TIMER {
    LIST_ENTRY ListEntry;
    ULONG ReferenceCount;
    PKPROCESS Process;
    LONG TimerNumber;
    ULONGLONG DueTime;
    ULONGLONG Interval;
    ULONG ExpirationCount;
    ULONG OverflowCount;
    PKTIMER Timer;
    PDPC Dpc;
    PWORK_ITEM WorkItem;
    SIGNAL_QUEUE_ENTRY SignalQueueEntry;
} PROCESS_TIMER, *PPROCESS_TIMER;

//
// ----------------------------------------------- Internal Function Prototypes
//

KSTATUS
PspCreateProcessTimer (
    PKPROCESS Process,
    PPROCESS_TIMER *Timer
    );

VOID
PspProcessTimerAddReference (
    PPROCESS_TIMER Timer
    );

VOID
PspProcessTimerReleaseReference (
    PPROCESS_TIMER Timer
    );

VOID
PspDestroyProcessTimer (
    PPROCESS_TIMER Timer
    );

VOID
PspFlushProcessTimer (
    PKPROCESS Process,
    PPROCESS_TIMER Timer
    );

VOID
PspProcessTimerDpcRoutine (
    PDPC Dpc
    );

VOID
PspProcessTimerWorkRoutine (
    PVOID Parameter
    );

VOID
PspProcessTimerSignalCompletion (
    PSIGNAL_QUEUE_ENTRY SignalQueueEntry
    );

//
// -------------------------------------------------------------------- Globals
//

//
// ------------------------------------------------------------------ Functions
//

VOID
PsSysQueryTimeCounter (
    ULONG SystemCallNumber,
    PVOID SystemCallParameter,
    PTRAP_FRAME TrapFrame,
    PULONG ResultSize
    )

/*++

Routine Description:

    This routine implements the system call for getting the current time
    counter value.

Arguments:

    SystemCallNumber - Supplies the system call number that was requested.

    SystemCallParameter - Supplies a pointer to the parameters supplied with
        the system call. This structure will be a stack-local copy of the
        actual parameters passed from user-mode.

    TrapFrame - Supplies a pointer to the trap frame generated by this jump
        from user mode to kernel mode.

    ResultSize - Supplies a pointer where the system call routine returns the
        size of the parameter structure to be copied back to user mode. The
        value returned here must be no larger than the original parameter
        structure size. The default is the original size of the parameters.

Return Value:

    None.

--*/

{

    PSYSTEM_CALL_QUERY_TIME_COUNTER Parameters;

    Parameters = (PSYSTEM_CALL_QUERY_TIME_COUNTER)SystemCallParameter;
    Parameters->Value = HlQueryTimeCounter();
    return;
}

VOID
PsSysTimerControl (
    ULONG SystemCallNumber,
    PVOID SystemCallParameter,
    PTRAP_FRAME TrapFrame,
    PULONG ResultSize
    )

/*++

Routine Description:

    This routine performs timer control operations.

Arguments:

    SystemCallNumber - Supplies the system call number that was requested.

    SystemCallParameter - Supplies a pointer to the parameters supplied with
        the system call. This structure will be a stack-local copy of the
        actual parameters passed from user-mode.

    TrapFrame - Supplies a pointer to the trap frame generated by this jump
        from user mode to kernel mode.

    ResultSize - Supplies a pointer where the system call routine returns the
        size of the parameter structure to be copied back to user mode. The
        value returned here must be no larger than the original parameter
        structure size. The default is the original size of the parameters.

Return Value:

    None.

--*/

{

    PLIST_ENTRY CurrentEntry;
    PPROCESS_TIMER CurrentTimer;
    BOOL LockHeld;
    TIMER_INFORMATION OriginalInformation;
    PSYSTEM_CALL_TIMER_CONTROL Parameters;
    PPROCESS_TIMER PreviousTimer;
    PKPROCESS Process;
    KSTATUS Status;
    PPROCESS_TIMER Timer;

    ASSERT(SystemCallNumber == SystemCallTimerControl);

    LockHeld = FALSE;
    Parameters = (PSYSTEM_CALL_TIMER_CONTROL)SystemCallParameter;
    Process = PsGetCurrentProcess();

    ASSERT(Process != PsGetKernelProcess());

    Timer = NULL;

    //
    // If it's not a create operation, find the timer being referenced.
    //

    if (Parameters->Operation != TimerOperationCreateTimer) {
        KeAcquireQueuedLock(Process->QueuedLock);
        LockHeld = TRUE;
        CurrentEntry = Process->TimerList.Next;
        while (CurrentEntry != &(Process->TimerList)) {
            CurrentTimer = LIST_VALUE(CurrentEntry, PROCESS_TIMER, ListEntry);
            if (CurrentTimer->TimerNumber == Parameters->TimerNumber) {
                Timer = CurrentTimer;
                break;
            }

            CurrentEntry = CurrentEntry->Next;
        }

        if (Timer == NULL) {
            Status = STATUS_INVALID_HANDLE;
            goto SysTimerControlEnd;
        }
    }

    Status = STATUS_SUCCESS;
    switch (Parameters->Operation) {

    //
    // Create a new processs timer and add it to the list.
    //

    case TimerOperationCreateTimer:
        Status = PspCreateProcessTimer(Process, &Timer);
        if (!KSUCCESS(Status)) {
            goto SysTimerControlEnd;
        }

        Timer->SignalQueueEntry.Parameters.SignalNumber =
                                                      Parameters->SignalNumber;

        Timer->SignalQueueEntry.Parameters.SignalCode = SIGNAL_CODE_TIMER;
        Timer->SignalQueueEntry.Parameters.Parameter = Parameters->SignalValue;

        //
        // Take a reference on the process to avoid a situation where the
        // process is destroyed before the work item gets around to running.
        //

        ObAddReference(Process);

        //
        // Insert this timer in the process. Assign the timer the ID of the
        // last timer in the list plus one.
        //

        KeAcquireQueuedLock(Process->QueuedLock);
        if (LIST_EMPTY(&(Process->TimerList)) != FALSE) {
            Timer->TimerNumber = 1;

        } else {
            PreviousTimer = LIST_VALUE(Process->TimerList.Previous,
                                       PROCESS_TIMER,
                                       ListEntry);

            Timer->TimerNumber = PreviousTimer->TimerNumber + 1;
        }

        if (Parameters->UseTimerNumber != FALSE) {
            Timer->SignalQueueEntry.Parameters.Parameter = Timer->TimerNumber;
        }

        INSERT_BEFORE(&(Timer->ListEntry), &(Process->TimerList));
        KeReleaseQueuedLock(Process->QueuedLock);
        Parameters->TimerNumber = Timer->TimerNumber;
        break;

    //
    // Delete an existing process timer.
    //

    case TimerOperationDeleteTimer:
        LIST_REMOVE(&(Timer->ListEntry));
        KeReleaseQueuedLock(Process->QueuedLock);
        LockHeld = FALSE;
        PspFlushProcessTimer(Process, Timer);
        PspProcessTimerReleaseReference(Timer);
        break;

    //
    // Get timer information, including the next due time and overflow count.
    //

    case TimerOperationGetTimer:
        Parameters->TimerInformation.DueTime = KeGetTimerDueTime(Timer->Timer);
        Parameters->TimerInformation.Period = Timer->Interval;
        Parameters->TimerInformation.OverflowCount = Timer->OverflowCount;
        break;

    //
    // Arm or disarm the timer. Save and return the original information.
    //

    case TimerOperationSetTimer:
        OriginalInformation.DueTime = KeGetTimerDueTime(Timer->Timer);
        OriginalInformation.Period = Timer->Interval;
        OriginalInformation.OverflowCount = 0;
        if (Timer->DueTime != 0) {
            KeCancelTimer(Timer->Timer);
        }

        Timer->DueTime = Parameters->TimerInformation.DueTime;
        Timer->Interval = Parameters->TimerInformation.Period;
        if ((Timer->DueTime != 0) || (Timer->Interval != 0)) {
            if (Timer->DueTime == 0) {
                Timer->DueTime = HlQueryTimeCounter();
            }

            Status = KeQueueTimer(Timer->Timer,
                                  TimerQueueSoftWake,
                                  Timer->DueTime,
                                  Timer->Interval,
                                  0,
                                  Timer->Dpc);

            if (!KSUCCESS(Status)) {
                goto SysTimerControlEnd;
            }
        }

        RtlCopyMemory(&(Parameters->TimerInformation),
                      &OriginalInformation,
                      sizeof(TIMER_INFORMATION));

        break;

    default:

        ASSERT(FALSE);

        Status = STATUS_INVALID_PARAMETER;
        goto SysTimerControlEnd;
    }

SysTimerControlEnd:
    if (LockHeld != FALSE) {
        KeReleaseQueuedLock(Process->QueuedLock);
    }

    Parameters->Status = Status;
    return;
}

VOID
PspDestroyProcessTimers (
    PKPROCESS Process
    )

/*++

Routine Description:

    This routine cleans up any timers a process may have. This routine assumes
    the process lock is already held.

Arguments:

    Process - Supplies a pointer to the process.

Return Value:

    None.

--*/

{

    PPROCESS_TIMER Timer;

    while (LIST_EMPTY(&(Process->TimerList)) == FALSE) {
        Timer = LIST_VALUE(Process->TimerList.Next, PROCESS_TIMER, ListEntry);
        LIST_REMOVE(&(Timer->ListEntry));

        //
        // Cancel the timer and flush the DPC to ensure that the reference
        // count is up to date. Then release the reference. This will either
        // clean up the object right away or the work item will run on its
        // own time.
        //

        KeCancelTimer(Timer->Timer);
        if (!KSUCCESS(KeCancelDpc(Timer->Dpc))) {
            KeFlushDpc(Timer->Dpc);
        }

        PspProcessTimerReleaseReference(Timer);
    }

    return;
}

//
// --------------------------------------------------------- Internal Functions
//

KSTATUS
PspCreateProcessTimer (
    PKPROCESS Process,
    PPROCESS_TIMER *Timer
    )

/*++

Routine Description:

    This routine attempts to create a new process timer.

Arguments:

    Process - Supplies a pointer to the process that owns the timer.

    Timer - Supplies a pointer where a pointer to the new timer is returned on
        success.

Return Value:

    STATUS_SUCCESS always.

--*/

{

    PPROCESS_TIMER NewTimer;
    KSTATUS Status;

    ASSERT(KeGetRunLevel() == RunLevelLow);

    Status = STATUS_INSUFFICIENT_RESOURCES;
    NewTimer = MmAllocateNonPagedPool(sizeof(PROCESS_TIMER),
                                      PROCESS_TIMER_ALLOCATION_TAG);

    if (NewTimer == NULL) {
        goto CreateProcessTimerEnd;
    }

    RtlZeroMemory(NewTimer, sizeof(PROCESS_TIMER));
    NewTimer->Process = Process;
    NewTimer->ReferenceCount = 1;
    NewTimer->Timer = KeCreateTimer(PROCESS_TIMER_ALLOCATION_TAG);
    if (NewTimer->Timer == NULL) {
        goto CreateProcessTimerEnd;
    }

    NewTimer->Dpc = KeCreateDpc(PspProcessTimerDpcRoutine, NewTimer);
    if (NewTimer->Dpc == NULL) {
        goto CreateProcessTimerEnd;
    }

    NewTimer->WorkItem = KeCreateWorkItem(NULL,
                                          WorkPriorityNormal,
                                          PspProcessTimerWorkRoutine,
                                          NewTimer,
                                          PROCESS_TIMER_ALLOCATION_TAG);

    if (NewTimer->WorkItem == NULL) {
        goto CreateProcessTimerEnd;
    }

    NewTimer->SignalQueueEntry.CompletionRoutine =
                                               PspProcessTimerSignalCompletion;

    Status = STATUS_SUCCESS;

CreateProcessTimerEnd:
    if (!KSUCCESS(Status)) {
        if (NewTimer != NULL) {
            if (NewTimer->Timer != NULL) {
                KeDestroyTimer(NewTimer->Timer);
            }

            if (NewTimer->Dpc != NULL) {
                KeDestroyDpc(NewTimer->Dpc);
            }

            if (NewTimer->WorkItem != NULL) {
                KeDestroyWorkItem(NewTimer->WorkItem);
            }

            MmFreeNonPagedPool(NewTimer);
            NewTimer = NULL;
        }
    }

    *Timer = NewTimer;
    return Status;
}

VOID
PspProcessTimerAddReference (
    PPROCESS_TIMER Timer
    )

/*++

Routine Description:

    This routine adds a reference to a process timer.

Arguments:

    Timer - Supplies a pointer to the timer.

Return Value:

    None.

--*/

{

    RtlAtomicAdd32(&(Timer->ReferenceCount), 1);
    return;
}

VOID
PspProcessTimerReleaseReference (
    PPROCESS_TIMER Timer
    )

/*++

Routine Description:

    This routine releases a reference on a process timer.

Arguments:

    Timer - Supplies a pointer to the timer.

Return Value:

    None.

--*/

{

    if (RtlAtomicAdd32(&(Timer->ReferenceCount), -1) == 1) {
        PspDestroyProcessTimer(Timer);
    }

    return;
}

VOID
PspDestroyProcessTimer (
    PPROCESS_TIMER Timer
    )

/*++

Routine Description:

    This routine destroys a process timer.

Arguments:

    Timer - Supplies a pointer to the timer to destroy.

Return Value:

    None.

--*/

{

    KeDestroyTimer(Timer->Timer);
    KeDestroyDpc(Timer->Dpc);
    KeDestroyWorkItem(Timer->WorkItem);
    ObReleaseReference(Timer->Process);
    MmFreeNonPagedPool(Timer);
    return;
}

VOID
PspFlushProcessTimer (
    PKPROCESS Process,
    PPROCESS_TIMER Timer
    )

/*++

Routine Description:

    This routine flushes a process timer to the point where the reference
    count is prepared for anyone about to release a reference, and the signal
    is either queued or cancelled.

Arguments:

    Process - Supplies a pointer to the process that owns the timer.

    Timer - Supplies a pointer to the timer to cancel/flush.

Return Value:

    None.

--*/

{

    //
    // After the timer's cancelled, the DPC is queued or it isn't going to be.
    //

    KeCancelTimer(Timer->Timer);

    //
    // Cancelling or flushing the DPC means that either the work item is queued
    // or isn't going to be.
    //

    if (!KSUCCESS(KeCancelDpc(Timer->Dpc))) {
        KeFlushDpc(Timer->Dpc);
    }

    //
    // After the work queue's flushed, either the signal is queued or it isn't
    // going to be.
    //

    KeFlushWorkQueue(NULL);

    //
    // Attempt to cancel the signal to prevent signals from coming in way
    // after the timer was deleted.
    //

    PspCancelQueuedSignal(Process, &(Timer->SignalQueueEntry));
    return;
}

VOID
PspProcessTimerDpcRoutine (
    PDPC Dpc
    )

/*++

Routine Description:

    This routine implements the DPC routine that fires when a process timer
    expires. It queues the work item.

Arguments:

    Dpc - Supplies a pointer to the DPC that is running.

Return Value:

    None.

--*/

{

    KSTATUS Status;
    PPROCESS_TIMER Timer;

    //
    // Increment the number of expirations, and queue the work item if this was
    // the first one.
    //

    Timer = (PPROCESS_TIMER)(Dpc->UserData);
    if (RtlAtomicAdd32(&(Timer->ExpirationCount), 1) == 0) {

        //
        // Increment the reference count to ensure this structure doesn't go
        // away while the signal is queued. Anybody trying to make the structure
        // go away needs to flush the DPC before decrementing their referecne
        // to ensure this gets a chance to run.
        //

        PspProcessTimerAddReference(Timer);
        Status = KeQueueWorkItem(Timer->WorkItem);

        ASSERT(KSUCCESS(Status));
    }

    return;
}

VOID
PspProcessTimerWorkRoutine (
    PVOID Parameter
    )

/*++

Routine Description:

    This routine implements the process timer expiration work routine.

Arguments:

    Parameter - Supplies a pointer to the process timer.

Return Value:

    None.

--*/

{

    ULONG ExpirationCount;
    PPROCESS_TIMER Timer;

    Timer = (PPROCESS_TIMER)Parameter;

    //
    // Read the current expiration count to determine how to set the overflow
    // count.
    //

    ExpirationCount = RtlAtomicOr32(&(Timer->ExpirationCount), 0);

    ASSERT(ExpirationCount != 0);

    Timer->OverflowCount = ExpirationCount - 1;
    Timer->SignalQueueEntry.Parameters.FromU.OverflowCount =
                                                          Timer->OverflowCount;

    PsSignalProcess(Timer->Process,
                    Timer->SignalQueueEntry.Parameters.SignalNumber,
                    &(Timer->SignalQueueEntry));

    return;
}

VOID
PspProcessTimerSignalCompletion (
    PSIGNAL_QUEUE_ENTRY SignalQueueEntry
    )

/*++

Routine Description:

    This routine is called when a process timer's signal was successfully
    completed in usermode.

Arguments:

    SignalQueueEntry - Supplies a pointer to the signal queue entry that was
        successfully sent to user mode.

Return Value:

    None.

--*/

{

    ULONG ExpirationCount;
    ULONG OverflowCount;
    PPROCESS_TIMER Timer;

    Timer = PARENT_STRUCTURE(SignalQueueEntry, PROCESS_TIMER, SignalQueueEntry);

    //
    // Slam a zero into the overflow count.
    //

    OverflowCount = Timer->OverflowCount;
    Timer->OverflowCount = 0;

    //
    // Subtract off the overflow count (plus one for the original non-overflow
    // expiration) from the expiration count.
    //

    OverflowCount += 1;
    ExpirationCount = RtlAtomicAdd32(&(Timer->ExpirationCount), -OverflowCount);

    ASSERT(ExpirationCount >= OverflowCount);

    //
    // If new intervals came in already, re-queue the work item immediately,
    // as the DPC is never going to.
    //

    if (ExpirationCount - OverflowCount != 0) {
        KeQueueWorkItem(Timer->WorkItem);

    //
    // Release the reference, until the next DPC runs all parties are done
    // touching this memory.
    //

    } else {
        PspProcessTimerReleaseReference(Timer);
    }

    return;
}
