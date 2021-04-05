//
// Copyright (C) Microsoft. All rights reserved.
//
// Abstract:
//
//  This module contains methods implementation of class SDHC
//
//  Workarounds used across the implementation are explained inline and
//  are prefixed with the word 'WORKAROUND'
//
// Environment:
//
//  Kernel mode only
//

#include "precomp.hpp"
#pragma hdrstop

#include "SdhcLogging.h"
#include "rpisdhc.tmh"

#include "rpisdhc.hpp"

SDHC_NONPAGED_SEGMENT_BEGIN; //================================================

_Use_decl_annotations_
NTSTATUS SDHC::readFromFifo (
    void* BufferPtr,
    ULONG Size
    ) throw ()
{
    ULONG* wordPtr = static_cast<ULONG*>(BufferPtr);
    ULONG count = Size / sizeof(ULONG);
    SDHC_ASSERT((Size % sizeof(ULONG)) == 0);
    ULONG waitTimeUs = 0;

#if ENABLE_PERFORMANCE_LOGGING

    LARGE_INTEGER startTimestamp = KeQueryPerformanceCounter(NULL);

#endif // ENABLE_PERFORMANCE_LOGGING

    while (count) {
        NTSTATUS waitStatus = this->waitForDataFlag(&waitTimeUs);
        if (!NT_SUCCESS(waitStatus)) {
            this->updateAllRegistersDump();
            SDHC_LOG_ERROR(
                "this->waitForDataFlag() failed. (waitStatus = %!STATUS!)",
                waitStatus);
            return waitStatus;
        } // if

        *wordPtr = this->readRegisterNoFence<_DATA>().AsUint32;

#if ENABLE_PERFORMANCE_LOGGING

        if (waitTimeUs > 0) {
            ++this->currRequestStats.FifoWaitCount;
            this->currRequestStats.FifoWaitTimeUs += waitTimeUs;
        } // if

        this->currRequestStats.FifoMaxWaitTimeUs =
            max(this->currRequestStats.FifoMaxWaitTimeUs, waitTimeUs);

#endif // ENABLE_PERFORMANCE_LOGGING

        ++wordPtr;
        --count;
    } // while (count)

#if ENABLE_PERFORMANCE_LOGGING

    LARGE_INTEGER endTimestamp = KeQueryPerformanceCounter(NULL);
    LONGLONG elapsedHpcTicks = endTimestamp.QuadPart - startTimestamp.QuadPart;
    this->currRequestStats.FifoIoTimeTicks += elapsedHpcTicks;

#endif // ENABLE_PERFORMANCE_LOGGING

    return STATUS_SUCCESS;
} // SDHC::readFromFifo (...)

_Use_decl_annotations_
NTSTATUS SDHC::writeToFifo (
    const void* BufferPtr,
    ULONG Size
    ) throw ()
{
    const ULONG* wordPtr = static_cast<const ULONG*>(BufferPtr);
    ULONG count = Size / sizeof(ULONG);
    SDHC_ASSERT((Size % sizeof(ULONG)) == 0);
    ULONG waitTimeUs = 0;

#if ENABLE_PERFORMANCE_LOGGING

    LARGE_INTEGER startTimestamp = KeQueryPerformanceCounter(NULL);

#endif // ENABLE_PERFORMANCE_LOGGING

    while (count) {
        NTSTATUS waitStatus = this->waitForDataFlag(&waitTimeUs);
        if (!NT_SUCCESS(waitStatus)) {
            this->updateAllRegistersDump();
            SDHC_LOG_ERROR(
                "this->waitForDataFlag() failed. (waitStatus = %!STATUS!)",
                waitStatus);
            return waitStatus;
        } // if

        this->writeRegisterNoFence(_DATA{ *wordPtr });

#if ENABLE_PERFORMANCE_LOGGING

        if (waitTimeUs > 0) {
            ++this->currRequestStats.FifoWaitCount;
            this->currRequestStats.FifoWaitTimeUs += waitTimeUs;
        } // if

        this->currRequestStats.FifoMaxWaitTimeUs =
            max(this->currRequestStats.FifoMaxWaitTimeUs, waitTimeUs);

#endif // ENABLE_PERFORMANCE_LOGGING

        ++wordPtr;
        --count;
    } // while (count)

#if ENABLE_PERFORMANCE_LOGGING

    LARGE_INTEGER endTimestamp = KeQueryPerformanceCounter(NULL);
    LONGLONG elapsedHpcTicks = endTimestamp.QuadPart - startTimestamp.QuadPart;
    this->currRequestStats.FifoIoTimeTicks += elapsedHpcTicks;

#endif // ENABLE_PERFORMANCE_LOGGING

    return STATUS_SUCCESS;
} // SDHC::readFromFifo (...)

_Use_decl_annotations_
NTSTATUS SDHC::waitForDataFlag (
    ULONG* WaitTimeUsPtr
    ) throw ()
{
    ULONG retry = _POLL_RETRY_COUNT;
    _HSTS hsts; this->readRegisterNoFence(&hsts);
    ULONG waitTimeUs = 0;

    while (!hsts.Fields.DataFlag &&
           !(hsts.AsUint32 & _HSTS::UINT32_ERROR_MASK) &&
            retry) {

        ::SdPortWait(_POLL_WAIT_US);
        waitTimeUs += _POLL_WAIT_US;
        this->readRegisterNoFence(&hsts);
        --retry;
    } // while (...)

    if (WaitTimeUsPtr) {
        *WaitTimeUsPtr = waitTimeUs;
    } // if

    if (hsts.AsUint32 & _HSTS::UINT32_ERROR_MASK) {
        return this->getErrorStatus(hsts);
    } else if (!retry) {
        return STATUS_IO_TIMEOUT;
    } else {
        SDHC_ASSERT(hsts.Fields.DataFlag);
        return STATUS_SUCCESS;
    } // iff

} // SDHC::waitForDataFlag ()

NTSTATUS SDHC::waitForFsmState (
    ULONG state
    ) throw ()
{
    ULONG retry = _POLL_RETRY_COUNT;
    _HSTS hsts; this->readRegisterNoFence(&hsts);
    _EDM edm; this->readRegisterNoFence(&edm);
    ULONG waitTimeUs = 0;

    while ((edm.Fields.StateMachine != state) &&
           !(hsts.AsUint32 & _HSTS::UINT32_ERROR_MASK) &&
            retry) {

        ::SdPortWait(_POLL_WAIT_US);
        waitTimeUs += _POLL_WAIT_US;
        this->readRegisterNoFence(&hsts);
        this->readRegisterNoFence(&edm);
        --retry;
    } // while (...)

#if ENABLE_PERFORMANCE_LOGGING

    if (waitTimeUs > 0) {
        ++this->currRequestStats.FsmStateWaitCount;
        this->currRequestStats.FsmStateWaitTimeUs += waitTimeUs;
    }

    this->currRequestStats.FsmStateMaxWaitTimeUs =
        max(this->currRequestStats.FsmStateMaxWaitTimeUs, waitTimeUs);
    this->currRequestStats.FsmStateMinWaitTimeUs =
        min(this->currRequestStats.FsmStateMinWaitTimeUs, waitTimeUs);

#endif // ENABLE_PERFORMANCE_LOGGING

    if (hsts.AsUint32 & _HSTS::UINT32_ERROR_MASK) {
        if (hsts.Fields.RewTimeOut) {
            SDHC_LOG_ERROR(
                "HW Read/Erase/Write timeout after %luus waiting on FSM state 0x%lx. (edm.Fields.StateMachine = 0x%lx)",
                waitTimeUs,
                state,
                edm.Fields.StateMachine);
        }
        return this->getErrorStatus(hsts);
    } else if (!retry) {
        SDHC_LOG_ERROR(
            "Poll timeout after %luus waiting on FSM state 0x%lx. (edm.Fields.StateMachine = 0x%lx)",
            waitTimeUs,
            state,
            edm.Fields.StateMachine);
        return STATUS_IO_TIMEOUT;
    } else {
        //
        // Use a threshold to catch bad SDCards taking too long time to
        // finish an FSM transition (e.g. taking too long to finish writing
        // a block physically)
        //
        if (waitTimeUs > _LONG_FSM_WAIT_TIME_THRESHOLD_US) {

#if ENABLE_PERFORMANCE_LOGGING

            ++this->currRequestStats.LongFsmStateWaitCount;
            this->currRequestStats.LongFsmStateWaitTimeUs += waitTimeUs;

#endif // ENABLE_PERFORMANCE_LOGGING

            SDHC_LOG_WARNING(
                "Long wait detected on FSM state 0x%lx for %luus",
                state,
                waitTimeUs);
        }

        SDHC_ASSERT(edm.Fields.StateMachine == state);
        return STATUS_SUCCESS;
    } // iff

} // SDHC::waitForFsmState ()

NTSTATUS SDHC::waitForLastCommandCompletion () throw ()
{
    ULONG retry = _POLL_RETRY_COUNT;
    _CMD cmd; this->readRegisterNoFence(&cmd);

    while ((cmd.Fields.NewFlag && !cmd.Fields.FailFlag) &&
           retry) {

        ::SdPortWait(_POLL_WAIT_US);
        this->readRegisterNoFence(&cmd);
        --retry;
    } // while (...)

    //
    // Wait for a command execution to come to an end, either
    // a successful or failed end, it doesn't matter
    //
    if (!cmd.Fields.NewFlag || cmd.Fields.FailFlag) {
        return STATUS_SUCCESS;
    } else {
        SDHC_ASSERT(!retry);
        return STATUS_IO_TIMEOUT;
    } // iff

} // SDHC::waitForLastCommandCompletion (...)

NTSTATUS SDHC::drainReadFifo () throw ()
{
    ULONG retry = _POLL_RETRY_COUNT;
    _HSTS hsts; this->readRegisterNoFence(&hsts);

    while (hsts.Fields.DataFlag &&
           retry) {

        (void)this->readRegisterNoFence<_DATA>();
        this->readRegisterNoFence(&hsts);
        --retry;
    } // while (...)

    if (!retry) {
        return STATUS_IO_TIMEOUT;
    } else {
        SDHC_ASSERT(!hsts.Fields.DataFlag);
        return STATUS_SUCCESS;
    } // iff
}

_Use_decl_annotations_
NTSTATUS SDHC::sdhcGetSlotCount (
   SD_MINIPORT* MiniportPtr,
   UCHAR* SlotCountPtr
   )
{
    SDHC_LOG_TRACE("()");

    switch (MiniportPtr->ConfigurationInfo.BusType) {
    case SdBusTypeAcpi:
        //
        // We don't currently have a mechanism to query the slot count for
        // ACPI enumerated host controllers. Default to one slot.
        //
        *SlotCountPtr = 1;
        break;

    default:
        SDHC_LOG_ASSERTION(
            "Unsupported SDPORT_BUS_TYPE. (MiniportPtr->ConfigurationInfo.BusType = %lu)",
            ULONG(MiniportPtr->ConfigurationInfo.BusType));
        return STATUS_NOT_SUPPORTED;
    } // switch (MiniportPtr->ConfigurationInfo.BusType)

    return STATUS_SUCCESS;
} // SDHC::sdhcGetSlotCount (...)

_Use_decl_annotations_
void SDHC::sdhcGetSlotCapabilities (
    void* PrivateExtensionPtr,
    SDPORT_CAPABILITIES* CapabilitiesPtr
    )
{
    auto thisPtr = static_cast<SDHC*>(PrivateExtensionPtr);
    SDHC_LOG_TRACE("()");

    *CapabilitiesPtr = thisPtr->sdhcCapabilities;
} // SDHC::sdhcGetSlotCapabilities (...)

_Use_decl_annotations_
BOOLEAN SDHC::sdhcInterrupt (
    void* PrivateExtensionPtr,
    ULONG* EventsPtr,
    ULONG* ErrorsPtr,
    BOOLEAN* NotifyCardChangePtr,
    BOOLEAN* NotifySdioInterruptPtr,
    BOOLEAN* NotifyTuningPtr
    )
{
    *NotifyCardChangePtr = FALSE;
    *NotifySdioInterruptPtr = FALSE;
    *NotifyTuningPtr = FALSE;

    auto thisPtr = static_cast<SDHC*>(PrivateExtensionPtr);

    _HCFG hcfg; thisPtr->readRegisterNoFence(&hcfg);
    _HSTS hsts; thisPtr->readRegisterNoFence(&hsts);
    _EDM edm; thisPtr->readRegisterNoFence(&edm);

    SDHC_LOG_TRACE(
        "(hcfg.AsUint32 = 0x%lx, hsts.AsUint32 = 0x%lx, edm.Fields.StateMachine = 0x%lx)",
        ULONG(hcfg.AsUint32),
        ULONG(hsts.AsUint32),
        ULONG(edm.Fields.StateMachine));

    //
    // If there aren't any events or errors to handle, then we don't
    // need to process anything
    //
    if (!(hsts.AsUint32 & _HSTS::UINT32_EVENTS_AND_ERRORS_MASK)) {
        return FALSE;
    } // if

    _HSTS sdhcEvents{ hsts.AsUint32 & _HSTS::UINT32_EVENTS_MASK };
    _HSTS sdhcErrors{ hsts.AsUint32 & _HSTS::UINT32_ERROR_MASK };

    //
    // WORKAROUND:
    // Data interrupt doesn't have a dedicated RWC status flag, we will assume that
    // a data interrupt occurred if both interrupt enabled and data flag is set
    //
    if (hcfg.Fields.DataIrptEn &&
        sdhcEvents.Fields.DataFlag) {
        //
        // There is no way to acknowledge data interrupt, we have to mask it
        // for the rest of the transfer request, will depend on block interrupts
        // from now on to chain transfer requests
        //
        _HCFG irptMask{ 0 };
        irptMask.Fields.DataIrptEn = 1;
        (void)thisPtr->maskInterrupts(irptMask);
    } // if

    *NotifySdioInterruptPtr = sdhcEvents.Fields.SdioIrpt;

    //
    // Since this SDHC is non-standard, we have to convert from its specific events and
    // errors to those expected by Sdport for a standard SDHC
    //
    *EventsPtr = thisPtr->getSdportEventsFromSdhcEvents(sdhcEvents).AsUint32;
    *ErrorsPtr = thisPtr->getSdportErrorsFromSdhcErrors(sdhcErrors).AsUint32;

    //
    // Acknowledge all interrupts/errors, since they have been
    // recorded and will be handled in the ISR DPC
    //
    thisPtr->writeRegisterNoFence(hsts);

    return TRUE;
} // SDHC::sdhcInterrupt (...)

_Use_decl_annotations_
void SDHC::sdhcRequestDpc (
    void* PrivateExtensionPtr,
    SDPORT_REQUEST* RequestPtr,
    ULONG Events,
    ULONG Errors
    )
{
    auto thisPtr = static_cast<SDHC*>(PrivateExtensionPtr);
    //
    // Since this SDHC is non-standard, we have to convert back from events and errors
    // understood by Sdport to those understood by this SDHC
    //
    _HSTS events{ thisPtr->getSdhcEventsFromSdportEvents(_SDPORT_EVENTS{ Events }).AsUint32 };
    _HSTS errors{ thisPtr->getSdhcErrorsFromSdportErrors(_SDPORT_ERRORS{ Errors }).AsUint32 };

    SDHC_LOG_TRACE(
        "(RequestPtr->RequiredEvents = 0x%lx, Events = 0x%lx, Errors = 0x%lx)",
        ULONG(RequestPtr->RequiredEvents),
        ULONG(events.AsUint32),
        ULONG(errors.AsUint32));

    //
    // Clear the request's required events if they have completed.
    //
    _HSTS* requiredEventsPtr = reinterpret_cast<_HSTS*>(&RequestPtr->RequiredEvents);
    requiredEventsPtr->AsUint32 &= ~events.AsUint32;

    //
    // WORKAROUND:
    // An SDHC bug in which deselecting the SDCard with CMD7
    // raises the cmd timeout error flag despite successful execution
    //
    if (errors.Fields.CmdTimeOut &&
        RequestPtr->Command.Index == SDCMD_SELECT_CARD) {
        errors.Fields.CmdTimeOut = 0;
    } // if

    if (errors.AsUint32) {
        RequestPtr->RequiredEvents = 0;
        thisPtr->completeRequest(
            RequestPtr,
            thisPtr->getErrorStatus(errors));
        return;
    } // if

    if (requiredEventsPtr->AsUint32 == 0) {

        bool isMultiBlockPioTransfer =
            ((RequestPtr->Type == SdRequestTypeStartTransfer) &&
            (RequestPtr->Command.TransferMethod == SdTransferMethodPio) &&
                ((RequestPtr->Command.TransferType == SdTransferTypeMultiBlock) ||
                (RequestPtr->Command.TransferType == SdTransferTypeMultiBlockNoStop)));

        //
        // A multi-block PIO transfer always gets postfixed with a STOP_TRANSMISSION
        // in either failure or success. This CMD on completion generates a busy signal
        // interrupt that leads to this DPC. We complete the request with whatever
        // request status set by the transfer worker in RequestPtr->Status
        //
        if (!isMultiBlockPioTransfer) {
            RequestPtr->Status = STATUS_SUCCESS;
        } else if (!events.Fields.BusyIrpt) {
            thisPtr->updateAllRegistersDump();
            SDHC_LOG_ASSERTION(
                "A multi-block transfer DPC is expected to get generated on a busy signal only");
        } // iff

        thisPtr->completeRequest(RequestPtr, RequestPtr->Status);
    } // if

} // SDHC::sdhcRequestDpc (...)

_Use_decl_annotations_
NTSTATUS SDHC::sdhcIssueRequest (
    void* PrivateExtensionPtr,
    SDPORT_REQUEST* RequestPtr
    )
{
    auto thisPtr = static_cast<SDHC*>(PrivateExtensionPtr);
    SDHC_LOG_TRACE(
        "(RequestPtr->Type = %lu, RequestPtr->Command.Index = %lu, RequestPtr->Command.Argument = 0x%lx)",
        ULONG(RequestPtr->Type),
        ULONG(RequestPtr->Command.Index),
        ULONG(RequestPtr->Command.Argument));

    NTSTATUS status;

    switch (RequestPtr->Type) {
    case SdRequestTypeCommandNoTransfer:
    case SdRequestTypeCommandWithTransfer:
    {
        status = thisPtr->sendRequestCommand(RequestPtr);
        if (!NT_SUCCESS(status)) {
            thisPtr->updateAllRegistersDump();
            SDHC_LOG_ERROR(
                "thisPtr->sendRequestCommand(...) failed. (status = %!STATUS!)",
                status);
            return status;
        } // if

#if ENABLE_PERFORMANCE_LOGGING

        if (RequestPtr->Type == SdRequestTypeCommandWithTransfer) {
            RtlZeroMemory(&thisPtr->currRequestStats, sizeof(_REQUEST_STATISTICS));
            thisPtr->currRequestStats.StartTimestamp = KeQueryPerformanceCounter(NULL);
            thisPtr->currRequestStats.FsmStateMinWaitTimeUs = MAXLONGLONG;
            thisPtr->currRequestStats.BlockCount= RequestPtr->Command.BlockCount;
        } // if

#endif // ENABLE_PERFORMANCE_LOGGING

        break;
    }
    case SdRequestTypeStartTransfer:
        status = thisPtr->startTransfer(RequestPtr);
        if (!NT_SUCCESS(status)) {
            thisPtr->updateAllRegistersDump();
            SDHC_LOG_ERROR(
                "thisPtr->startTransfer(...) failed. (status = %!STATUS!)",
                status);
            return status;
        } // if
        break;

    default:
        SDHC_LOG_ASSERTION(
            "Unsupported SDPORT_REQUEST_TYPE value. (RequestPtr->Type = %lu)",
            ULONG(RequestPtr->Type));
        return STATUS_NOT_SUPPORTED;
    } // switch (RequestPtr->Type)

    //
    // SDPORT WORKAROUND:
    // Sdport is expecting STATUS_PENDING for successful request issuing even if the
    // request was successfully completed inline. It will figure out whether the request
    // was completed inline by checking if Request->Status is set to STATUS_SUCCESS
    //
    SDHC_ASSERT(status == STATUS_SUCCESS);
    return STATUS_PENDING;
} // SDHC::sdhcIssueRequest (...)

_Use_decl_annotations_
void SDHC::sdhcGetResponse (
    void* PrivateExtensionPtr,
    SDPORT_COMMAND* CommandPtr,
    void* ResponseBufferPtr
    )
{
    auto thisPtr = static_cast<SDHC*>(PrivateExtensionPtr);
    SDHC_LOG_TRACE(
        "(CommandPtr->Index = %lu, CommandPtr->Argument = 0x%lx)",
        ULONG(CommandPtr->Index),
        ULONG(CommandPtr->Argument));

    *reinterpret_cast<ULONG*>(ResponseBufferPtr) = 0;
    auto response = thisPtr->getCommandResponseFromType(CommandPtr->ResponseType);

    switch (response) {
    case _COMMAND_RESPONSE::LONG_136BIT:
    {
        ULONG longResponseBuffer[4];

        longResponseBuffer[0] = thisPtr->readRegister<_RSP0>().AsUint32;
        longResponseBuffer[1] = thisPtr->readRegister<_RSP1>().AsUint32;
        longResponseBuffer[2] = thisPtr->readRegister<_RSP2>().AsUint32;
        longResponseBuffer[3] = thisPtr->readRegister<_RSP3>().AsUint32;

        //
        // Shift the whole response buffer 8-bits right to strip down the
        // CRC and start bit, which in case of an SD compliant SDHC this is
        // not required, since the SDHC should take care of that per specs
        //
        RtlCopyMemory(
            ResponseBufferPtr,
            (reinterpret_cast<UCHAR*>(longResponseBuffer) + 1),
            sizeof(longResponseBuffer) - 1);

        break;
    } // case _COMMAND_RESPONSE::LONG_136BIT:

    case _COMMAND_RESPONSE::SHORT_48BIT:
        *(static_cast<ULONG*>(ResponseBufferPtr)) = thisPtr->readRegister<_RSP0>().AsUint32;
        break; // case _COMMAND_RESPONSE::SHORT_48BIT

    case _COMMAND_RESPONSE::NO:
        break; // case _COMMAND_RESPONSE::NO

    default:
        SDHC_LOG_ASSERTION("Unexpected response type value");
    } // switch (response)

} // SDHC::sdhcGetResponse (...)

_Use_decl_annotations_
void SDHC::sdhcToggleEvents (
    void* PrivateExtensionPtr,
    ULONG EventMask,
    BOOLEAN Enable
    )
{
    auto thisPtr = static_cast<SDHC*>(PrivateExtensionPtr);
    SDHC_LOG_TRACE(
        "(EventMask = 0x%lx, Enable = %lu)",
        ULONG(EventMask),
        ULONG(Enable));

    _SDPORT_EVENTS sdportEvents{ EventMask };
    _HSTS sdhcEvents{ thisPtr->getSdhcEventsFromSdportEvents(sdportEvents).AsUint32 };
    _HCFG hcfg = thisPtr->getInterruptSourcesFromEvents(sdhcEvents);

    //
    // Block and Data interrupts are internally managed by SDHC due to its non-standard
    // modes of operation
    //
    hcfg.Fields.BlockIrptEn = 0;
    hcfg.Fields.DataIrptEn = 0;

    //
    // It has been observed that ToggleEvents is called in 2 situations:
    // 1- Host soft-reset: All host interrupts will be enabled
    // 2- Request error recovery: All host interrupts will be disabled,
    //    acknowledged and re-enabled again
    //
    if (Enable) {
        (void)thisPtr->unmaskInterrupts(hcfg);
    } else {
        (void)thisPtr->maskInterrupts(hcfg);
    } // iff

} // SDHC::sdhcToggleEvents (...)

_Use_decl_annotations_
void SDHC::sdhcClearEvents (
    void* PrivateExtensionPtr,
    ULONG EventMask
    )
{
    auto thisPtr = static_cast<SDHC*>(PrivateExtensionPtr);
    SDHC_LOG_TRACE("()");

    thisPtr->writeRegisterNoFence(_HSTS{ EventMask & _HSTS::UINT32_IRPT_MASK });
} // SDHC::sdhcClearEvents (...)

_Use_decl_annotations_
void SDHC::sdhcSaveContext (
    void* /* PrivateExtensionPtr */
    )
{
    SDHC_LOG_TRACE("()");

} // SDHC::sdhcSaveContext (...)

_Use_decl_annotations_
void SDHC::sdhcRestoreContext (
    void* /* PrivateExtensionPtr */
    )
{
    SDHC_LOG_TRACE("()");

} // SDHC::sdhcRestoreContext (...)

_Use_decl_annotations_
NTSTATUS SDHC::sdhcInitialize (
    void* PrivateExtensionPtr,
    PHYSICAL_ADDRESS PhysicalBase,
    void* VirtualBasePtr,
    ULONG Length,
    BOOLEAN CrashdumpMode
    )
{
    auto thisPtr = new (PrivateExtensionPtr) SDHC(
            PhysicalBase,
            VirtualBasePtr,
            Length,
            CrashdumpMode);
    _Analysis_assume_(thisPtr);

    //
    // According to BCM2835 specs we can't query SDHC caps in runtime thus
    // we will hardcode these here to be exactly the same as queried through
    // ARASAN
    //
    thisPtr->sdhcCapabilities.SpecVersion = 0x02;
    thisPtr->sdhcCapabilities.MaximumOutstandingRequests = 1;
    thisPtr->sdhcCapabilities.MaximumBlockSize = 0x200;
    thisPtr->sdhcCapabilities.MaximumBlockCount = 0xFFFF;

    //
    // TODO : Integrate RPIQ mail box driver so SD port driver is able to query
    // for base clock actual value
    // For now use the 250MHz core clock specified in Config.txt
    //
    thisPtr->sdhcCapabilities.BaseClockFrequencyKhz = 250 * 1000;
    thisPtr->sdhcCapabilities.Supported.DriverTypeB = 1;

    //
    // The miniport will not receive STOP_TRANSMISSION requests from Sdport
    // and will be responsible to stop transmission on its own
    //
    thisPtr->sdhcCapabilities.Supported.AutoCmd12 = 1;
    thisPtr->sdhcCapabilities.Supported.AutoCmd23 = 1;

    thisPtr->sdhcCapabilities.Supported.HighSpeed = 1;
    thisPtr->sdhcCapabilities.Supported.Voltage33V = 1;

    //
    // Fast return, crashdump environment is running in CLOCK_LEVEL IRQL which
    // restricts the usage of threads and synchronization objects
    //
    if (thisPtr->crashdumpMode) {
        return STATUS_SUCCESS;
    } // if

    ExInitializeFastMutex(&thisPtr->outstandingRequestLock);

    KeInitializeEvent(
        &thisPtr->transferWorkerStartedEvt,
        NotificationEvent,
        FALSE);

    KeInitializeEvent(
        &thisPtr->transferWorkerDoIoEvt,
        SynchronizationEvent,
        FALSE);

    KeInitializeEvent(
        &thisPtr->transferWorkerShutdownEvt,
        NotificationEvent,
        FALSE);

    HANDLE transferThread;
    NTSTATUS status = PsCreateSystemThread(
        &transferThread,
        THREAD_ALL_ACCESS,
        NULL,
        NULL,
        NULL,
        &thisPtr->transferWorker,
        thisPtr);
    if (!NT_SUCCESS(status)) {
        thisPtr->updateAllRegistersDump();
        SDHC_LOG_ERROR(
            "Failed to create transfer worker thread. (status = %!STATUS!)",
            status);
        return status;
    } // if

    status = ObReferenceObjectByHandle(
        transferThread,
        THREAD_ALL_ACCESS,
        NULL,
        KernelMode,
        (PVOID*)&thisPtr->transferThreadObjPtr,
        NULL);
    SDHC_ASSERT(NT_SUCCESS(status));

    (void)ZwClose(transferThread);

    status = KeWaitForSingleObject(
        &thisPtr->transferWorkerStartedEvt,
        Executive,
        KernelMode,
        FALSE,
        NULL);
    if (!NT_SUCCESS(status)) {
        SDHC_LOG_ERROR(
            "Wait for transfer worker thread to start failed. (status = %!STATUS!)",
            status);
        return status;
    } // if

#if ENABLE_STATUS_SAMPLING

    KeInitializeEvent(
        &thisPtr->samplingStartedEvt,
        NotificationEvent,
        FALSE);

    thisPtr->shutdownSampling = 0;

    HANDLE statusSamplingThread;
    status = PsCreateSystemThread(
        &statusSamplingThread,
        THREAD_ALL_ACCESS,
        NULL,
        NULL,
        NULL,
        &thisPtr->sampleStatusWorker,
        thisPtr);
    if (!NT_SUCCESS(status)) {
        thisPtr->updateAllRegistersDump();
        SDHC_LOG_ERROR(
            "Failed to create status sampling thread. (status = %!STATUS!)",
            status);
        return status;
    } // if

    status = ObReferenceObjectByHandle(
        statusSamplingThread,
        THREAD_ALL_ACCESS,
        NULL,
        KernelMode,
        (PVOID*)&thisPtr->statusSamplingThreadObjPtr,
        NULL);
    SDHC_ASSERT(NT_SUCCESS(status));

    (void)ZwClose(statusSamplingThread);

    status = KeWaitForSingleObject(
        &thisPtr->samplingStartedEvt,
        Executive,
        KernelMode,
        FALSE,
        NULL);
    if (!NT_SUCCESS(status)) {
        InterlockedOr(&thisPtr->shutdownSampling, 1);
        SDHC_LOG_ERROR(
            "Wait for sampling status thread to start failed. (status = %!STATUS!)",
            status);
        return status;
    } // if

#endif // ENABLE_STATUS_SAMPLING

    return STATUS_SUCCESS;
} // SDHC::sdhcInitialize (...)

_Use_decl_annotations_
NTSTATUS SDHC::sdhcIssueBusOperation (
    void* PrivateExtensionPtr,
    SDPORT_BUS_OPERATION* BusOperationPtr
    )
{
    auto thisPtr = static_cast<SDHC*>(PrivateExtensionPtr);
    SDHC_LOG_TRACE(
        "(BusOperationPtr->Type = %lu)",
        ULONG(BusOperationPtr->Type));

    NTSTATUS status;
    _HCFG hcfg{ 0 };

    switch (BusOperationPtr->Type) {
    case SdResetHost:
        status = thisPtr->resetHost(BusOperationPtr->Parameters.ResetType);
        if (!NT_SUCCESS(status)) {
            thisPtr->updateAllRegistersDump();
            SDHC_LOG_ERROR(
                "thisPtr->sdResetHost(...) failed. (status = %!STATUS!, BusOperationPtr->Parameters.ResetType = %lu)",
                status,
                ULONG(BusOperationPtr->Parameters.ResetType));
            return status;
        } // if
        break;

    case SdSetClock:
        status = thisPtr->setClock(BusOperationPtr->Parameters.FrequencyKhz);
        if (!NT_SUCCESS(status)) {
            thisPtr->updateAllRegistersDump();
            SDHC_LOG_ERROR(
                "thisPtr->sdSetClock(...) failed. (status = %!STATUS!, BusOperationPtr->Parameters.FrequencyKhz = %lu)",
                status,
                ULONG(BusOperationPtr->Parameters.FrequencyKhz));
            return status;
        } // if
        break;

    case SdResetHw:
    case SdSetVoltage:
    case SdSetBusSpeed:
    case SdSetSignalingVoltage:
    case SdSetDriveStrength:
    case SdSetDriverType:
    case SdSetPresetValue:
    case SdSetBlockGapInterrupt:
    case SdExecuteTuning:
        SDHC_LOG_TRACE(
            "Ignored request for known unsupported bus operation. (BusOperationPtr->Type = %lu)",
            ULONG(BusOperationPtr->Type));
        return STATUS_SUCCESS;

    case SdSetBusWidth:
        thisPtr->readRegisterNoFence(&hcfg);
        if((BusOperationPtr->Parameters.BusWidth == SdBusWidth8Bit) || (BusOperationPtr->Parameters.BusWidth == SdBusWidth4Bit)) {
            hcfg.Fields.WideExtBus = 1;
        }
        if(BusOperationPtr->Parameters.BusWidth == SdBusWidth1Bit) {
            hcfg.Fields.WideExtBus = 0;
        }
        thisPtr->writeRegisterNoFence(hcfg);
        return STATUS_SUCCESS;
    default:
        SDHC_LOG_ASSERTION(
            "Ignored request for unsupported bus operation. (BusOperationPtr->Type = %lu)",
            ULONG(BusOperationPtr->Type));
        return STATUS_SUCCESS;
    } // switch (BusOperationPtr->Type)

    SDHC_ASSERT(status == STATUS_SUCCESS);
    return status;
} // SDHC::sdhcIssueBusOperation (...)

_Use_decl_annotations_
BOOLEAN SDHC::sdhcGetCardDetectState (
    void* /* PrivateExtensionPtr */
    )
{
    SDHC_LOG_TRACE("()");

    //
    // According to BCM2835 specs there is no way to detect card via controller.
    // Since we can only boot windows using SD card on BCM2835, we can safely
    // assume presence of the card in the slot.
    //
    return TRUE;
} // SDHC::sdhcGetCardDetectState (...)

_Use_decl_annotations_
BOOLEAN SDHC::sdhcGetWriteProtectState (
    void* /* PrivateExtensionPtr */
    )
{
    SDHC_LOG_TRACE("()");

    //
    // According to BCM2835 specs there is no way to detect write protection
    // state via controller - assuming non-protected state.
    //
    return FALSE;
} // SDHC::sdhcGetWriteProtectState (...)

_Use_decl_annotations_
NTSTATUS SDHC::sdhcPowerControlCallback(
    PSD_MINIPORT Miniport,
    LPCGUID PowerControlCode,
    PVOID InputBuffer,
    SIZE_T InputBufferSize,
    PVOID OutputBuffer,
    SIZE_T OutputBufferSize,
    PSIZE_T BytesReturned
)
{

    UNREFERENCED_PARAMETER(Miniport);
    UNREFERENCED_PARAMETER(PowerControlCode);
    UNREFERENCED_PARAMETER(InputBuffer);
    UNREFERENCED_PARAMETER(InputBufferSize);
    UNREFERENCED_PARAMETER(OutputBuffer);
    UNREFERENCED_PARAMETER(OutputBufferSize);
    UNREFERENCED_PARAMETER(BytesReturned);

    return STATUS_NOT_IMPLEMENTED;
}

_Use_decl_annotations_
void SDHC::sdhcCleanup (
    SD_MINIPORT* MiniportPtr
    )
{
    SDHC_LOG_TRACE("(MiniportPtr = 0x%p)", MiniportPtr);

    UCHAR slotCount = MiniportPtr->SlotCount;
    while (slotCount) {
        void* privateExtensionPtr =
            MiniportPtr->SlotExtensionList[--slotCount]->PrivateExtension;
        auto thisPtr = static_cast<SDHC*>(privateExtensionPtr);

        //
        // Signal transfer worker thread for shutdown
        //
        (void)KeSetEvent(&thisPtr->transferWorkerShutdownEvt, 0, FALSE);

        //
        // Wait for thread to terminate before dereferencing it
        //
        (void)KeWaitForSingleObject(
            thisPtr->transferThreadObjPtr,
            Executive,
            KernelMode,
            FALSE,
            NULL);

        ObDereferenceObject(thisPtr->transferThreadObjPtr);
        thisPtr->transferThreadObjPtr = nullptr;

#if ENABLE_STATUS_SAMPLING

        //
        // Signal worker thread for shutdown
        //
        InterlockedOr(&thisPtr->shutdownSampling, 1);

        //
        // Wait for thread to terminate before dereferencing it
        //
        (void)KeWaitForSingleObject(
            thisPtr->statusSamplingThreadObjPtr,
            Executive,
            KernelMode,
            FALSE,
            NULL);

        ObDereferenceObject(thisPtr->statusSamplingThreadObjPtr);
        thisPtr->statusSamplingThreadObjPtr = nullptr;

#endif // ENABLE_STATUS_SAMPLING

        thisPtr->~SDHC();
    } // while (slotCount)

    SDHC_LOG_CLEANUP();
} // SDHC::sdhcCleanup (...)

_Use_decl_annotations_
NTSTATUS SDHC::resetHost (
    SDPORT_RESET_TYPE ResetType
    ) throw ()
{
    SDHC_LOG_INFORMATION(
        "(ResetType = %lu)",
        ULONG(ResetType));

    if (!this->crashdumpMode) {
        ExAcquireFastMutex(&this->outstandingRequestLock);

        //
        // Succeeding to acquire the request lock has two possibilities:
        // 1- There is an outstanding transfer request not acquired by transfer yet,
        //    in this case we reclaim ownership of that request, so that the transfer
        //    worker will wake-up/acquire request lock and won't find a valid request
        //    leading it to ignore the DoIo event silently
        // 2- There isn't an outstanding transfer request for the worker to pick-up,
        //    in this case we are safe and no action needed
        //

        //
        // Try to acquire request ownership
        //
        SDPORT_REQUEST* requestPtr =
            reinterpret_cast<SDPORT_REQUEST*>(
                InterlockedExchangePointer(
                    reinterpret_cast<PVOID volatile *>(&this->outstandingRequestPtr),
                    nullptr));
        if (requestPtr) {
            SDHC_LOG_TRACE(
                "Acquired transfer request before reaching transfer worker (requestPtr = 0x%p)",
                requestPtr);
        } // if
    } // if

    NTSTATUS status;

    switch (ResetType) {
    case SdResetTypeAll:
    {
        //
        // Perform a soft-reset to return both SDCard and SDHC
        // interface to their default states
        //
        this->writeRegisterNoFence(_VDD{ 0 });

        //
        // Reset cmd and configuration
        //
        this->writeRegisterNoFence(_CMD{ 0 });
        this->writeRegisterNoFence(_HCFG{ 0 });

        //
        // Clear stale error and interrupt status
        //
        this->writeRegisterNoFence(_HSTS{ _HSTS::UINT32_EVENTS_AND_ERRORS_MASK });

        //
        // Power-on the host interface and FSM
        //
        _VDD vdd{ 0 };
        vdd.Fields.PowerOn = 1;
        this->writeRegisterNoFence(vdd);

        _HCFG hcfg{ 0 };

        //
        // Config FIFO word size to be 4-bytes, we know that all
        // transfers are multiple of 4, no padding/loss possible
        //
        hcfg.Fields.WideIntBus = 1;

        //
        // SDHC to use all bits of CDIV not only the 3 LSB bits to
        // be able to achieve low SD frequencies during initialization
        // phase with high core clock frequencies
        //
        hcfg.Fields.SlowCard = 1;
        this->writeRegisterNoFence(hcfg);

        //
        // Specify FIFO read/write thresholds, based on the fact that
        // SDHC FIFO size is 16 4-byte word when HCFG.WideIntBus=1
        // Specified threshold values are based on recommendations from
        // RPi foundation
        //
        _EDM edm; this->readRegisterNoFence(&edm);
        edm.Fields.ReadThreshold = 4;
        edm.Fields.WriteThreshold = 4;
        this->writeRegisterNoFence(edm);
        _HBCT hbct{ 0 };
        hbct.Fields.ByteCount = 512;
        this->writeRegisterNoFence(hbct);

        status = STATUS_SUCCESS;
        break;
    } // case SdResetTypeAll

    case SdResetTypeCmd:
    {
        _HSTS hsts;
        hsts.Fields.BusyIrpt = 1;
        this->writeRegisterNoFence(hsts);

        status = STATUS_SUCCESS;
        break;
    } // case SdResetTypeCmd

    case SdResetTypeDat:
    {
        // Clear FIFO
        _EDM edm; this->readRegisterNoFence(&edm);
        edm.Fields.ClearFifo = 1;
        this->writeRegisterNoFence(edm);

        // Acknowledge all interrupts
        this->writeRegisterNoFence(_HSTS{ _HSTS::UINT32_IRPT_MASK });

        status = STATUS_SUCCESS;
        break;
    } // case SdResetTypeDat

    default:
        SDHC_LOG_ASSERTION(
            "Unsupported SDPORT_RESET_TYPE. (ResetType = %ul)",
            ULONG(ResetType));
        status = STATUS_NOT_SUPPORTED;
    } // switch (ResetType)

    if (!this->crashdumpMode) {
        ExReleaseFastMutex(&this->outstandingRequestLock);
    } // if

    return status;
} // SDHC::resetHost(...)

_Use_decl_annotations_
NTSTATUS SDHC::setClock (
    ULONG FrequencyKhz
    ) throw ()
{
    if (!FrequencyKhz) return STATUS_INVALID_PARAMETER;

    //
    // Card clock fSDCLK is derived from core clock fcore_pclk as follows:
    // fSDCLK = fcore_pclk / (CDIV + 2)
    // Solving for CDIV:
    // CDIV = (fcore_pclk - (2 * fSDCLK)) / fSDCLK
    //
    ULONG coreClockFreqHz = (this->sdhcCapabilities.BaseClockFrequencyKhz * 1000);
    ULONG targetSdFreqHz = (FrequencyKhz * 1000);

    //
    // TODO: Consider improving precision of (coreClockFreqHz / FrequencyKhz - 2)
    //
    ULONG clockDiv = ((coreClockFreqHz - (2 * targetSdFreqHz)) / targetSdFreqHz);

    _CDIV cdiv{ 0 };
    cdiv.Fields.Clockdiv = clockDiv;
    this->writeRegisterNoFence(cdiv);

    ULONG actualSdFreqHz = (coreClockFreqHz / (clockDiv + 2));

    //
    // Specify Freq / _RWE_TIMEOUT_CLOCK_DIV as the read/write/erase timeout in seconds
    // e.g. If _RWE_TIMEOUT_CLOCK_DIV = 4, then the timeout is 1/4 second
    //
    _TOUT tout{ 0 };
    tout.Fields.Timeout = actualSdFreqHz / _RWE_TIMEOUT_CLOCK_DIV;
    this->writeRegisterNoFence(tout);

    SDHC_LOG_INFORMATION(
        "(CoreClock=%luHz, CDIV=%lu, SdClock Requested=%luHz, Actual=%luHz)",
        coreClockFreqHz,
        clockDiv,
        targetSdFreqHz,
        actualSdFreqHz);

    return STATUS_SUCCESS;
} // SDHC::setClock (...)

_Use_decl_annotations_
SDHC::_HCFG SDHC::unmaskInterrupts (
    _HCFG Mask
    ) throw ()
{
    _HCFG oldHcfg; this->readRegisterNoFence(&oldHcfg);
    _HCFG newHcfg = oldHcfg;
    newHcfg.AsUint32 |= (Mask.AsUint32 & _HCFG::UINT32_IRPT_EN_MASK);
    this->writeRegisterNoFence(newHcfg);
    return oldHcfg;
} // SDHC::unmaskInterrupts (...)

_Use_decl_annotations_
SDHC::_HCFG SDHC::maskInterrupts (
    _HCFG Mask
    ) throw ()
{
    _HCFG oldHcfg; this->readRegisterNoFence(&oldHcfg);
    _HCFG newHcfg = oldHcfg;
    newHcfg.AsUint32 &= ~(Mask.AsUint32 & _HCFG::UINT32_IRPT_EN_MASK);
    this->writeRegisterNoFence(newHcfg);
    return oldHcfg;
} // SDHC::maskInterrupts (...)i

_Use_decl_annotations_
NTSTATUS SDHC::sendRequestCommand (
    SDPORT_REQUEST* RequestPtr
    ) throw ()
{
    NTSTATUS status;

    RequestPtr->RequiredEvents = 0;

    //
    // Initialize transfer parameters if this command is a data command.
    //
    if (RequestPtr->Type == SdRequestTypeCommandWithTransfer) {
        switch (RequestPtr->Command.TransferMethod) {
        case SdTransferMethodPio:
            status = this->prepareTransferPio(RequestPtr);
            if (!NT_SUCCESS(status)) {
                this->updateAllRegistersDump();
                SDHC_LOG_ERROR(
                    "this->prepareTransferPio(...) failed. (status = %!STATUS!)",
                    status);
                return status;
            } // if
            break; // case SdTransferMethodPio

        default:
            SDHC_LOG_ASSERTION(
                "Unsupported SDPORT_TRANSFER_METHOD. (RequestPtr->Command.TransferMethod = %lu)",
                ULONG(RequestPtr->Command.TransferMethod));
            return STATUS_NOT_SUPPORTED;
        } // switch (RequestPtr->Command.TransferMethod)
    } // if

    _CMD cmd = this->buildCommand(
        RequestPtr->Command.Index,
        RequestPtr->Command.TransferDirection,
        RequestPtr->Command.ResponseType);
    {
        _HSTS* requiredEventsPtr = reinterpret_cast<_HSTS*>(&RequestPtr->RequiredEvents);
        requiredEventsPtr->Fields.BusyIrpt = cmd.Fields.BusyCmd;
    }

    _ARG arg = { RequestPtr->Command.Argument };

    //
    // WORKAROUND:
    // Data transfers require FIFO ready signal (i.e Data interrupt)
    // to start reading/writing to the SDHC FIFO
    // Note: Data interrupt gets disabled on first occurrence in the ISR
    // that's why it gets re-enabled again here before issuing the cmd
    //
    // It is generally unsafe to unmask an interrupt in a non-ISR synchronized
    // routine like this one, but we know that it is safe in our case since
    // host FSM by this point is settled down
    //
    _HSTS requiredEvents = { RequestPtr->RequiredEvents };
    if (requiredEvents.Fields.DataFlag) {
        _HCFG irptMask{ 0 };
        irptMask.Fields.DataIrptEn = 1;
        (void)this->unmaskInterrupts(irptMask);
    } // if

    bool waitCompletion = (RequestPtr->RequiredEvents == 0);
    status = this->sendCommandInternal(cmd, arg, waitCompletion);

    //
    // In case this request had no required events, then sendCommandInternal
    // was a blocking call that didn't return until command finished execution
    // after which we will complete the request inline here before returning
    //
    if (waitCompletion) {
        this->completeRequest(RequestPtr, status);
        if (!NT_SUCCESS(status)) {
            return status;
        } // if
    } // if

    return STATUS_SUCCESS;
} // SDHC::sendRequestCommand (...)

_Use_decl_annotations_
NTSTATUS SDHC::sendNoTransferCommand (
    UCHAR Cmd,
    ULONG Arg,
    SDPORT_TRANSFER_DIRECTION TransferDirection,
    SDPORT_RESPONSE_TYPE ResponseType,
    bool WaitCompletion
    ) throw ()
{
     _ARG arg = { Arg };

    _CMD cmd = this->buildCommand(
        Cmd,
        TransferDirection,
        ResponseType);

    return this->sendCommandInternal(cmd, arg, WaitCompletion);
} // SDHC::sendNoTransferCommand (...)

NTSTATUS SDHC::sendCommandInternal (
    _CMD Cmd,
    _ARG Arg,
    bool WaitCompletion
    )
{
    NTSTATUS status = this->waitForLastCommandCompletion();
    if (!NT_SUCCESS(status)) {
        this->updateAllRegistersDump();
        SDHC_LOG_ERROR(
            "this->waitForLastCommandCompletion() failed. (status = %!STATUS!)",
            status);
        return status;
    } // if

    //
    // Start execution from a clean state
    //
    {
        _HSTS hsts{ 0 };
        hsts.AsUint32 |= _HSTS::UINT32_ERROR_MASK;
        this->writeRegisterNoFence(hsts);

        //
        // Drain read FIFO before starting a new read command
        //
        if (Cmd.Fields.ReadCmd) {
            status = this->drainReadFifo();
            if (!NT_SUCCESS(status)) {
                this->updateAllRegistersDump();
                this->readRegisterNoFence(&hsts);
                SDHC_LOG_ERROR(
                    "Timed-out draining Read FIFO (hsts.AsUint32 = 0x%lx)",
                    ULONG(hsts.AsUint32));
                return status;
            } // if
        } // if
    } // Start execution from a clean state

    //
    // Send a new command for execution
    //
    this->writeRegisterNoFence(Arg);
    this->writeRegisterNoFence(Cmd);

    //
    // Waiting for completion means that a command execution has to come
    // to an end, either a successful or failed end.
    //
    if (WaitCompletion) {
        status = this->waitForLastCommandCompletion();
        if (!NT_SUCCESS(status)) {
            this->updateAllRegistersDump();
            SDHC_LOG_ERROR(
                "this->waitForLastCommandCompletion() failed. (status = %!STATUS!)",
                status);
            return status;
        } // if

        status = this->getLastCommandCompletionStatus();
        if (!NT_SUCCESS(status)) {
            //
            // Need to read status register again to get real error code
            //
            _HSTS hsts{ 0 };
            this->readRegisterNoFence(&hsts);
            //
            // CMD1 alwary return CRC7 error on EMMC device
            // Clear all ERROR mask set and return Success
            // Temp clear error flags for read/write test command
            //
            if((Cmd.Fields.Command == 0x1) && (hsts.Fields.Crc7Error))
               {
                //
                // CMD1 alwary return CRC7 error on EMMC device
                // Clear all ERROR mask set and return Success
                //
                this->writeRegisterNoFence(hsts);
                SDHC_LOG_ERROR(
                    "Ignore CRC7 error for CMD1");
                return STATUS_SUCCESS;
            } else {
            this->updateAllRegistersDump();
            SDHC_LOG_ERROR(
                "Device command failed. (cmd.Fields.Command = 0x%lx, status = %!STATUS!)",
                ULONG(Cmd.Fields.Command),
                status);
            return status;
            }
        } // if
    } // if

    return STATUS_SUCCESS;
} // SDHC::sendCommandInternal (...)

_Use_decl_annotations_
NTSTATUS SDHC::startTransfer (
    SDPORT_REQUEST* RequestPtr
    ) throw ()
{
    switch (RequestPtr->Command.TransferMethod) {
    case SdTransferMethodPio:
    {
        SDHC_LOG_TRACE(
            "(RequestPtr->Command.TransferDirection = %lu, RequestPtr->RequiredEvents = 0x%lx, RequestPtr->Command.BlockSize = %lu, RequestPtr->Command.BlockCount = %lu)",
            ULONG(RequestPtr->Command.TransferDirection),
            ULONG(RequestPtr->RequiredEvents),
            ULONG(RequestPtr->Command.BlockSize),
            ULONG(RequestPtr->Command.BlockCount));

            switch (RequestPtr->Command.TransferDirection) {
            case SdTransferDirectionRead:
            case SdTransferDirectionWrite:
                return this->startTransferPio(RequestPtr);

            default:
            SDHC_LOG_ASSERTION(
                "Unsupported SDPORT_TRANSFER_DIRECTION. (RequestPtr->Command.TransferDirection = %lu)",
                ULONG(RequestPtr->Command.TransferDirection));
            return STATUS_NOT_SUPPORTED;
        } // switch (RequestPtr->Command.TransferDirection)

    } // case SdTransferMethodPio

    default:
        SDHC_LOG_ASSERTION(
            "Unsupported SDPORT_TRANSFER_METHOD. (RequestPtr->Command.TransferMethod = %lu)",
            ULONG(RequestPtr->Command.TransferMethod));
        return STATUS_NOT_SUPPORTED;
    } // switch (RequestPtr->Command.TransferMethod)

} // SDHC::startTransfer (...)

_Use_decl_annotations_
NTSTATUS SDHC::startTransferPio (
    SDPORT_REQUEST* RequestPtr
    ) throw ()
{
    SDHC_ASSERT(
        (RequestPtr->Command.TransferDirection == SdTransferDirectionRead) ||
        (RequestPtr->Command.TransferDirection == SdTransferDirectionWrite));
    SDHC_ASSERT(RequestPtr->Command.BlockCount);
    SDHC_ASSERT(
        (RequestPtr->Command.TransferType == SdTransferTypeSingleBlock) ||
        (RequestPtr->Command.TransferType == SdTransferTypeMultiBlock) ||
        (RequestPtr->Command.TransferType == SdTransferTypeMultiBlockNoStop));

    if ((RequestPtr->Command.TransferType == SdTransferTypeMultiBlock) ||
        (RequestPtr->Command.TransferType == SdTransferTypeMultiBlockNoStop)) {

        //
        // Specify we require a busy signal to complete multi-block request after issuing
        // STOP_TRANSMISSION which will happen in the transfer worker
        //
        _HSTS* requiredEventsPtr = reinterpret_cast<_HSTS*>(&RequestPtr->RequiredEvents);
        requiredEventsPtr->Fields.BusyIrpt = 1;
    }

    //
    // In Crashdump mode, inline perform and complete transfer requests
    //
    if (this->crashdumpMode) {

        if (RequestPtr->Command.TransferType == SdTransferTypeSingleBlock) {
            NTSTATUS status = this->transferSingleBlockPio(RequestPtr);
            this->completeRequest(RequestPtr, status);
            if (!NT_SUCCESS(status)) {
                return status;
            } // if
        } else {
            //
            // Perform multi-block transfers and complete them inline since there is no
            // transfer worker in the crashdump mode
            //
            NTSTATUS status = this->transferMultiBlockPio(RequestPtr);
            SDHC_ASSERT(status == RequestPtr->Status);
            if (!NT_SUCCESS(status)) {
                return status;
            } // if
        } // if

    } else {
        //
        // Wake-up the transfer worker thread to do IO and return to Sdport with
        // STATUS_PENDING to indicate that completion will happen asynchronously
        //
        if (InterlockedCompareExchangePointer(
                reinterpret_cast<PVOID volatile *>(&this->outstandingRequestPtr),
                RequestPtr,
                nullptr)) {
            this->updateAllRegistersDump();
            SDHC_LOG_ASSERTION(
                "A stale request not acquired by transfer worker");
            return STATUS_DEVICE_PROTOCOL_ERROR;
        } // if

        (void)KeSetEvent(&this->transferWorkerDoIoEvt, 0, FALSE);
    } // iff

    return STATUS_SUCCESS;
} // SDHC::startTransferPio (...)

_Use_decl_annotations_
NTSTATUS SDHC::transferSingleBlockPio (
    SDPORT_REQUEST* RequestPtr
    ) throw ()
{
    NTSTATUS status;

    switch (RequestPtr->Command.TransferDirection) {
    case SdTransferDirectionRead:
        status = this->readFromFifo(
            RequestPtr->Command.DataBuffer,
            RequestPtr->Command.BlockSize);
        break;

    case SdTransferDirectionWrite:
        status = this->writeToFifo(
            RequestPtr->Command.DataBuffer,
            RequestPtr->Command.BlockSize);
        //
        // It is not mentioned in the datasheet, but it was observed that in case
        // of successful write, we have to wait for the correct SDHC FSM state before
        // writing the next block, otherwise a random SDCard corruption can happen
        // The chosen states below to wait on were decided by experimentation
        //
        if (NT_SUCCESS(status)) {
            if (RequestPtr->Command.TransferType == SdTransferTypeSingleBlock) {
                status = this->waitForFsmState(_EDM::UINT32_FSM_DATAMODE);
            } else {
                status = this->waitForFsmState(_EDM::UINT32_FSM_WRITESTART1);
            } //iff
        } // if
        break;

    default:
        SDHC_LOG_ASSERTION(
            "Unsupported SDPORT_TRANSFER_DIRECTION. (RequestPtr->Command.TransferDirection = %lu)",
            ULONG(RequestPtr->Command.TransferDirection));
        status = STATUS_NOT_SUPPORTED;
    } // switch (RequestPtr->Command.TransferDirection)

    return status;
} // SDHC::transferSingleBlockPio (...)

_Use_decl_annotations_
NTSTATUS SDHC::transferMultiBlockPio (
    SDPORT_REQUEST* RequestPtr
    ) throw ()
{
    SDHC_ASSERT(RequestPtr->Type == SdRequestTypeStartTransfer);
    SDHC_ASSERT(RequestPtr->Command.TransferMethod == SdTransferMethodPio);
    SDHC_ASSERT(
        (RequestPtr->Command.TransferDirection == SdTransferDirectionRead) ||
        (RequestPtr->Command.TransferDirection == SdTransferDirectionWrite));
    SDHC_ASSERT(
        (RequestPtr->Command.TransferType == SdTransferTypeMultiBlock) ||
        (RequestPtr->Command.TransferType == SdTransferTypeMultiBlockNoStop));

    NTSTATUS status = STATUS_SUCCESS;

    while (RequestPtr->Command.BlockCount) {
        status = this->transferSingleBlockPio(RequestPtr);
        if (!NT_SUCCESS(status)) {
            break;
        } // if

        RequestPtr->Command.DataBuffer += RequestPtr->Command.BlockSize;
        --RequestPtr->Command.BlockCount;
    } // while (RequestPtr->Command.BlockCount)

    //
    // The status with which we will complete the request in the DPC
    //
    RequestPtr->Status = status;

    //
    // WORKAROUND:
    // We have to issue STOP_TRANSMISSION even in case of IO failure to return
    // the SDCard to tran state, otherwise the SDCard FSM will get stuck in
    // either rcv/data state depending on whether it was reading or writing
    //
    // STOP_TRANSMISSION once completed will cause a busy interrupt to fire
    // that will lead the request to complete in the DPC
    //
    NTSTATUS cmdStatus = this->stopTransmission(false);
    if (NT_SUCCESS(status) &&
        !NT_SUCCESS(cmdStatus)) {
        //
        // It is not safe to complete the request with failure here due to possibility of
        // race-condition with Sdport. A failure to issue STOP_TRANSMISSION means that the
        // SDHC and/or SDCard are in a very bad state and a host reset bus operation is
        // required. If we don't complete request the Sdport will timeout the request and
        // issue an error recovery that leads to host reset bus operation, which is the
        // required mitigation behavior
        //
        this->updateAllRegistersDump();
        SDHC_LOG_ERROR("Failed to stop transmission after a successful transfer");
    } // if

    return status;
} // SDHC::transferMultiBlockPio (...)

_Use_decl_annotations_
void SDHC::completeRequest (
    SDPORT_REQUEST* RequestPtr,
    NTSTATUS Status
    )
{
    //
    // Legal request completion statuses expected by Sdport
    //
    SDHC_ASSERT(
        (Status == STATUS_SUCCESS) ||
        (Status == STATUS_MORE_PROCESSING_REQUIRED) ||
        (Status == STATUS_IO_TIMEOUT) ||
        (Status == STATUS_CRC_ERROR) ||
        (Status == STATUS_DEVICE_DATA_ERROR) ||
        (Status == STATUS_DEVICE_PROTOCOL_ERROR) ||
        (Status == STATUS_DEVICE_POWER_FAILURE) ||
        (Status == STATUS_IO_DEVICE_ERROR));

#if ENABLE_PERFORMANCE_LOGGING

    LARGE_INTEGER hpcFreqHz;
    LARGE_INTEGER requestEndTimestamp = KeQueryPerformanceCounter(&hpcFreqHz);

#endif // ENABLE_PERFORMANCE_LOGGING

    //
    // This SDHC is not a standard host, we need to be very aggressive about state
    // integrity, and what host state to expect on claiming successful completion
    //
    if (NT_SUCCESS(Status)) {

        //
        // Generally, we shouldn't be completing successfully in case of HW errors
        //
        _HSTS hsts; this->readRegisterNoFence(&hsts);
        if (hsts.AsUint32 & _HSTS::UINT32_ERROR_MASK) {
            this->updateAllRegistersDump();
            SDHC_LOG_CRITICAL_ERROR(
                "Completing request successfully despite HW errors reported");
        } // if

        //
        // On completing a transfer request, there should not be any on-going IO activity
        // on the SDCard, done is done
        //
        _EDM edm; this->readRegisterNoFence(&edm);
        if ((RequestPtr->Type ==  SdRequestTypeStartTransfer) &&
            (edm.Fields.StateMachine != _EDM::UINT32_FSM_IDENTMODE) &&
            (edm.Fields.StateMachine != _EDM::UINT32_FSM_DATAMODE)) {
            this->updateAllRegistersDump();
            SDHC_LOG_CRITICAL_ERROR(
                "Completing request successfully despite HW FSM not in expected state");
        } // if

    }  // iff (NT_SUCCESS(Status))

    if (RequestPtr->Type == SdRequestTypeStartTransfer) {

#if ENABLE_PERFORMANCE_LOGGING

        //
        // Collect SDCH wide statistics tied to inserted SDCard
        // Since RaspberryPi uses SDCard as the boot media, the SDCard will always stay
        // inserted while the OS is running
        //
        this->sdhcStats.TotalFsmStateWaitTimeUs += this->currRequestStats.FsmStateWaitTimeUs;
        this->sdhcStats.LongFsmStateWaitCount += this->currRequestStats.LongFsmStateWaitCount;
        this->sdhcStats.TotalLongFsmStateWaitTimeUs += this->currRequestStats.LongFsmStateWaitTimeUs;

        if ((RequestPtr->Command.TransferDirection == SdTransferDirectionWrite)) {
            this->sdhcStats.BlocksWrittenCount += this->currRequestStats.BlockCount;

            if (RequestPtr->Command.Length == PAGE_SIZE) {
                ++this->sdhcStats.PageSized4KWritesCount;
            } // if
        } // if

        //
        // Assume no overhead with HighSpeed mode 25MB/s where MB in the transfer rating of SDCards
        // means 25 * 1000 * 1000 byte according to SD specs
        //
        LONGLONG optimalRequestServiceTimeUs =
            (LONGLONG(RequestPtr->Command.Length) * 1000000ll) / 25000000ll;

        auto& logData = this->currRequestStats;
        LONGLONG requestServiceTimeUs;
        requestServiceTimeUs = requestEndTimestamp.QuadPart - logData.StartTimestamp.QuadPart;
        requestServiceTimeUs *= 1000000ll;
        requestServiceTimeUs /= hpcFreqHz.QuadPart;

        //
        // Transfers taking less than 1us to complete will not have full info, their log will show:
        // Actual: 0us @ 0MB/s, Utilization = 0%
        //
        LONGLONG actualTransferRateMBs{ 0 };
        LONGLONG utilization = 0;

        if (requestServiceTimeUs > 0) {
            actualTransferRateMBs =
                (LONGLONG(RequestPtr->Command.Length) * 1000000ll) / (requestServiceTimeUs * 1024ll * 1024ll);
            utilization = (optimalRequestServiceTimeUs * 100ll) / requestServiceTimeUs;
        } // if

        LONGLONG fifoIoTimeUs = this->currRequestStats.FifoIoTimeTicks;
        fifoIoTimeUs *= 1000000ll;
        fifoIoTimeUs /= hpcFreqHz.QuadPart;

        SDHC_LOG_INFORMATION(
            "%s%d %s(0x%lx, %luB) %lldus %lldMB/s, Util:%lld%%, "
            "Fifo Time:%lldus, "
            "Fifo Waits:%lldus Max:%lldus Avg:%lldus, "
            "Fsm Waits:%lldus Max:%lldus Avg:%lldus Min:%lldus. "
            "(RequestPtr = 0x%p, RequestPtr->Status = %!STATUS!)",
            ((RequestPtr->Command.Class == SdCommandClassApp) ? "ACMD" : "CMD"),
            RequestPtr->Command.Index,
            ((RequestPtr->Command.TransferDirection == SdTransferDirectionRead) ? "Read" : "Write"),
            RequestPtr->Command.Argument,
            RequestPtr->Command.Length,
            requestServiceTimeUs,
            actualTransferRateMBs,
            utilization,
            fifoIoTimeUs,
            logData.FifoWaitTimeUs,
            logData.FifoMaxWaitTimeUs,
            ((logData.FifoWaitCount > 0) ?
                (logData.FifoWaitTimeUs / LONGLONG(logData.FifoWaitCount)) : 0ll),
            logData.FsmStateWaitTimeUs,
            logData.FsmStateMaxWaitTimeUs,
            ((logData.FsmStateWaitCount > 0) ?
                (logData.FsmStateWaitTimeUs / LONGLONG(logData.FsmStateWaitCount)) : 0ll),
            (logData.FsmStateMinWaitTimeUs == MAXLONGLONG ? 0ll : logData.FsmStateMinWaitTimeUs),
            RequestPtr,
            Status);

        SDHC_LOG_INFORMATION(
            "SDHC Stats: Fsm Waits:%lldus, #Long Waits:%lld %lldus, #Block Writes:%lld, #4K Writes:%lld",
            this->sdhcStats.TotalFsmStateWaitTimeUs,
            this->sdhcStats.LongFsmStateWaitCount,
            this->sdhcStats.TotalLongFsmStateWaitTimeUs,
            this->sdhcStats.BlocksWrittenCount,
            this->sdhcStats.PageSized4KWritesCount);

#else

        SDHC_LOG_INFORMATION(
            "%s%d %s(0x%lx, %luB) (RequestPtr = 0x%p, RequestPtr->Status = %!STATUS!)",
            ((RequestPtr->Command.Class == SdCommandClassApp) ? "ACMD" : "CMD"),
            RequestPtr->Command.Index,
            ((RequestPtr->Command.TransferDirection == SdTransferDirectionRead) ? "Read" : "Write"),
            RequestPtr->Command.Argument,
            RequestPtr->Command.Length,
            RequestPtr,
            Status);

#endif // ENABLE_PERFORMANCE_LOGGING

    } else if (RequestPtr->Type != SdRequestTypeCommandWithTransfer) {
        SDHC_LOG_INFORMATION(
            "%s%d (RequestPtr = 0x%p, RequestPtr->Status = %!STATUS!)",
            ((RequestPtr->Command.Class == SdCommandClassApp) ? "ACMD" : "CMD"),
            RequestPtr->Command.Index,
            RequestPtr,
            Status);
    } // iff

    RequestPtr->Status = Status;
    ::SdPortCompleteRequest(RequestPtr, Status);

} // SDHC::completeRequest (...)

SDHC::_COMMAND_RESPONSE SDHC::getCommandResponseFromType (
    SDPORT_RESPONSE_TYPE ResponseType
    ) throw ()
{
    switch (ResponseType) {
    case SdResponseTypeR1:
    case SdResponseTypeR3:
    case SdResponseTypeR4:
    case SdResponseTypeR5:
    case SdResponseTypeR6:
    case SdResponseTypeR1B:
    case SdResponseTypeR5B:
        return _COMMAND_RESPONSE::SHORT_48BIT;

    case SdResponseTypeR2:
        return _COMMAND_RESPONSE::LONG_136BIT;

    case SdResponseTypeNone:
        return _COMMAND_RESPONSE::NO;

    default:
        SDHC_LOG_ASSERTION("Invalid response type");
        return _COMMAND_RESPONSE::NO;
    } // switch (CommandPtr->ResponseType)

} // SDHC::getCommandResponseType (...)

SDHC::_CMD SDHC::buildCommand (
    UCHAR Command,
    SDPORT_TRANSFER_DIRECTION TransferDirection,
    SDPORT_RESPONSE_TYPE ResponseType
    ) throw ()
{
    _CMD cmd{ 0 };

    cmd.Fields.Command = Command;
    cmd.Fields.ResponseCmd = UCHAR(this->getCommandResponseFromType(ResponseType));

    if (ResponseType == SdResponseTypeR1B) {
        cmd.Fields.BusyCmd = 1;
    } // if

    switch (TransferDirection) {
    case SdTransferDirectionRead:
        cmd.Fields.ReadCmd = 1;
        break;

    case SdTransferDirectionWrite:
        cmd.Fields.WriteCmd = 1;
        break;
    } // switch (TransferDirection)

    cmd.Fields.NewFlag = 1;

    return cmd;
} // SDHC::buildCommand (...)

NTSTATUS SDHC::prepareTransferPio (
    SDPORT_REQUEST* RequestPtr
    ) throw ()
{
    _HSTS* requiredEventsPtr = reinterpret_cast<_HSTS*>(&RequestPtr->RequiredEvents);
    requiredEventsPtr->Fields.DataFlag = 1;

    SDHC_ASSERT(RequestPtr->Type == SdRequestTypeCommandWithTransfer);
    switch (RequestPtr->Command.TransferDirection) {
    case SdTransferDirectionRead:
    case SdTransferDirectionWrite:
        break;

    default:
        SDHC_LOG_ASSERTION(
            "Unsupported SDPORT_TRANSFER_DIRECTION. (RequestPtr->Command.TransferDirection = %lu)",
            ULONG(RequestPtr->Command.TransferDirection));
        return STATUS_NOT_SUPPORTED;
    } // switch (RequestPtr->Command.TransferDirection)

    _HBCT hbct{ 0 };
    hbct.Fields.ByteCount = RequestPtr->Command.BlockSize;
    this->writeRegisterNoFence(hbct);

    _HBLC hblc{ 0 };
    this->writeRegisterNoFence(hblc);

    return STATUS_SUCCESS;
} // SDHC::prepareTransferPio ()

NTSTATUS SDHC::getErrorStatus (
    _HSTS Hsts
    ) throw ()
{
    if (Hsts.Fields.FifoError) {
        return STATUS_DEVICE_DATA_ERROR;
    } else if ((Hsts.Fields.Crc7Error) || (Hsts.Fields.Crc16Error)) {
        return STATUS_CRC_ERROR;
    } else if ((Hsts.Fields.CmdTimeOut) || (Hsts.Fields.RewTimeOut)) {
        return STATUS_IO_TIMEOUT;
    } else {
        return STATUS_IO_DEVICE_ERROR;
    } // iff

} // SDHC::getErrorStatus (...)

NTSTATUS SDHC::getLastCommandCompletionStatus () throw ()
{
    _CMD cmd; this->readRegisterNoFence(&cmd);
    SDHC_ASSERT("Command still executing" && !cmd.Fields.NewFlag);

    if (cmd.Fields.FailFlag) {
        _HSTS hsts; this->readRegisterNoFence(&hsts);
        return this->getErrorStatus(hsts);
    } // if

    return STATUS_SUCCESS;
} // SDHC::getLastCommandCompletionStatus (...)

_Use_decl_annotations_
VOID SDHC::transferWorker (
    PVOID ContextPtr
    )
{
    auto thisPtr = static_cast<SDHC*>(ContextPtr);

    //
    // This worker thread is running in PASSIVE_LEVEL doing polling which means that we have
    // a big chance to be scheduled out during polling. We want to reduce this undesirable
    // effect by restricting the thread to be scheduled only on the secondary cores and give
    // it a thread priority boost
    //
    KPRIORITY oldPriority = KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);
    (void)thisPtr->restrictCurrentThreadToSecondaryCores();

    SDHC_LOG_INFORMATION(
        "Thread startup - running on CPU%lu with boosted priority from %lu to %lu",
        KeGetCurrentProcessorNumberEx(NULL),
        ULONG(oldPriority),
        KeQueryPriorityThread(KeGetCurrentThread()));

    PVOID waitEvents[] = { &thisPtr->transferWorkerDoIoEvt, &thisPtr->transferWorkerShutdownEvt };
    enum : NTSTATUS {
        _WAIT_DO_IO_EVENT = STATUS_WAIT_0,
        _WAIT_SHUTDOWN_EVENT = STATUS_WAIT_1
    };

    (void)KeSetEvent(&thisPtr->transferWorkerStartedEvt, 0, FALSE);

    for (;;) {

        NTSTATUS waitStatus = KeWaitForMultipleObjects(
            ULONG(ARRAYSIZE(waitEvents)),
            waitEvents,
            WaitAny,
            Executive,
            KernelMode,
            FALSE,
            NULL,
            NULL);

        if (waitStatus == _WAIT_DO_IO_EVENT) {

            ExAcquireFastMutex(&thisPtr->outstandingRequestLock);

            //
            // Try to acquire request ownership
            //
            SDPORT_REQUEST* requestPtr =
                reinterpret_cast<SDPORT_REQUEST*>(
                    InterlockedExchangePointer(
                        reinterpret_cast<PVOID volatile *>(&thisPtr->outstandingRequestPtr),
                        nullptr));
            if (!requestPtr) {
                SDHC_LOG_WARNING("Ignoring DoIo event, found no outstanding request to service");
                ExReleaseFastMutex(&thisPtr->outstandingRequestLock);
                continue;
            } // if

            SDHC_ASSERT(KeGetCurrentProcessorNumberEx(NULL) != 0);
            SDHC_LOG_TRACE(
                "Started servicing transfer request on CPU%lu (requestPtr = 0x%p)",
                KeGetCurrentProcessorNumberEx(NULL),
                requestPtr);

            if (requestPtr->Command.TransferType == SdTransferTypeSingleBlock) {
                //
                // Single block transfers do not require a STOP_TRANSMISSION, and hence
                // completing the request inline is appropriate
                //
                NTSTATUS status = thisPtr->transferSingleBlockPio(requestPtr);
                thisPtr->completeRequest(requestPtr, status);
            } else {
                //
                // Multi block transfers require a STOP_TRANSMISSION, and hence the
                // request completion will happen async in the STOP_TRANSMISSION command
                // completion DPC
                //
                (void)thisPtr->transferMultiBlockPio(requestPtr);
            } // iff

            SDHC_LOG_TRACE("Finished servicing IO transfer");

            ExReleaseFastMutex(&thisPtr->outstandingRequestLock);

        } else if (waitStatus == _WAIT_SHUTDOWN_EVENT) {
            SDHC_LOG_TRACE("Shutdown requested ...");
            break;

        } else if (!NT_SUCCESS(waitStatus)) {
            SDHC_LOG_CRITICAL_ERROR(
                "KeWaitForMultipleObjects failed unexpectedly. (waitStatus = %!STATUS!)",
                waitStatus);

        } else {
            SDHC_LOG_ASSERTION(
                "Unexpected KeWaitForMultipleObjects status. (waitStatus = %!STATUS!)",
                waitStatus);

        } // iff

    } // for (;;)

    SDHC_LOG_TRACE("Thread shutdown");

} // SDHC::transferWorker (...)

SDHC::_REGISTERS_DUMP::_REGISTERS_DUMP () throw () :
    cmd(),
    arg(),
    tout(),
    cdiv(),
    rsp0(),
    rsp1(),
    rsp2(),
    rsp3(),
    hsts(),
    vdd(),
    edm(),
    hcfg(),
    hbct(),
    hblc()
{} // SDHC::...::_REGISTERS_DUMP ()

void SDHC::_REGISTERS_DUMP::UpdateAll (
    const SDHC* SdhcPtr
    ) throw ()
{
    SdhcPtr->readRegisterNoFence(&this->cmd);
    SdhcPtr->readRegisterNoFence(&this->arg);
    SdhcPtr->readRegisterNoFence(&this->tout);
    SdhcPtr->readRegisterNoFence(&this->cdiv);
    SdhcPtr->readRegisterNoFence(&this->rsp0);
    SdhcPtr->readRegisterNoFence(&this->rsp1);
    SdhcPtr->readRegisterNoFence(&this->rsp2);
    SdhcPtr->readRegisterNoFence(&this->rsp3);
    SdhcPtr->readRegisterNoFence(&this->hsts);
    SdhcPtr->readRegisterNoFence(&this->vdd);
    SdhcPtr->readRegisterNoFence(&this->edm);
    SdhcPtr->readRegisterNoFence(&this->hcfg);
    SdhcPtr->readRegisterNoFence(&this->hbct);
    SdhcPtr->readRegisterNoFence(&this->hblc);
} // SDHC::_REGISTERS_DUMP::Update ( ... )

#if ENABLE_STATUS_SAMPLING // - dump registers for debugging

_Use_decl_annotations_
VOID SDHC::sampleStatusWorker(
    PVOID ContextPtr
    )
{
    auto thisPtr = static_cast<SDHC*>(ContextPtr);

    KAFFINITY callerAffinity = thisPtr->restrictCurrentThreadToSecondaryCores();

    SDHC_LOG_INFORMATION(
        "Thread startup - running on CPU%lu",
        KeGetCurrentProcessorNumberEx(NULL));

    (void)KeSetEvent(&thisPtr->samplingStartedEvt, 0, FALSE);

    while (!InterlockedOr(&thisPtr->shutdownSampling, 0)) {
        thisPtr->updateStatusRegistersDump();
    } // while (...)

    KeRevertToUserAffinityThreadEx(callerAffinity);

    SDHC_LOG_TRACE("Thread shutdown");

} // SDHC::sampleStatusWorker (...)

void SDHC::_REGISTERS_DUMP::UpdateStatus (
    const SDHC* SdhcPtr
    ) throw ()
{
    SdhcPtr->readRegisterNoFence(&this->cmd);
    SdhcPtr->readRegisterNoFence(&this->rsp0);
    SdhcPtr->readRegisterNoFence(&this->hsts);
    SdhcPtr->readRegisterNoFence(&this->edm);
} // SDHC::_REGISTERS_DUMP::Update ( ... )

#endif // ENABLE_STATUS_SAMPLING - - dump registers for debugging

SDHC::SDHC (
    PHYSICAL_ADDRESS BasePhysicalAddress,
    void* BasePtr,
    ULONG BaseSpaceSize,
    BOOLEAN CrashdumpMode
    ) throw () :
    basePhysicalAddress(BasePhysicalAddress),
    basePtr(BasePtr),
    baseSpaceSize(BaseSpaceSize),
    outstandingRequestPtr(nullptr),
    sdhcCapabilities(),
    crashdumpMode(CrashdumpMode)
{
} // ...::SDHC (...)

SDHC::~SDHC () throw ()
{
} // ...::~SDHC ()

SDHC_NONPAGED_SEGMENT_END; //==================================================
SDHC_INIT_SEGMENT_BEGIN; //====================================================

_Use_decl_annotations_
NTSTATUS
DriverEntry (
    DRIVER_OBJECT* DriverObjectPtr,
    UNICODE_STRING* RegistryPathPtr
    )
{
    //
    // Crashdump stack will call DriverEntry at IRQL >= DISPATCH_LEVEL, which is not
    // possible to initialize WPP at by design
    //
    if (KeGetCurrentIrql() < DISPATCH_LEVEL) {
        SDHC_LOG_INIT(DriverObjectPtr, RegistryPathPtr);
    } // if

    SDHC_LOG_INFORMATION(
        "(DriverObjectPtr = 0x%p, RegistryPathPtr = 0x%p)",
        DriverObjectPtr,
        RegistryPathPtr);

    auto sdhcInitializationData = SDPORT_INITIALIZATION_DATA();
    {
        sdhcInitializationData.StructureSize = sizeof(sdhcInitializationData);

        sdhcInitializationData.GetSlotCount = SDHC::sdhcGetSlotCount;
        sdhcInitializationData.GetSlotCapabilities = SDHC::sdhcGetSlotCapabilities;
        sdhcInitializationData.Interrupt = SDHC::sdhcInterrupt;
        sdhcInitializationData.IssueRequest = SDHC::sdhcIssueRequest;
        sdhcInitializationData.GetResponse = SDHC::sdhcGetResponse;
        sdhcInitializationData.RequestDpc = SDHC::sdhcRequestDpc;
        sdhcInitializationData.ToggleEvents = SDHC::sdhcToggleEvents;
        sdhcInitializationData.ClearEvents = SDHC::sdhcClearEvents;
        sdhcInitializationData.SaveContext = SDHC::sdhcSaveContext;
        sdhcInitializationData.RestoreContext = SDHC::sdhcRestoreContext;
        sdhcInitializationData.Initialize = SDHC::sdhcInitialize;
        sdhcInitializationData.IssueBusOperation = SDHC::sdhcIssueBusOperation;
        sdhcInitializationData.GetCardDetectState = SDHC::sdhcGetCardDetectState;
        sdhcInitializationData.GetWriteProtectState = SDHC::sdhcGetWriteProtectState;
        sdhcInitializationData.PowerControlCallback = SDHC::sdhcPowerControlCallback;
        sdhcInitializationData.Cleanup = SDHC::sdhcCleanup;
        sdhcInitializationData.PrivateExtensionSize = sizeof(SDHC);
        sdhcInitializationData.CrashdumpSupported = TRUE;
    } // sdhcInitializationData

    NTSTATUS status = ::SdPortInitialize(
            DriverObjectPtr,
            RegistryPathPtr,
            &sdhcInitializationData);
    if (!NT_SUCCESS(status)) {
        SDHC_LOG_ERROR(
            "SdPortInitialize(...) failed. (status = %!STATUS!)",
            status);
        if (KeGetCurrentIrql() < DISPATCH_LEVEL) {
            SDHC_LOG_CLEANUP();
        } // if
        return status;
    } // if

    return STATUS_SUCCESS;
} // DriverEntry (...)

SDHC_INIT_SEGMENT_END; //======================================================