/*++

Copyright (C) Microsoft. All rights reserved.

Module Name:

    sdhc.c

Abstract:

    This file contains the code that interfaces with the
    Broadcom 2836 Arasan SD Host Controller implementation.
    Adapted from greggar's SD miniport sample.

Environment:

    Kernel mode only.

--*/

#include <Ntddk.h>

#include <sdport.h>
#include <sddef.h>

#include "bcm2836sdhc.h"
#include "trace.h"

#ifdef WPP_TRACING
#include "bcm2836sdhc.tmh"
#else
ULONG DefaultDebugFlags = 0xFFFFFFFF;
ULONG DefaultDebugLevel = TRACE_LEVEL_ERROR;
#endif // WPP_TRACING

#ifdef ALLOC_PRAGMA
    #pragma alloc_text(INIT, DriverEntry)
#endif

//
// If to ignore SDHC_IS_CARD_DETECT to temporarily 
// workaround an sdport issue on Rpi.
//
#define SDHC_IGNORE_CARD_DETECT_INTERRUPT   1

//
// Workaround offset was introduced early in the enabling effort to support GPT
// partition. The bcm2836 platform (RPi2) only supported MBR partition and the
// early UEFI does not allow Windows to boot from MBR. The solution then was
// to go with a MBR + GPT solution. That required the SD host controller to
// be able to recognize GPT partition as the first LBA offset. Thus the
// "WorkAroundOffset" was introduced where the driver would recognize from the
// specified offset onward. The "WorkAroundOffset" feature is not needed anymore
// as MBR boot is now supported. The default offset is now set to 0 but the
// "WorkAroundOffset" feature is preserved if the need to revert back to GPT arises.
//
ULONG WorkAroundOffset = 0;

//
// For debugging save the single device extension.
//
volatile PSDHC_EXTENSION Bcm2836Extension = NULL;

//
// SlotExtension routines.
//

/*++

Routine Description:

    This routine is the entry point for the standard sdhsot miniport driver.

Arguments:

    DriverObject - DriverObject for the standard host controller.

    RegistryPath - Registry path for this standard host controller.

Return Value:

    NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS
DriverEntry (
    struct _DRIVER_OBJECT *DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    SDPORT_INITIALIZATION_DATA InitializationData;
    NTSTATUS Status;

#ifdef WPP_TRACING
    //
    // Initialize tracing
    //
    {
        WPP_INIT_TRACING(DriverObject, RegistryPath);

        RECORDER_CONFIGURE_PARAMS recorderConfigureParams;
        RECORDER_CONFIGURE_PARAMS_INIT(&recorderConfigureParams);

        WppRecorderConfigure(&recorderConfigureParams);
    } // Tracing
#endif // WPP_TRACING

    RtlZeroMemory(&InitializationData, sizeof(InitializationData));
    InitializationData.StructureSize = sizeof(InitializationData);

    //
    // Initialize the entry points/callbacks for the miniport interface.
    //

    InitializationData.GetSlotCount = SdhcGetSlotCount;
    InitializationData.GetSlotCapabilities = SdhcGetSlotCapabilities;
    InitializationData.Initialize = SdhcSlotInitialize;
    InitializationData.IssueBusOperation = SdhcSlotIssueBusOperation;
    InitializationData.GetCardDetectState = SdhcSlotGetCardDetectState;
    InitializationData.GetWriteProtectState = SdhcSlotGetWriteProtectState;
    InitializationData.Interrupt = SdhcSlotInterrupt;
    InitializationData.IssueRequest = SdhcSlotIssueRequest;
    InitializationData.GetResponse = SdhcSlotGetResponse;
    InitializationData.ToggleEvents = SdhcSlotToggleEvents;
    InitializationData.ClearEvents = SdhcSlotClearEvents;
    InitializationData.RequestDpc = SdhcRequestDpc;
    InitializationData.SaveContext = SdhcSaveContext;
    InitializationData.RestoreContext = SdhcRestoreContext;
    InitializationData.PowerControlCallback = SdhcPowerControlCallback;
    InitializationData.Cleanup = SdhcCleanup;

    //
    // Provide the number of slots and their size.
    //

    InitializationData.PrivateExtensionSize = sizeof(SDHC_EXTENSION);

    //
    // Read registry for for WorkAroundOffset override
    //
    do { // once
        OBJECT_ATTRIBUTES ObjectAttributes;
        HANDLE ServiceHandle;
        UNICODE_STRING UnicodeKey;
        UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 512 * sizeof(UCHAR)];
        PKEY_VALUE_PARTIAL_INFORMATION Value;
        ULONG ResultLength;

        RtlZeroMemory(&ObjectAttributes, sizeof(OBJECT_ATTRIBUTES));

        //
        // Every platform should overwrite the offset according to the boot
        // boot process. The following registry needs to be set
        // Registry\Machine\System\CurrentControlSet\Services\bcm2836sdhc.
        // Name="WorkAroundOffset"
        // Value = "0"
        // Type = "REG_DWORD"
        //
        InitializeObjectAttributes(&ObjectAttributes,
                                   RegistryPath,
                                   OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                   NULL,
                                   NULL);

        Status = ZwOpenKey(&ServiceHandle, KEY_READ, &ObjectAttributes);
        if (!NT_SUCCESS(Status)) {
            break;
        } // if

        RtlInitUnicodeString(&UnicodeKey, L"WorkAroundOffset");

        Value = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
        Status = ZwQueryValueKey(ServiceHandle,
                                 &UnicodeKey,
                                 KeyValuePartialInformation,
                                 Value,
                                 sizeof(Buffer),
                                 &ResultLength);
        if (!NT_SUCCESS(Status)) {
            ZwClose(ServiceHandle);
            break;
        } // if

        if (Value->Type == REG_DWORD) {
            WorkAroundOffset = (ULONG)*(Value->Data);
        } // if

        ZwClose(ServiceHandle);
    } while (SdhcFalse());

    //
    // Hook up the IRP dispatch routines.
    //
    Status = SdPortInitialize(DriverObject, RegistryPath, &InitializationData);

#ifdef WPP_TRACING
    if (!NT_SUCCESS(Status)) {
        WPP_CLEANUP(DriverObject);
    }
#endif

    return Status;
} // DriverEntry (...)

/*++

Routine Description:

    Return the number of slots present on this controller.

Arguments:

    Miniport - Functional device object for this controller.

    SlotCount - The number of slots present on this controller.

Return value:

    STATUS_UNSUCCESSFUL - PCI config space was unable to be queried.

    STATUS_INVALID_PARAMETER - Invalid underlying bus type for the miniport.

    STATUS_SUCCESS - SlotCount returned properly.

--*/
_Use_decl_annotations_
NTSTATUS
SdhcGetSlotCount (
    struct _SD_MINIPORT *Miniport,
    PUCHAR SlotCount
    )
{
    NTSTATUS Status;
    SDPORT_BUS_TYPE BusType = Miniport->ConfigurationInfo.BusType;

    switch (BusType) {
    case SdBusTypeAcpi:
        //
        // We don't currently have a mechanism to query the slot count for ACPI
        // enumerated host controllers. Default to one slot.
        //
        *SlotCount = 1;
        Status = STATUS_SUCCESS;
        break;

    case SdBusTypePci:
        //
        // The Arasan host controller is NOT PCI enumerated.
        //
        *SlotCount = 1;
        Status = STATUS_INVALID_PARAMETER;
        TraceMessage(TRACE_LEVEL_ERROR,
                     DRVR_LVL_ERR,
                     (__FUNCTION__ ": BusType Invalid: %d",
                      BusType));
        break;

    default:
        NT_ASSERT((BusType == SdBusTypeAcpi) || (BusType == SdBusTypePci));

        *SlotCount = 1;
        Status = STATUS_INVALID_PARAMETER;
        TraceMessage(TRACE_LEVEL_ERROR,
                     DRVR_LVL_ERR,
                     (__FUNCTION__ ": BusType Unexpected: %d",
                      BusType));
        break;
    } // switch (BusType)

    return Status;
} // SdhcGetSlotCount (...)

/*++

Routine Description:

    Override for miniport to provide host register mapping information if the
    memory range provideed by the underlying bus isn't sufficient.

Arguments:

    PrivateExtension - SlotExtension interface between sdhost and this miniport.

    Capabilities - Miniport provided capabilities.

Return value:

    STATUS_SUCCESS - Capabilities returned successfully.

--*/
_Use_decl_annotations_
VOID
SdhcGetSlotCapabilities (
    PVOID PrivateExtension,
    struct _SDPORT_CAPABILITIES *Capabilities
    )
{
    PSDHC_EXTENSION SdhcExtension = (PSDHC_EXTENSION)PrivateExtension;

    RtlCopyMemory(Capabilities,
                  &SdhcExtension->Capabilities,
                  sizeof(SdhcExtension->Capabilities));
} // SdhcGetSlotCapabilities (...)

/*++

Routine Description:

    Initialize the miniport for standard host controllers.

Arguments:

    PrivateExtension - SlotExtension interface between sdhost and this miniport.

Return value:

    NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS
SdhcSlotInitialize (
    PVOID PrivateExtension,
    PHYSICAL_ADDRESS PhysicalBase,
    PVOID VirtualBase,
    ULONG Length,
    BOOLEAN CrashdumpMode
    )
{
    PSDHC_EXTENSION SdhcExtension = (PSDHC_EXTENSION)PrivateExtension;
    PSDPORT_CAPABILITIES Capabilities;
    ULONG CurrentLimits;
    ULONG CurrentLimitMax;
    ULONG CurrentLimitMask;
    ULONG CurrentLimitShift;
    USHORT SpecVersion;

    //
    // For debugging save the single device extension.
    //

    Bcm2836Extension = SdhcExtension;

    //
    // Initialize the SDHC_EXTENSION register space.
    //

    SdhcExtension->PhysicalBaseAddress = PhysicalBase;
    SdhcExtension->BaseAddress = VirtualBase;
    SdhcExtension->BaseAddressSpaceSize = Length;
    SdhcExtension->BaseAddressDebug =
        (PSD_HOST_CONTROLLER_REGISTERS)VirtualBase;

    SdhcExtension->CrashdumpMode = CrashdumpMode;

    //
    // Initialize host capabilities.
    //

    Capabilities = (PSDPORT_CAPABILITIES)&SdhcExtension->Capabilities;
    
    SpecVersion = SdhcReadRegisterUlong(SdhcExtension, 
                                        SDHC_SLOT_INFORMATION_VERSION) >>
                  SDHC_REG_SHIFT_UPPER_HALF_TO_LOWER;

    Capabilities->SpecVersion = SpecVersion & 0xFF;
    Capabilities->MaximumOutstandingRequests = 1;
    Capabilities->MaximumBlockSize = (USHORT)(512);
    Capabilities->MaximumBlockCount = 0xFFFF;

    // TODO : Integrate RPIQ mail box driver so SD port driver is able to query
    // for base clock actual value. For now use the default value 250MHz
    Capabilities->BaseClockFrequencyKhz = 250 * 1000;

    Capabilities->DmaDescriptorSize = 0;
    Capabilities->Supported.ScatterGatherDma = 0;

    Capabilities->Supported.Address64Bit = 0;
    Capabilities->Supported.BusWidth8Bit = 0;
    Capabilities->Supported.HighSpeed = 1;

    Capabilities->Supported.SDR50 = 0;
    Capabilities->Supported.DDR50 = 0;
    Capabilities->Supported.SDR104 = 0;
    Capabilities->Supported.SignalingVoltage18V = 0;

    Capabilities->Supported.HS200 = 0;
    Capabilities->Supported.HS400 = 0;

    Capabilities->Supported.DriverTypeA = 1;
    Capabilities->Supported.DriverTypeB = 1;
    Capabilities->Supported.DriverTypeC = 1;

    Capabilities->Supported.TuningForSDR50 = 0;
    Capabilities->Supported.SoftwareTuning = 0;

    Capabilities->Supported.AutoCmd12 = 1;
    Capabilities->Supported.AutoCmd23 = 0;

    Capabilities->Supported.Voltage18V = 0;
    Capabilities->Supported.Voltage30V = 0;
    Capabilities->Supported.Voltage33V = 1;

    //
    // Find the current limits supported by the controller.
    //

    CurrentLimitMask = 0;
    CurrentLimitShift = 0;
    if (Capabilities->Supported.Voltage33V) {
        CurrentLimitMask = 0xFF;
        CurrentLimitShift = 0;

    } else if (Capabilities->Supported.Voltage30V) {
        CurrentLimitMask = 0xFF00;
        CurrentLimitShift = 8;

    } else if (Capabilities->Supported.Voltage18V) {
        CurrentLimitMask = 0xFF0000;
        CurrentLimitShift = 16;
    } // iff

    CurrentLimits = SdhcReadRegisterUlong(SdhcExtension, SDHC_MAXIMUM_CURRENT);
    CurrentLimitMax =
        ((CurrentLimits & CurrentLimitMask) >> CurrentLimitShift) * 4;

    if (CurrentLimitMax >= 800) {
        Capabilities->Supported.Limit800mA;
    } // if

    if (CurrentLimitMax >= 600) {
        Capabilities->Supported.Limit600mA;
    } // if

    if (CurrentLimitMax >= 400) {
        Capabilities->Supported.Limit400mA;
    } // if

    if (CurrentLimitMax >= 200) {
        Capabilities->Supported.Limit200mA;
    } // if

    //
    // Unaligned requests handling
    //

    SdhcExtension->UnalignedReqState = UnalignedReqStateIdle;
    RtlZeroMemory(&SdhcExtension->UnalignedRequest, sizeof(SdhcExtension->UnalignedRequest));

    //
    // The single active request.
    //

    SdhcExtension->OutstandingRequest = NULL;

    //
    // Disable interrupts until we're ready to handle them. No need to have
    // them enabled yet, and some versions of sdport.sys crash if any
    // interrupts come at this point.
    //

    SdhcWriteRegisterUlong(SdhcExtension,
                           SDHC_INTERRUPT_ERROR_STATUS_ENABLE,
                           0);
    SdhcWriteRegisterUlong(SdhcExtension,
                           SDHC_INTERRUPT_ERROR_SIGNAL_ENABLE,
                           0);

    return STATUS_SUCCESS;
} // SdhcSlotInitialize (...)

/*++

Routine Description:

    Issue host bus operation specified by BusOperation.

Arguments:

    PrivateExtension - SlotExtension interface between sdhost and this miniport.

    BusOperation - Bus operation to perform.

Return value:

    NTSTATUS - Return value of the bus operation performed.

--*/
_Use_decl_annotations_
NTSTATUS
SdhcSlotIssueBusOperation (
    PVOID PrivateExtension,
    struct _SDPORT_BUS_OPERATION *BusOperation
    )
{
    PSDHC_EXTENSION SdhcExtension = (PSDHC_EXTENSION)PrivateExtension;
    NTSTATUS Status = STATUS_INVALID_PARAMETER;

    switch (BusOperation->Type) {
    case SdResetHost:
        Status = SdhcResetHost(SdhcExtension,
                               BusOperation->Parameters.ResetType);
        break;

    case SdSetClock:
        Status = SdhcSetClock(SdhcExtension,
                              BusOperation->Parameters.FrequencyKhz);
        break;

    case SdSetVoltage:
        Status = SdhcSetVoltage(SdhcExtension,
                                BusOperation->Parameters.Voltage);
        break;

    case SdSetBusWidth:
        Status = SdhcSetBusWidth(SdhcExtension,
                                 BusOperation->Parameters.BusWidth);
        break;

    case SdSetBusSpeed:
        Status = SdhcSetSpeed(SdhcExtension,
                              BusOperation->Parameters.BusSpeed);
        break;

    case SdSetSignalingVoltage:
        Status = SdhcSetSignaling(SdhcExtension,
                                  BusOperation->Parameters.SignalingVoltage);
        break;

    case SdSetDriveStrength:
        break;

    case SdSetDriverType:
        break;

    case SdSetPresetValue:
        Status = 
            SdhcSetPresetValue(SdhcExtension,
                               BusOperation->Parameters.PresetValueEnabled);
        break;

    case SdExecuteTuning:
        Status = SdhcExecuteTuning(SdhcExtension);
        break;

    default:
        Status = STATUS_INVALID_PARAMETER;
        break;
    } // switch (BusOperation->Type)

    TraceMessage(TRACE_LEVEL_INFORMATION,
                 DRVR_LVL_FUNC,
                 (__FUNCTION__ "Exit: Status: %08x, BusOperation->Type: %d",
                  Status,
                  BusOperation->Type));

    return Status;
} // SdhcSlotIssueBusOperation (...)

/*++

Routine Description:

    Determine whether a card is inserted in the slot.

Arguments:

    PrivateExtension - SlotExtension interface between sdhost and this miniport.

Return value:

    TRUE - Card is inserted.

    FALSE - Slot is empty.

--*/
_Use_decl_annotations_
BOOLEAN
SdhcSlotGetCardDetectState (
    PVOID PrivateExtension
    )
{
    const SDHC_EXTENSION* SdhcExtension = 
        (const SDHC_EXTENSION*)PrivateExtension;

    return SdhcIsCardInserted(SdhcExtension);
} // SdhcSlotGetCardDetectState (...)

/*++

Routine Description:

    Determine whether the slot write protection is engaged.

Arguments:

    PrivateExtension - SlotExtension interface between sdhost and this miniport.

Return value:

    TRUE - Slot is write protected.

    FALSE - Slot is writable.

--*/
_Use_decl_annotations_
BOOLEAN
SdhcSlotGetWriteProtectState (
    PVOID PrivateExtension
    )
{
    const SDHC_EXTENSION* SdhcExtension = 
        (const SDHC_EXTENSION*)PrivateExtension;

    return SdhcIsWriteProtected(SdhcExtension);
} // SdhcSlotGetWriteProtectState (...)

/*++

Routine Description:

    Level triggered DIRQL interrupt handler (ISR) for this controller.

Arguments:

    PrivateExtension - SlotExtension interface between sdhost and this miniport.

Return value:

    Whether the interrupt is handled.

--*/
_Use_decl_annotations_
BOOLEAN
SdhcSlotInterrupt (
    PVOID PrivateExtension,
    PULONG Events,
    PULONG Errors,
    PBOOLEAN NotifyCardChange,
    PBOOLEAN NotifySdioInterrupt,
    PBOOLEAN NotifyTuning
    )
{
    USHORT InterruptStatus;
    PSDHC_EXTENSION SdhcExtension = (PSDHC_EXTENSION)PrivateExtension;
    
    InterruptStatus = SdhcGetInterruptStatus(SdhcExtension);

    *Events = (ULONG)InterruptStatus;

    *Errors = 0;
    *NotifyCardChange = FALSE;
    *NotifySdioInterrupt = FALSE;
    *NotifyTuning = FALSE;

    //
    // If there aren't any events to handle, then we don't need to
    // process anything.
    //
    if (*Events == 0) {
        return FALSE;
    } // if

    if (*Events & SDHC_IS_ERROR_INTERRUPT) {
        *Errors = (ULONG)SdhcGetErrorStatus(SdhcExtension);
    } // if

    //
    // If a card has changed, notify the port driver.
    //
    if (*Events & SDHC_IS_CARD_DETECT) {
        #if !SDHC_IGNORE_CARD_DETECT_INTERRUPT
            *NotifyCardChange = TRUE;
        #endif // !SDHC_IGNORE_CARD_DETECT_INTERRUPT
        *Events &= ~SDHC_IS_CARD_DETECT;
    } // if

    //
    // If we have an external SDIO interrupt, notify the port driver.
    //
    if (*Events & SDHC_IS_CARD_INTERRUPT) {
        *NotifySdioInterrupt = TRUE;
        *Events &= ~SDHC_IS_CARD_INTERRUPT;
    } // if

    //
    // If there's a tuning request, notify the port driver.
    //
    if (*Events & SDHC_IS_TUNING_INTERRUPT) {
        *NotifyTuning = TRUE;
        *Events &= ~SDHC_IS_TUNING_INTERRUPT;
    } // if

    //
    // Acknowledge/clear interrupt status. Request completions will occur in
    // the port driver's slot completion DPC. We need to make the members of
    // SDPORT_REQUEST that only the port driver uses opaque to the miniport.
    // See how Storport does this (if it does).
    //
    SdhcAcknowledgeInterrupts(SdhcExtension, InterruptStatus);

    TraceMessage(TRACE_LEVEL_INFORMATION,
                 DRVR_LVL_FUNC,
                 (__FUNCTION__ 
                  " Exit: Interrupts: %04x, *Events: %08x, *Errors: %08x",
                  InterruptStatus,
                  *Events,
                  *Errors));

    //
    // Temporary to workaround an sdport issue
    //
    #if SDHC_IGNORE_CARD_DETECT_INTERRUPT
        InterruptStatus &= ~SDHC_IS_CARD_DETECT;
    #endif // SDHC_IGNORE_CARD_DETECT_INTERRUPT

    return (InterruptStatus != 0);
} // SdhcSlotInterrupt (...)

/*++

Routine Description:

    Issue hardware request specified by Request.

Arguments:

    PrivateExtension - This driver's device extension (SdhcExtension).

    Request - Request operation to perform.

Return value:

    NTSTATUS - Return value of the request performed.

--*/
_Use_decl_annotations_
NTSTATUS
SdhcSlotIssueRequest (
    PVOID PrivateExtension,
    PSDPORT_REQUEST Request
    )
{
    PSDHC_EXTENSION SdhcExtension = (PSDHC_EXTENSION)PrivateExtension;
    NTSTATUS Status;

    if (InterlockedExchangePointer(&SdhcExtension->OutstandingRequest,
                                   Request) != NULL) {
        TraceMessage(TRACE_LEVEL_WARNING,
                     DRVR_LVL_WARN,
                     (__FUNCTION__ " Previous request is in progress"));
    }
    InterlockedIncrement(&SdhcExtension->CmdIssued);

    //
    // Dispatch the request based off of the request type.
    //
    switch (Request->Type) {
    case SdRequestTypeCommandNoTransfer:
    case SdRequestTypeCommandWithTransfer:
        Status = SdhcSendCommand(SdhcExtension, Request);
        break;

    case SdRequestTypeStartTransfer:
        Status = SdhcStartTransfer(SdhcExtension, Request);
        //
        // On successful transfer initiation reset the status to 
        // STATUS_PENDING as expected by SDPORT.
        //
        if (NT_SUCCESS(Status)) {
            Status = STATUS_PENDING;
        }
        break;

    default:
        Status = STATUS_NOT_SUPPORTED;
        break;
    } // switch (Request->Type)

    TraceMessage(TRACE_LEVEL_INFORMATION,
                 DRVR_LVL_FUNC,
                 (__FUNCTION__ " Exit: Status: %08x, Request->Type: %d.",
                  Status,
                  Request->Type));

    return Status;
} // SdhcSlotIssueRequest (...)

/*++

Routine Description:

    Return the response data for a given command back to the port driver.

Arguments:

    PrivateExtension - This driver's device extension (SdhcExtension).

    Command - Command for which we're getting the response.

    ResponseBuffer - Response data for the given command.

Return value:

    None.

--*/
_Use_decl_annotations_
VOID
SdhcSlotGetResponse (
    PVOID PrivateExtension,
    struct _SDPORT_COMMAND *Command,
    PVOID ResponseBuffer
    )
{
    PSDHC_EXTENSION SdhcExtension = (PSDHC_EXTENSION)PrivateExtension;
    NTSTATUS Status;

    Status = SdhcGetResponse(SdhcExtension, Command, ResponseBuffer);
    if (!NT_SUCCESS(Status)) {
        TraceMessage(TRACE_LEVEL_ERROR,
                     DRVR_LVL_ERR,
                     (__FUNCTION__ ": SdhcGetResponse: Status: %08x\n",
                      Status));
        NT_ASSERT(!"SdhcGetResponse failed");
    } // if
} // SdhcSlotGetResponse (...)

/*++

Routine Description:

    DPC for interrupts associated with the given request.

Arguments:

    PrivateExtension - This driver's device extension (SdhcExtension).

    Request - Request operation to perform.

    Events - Normal interrupt events.

    Errors - Error interrupt events.

Return value:

    NTSTATUS - Return value of the request performed.

--*/
_Use_decl_annotations_
VOID
SdhcRequestDpc (
    PVOID PrivateExtension,
    struct _SDPORT_REQUEST *Request,
    ULONG Events,
    ULONG Errors
    )
{
    PSDHC_EXTENSION SdhcExtension = (PSDHC_EXTENSION)PrivateExtension;
    NTSTATUS Status;

    //
    // Miniport DPC handles command related events only!
    //
    if (((Events & SDHC_IS_COMMAND_EVENT) == 0) && (Errors == 0)) {
        return;
    }

    //
    // Save current events, since we may not be waiting for them
    // at this stage, but we may be on the next phase of the command 
    // processing.
    // For instance with short data read requests, the transfer is probably
    // completed by the time the command is done.
    // If that case if we wait for that events after it has already arrived,
    // we will fail with timeout.
    //
    InterlockedOr((PLONG)&SdhcExtension->CurrentEvents, Events);

    //
    // Check for out of sequence call?
    // SDPORT does not maintain a request state, so we may get a request that
    // has not been issued yet!
    //
    if (InterlockedExchangePointer(&SdhcExtension->OutstandingRequest,
                                   SdhcExtension->OutstandingRequest) 
        == NULL) {
        return;
    }

    //
    // Clear the request's required events if they have completed.
    //
    Request->RequiredEvents &= ~Events;

    //
    // If there are errors, we need to fail whatever outstanding request
    // was on the bus. Otherwise, the request succeeded.
    //
    if (Errors) {
        TraceMessage(TRACE_LEVEL_WARNING,
                     DRVR_LVL_WARN,
                     (__FUNCTION__ " Cmd %d failed, errors %x",
                      Request->Command.Index,
                      Errors));
        Status = SdhcConvertErrorToStatus((USHORT)Errors);
        (void)SdhcCompleteNonBlockSizeAlignedRequest(SdhcExtension,
                                                     Request, 
                                                     Status);

        Request->RequiredEvents = 0;
        InterlockedAnd((PLONG)&SdhcExtension->CurrentEvents, 0);
        SdhcCompleteRequest(SdhcExtension, Request, Status);
    } else if (Request->RequiredEvents == 0) {
        if (Request->Status != STATUS_MORE_PROCESSING_REQUIRED) {
            Request->Status = STATUS_SUCCESS;
        } // if

        if (SdhcCompleteNonBlockSizeAlignedRequest(SdhcExtension,
                                                   Request,
                                                   Request->Status) ==
            STATUS_MORE_PROCESSING_REQUIRED) {
            //
            // Unaligned request handling is in-progress,
            // reading/writing trailing bytes...
            //
            return;
        } // if

        SdhcCompleteRequest(SdhcExtension, Request, Request->Status);
    } // iff
} // SdhcRequestDpc (...)

/*++

Routine Description:

    Enable or disable the given event mask.

Arguments:

    PrivateExtension - SlotExtension interface between sdhost and this miniport.

    Events - The event mask to toggle.

    Enable - TRUE to enable, FALSE to disable.

Return value:

    None.

--*/
_Use_decl_annotations_
VOID
SdhcSlotToggleEvents (
    PVOID PrivateExtension,
    ULONG EventMask,
    BOOLEAN Enable
    )
{
    PSDHC_EXTENSION SdhcExtension = (PSDHC_EXTENSION)PrivateExtension;
    USHORT InterruptMask = SdhcConvertEventsToHwMask(EventMask);

    if (Enable) {
        SdhcEnableInterrupt(SdhcExtension, InterruptMask);
    } else {
        SdhcDisableInterrupt(SdhcExtension, InterruptMask);
    } // iff
} // SdhcSlotToggleEvents (...)

/*++

Routine Description:

    Clear the given event mask.

Arguments:

    PrivateExtension - SlotExtension interface between sdhost and this miniport.

    Events - The event mask to clear.

Return value:

    None.

--*/
_Use_decl_annotations_
VOID
SdhcSlotClearEvents (
    PVOID PrivateExtension,
    ULONG EventMask
    )
{
    PSDHC_EXTENSION SdhcExtension = (PSDHC_EXTENSION)PrivateExtension;
    USHORT Interrupts = SdhcConvertEventsToHwMask(EventMask);
    SdhcAcknowledgeInterrupts(SdhcExtension, Interrupts);
} // SdhcSlotClearEvents (...)

/*++

Routine Description:

    Save slot register context.

Arguments:

    PrivateExtension - SlotExtension interface between sdhost and this miniport.

Return value:

    None.

--*/
_Use_decl_annotations_
VOID
SdhcSaveContext (
    PVOID PrivateExtension
    )
{
    UNREFERENCED_PARAMETER(PrivateExtension);
} // SdhcSaveContext (...)

/*++

Routine Description:

    Restore slot register context from a previously saved context.

Arguments:

    PrivateExtension - SlotExtension interface between sdhost and this miniport.

Return value:

    None.

--*/
_Use_decl_annotations_
VOID
SdhcRestoreContext (
    PVOID PrivateExtension
    )
{
    UNREFERENCED_PARAMETER(PrivateExtension);
} // SdhcRestoreContext (...)

//
// Host routine implementations.
//

/*++

Routine Description:

    Execute a soft reset to the socket specified.

Arguments:

    SdhcExtension - Host controller specific driver context.

    ResetType - Either full, CMD, or DAT reset.

Return value:

    STATUS_SUCCESS - Reset succeeded.

    STATUS_INVALID_PARAMETER - Invalid reset type chosen.

--*/
_Use_decl_annotations_
NTSTATUS
SdhcResetHost (
    PSDHC_EXTENSION SdhcExtension,
    SDPORT_RESET_TYPE ResetType
    )
{
    ULONG Mask;
    ULONG Control1;

    switch (ResetType) {
    case SdResetTypeAll:
        Mask = SDHC_RESET_ALL;
        break;

    case SdResetTypeCmd:
        Mask = SDHC_RESET_CMD;
        break;

    case SdResetTypeDat:
        Mask = SDHC_RESET_DAT;
        break;

    default:
        return STATUS_INVALID_PARAMETER;
    } // switch (ResetType)

    if (InterlockedExchangePointer(&SdhcExtension->OutstandingRequest,
                                   NULL) != NULL) {
        InterlockedIncrement(&SdhcExtension->CmdAborted);
    }
    SdhcExtension->UnalignedReqState = UnalignedReqStateIdle;

    //
    // Reset the host controller
    //
    {
        UCHAR Retries = 100;
        ULONG Reset;

        Control1 = SdhcReadRegisterUlong(SdhcExtension, SDHC_CONTROL_1);
        SdhcWriteRegisterUlong(SdhcExtension,
                               SDHC_CONTROL_1,
                               Control1 | Mask);
        do {
            if (--Retries == 0) {
                return STATUS_TIMEOUT;
            } // if

            Reset = SdhcReadRegisterUlong(SdhcExtension, SDHC_CONTROL_1);

            if ((Reset & Mask) != 0) {
                SdPortWait(1);
            } // if

        } while ((Reset & Mask) != 0);
    } // Reset the host controller

    //
    // Set the max HW timeout for bus operations.
    //

    SdhcWriteRegisterUlong(SdhcExtension,
                           SDHC_CONTROL_1,
                           (Control1 & ~SDHC_TC_COUNTER_MASK) |
                            SDHC_TC_MAX_DATA_TIMEOUT);

    //
    // Enable all interrupt signals from controller to the OS,
    // but mask all.
    // Means we are only using SDHC_INTERRUPT_ERROR_STATUS_ENABLE to
    // control interrupts, this way disabled events do not get reflected
    // in the status register.
    //

    SdhcWriteRegisterUlong(SdhcExtension,
                           SDHC_INTERRUPT_ERROR_STATUS_ENABLE,
                           0);
    SdhcWriteRegisterUlong(SdhcExtension,
                           SDHC_INTERRUPT_ERROR_SIGNAL_ENABLE,
                           SDHC_ALL_EVENTS);

    TraceMessage(TRACE_LEVEL_INFORMATION,
                 DRVR_LVL_FUNC,
                 (__FUNCTION__ " Exit: ResetType: %d",
                  ResetType));

    return STATUS_SUCCESS;
} // SdhcResetHost (...)

/*++

Routine Description:

    Set the clock to a given frequency.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Frequency - The target frequency.

Return value:

    STATUS_SUCCESS - The clock was successfuly set.

    STATUS_TIMEOUT - The clock did not stabilize in time.

--*/
_Use_decl_annotations_
NTSTATUS
SdhcSetClock (
    PSDHC_EXTENSION SdhcExtension,
    ULONG Frequency
    )
{
    ULONG ActualFrequency;
    ULONG ClockControl;

    ClockControl = SdhcReadRegisterUlong(SdhcExtension, SDHC_CONTROL_1);
    ClockControl &= ~(SDHC_CC_CLOCK_ENABLE | SDHC_CC_INTERNAL_CLOCK_ENABLE);
    SdhcWriteRegisterUlong(SdhcExtension, SDHC_CONTROL_1, ClockControl);
    ClockControl &= SDHC_REG_UPPER_HALF_MASK;
    ClockControl |= SdhcCalcClockFrequency(SdhcExtension,
                                           Frequency,
                                           &ActualFrequency);

    ClockControl |= SDHC_CC_INTERNAL_CLOCK_ENABLE;
    SdhcWriteRegisterUlong(SdhcExtension, SDHC_CONTROL_1, ClockControl);

    //
    // Now the frequency is set, delay a few times to wait for it to become
    // stable.
    //
    {
        UCHAR Retries = 100;
        USHORT Mask = SDHC_CC_CLOCK_STABLE;
        do {
            if (--Retries == 0) {
                return STATUS_TIMEOUT;
            } // if

            ClockControl = SdhcReadRegisterUlong(SdhcExtension, SDHC_CONTROL_1);
            if ((ClockControl & Mask) == 0) {
                SdPortWait(1);
            } // if
        } while ((ClockControl & Mask) == 0);
    }

    //
    // Clock is now stable, now enable it.
    //

    ClockControl |= SDHC_CC_CLOCK_ENABLE;
    SdhcWriteRegisterUlong(SdhcExtension, SDHC_CONTROL_1, ClockControl);

    //
    // Some hardware need more time here to stabilize, but minimize latency
    // for fixed eMMC devices during runtime Dx transitions.
    //
    {
        ULONG Delay = (SdhcExtension->Removable) ? (10 * 1000) : 50;
        SdPortWait(Delay);
    }

    TraceMessage(TRACE_LEVEL_INFORMATION,
                 DRVR_LVL_FUNC,
                 (__FUNCTION__ " Exit: ClockControl: %08x, "
                  "Frequency: %d, ActualFrequency: %d",
                  ClockControl,
                  Frequency,
                  ActualFrequency));

    return STATUS_SUCCESS;
} // SdhcSetClock (...)

/*++

Routine Description:

    Set the slot's voltage profile.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Voltage - Indicates which power voltage to use. If 0, turn off the
              power

Return value:

    STATUS_SUCCESS - The bus voltage was successfully switched.

    STATUS_INVALID_PARAMETER - The voltage profile provided was invalid.

    STATUS_TIMEOUT - The bus voltage did not stabilize in time.

--*/
_Use_decl_annotations_
NTSTATUS
SdhcSetVoltage (
    const SDHC_EXTENSION* SdhcExtension,
    SDPORT_BUS_VOLTAGE Voltage
    )
{
    //
    // The Arasan Host Controller does not support setting Voltage or Power.
    //

    UNREFERENCED_PARAMETER(SdhcExtension);
    UNREFERENCED_PARAMETER(Voltage);

    return STATUS_SUCCESS;
} // SdhcSetVoltage (...)

/*++

Routine Description:

    Set bus width for host controller.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Width - The data bus width of the slot.

Return value:

    STATUS_SUCCESS - The bus width was properly changed.

--*/
_Use_decl_annotations_
NTSTATUS
SdhcSetBusWidth (
    PSDHC_EXTENSION SdhcExtension,
    SDPORT_BUS_WIDTH Width
    )
{
    ULONG HostControl= SdhcReadRegisterUlong(SdhcExtension, SDHC_CONTROL_0);

    HostControl &= ~(SDHC_HC_DATA_WIDTH_4BIT | SDHC_HC_DATA_WIDTH_8BIT);

    switch (Width) {
    case 1:
        break;
    case 4:
        HostControl |= SDHC_HC_DATA_WIDTH_4BIT;
        break;
    case 8:
        HostControl |= SDHC_HC_DATA_WIDTH_8BIT;
        break;

    default:
        NT_ASSERT(!"SDHC - Provided bus width is invalid");
        break;
    } // switch (Width)

    SdhcWriteRegisterUlong(SdhcExtension, SDHC_CONTROL_0, HostControl);

    TraceMessage(TRACE_LEVEL_INFORMATION,
                 DRVR_LVL_FUNC,
                 (__FUNCTION__ " Exit: Width: %d",
                  Width));

    return STATUS_SUCCESS;
} // SdhcSetBusWidth (...)

/*++

Routine Description:

    Based on the capabilities of card and host, turns on the maximum performing
    speed mode for the host. Caller is expected to know the capabilities of the
    card before setting the speed mode.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Speed - Bus speed to set.

Return value:

    STATUS_SUCCESS - The selected speed mode was successfully set.

    STATUS_INVALID_PARAMETER - The speed mode selected is not valid.

--*/
_Use_decl_annotations_
NTSTATUS
SdhcSetSpeed (
    PSDHC_EXTENSION SdhcExtension,
    SDPORT_BUS_SPEED Speed
    )
{
    NTSTATUS Status;
    ULONG UhsMode;

    switch (Speed) {
    case SdBusSpeedNormal:
        Status = SdhcSetHighSpeed(SdhcExtension, FALSE);
        break;
    case SdBusSpeedHigh:
        Status = SdhcSetHighSpeed(SdhcExtension, TRUE);
        break;
    case SdBusSpeedSDR12:
    case SdBusSpeedSDR25:
    case SdBusSpeedSDR50:
    case SdBusSpeedDDR50:
    case SdBusSpeedSDR104:
    case SdBusSpeedHS200:
    case SdBusSpeedHS400:
        UhsMode = SdhcGetHwUhsMode(Speed);
        Status = SdhcSetUhsMode(SdhcExtension, UhsMode);
        break;
    default:
        NT_ASSERT(!"SDHC - Invalid speed mode selected.");
        Status = STATUS_INVALID_PARAMETER;
        break;
    } // switch (Speed)

    TraceMessage(TRACE_LEVEL_INFORMATION,
                 DRVR_LVL_FUNC,
                 (__FUNCTION__ " Exit: Speed: %d, Status: %08x",
                  Speed,
                  Status));

    return Status;
} // SdhcSetSpeed (...)

/*++

Routine Description:

    Based on the capabilities of card and host, enables or disables high speed.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Enable - TRUE to enable high speed, FALSE to disable.

Return value:

    STATUS_SUCCESS - High speed was successfully enabled.

--*/
_Use_decl_annotations_
NTSTATUS
SdhcSetHighSpeed (
    PSDHC_EXTENSION SdhcExtension,
    BOOLEAN Enable
    )
{
    ULONG HostControl = SdhcReadRegisterUlong(SdhcExtension, SDHC_CONTROL_0);
    HostControl &= ~SDHC_HC_ENABLE_HIGH_SPEED;

    if (Enable) {
        // Per Linux driver Git history, this is a fix for some timing bug.
        // HostControl |= SDHC_HC_ENABLE_HIGH_SPEED;
    } // if

    SdhcWriteRegisterUlong(SdhcExtension, SDHC_CONTROL_0, HostControl);

    TraceMessage(TRACE_LEVEL_INFORMATION,
                 DRVR_LVL_FUNC,
                 (__FUNCTION__ " Exit: Enable: %d",
                  Enable));

    return STATUS_SUCCESS;
} // SdhcSetHighSpeed (...)

/*++

Routine Description:

    Based on the capabilities of card and host, turns on the maximum performing
    speed mode for the host. Caller is expected to know the capabilities of the
    card before setting the speed mode.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Mode - UHS mode to set.

Return value:

    STATUS_SUCCESS - Mode requested is set on the controller.

    STATUS_INVALID_PARAMETER - The mode selected is not a UHS mode.

--*/
_Use_decl_annotations_
NTSTATUS
SdhcSetUhsMode (
    PSDHC_EXTENSION SdhcExtension,
    ULONG Mode
    )
{
    ULONG HostControl2 = SdhcReadRegisterUlong(SdhcExtension, SDHC_CONTROL_2);
    ULONG ClockControl;

    //
    // If we're already in the requested mode, return.
    //
    if ((HostControl2 & SDHC_HC2_UHS_MODES) == Mode) {
        return STATUS_SUCCESS;
    } // if

    ClockControl = SdhcReadRegisterUlong(SdhcExtension, SDHC_CONTROL_1);
    ClockControl &= ~SDHC_CC_CLOCK_ENABLE;
    SdhcWriteRegisterUlong(SdhcExtension, SDHC_CONTROL_1, ClockControl);
    SdPortWait(10 * 1000);

    //
    // Set the UHS mode.
    //

    HostControl2 &= ~SDHC_HC2_UHS_MODES;
    HostControl2 |= Mode;
    SdhcWriteRegisterUlong(SdhcExtension, SDHC_CONTROL_2, HostControl2);
    ClockControl = SdhcReadRegisterUlong(SdhcExtension, SDHC_CONTROL_1);
    ClockControl |= SDHC_CC_CLOCK_ENABLE;
    SdhcWriteRegisterUlong(SdhcExtension, SDHC_CONTROL_1, ClockControl);
    SdPortWait(10 * 1000);

    TraceMessage(TRACE_LEVEL_INFORMATION,
                 DRVR_LVL_FUNC,
                 (__FUNCTION__ " Entry: Mode: %08x",
                  Mode));

    return STATUS_SUCCESS;
} // SdhcSetUhsMode (...)

/*++

Routine Description:

    Set signaling voltage (1.8V or 3.3V).

Arguments:

    SdhcExtension - Host controller specific driver context.

    SignalingVoltage - Signaling voltage

Return value:

    STATUS_SUCCESS - Signaling voltage switch successful.

    STATUS_UNSUCCESSFUL - Signaling voltage switch unsuccessful.

--*/
_Use_decl_annotations_
NTSTATUS
SdhcSetSignaling (
    const SDHC_EXTENSION* SdhcExtension,
    SDPORT_SIGNALING_VOLTAGE SignalingVoltage
    )
{
    //
    // The Arasan Host Controller does not support setting the Signal Voltage.
    //

    UNREFERENCED_PARAMETER(SdhcExtension);
    UNREFERENCED_PARAMETER(SignalingVoltage);

    return STATUS_SUCCESS;
} // SdhcSetSignaling (...)

/*++

Routine Description:

    Tune the bus sampling point due to variations in voltage,
    temperature, and time.

    Caller guarantees that the bus is in a UHS mode that
    requires tuning.

Arguments:

    SdhcExtension - Host controller specific driver context.

Return value:

    STATUS_SUCCESS - Tuning successful.

--*/
_Use_decl_annotations_
NTSTATUS
SdhcExecuteTuning (
    PSDHC_EXTENSION SdhcExtension
    )
{
    ULONG HostControl2 = SdhcReadRegisterUlong(SdhcExtension, SDHC_CONTROL_2);
    SDPORT_REQUEST TuningRequest;

    NT_ASSERT((HostControl2 & SDHC_HC2_EXECUTE_TUNING) == 0);

    //
    // Disable controller events
    //
    // Technically, all controller events should be disabled at tuning execute
    // time, but some controllers do not follow this requirement.
    //

    if ((HostControl2 & SDHC_HC2_EXECUTE_TUNING) == 0) {
        HostControl2 |= SDHC_HC2_EXECUTE_TUNING;
        SdhcWriteRegisterUlong(SdhcExtension,
                               SDHC_CONTROL_2,
                               HostControl2);
    } // if

    RtlZeroMemory(&TuningRequest, sizeof(TuningRequest));
    TuningRequest.Command.TransferType = SdTransferTypeSingleBlock;
    TuningRequest.Command.TransferDirection = SdTransferDirectionRead;
    TuningRequest.Command.Class = SdCommandClassStandard;
    TuningRequest.Command.ResponseType = SdResponseTypeR1;
    if (SdhcExtension->SpeedMode == SdhcSpeedModeSDR104) {
        TuningRequest.Command.Index = 19;
        TuningRequest.Command.BlockSize = 64;
    } else {
        TuningRequest.Command.Index = 21;
        TuningRequest.Command.BlockSize = 128;
    } // if

    TraceMessage(TRACE_LEVEL_INFORMATION,
                 DRVR_LVL_FUNC,
                 (__FUNCTION__ "Exit"));

    return STATUS_SUCCESS;
} // SdhcExecuteTuning (...)

/*++

Routine Description:

    Turn the controller activity LED on/off.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Enable - Indicate whether to enable or disable the LED.

Return value:

    None.

--*/
_Use_decl_annotations_
VOID
SdhcSetLed (
    const SDHC_EXTENSION* SdhcExtension,
    BOOLEAN Enable
    )
{
    //
    // Not Supported by Host Controller.
    //

    UNREFERENCED_PARAMETER(SdhcExtension);
    UNREFERENCED_PARAMETER(Enable);
} // SdhcSetLed (...)

/*++

Routine Description:

    Enable or disable setting of preset values. Caller must
    ensure that the controller supports preset value.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Enable - Indicate whether to enable or disable preset values.

Return value:

    None.

--*/
_Use_decl_annotations_
NTSTATUS
SdhcSetPresetValue (
    const SDHC_EXTENSION* SdhcExtension,
    BOOLEAN Enable
    )
{
    //
    // Not Supported by Host Controller.
    //

    UNREFERENCED_PARAMETER(SdhcExtension);
    UNREFERENCED_PARAMETER(Enable);

    return STATUS_SUCCESS;
} // SdhcSetPresetValue (...)

/*++

Routine Description:

    Enable block gap interrupt requests.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Continue - Continue after the next block gap.

    RequestStop - Request the block gap interrupt request.

Return value:

    None.

--*/
_Use_decl_annotations_
VOID
SdhcSetBlockGapControl (
    PSDHC_EXTENSION SdhcExtension,
    BOOLEAN Continue,
    BOOLEAN RequestStop
    )
{
    ULONG BlockGapControl = SdhcReadRegisterUlong(SdhcExtension, SDHC_CONTROL_0);

    BlockGapControl &= ~SDHC_BGC_CONTINUE;
    BlockGapControl &= ~SDHC_BGC_STOP_NEXT_GAP;

    if (Continue) {
        BlockGapControl |= SDHC_BGC_CONTINUE;
    } // if

    if (RequestStop) {
        BlockGapControl |= SDHC_BGC_STOP_NEXT_GAP;
    } // if

    SdhcWriteRegisterUlong(SdhcExtension,
                           SDHC_CONTROL_0,
                           BlockGapControl);

    TraceMessage(TRACE_LEVEL_INFORMATION,
                 DRVR_LVL_FUNC,
                 (__FUNCTION__ " Exit: Continue: %d, RequestStop: %d",
                  Continue,
                  RequestStop));
} // SdhcSetBlockGapControl (...)

/*++

Routine Description:

    Set the host event mask to the new value specified.

Arguments:

    SdhcExtension - Host controller specific driver context.

    NormalInterruptMask - The new normal events to set.

Return value:

    None.

--*/
_Use_decl_annotations_
VOID
SdhcEnableInterrupt (
    PSDHC_EXTENSION SdhcExtension,
    ULONG NormalInterruptMask
    )
{
    ULONG InterruptEnable = SdhcReadRegisterUlong(SdhcExtension,
                                                  SDHC_INTERRUPT_ERROR_STATUS_ENABLE);

    //
    // The upper half of the register controls the error interrupts.
    // Unmask all of them.
    //
    InterruptEnable |= NormalInterruptMask;
    InterruptEnable |= 0xFFFF0000;

    //
    // Unmask the interrupt signals from controller to OS.
    //
    if (!SdhcExtension->CrashdumpMode) {
        SdhcWriteRegisterUlong(SdhcExtension,
                               SDHC_INTERRUPT_ERROR_STATUS_ENABLE,
                               InterruptEnable);
    }

    TraceMessage(TRACE_LEVEL_INFORMATION,
                 DRVR_LVL_FUNC,
                 (__FUNCTION__ " Exit: NormalInterruptMask: %08x",
                  NormalInterruptMask));
} // SdhcEnableInterrupt (...)

/*++

Routine Description:

    Set the host event mask to the new value specified.

Arguments:

    SdhcExtension - Host controller specific driver context.

    NormalInterruptMask - Normal Interrupts to disable.

Return value:

    None.

--*/
_Use_decl_annotations_
VOID
SdhcDisableInterrupt (
    PSDHC_EXTENSION SdhcExtension,
    ULONG NormalInterruptMask
    )
{
    ULONG InterruptDisable = SdhcReadRegisterUlong(SdhcExtension,
                                                   SDHC_INTERRUPT_ERROR_STATUS_ENABLE);

    //
    // The upper half of the register controls the error interrupts.
    // Mask all of them.
    //
    InterruptDisable &= ~NormalInterruptMask;
    InterruptDisable &= 0x0000FFFF;

    //
    // Mask the interrupt signals from controller to OS.
    //
    SdhcWriteRegisterUlong(SdhcExtension,
                           SDHC_INTERRUPT_ERROR_STATUS_ENABLE,
                           InterruptDisable);

    TraceMessage(TRACE_LEVEL_INFORMATION,
                 DRVR_LVL_FUNC,
                 (__FUNCTION__ " Entry: NormalInterruptMask: %08x",
                  NormalInterruptMask));
} // SdhcDisableInterrupt (...)

/*++

Routine Description:

    Get current pending events from the interrupt status. This function does
    not acknowledge the interrupt. The caller should acknowledge or disable
    the corresponding events.

Arguments:

    SdhcExtension - Host controller specific driver context.

Return value:

    The event mask.

--*/
_Use_decl_annotations_
USHORT
SdhcGetInterruptStatus (
    PSDHC_EXTENSION SdhcExtension
    )
{
    USHORT InterruptStatus =
        SdhcReadRegisterUlong(SdhcExtension, SDHC_INTERRUPT_ERROR_STATUS) &
        SDHC_REG_LOWER_HALF_MASK;

    //
    // 0xFFFF means HC is no longer accessible. This interrupt does not belong
    // to us.
    //
    if (InterruptStatus == 0xFFFF) {
        return 0;
    } // if

    return InterruptStatus;
} // SdhcGetInterruptStatus (...)

/*++

Routine Description:

    This routine returns the current error status, if any.

Arguments:

    SdhcExtension - Host controller specific driver context.

Return value:

    The error interrupt status.

--*/
_Use_decl_annotations_
USHORT
SdhcGetErrorStatus (
    PSDHC_EXTENSION SdhcExtension
    )
{
    USHORT InterruptStatus =
        (SdhcReadRegisterUlong(SdhcExtension, SDHC_INTERRUPT_ERROR_STATUS) &
         SDHC_REG_UPPER_HALF_MASK) >>
        SDHC_REG_SHIFT_UPPER_HALF_TO_LOWER;

    return InterruptStatus;
} // SdhcGetErrorStatus (...)

/*++

Routine Description:

    This routine returns the current Auto CMD12 error status, if any.

Arguments:

    SdhcExtension - Host controller specific driver context.

Return value:

    The Auto CMD12 error status.

--*/
_Use_decl_annotations_
USHORT
SdhcGetAutoCmd12ErrorStatus (
    PSDHC_EXTENSION SdhcExtension
    )
{
    USHORT AutoCmd12ErrorStatus =
        SdhcReadRegisterUlong(SdhcExtension, SDHC_CONTROL_2) &
        SDHC_REG_LOWER_HALF_MASK;

    TraceMessage(TRACE_LEVEL_INFORMATION,
                 DRVR_LVL_FUNC,
                 (__FUNCTION__ ": AutoCmd12ErrorStatus: %08x",
                  AutoCmd12ErrorStatus));

    return AutoCmd12ErrorStatus;
} // SdhcGetAutoCmd12ErrorStatus (...)

/*++

Routine Description:

    Acknowlege the interrupts specified.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Interrupts - event mask to acknowledge (single event only)

Return value:

    None.

--*/
_Use_decl_annotations_
VOID
SdhcAcknowledgeInterrupts (
    PSDHC_EXTENSION SdhcExtension,
    USHORT Interrupts
    )
{
    if ((Interrupts & SDHC_IS_ERROR_INTERRUPT) != 0) {
        //
        // The Auto CMD12 error interrupt status bit of some Ricoh controllers
        // can't get cleared by writing to the error status register alone.
        // Write all-ones and all-zeroes to the Auto CMD12 error status register
        // first to work around this issue. This write should have no effect on
        // other types of controllers since the register should be read-only
        // according to the spec.
        //

        SdhcWriteRegisterUlong(SdhcExtension,
                               SDHC_CONTROL_2,
                               0xFFFF);

        SdhcWriteRegisterUlong(SdhcExtension,
                               SDHC_CONTROL_2,
                               0x0);

        //
        // Clear the error interrupt by writing all-ones.
        //

        SdhcWriteRegisterUlong(SdhcExtension,
                               SDHC_INTERRUPT_ERROR_STATUS,
                               0xFFFF0000);
        Interrupts &= ~SDHC_IS_ERROR_INTERRUPT;
    } // if

    //
    // Clear other interrupts in the interrupt status register.
    //
    SdhcWriteRegisterUlong(SdhcExtension,
                           SDHC_INTERRUPT_ERROR_STATUS,
                           Interrupts);

    TraceMessage(TRACE_LEVEL_INFORMATION,
                 DRVR_LVL_FUNC,
                 (__FUNCTION__ " Exit: Interrupts: %08x",
                  Interrupts));
} // SdhcAcknowledgeInterrupts (...)

/*++

Routine Description:

    To detect whether there is a card in the socket.

Arguments:

    SdhcExtension - Host controller specific driver context.

Return value:

    BOOLEAN value to indicate whether there is a card in the socket.

--*/
_Use_decl_annotations_
BOOLEAN
SdhcIsCardInserted (
    const SDHC_EXTENSION* SdhcExtension
    )
{
    UNREFERENCED_PARAMETER(SdhcExtension);

    //
    // According to the BCM 2835 spec, the SDHC_PS_CARD_INSERTED bit field in the
    // SDHC_PRESENT_STATE register is "Read as Don't Care", so defaulting to TRUE
    // for this platform. Assuming that an SD card is always required for boot.
    //
    return TRUE;
} // SdhcIsCardInserted (...)

/*++

Routine Description:

    To detect whether the card is write protected.

Arguments:

    SdhcExtension - Host controller specific driver context.

Return value:

    BOOLEAN value to indicate whether the card is write protected.

--*/
_Use_decl_annotations_
BOOLEAN
SdhcIsWriteProtected (
    const SDHC_EXTENSION* SdhcExtension
    )
{
    UNREFERENCED_PARAMETER(SdhcExtension);

    //
    // SDHC_PS_WRITE_PROTECT in the SDHC_PRESENT_STATE register is
    // "Read as Don't Care", so defaulting to FALSE for this platform.
    //
    return FALSE;
} // SdhcIsWriteProtected (...)

/*++

Routine Description:

    This routine takes the SD command package and writes it to the appropriate
    registers on the host controller. It also computes the proper flag
    settings.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - Supplies the descriptor for this SD command

Return value:

    STATUS_SUCCESS - Command successfully sent.

    STATUS_INVALID_PARAMETER - Invalid command response type specified.

--*/
_Use_decl_annotations_
NTSTATUS
SdhcSendCommand (
    PSDHC_EXTENSION SdhcExtension,
    PSDPORT_REQUEST Request
    )
{
    PSDPORT_COMMAND Command = &Request->Command;
    ULONG CommandType;
    ULONG CommandReg;
    USHORT TransferMode = 0;
    ULONG CommandTransferModeReg;
    NTSTATUS Status;

    //
    // Initialize transfer parameters if this command is a data command.
    //
    if ((Command->TransferType != SdTransferTypeNone) &&
        (Command->TransferType != SdTransferTypeUndefined)) {
        Status = SdhcBuildTransfer(SdhcExtension, Request, &TransferMode);
        if (!NT_SUCCESS(Status)) {
            TraceMessage(TRACE_LEVEL_ERROR,
                         DRVR_LVL_ERR,
                         (__FUNCTION__ ": SdhcBuildTransfer: Status: %08x\n",
                          Status));
            return Status;
        } // if
    } // if

    //
    // Clear DMA vars, since we don't support it.
    // It maybe related to an issue in sdhost, experienced 
    // a crash when sdport was trying to flush DMA buffers, even though 
    // miniport does not support DMA.
    //
    // To do:
    // Remove once issue is resolved.
    //
    Command->DmaVirtualAddress = NULL;
    Command->ScatterGatherList = NULL;
    Command->ScatterGatherListSize = 0;

    //
    // Explanation for WorkAroundOffset is in the header file.
    // When OS wants to read from say LBA 0, the miniport actually reads
    // from WorkAroundOffset if needed. The value is registry configurable
    //
    if (Request->Type == SdRequestTypeCommandWithTransfer) {
        Command->Argument += WorkAroundOffset;
    } // if

    //
    // Set the response parameters based off the given response type.
    //

    SdhcWriteRegisterUlong(SdhcExtension, SDHC_ARGUMENT, Command->Argument);

    CommandReg = Command->Index << 24;
    switch (Command->ResponseType) {
    case SdResponseTypeNone:
        break;

    case SdResponseTypeR1:
    case SdResponseTypeR5:
    case SdResponseTypeR6:
        CommandReg |= SDHC_CMD_RESPONSE_48BIT_NOBUSY |
                      SDHC_CMD_CRC_CHECK_ENABLE |
                      SDHC_CMD_INDEX_CHECK_ENABLE;
        break;

    case SdResponseTypeR1B:
    case SdResponseTypeR5B:
        CommandReg |= SDHC_CMD_RESPONSE_48BIT_WBUSY |
                      SDHC_CMD_CRC_CHECK_ENABLE |
                      SDHC_CMD_INDEX_CHECK_ENABLE;
        break;

    case SdResponseTypeR2:
        CommandReg |= SDHC_CMD_RESPONSE_136BIT |
                      SDHC_CMD_CRC_CHECK_ENABLE;
        break;

    case SdResponseTypeR3:
    case SdResponseTypeR4:
        CommandReg |= SDHC_CMD_RESPONSE_48BIT_NOBUSY;
        break;

    default:
        NT_ASSERTMSG("SDHC - Invalid response type", FALSE);

        return STATUS_INVALID_PARAMETER;
    } // switch (Command->ResponseType)

    if (Command->TransferType != SdTransferTypeNone) {
        CommandReg |= SDHC_CMD_DATA_PRESENT;
    } else {
        TransferMode =
            SdhcReadRegisterUlong(SdhcExtension, SDHC_TRANSFER_MODE_COMMAND) &
            SDHC_REG_LOWER_HALF_MASK;

        TransferMode &= ~SDHC_TM_DMA_ENABLE;
        TransferMode &= ~SDHC_TM_AUTO_CMD12_ENABLE;
        TransferMode &= ~SDHC_TM_AUTO_CMD23_ENABLE;
    } // iff

    switch (Command->Type) {
    case SdCommandTypeSuspend:
        CommandType = SDHC_CMD_TYPE_SUSPEND;
        break;

    case SdCommandTypeResume:
        CommandType = SDHC_CMD_TYPE_RESUME;
        break;

    case SdCommandTypeAbort:
        CommandType = SDHC_CMD_TYPE_ABORT;
        break;

    default:
        CommandType = 0;
        break;
    } // switch (Command->Type)

    //
    // Set the bitmask for the required events that will fire after
    // writing to the command register. Depending on the response
    // type or whether the command involves data transfer, we will need
    // to wait on a number of different events.
    //

    InterlockedAnd((PLONG)&SdhcExtension->CurrentEvents, 0);
    Request->RequiredEvents = SDHC_IS_CMD_COMPLETE;
    if ((Command->ResponseType == SdResponseTypeR1B) ||
        (Command->ResponseType == SdResponseTypeR5B)) {

        Request->RequiredEvents |= SDHC_IS_TRANSFER_COMPLETE;
    } // if

    if (Command->TransferType != SdTransferTypeNone) {
        if (Command->TransferMethod == SdTransferMethodSgDma) {
            Request->RequiredEvents |= SDHC_IS_TRANSFER_COMPLETE;
        }
        else if (Command->TransferMethod == SdTransferMethodPio) {
            if (Command->TransferDirection == SdTransferDirectionRead) {
                Request->RequiredEvents |= SDHC_IS_BUFFER_READ_READY;
            }
            else {
                Request->RequiredEvents |= SDHC_IS_BUFFER_WRITE_READY;
            } // iff
        } // iff
    } // iff

    //
    // Issue the actual command.
    //
    CommandReg |= CommandType;
    CommandTransferModeReg = CommandReg | TransferMode;
    SdhcWriteRegisterUlong(SdhcExtension,
                           SDHC_TRANSFER_MODE_COMMAND,
                           CommandTransferModeReg);

    TraceMessage(TRACE_LEVEL_INFORMATION,
                 DRVR_LVL_FUNC,
                 (__FUNCTION__ " Exit: Cmd %d, CommandTransferModeReg: %08x, "
                  "Command->Argument: %08x, Request->RequiredEvents: %08x",
                  Command->Index,
                  CommandTransferModeReg,
                  Command->Argument,
                  Request->RequiredEvents));

    //
    // We must wait until the request completes.
    //
    return STATUS_PENDING;
} // SdhcSendCommand (...)

/*++

Routine Description:

    This routine reads the response of the SD card which is sent over
    the command line, and stores it in the specified buffer

Arguments:

    SdhcExtension - Host controller specific driver context.

    Command - Supplies the descriptor for this SD command.

    ResponseBuffer - Holds the response data.

Return value:

    STATUS_SUCCESS - Response successfully returned.

    STATUS_INVALID_PARAMETER - Invalid response length.

--*/
_Use_decl_annotations_
NTSTATUS
SdhcGetResponse (
    PSDHC_EXTENSION SdhcExtension,
    PSDPORT_COMMAND Command,
    PVOID ResponseBuffer
    )
{
    PULONG Response = (PULONG)ResponseBuffer;
    UCHAR ResponseLength = SdhcGetResponseLength(Command);

    ULONG OriginalCardSize;
    ULONG WorkaroundCardSize;

    switch (ResponseLength) {
    case 0:
        break;

    case 4:
        Response[0] = SdhcReadRegisterUlong(SdhcExtension, SDHC_RESPONSE_0);
        TraceMessage(TRACE_LEVEL_INFORMATION,
                     DRVR_LVL_FUNC,
                     (__FUNCTION__ ": Response[0]: %08x",
                      Response[0]));
        break; // case 4

    case 16:
        Response[0] = SdhcReadRegisterUlong(SdhcExtension, SDHC_RESPONSE_0);
        Response[1] = SdhcReadRegisterUlong(SdhcExtension, SDHC_RESPONSE_1);
        Response[2] = SdhcReadRegisterUlong(SdhcExtension, SDHC_RESPONSE_2);
        Response[3] = SdhcReadRegisterUlong(SdhcExtension, SDHC_RESPONSE_3);

        //
        // Since we fake the SD Card to be GPT, with the GPT header in the middle
        // of the card, the actual card size needs to be reduced (since everything
        // before the GPT header cannot be seen/accessed by the OS). The card size
        // is obtained from the host controller through CmdSendCsdSd (CMD9), so
        // here we hijack the response data and reduce the card size.
        //
        if (Command->Index == 9) {
            //
            // The SD Spec's 'CardSize' terminology is confusing. To determine
            // 'Real Card Size' (in bytes):
            // 'CardSize' = (Response[1] >> 8) & 0x3FFFFF;
            // NumBlocks = (('CardSize' + 1) * 1024);
            // 'RealCardSize' = NumBlocks * BLOCK_SIZE_IN_BYTES; (BLOCK_SIZE_IN_BYTES is 512)
            //
            // So to decrease NumBlocks by MBR_GPT_WORKAROUND_OFFSET_LBA, we can
            // decrease 'CardSize' by (WorkAroundOffset / 1024).
            //
            // This only works for High Capacity SD Cards,
            // support for non-HC to be added later.
            //

            OriginalCardSize = (Response[1] >> 8) & 0x3FFFFF;
            NT_ASSERT(OriginalCardSize > (WorkAroundOffset / 1024));

            WorkaroundCardSize = OriginalCardSize - (WorkAroundOffset / 1024);

            Response[1] &= ~(0x3FFFFF << 8);
            Response[1] |= (WorkaroundCardSize << 8);
        } // if

        TraceMessage(TRACE_LEVEL_INFORMATION,
                     DRVR_LVL_FUNC,
                     (__FUNCTION__ ": Response[0-3]: %08x, %08x, %08x, %08x",
                      Response[0],
                      Response[1],
                      Response[2],
                      Response[3]));
        break; // case 16

    default:
        return STATUS_INVALID_PARAMETER;
    } // switch (ResponseLength)

    return STATUS_SUCCESS;
} // SdhcGetResponse (...)

/*++

Routine Description:

    This routine sets up the host for a data transfer.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - The command for which we're building the transfer.

    TransferMode - The bits that go into the TransferMode register.

Return value:

    STATUS_SUCCESS - Transfer mode successfully set.

    STATUS_INVALID_PARAMETER - Invalid command transfer parameters.

--*/
_Use_decl_annotations_
NTSTATUS
SdhcSetTransferMode (
    PSDHC_EXTENSION SdhcExtension,
    PSDPORT_REQUEST Request,
    PUSHORT TransferMode
    )
{
    USHORT BlockCount = Request->Command.BlockCount;
    USHORT BlockSize = Request->Command.BlockSize;

    NT_ASSERT(Request->Command.TransferMethod != SdTransferMethodUndefined);
    NT_ASSERT(BlockSize <= SdhcExtension->Capabilities.MaximumBlockSize);
    NT_ASSERT(BlockSize != 0);
    NT_ASSERT(Request->Command.Length != 0);

    *TransferMode = 0;

    if (BlockSize > 2048) {
        NT_ASSERT(!"SDHC - Invalid block size for command");
        return STATUS_INVALID_PARAMETER;
    } // if

    if ((Request->Command.TransferDirection != SdTransferDirectionRead) &&
        (Request->Command.TransferDirection != SdTransferDirectionWrite)) {
        return STATUS_INVALID_PARAMETER;
    } // if

    *TransferMode &= ~(SDHC_TM_AUTO_CMD12_ENABLE |
                       SDHC_TM_AUTO_CMD23_ENABLE |
                       SDHC_TM_DMA_ENABLE |
                       SDHC_TM_BLKCNT_ENABLE |
                       SDHC_TM_MULTIBLOCK);

    //
    // Adjust BlockSize and BlockCount for 
    // unaligned requests, if needed...
    //
    BlockCount = Request->Command.BlockCount =
        (USHORT) (Request->Command.Length / BlockSize);
    if (BlockCount == 0) {
        BlockCount = Request->Command.BlockCount = 1;
        BlockSize = Request->Command.BlockSize =
            (USHORT) Request->Command.Length;
    } // if

    //
    // Check and start Non BlockSize aligned requests, if needed
    //
    if (SdhcStartNonBlockSizeAlignedRequest(SdhcExtension, Request)) {
        TraceMessage(TRACE_LEVEL_INFORMATION,
                     DRVR_LVL_INFO,
                     (__FUNCTION__ " Unaligned request initiated: Cmd %d, "
                      "Length: %d, BlockCount: %d, BlockSize: %d",
                      Request->Command.Index,
                      Request->Command.Length,
                      BlockCount,
                      BlockSize));
    } // if

    if (BlockCount > 1) {
        *TransferMode |= SDHC_TM_MULTIBLOCK;
        *TransferMode |= SDHC_TM_BLKCNT_ENABLE;
        *TransferMode |= SDHC_TM_AUTO_CMD12_ENABLE;
    } // if

    // 
    // Update command argument according to modified settings...
    //
    switch (Request->Command.Index) {
    case SDCMD_IO_RW_EXTENDED:
    {
        SD_RW_EXTENDED_ARGUMENT* ArgumentExt = 
            (SD_RW_EXTENDED_ARGUMENT*)&Request->Command.Argument;
        //
        // Cmd53 uses I/O abort function select bits (ASx) in the CCCR 
        //
        *TransferMode &= ~SDHC_TM_AUTO_CMD12_ENABLE;

        if (BlockCount > 1) {
            ArgumentExt->u.bits.Count = (ULONG)BlockCount;
            ArgumentExt->u.bits.BlockMode = 1;
        } else {
            NT_ASSERT(BlockCount == 1);
            ArgumentExt->u.bits.Count = (ULONG)Request->Command.Length;
            ArgumentExt->u.bits.BlockMode = 0;
        } // iff
        break;
    }

    default:
        break;
    } // SDCMD_IO_RW_EXTENDED

    if (Request->Command.TransferMethod == SdTransferMethodSgDma) {
        *TransferMode |= SDHC_TM_DMA_ENABLE;
    } else {
        NT_ASSERT(Request->Command.TransferMethod == SdTransferMethodPio);
    } // iff

    *TransferMode &= ~SDHC_TM_TRANSFER_READ;
    if (Request->Command.TransferDirection == SdTransferDirectionRead) {
        *TransferMode |= SDHC_TM_TRANSFER_READ;
    } // if

    SdhcWriteRegisterUlong(SdhcExtension, SDHC_SYSADDR, BlockCount);
    SdhcWriteRegisterUlong(SdhcExtension,
                           SDHC_BLOCK_SIZE_COUNT,
                           (BlockCount << 16) | BlockSize);

    TraceMessage(TRACE_LEVEL_INFORMATION,
                 DRVR_LVL_FUNC,
                 (__FUNCTION__ " Exit: Cmd %d, TransferMode: %08x, "
                  "Length: %d, BlockCount: %d, BlockSize: %d",
                  Request->Command.Index,
                  *TransferMode,
                  Request->Command.Length,
                  BlockCount,
                  BlockSize));

    return STATUS_SUCCESS;
} // SdhcSetTransferMode (...)

/*++

Routine Description:

    The data port must be accessed maintaining DWORD alignment.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Buffer - Data buffer to read.

    Length - Number of bytes to read.

Return value:

--*/
_Use_decl_annotations_
VOID
SdhcReadDataPort (
    PSDHC_EXTENSION SdhcExtension,
    PUCHAR Buffer,
    ULONG Length
    )
{
    ULONG ByteCount = Length % sizeof(ULONG);
    ULONG WordCount = Length / sizeof(ULONG);
    ULONG* Register =
        (ULONG*)((ULONG_PTR)SdhcExtension->BaseAddress + SDHC_DATA_PORT);
    ULONG* Target = (ULONG*)Buffer;

    while (WordCount) {
        READ_REGISTER_NOFENCE_BUFFER_ULONG(Register, Target, 1);
        ++Target;
        --WordCount;
    } // while (WordCount)

    if (ByteCount != 0) {
        ULONG LastData;
        READ_REGISTER_NOFENCE_BUFFER_ULONG(Register, &LastData, 1);
        RtlCopyMemory(Target, &LastData, ByteCount);
    } // if

} // SdhcReadDataPort (...)

/*++

Routine Description:

    The data port must be accessed maintaining DWORD alignment.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Buffer - Data buffer to write.

    Length - Number of bytes to write.

Return value:

--*/
_Use_decl_annotations_
VOID
SdhcWriteDataPort (
    PSDHC_EXTENSION SdhcExtension,
    PUCHAR Buffer,
    ULONG Length
    )
{
    ULONG ByteCount = Length % sizeof(ULONG);
    ULONG WordCount = Length / sizeof(ULONG);
    ULONG* Register = (ULONG*)((ULONG_PTR)SdhcExtension->BaseAddress + SDHC_DATA_PORT);
    ULONG* Source = (ULONG*)Buffer;

    while (WordCount) {
        WRITE_REGISTER_NOFENCE_BUFFER_ULONG(Register, Source, 1);
        ++Source;
        --WordCount;
    } // while (WordCount)

    if (ByteCount != 0) {
        ULONG LastData = 0;
        RtlCopyMemory(&LastData, Source, ByteCount);
        WRITE_REGISTER_NOFENCE_BUFFER_ULONG(Register, &LastData, 1);
    } // if
} // SdhcWriteDataPort (...)

/*++

Routine Description:

    Prepare the transfer request.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Command - The command for which we're building the transfer request.

    TransferMode - The bits that go into the TransferMode register.

Return value:

    NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS
SdhcBuildTransfer (
    PSDHC_EXTENSION SdhcExtension,
    PSDPORT_REQUEST Request,
    PUSHORT TransferMode
    )
{
    NTSTATUS Status;

    NT_ASSERT(Request->Command.TransferType != SdTransferTypeNone);
    NT_ASSERT(Request->Command.TransferMethod != SdTransferMethodUndefined);

    switch (Request->Command.TransferMethod) {
    case SdTransferMethodPio:
        Status = SdhcBuildPioTransfer(SdhcExtension, Request, TransferMode);
        break;

    case SdTransferMethodSgDma:
        Status = SdhcBuildAdmaTransfer(SdhcExtension, Request, TransferMode);
        break;

    default:
        Status = STATUS_NOT_SUPPORTED;
        break;
    } // switch (Request->Command.TransferMethod)

    return Status;
} // SdhcBuildTransfer (...)

/*++

Routine Description:

    Execute the transfer request.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - The command for which we're building the transfer request.

Return value:

    NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS
SdhcStartTransfer (
    PSDHC_EXTENSION SdhcExtension,
    PSDPORT_REQUEST Request
    )
{

    NTSTATUS Status;

    NT_ASSERT(Request->Command.TransferType != SdTransferTypeNone);

    switch (Request->Command.TransferMethod) {
    case SdTransferMethodPio:
        Status = SdhcStartPioTransfer(SdhcExtension, Request);
        break;

    case SdTransferMethodSgDma:
        Status = SdhcStartAdmaTransfer(SdhcExtension, Request);
        break;

    default:
        Status = STATUS_NOT_SUPPORTED;
        break;
    } // switch (Request->Command.TransferMethod)

    return Status;
} // SdhcStartTransfer (...)

/*++

Routine Description:

    Prepare the PIO transfer request.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - The command for which we're building the transfer.

    TransferMode - The bits that go into the TransferMode register.

Return value:

    NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS
SdhcBuildPioTransfer (
    PSDHC_EXTENSION SdhcExtension,
    PSDPORT_REQUEST Request,
    PUSHORT TransferMode
    )
{
    return SdhcSetTransferMode(SdhcExtension, Request, TransferMode);
} // SdhcBuildPioTransfer (...)

/*++

Routine Description:

    Prepare the ADMA2 transfer request.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - The command for which we're building the transfer.

    TransferMode - The bits that go into the TransferMode register.

Return value:

    NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS
SdhcBuildAdmaTransfer (
    const SDHC_EXTENSION* SdhcExtension,
    const SDPORT_REQUEST* Request,
    PUSHORT TransferMode
    )
{
    //
    // The miniport does not support DMA tranfers.
    //

    UNREFERENCED_PARAMETER(SdhcExtension);
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(TransferMode);

    return STATUS_NOT_IMPLEMENTED;
} // SdhcBuildAdmaTransfer (...)

/*++

Routine Description:

    Execute the PIO transfer request.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - The command for which we're building the transfer.

Return value:

    NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS
SdhcStartPioTransfer (
    PSDHC_EXTENSION SdhcExtension,
    PSDPORT_REQUEST Request
    )
{
    ULONG CurrentEvents;
    NTSTATUS Status = STATUS_PENDING;

    NT_ASSERT((Request->Command.TransferDirection == SdTransferDirectionRead) ||
              (Request->Command.TransferDirection == SdTransferDirectionWrite));

    CurrentEvents = InterlockedExchange((PLONG)&SdhcExtension->CurrentEvents,
                                        0);

    if (Request->Command.TransferDirection == SdTransferDirectionRead) {
        SdhcReadDataPort(SdhcExtension,
                         Request->Command.DataBuffer,
                         Request->Command.BlockSize);
    } else {
        SdhcWriteDataPort(SdhcExtension,
                          Request->Command.DataBuffer,
                          Request->Command.BlockSize);
    } // iff

    --Request->Command.BlockCount;
    if (Request->Command.BlockCount >= 1) {
        Request->Command.DataBuffer += Request->Command.BlockSize;
        if (Request->Command.TransferDirection == SdTransferDirectionRead) {
            Request->RequiredEvents |= SDHC_IS_BUFFER_READ_READY;

        } else {
            Request->RequiredEvents |= SDHC_IS_BUFFER_WRITE_READY;
        } // if
        Request->Status = STATUS_MORE_PROCESSING_REQUIRED;
    } else {
        NT_ASSERT(Request->Command.BlockCount == 0);

        Request->Status = STATUS_SUCCESS;
        if ((CurrentEvents & SDHC_IS_TRANSFER_COMPLETE) != 0) {
            SdhcCompleteRequest(SdhcExtension, Request, STATUS_SUCCESS);
            Status = STATUS_SUCCESS;
        } else {
            Request->RequiredEvents |= SDHC_IS_TRANSFER_COMPLETE;
        } // iff
    } // iff

    TraceMessage(TRACE_LEVEL_INFORMATION,
                 DRVR_LVL_FUNC,
                 (__FUNCTION__ ": TransferDirection: %d, BlockSize: %d, "
                  "BlockCount: %d, RequiredEvents: %08x",
                  Request->Command.TransferDirection,
                  Request->Command.BlockSize,
                  Request->Command.BlockCount,
                  Request->RequiredEvents));

    return Status;
} // SdhcStartPioTransfer (...)

/*++

Routine Description:

    SdhcStartNonBlockSizeAlignedRequest is called for every command with data.
    With SDIO Cmd53 it is possible that request data length may not
    be integer product of BlockSize.
    In this case an additional internal request is initialized for
    reading/writing the trailing bytes.
    SdhcStartNonBlockSizeAlignedRequest checks the request data length
    and initiates the processing state machine if needed.

Arguments:

    SdhcExtension - Host controller specific driver context.

Request - The original unaligned request from sdport.

    StateStatus - The current unaligned request handling status

Return value:

    TRUE - Request length is not aligned to BlockSize, processing SM 
        was initailzed.
    FALSE - Request length is aligned to BlockSize.

--*/
_Use_decl_annotations_
BOOLEAN
SdhcStartNonBlockSizeAlignedRequest (
    PSDHC_EXTENSION SdhcExtension,
    const SDPORT_REQUEST* Request
    )
{
    //
    // SDIO request (Cmd53) only
    //
    if (Request->Command.Index != SDCMD_IO_RW_EXTENDED) {
        SdhcExtension->UnalignedReqState = UnalignedReqStateIdle;
        return FALSE;
    }

    if (Request == &SdhcExtension->UnalignedRequest) {
        return FALSE;
    }

    NT_ASSERT(Request->Command.Length != 0);
    NT_ASSERT(Request->Command.BlockCount != 0);
    NT_ASSERT(Request->Command.BlockSize != 0);

    //
    // Check if request length is not aligned to BlockSize
    //
    if (Request->Command.Length >
        (ULONG)(Request->Command.BlockCount * Request->Command.BlockSize)) {

        NT_ASSERT(SdhcExtension->UnalignedReqState == UnalignedReqStateIdle);

        //
        // Prepare the internal request we need to read/send 
        // after aligned part was read/sent.
        //
        SdhcPrepareInternalRequest(SdhcExtension, Request);

        //
        // Start the SM
        //
        SdhcExtension->UnalignedReqState = UnalignedReqStateReady;
        return TRUE;
    } // if

    return FALSE;
}// SdhcStartNonBlockSizeAlignedRequest (...)

/*++

Routine Description:

    SdhcCompleteNonBlockSizeAlignedRequest is called for every completed command.
    If the command has completed successfully, it runs the 
    'Non BlockSize Aligned' state machine to handle the additional 
    request that sends/reads the unaligned part.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - The original unaligned request from sdport.

    CompletionStatus - Request completion status

Return value:

    STATUS_MORE_PROCESSING_REQUIRED- More processing is required. Caller
    should withhold further processing for Request, otherwise caller
    may continue Request processing.

--*/
_Use_decl_annotations_
NTSTATUS
SdhcCompleteNonBlockSizeAlignedRequest (
    PSDHC_EXTENSION SdhcExtension,
    const SDPORT_REQUEST* Request,
    NTSTATUS CompletionStatus
    )
{
    //
    // On error, reset the state machine...
    //
    if (!NT_SUCCESS(CompletionStatus) &&
        (CompletionStatus != STATUS_MORE_PROCESSING_REQUIRED)) {
        SdhcExtension->UnalignedReqState = UnalignedReqStateIdle;
        return STATUS_SUCCESS;
    } // if

    //
    // Run the state machine...  
    //
    return SdhcNonBlockSizeAlignedRequestSM(SdhcExtension, Request);
} // SdhcCompleteNonBlockSizeAlignedRequest (...)

/*++

Routine Description:

    SdhcNonBlockSizeAlignedRequestSM implements the state machine 
    of handling the transmission of the additional request after the 
    BlockSize aligned part was received/sent.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - The original unaligned request from sdport.

    StateStatus - The current unaligned request handling status

Return value:

    STATUS_MORE_PROCESSING_REQUIRED- More processing is required. Caller 
    should withhold further processing for Request, otherwise caller
    may continue Request processing.

--*/
_Use_decl_annotations_
NTSTATUS
SdhcNonBlockSizeAlignedRequestSM (
    PSDHC_EXTENSION SdhcExtension,
    const SDPORT_REQUEST* Request
    )
{
    PSDPORT_REQUEST InternalRequest = &SdhcExtension->UnalignedRequest;
    NTSTATUS Status;

    switch (SdhcExtension->UnalignedReqState) {
    case UnalignedReqStateIdle:
    {
        return STATUS_SUCCESS;
    } // UnalignedReqStateIdle

    case UnalignedReqStateReady:
    {
        //
        // We wait until aligned part of request is done...
        //
        if (Request->Command.BlockCount != 0) {
            return STATUS_SUCCESS;
        } // if
        SdhcExtension->UnalignedReqState = UnalignedReqStateSendCommand;

        Status = SdhcSendCommand(SdhcExtension, InternalRequest);
        if (!NT_SUCCESS(Status)) {
            TraceMessage(TRACE_LEVEL_WARNING,
                         DRVR_LVL_WARN,
                         (__FUNCTION__ ": Unaligned Cmd %d failed "
                          "during SendCommand",
                          Request->Command.Index));
            SdhcExtension->UnalignedReqState = UnalignedReqStateIdle;
            return Status;
        } // if
        return STATUS_MORE_PROCESSING_REQUIRED;
    } // UnalignedReqStateReady

    case UnalignedReqStateSendCommand:
    {
        SdhcExtension->UnalignedReqState = UnalignedReqStateStartTransfer;

        Status = SdhcStartTransfer(SdhcExtension, InternalRequest);
        if (!NT_SUCCESS(Status)) {
            TraceMessage(TRACE_LEVEL_WARNING,
                         DRVR_LVL_WARN,
                         (__FUNCTION__ ": Unaligned Cmd %d failed "
                          "during StartTransfer",
                          Request->Command.Index));
            SdhcExtension->UnalignedReqState = UnalignedReqStateIdle;
            return Status;
        } else if (Status == STATUS_PENDING) {
            return STATUS_MORE_PROCESSING_REQUIRED;
        }

        //
        // Request has completed after transfer, proceed to next state
        //
        NT_ASSERT(Status == STATUS_SUCCESS);
        __fallthrough;
    } // UnalignedReqStateSendCommand

    case UnalignedReqStateStartTransfer:
    {
        //
        // We are done, original request can be now completed.
        //
        SdhcExtension->UnalignedReqState = UnalignedReqStateIdle;
        TraceMessage(TRACE_LEVEL_INFORMATION,
                     DRVR_LVL_INFO,
                     (__FUNCTION__ ": Unaligned Cmd %d completed successfully",
                      Request->Command.Index));
        return STATUS_SUCCESS;
    } // UnalignedReqStateStartTransfer

    default:
        SdhcExtension->UnalignedReqState = UnalignedReqStateIdle;
        return STATUS_INVALID_PARAMETER;
    } // switch

} // SdhcNonBlockSizeAlignedRequestSM (...)

/*++

Routine Description:

    This routine is called by SdhcStartNonBlockSizeAlignedRequest to prepare 
    the internal request that will read/write the last bytes of the unaligned 
    request.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - The original unaligned request from sdport.

Return value:

--*/
_Use_decl_annotations_
VOID
SdhcPrepareInternalRequest (
    PSDHC_EXTENSION SdhcExtension,
    const SDPORT_REQUEST* Request
    )
{
    USHORT BlockSize = Request->Command.BlockSize;
    USHORT BlockCount = Request->Command.BlockCount;
    PSDPORT_REQUEST InternalRequest = &SdhcExtension->UnalignedRequest;
    SD_RW_EXTENDED_ARGUMENT* InternalArgumentExt =
        (SD_RW_EXTENDED_ARGUMENT*)&InternalRequest->Command.Argument;

    //
    // Prepare a single block request that transfers the last 
    // bytes of the original request data (of size less than BlockSize).
    //
    RtlCopyMemory(InternalRequest, Request, sizeof(SDPORT_REQUEST));

    //
    // Change to a single block
    //
    InternalRequest->Command.TransferType = SdTransferTypeSingleBlock;
    //
    // Set the length parameters for the last bytes of the data
    //
    InternalRequest->Command.Length = Request->Command.Length % BlockSize;
    InternalRequest->Command.BlockCount = 1;
    InternalRequest->Command.BlockSize = 
        (USHORT) InternalRequest->Command.Length;
    //
    // Set the data pointer the address that follows the
    // BlockSize aligned part.
    //
    InternalRequest->Command.DataBuffer += (BlockSize * BlockCount);
    //
    // If the command writes to a region of addresses rather than
    // to a single address, update the address to point the 
    // address that follows the BlockSize aligned part.
    //
    if (InternalArgumentExt->u.bits.OpCode) {
        InternalArgumentExt->u.bits.Address += (BlockSize * BlockCount);
    } // if

    TraceMessage(TRACE_LEVEL_INFORMATION,
                 DRVR_LVL_INFO,
                 (__FUNCTION__ ": Preparing Unaligned Cmd %d, Addr: %x, "
                  "Length: %d, Orig Length %d, Block size: %d",
                  Request->Command.Index,
                  InternalArgumentExt->u.bits.Address,
                  InternalRequest->Command.Length,
                  Request->Command.Length,
                  BlockSize));
} // SdhcPrepareInternalRequest (...)

/*++

Routine Description:

    Execute the ADMA2 transfer request.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - The command for which we're building the transfer.

Return value:

    NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS
SdhcStartAdmaTransfer (
    const SDHC_EXTENSION* SdhcExtension,
    PSDPORT_REQUEST Request
    )
{
    UNREFERENCED_PARAMETER(SdhcExtension);

    Request->Status = STATUS_SUCCESS;
    SdPortCompleteRequest(Request, Request->Status);
    return STATUS_SUCCESS;
} // SdhcStartAdmaTransfer (...)

/*++

Routine Description:

    Calculates the appropriate clock divisor based on the
    given target clock frequency.

Arguments:

    SdhcExtension - Host controller specific driver context.

    TargetFrequency - The frequency in kHz to which we want to set the bus.

    ActualFrequency - The actual frequency to which the bus is set.

Return value:

    ClockControl - The value of the Clock Control register to be written.

--*/
_Use_decl_annotations_
USHORT
SdhcCalcClockFrequency (
    PSDHC_EXTENSION SdhcExtension,
    ULONG TargetFrequency,
    PULONG ActualFrequency
    )
{
    ULONG BaseFrequency = SdhcExtension->Capabilities.BaseClockFrequencyKhz;
    USHORT SpecVersion =
        (SdhcReadRegisterUlong(SdhcExtension, SDHC_SLOT_INFORMATION_VERSION) >>
         SDHC_REG_SHIFT_UPPER_HALF_TO_LOWER) & 0xFF;
    ULONG Divisor;
    USHORT ClockControl;

    *ActualFrequency = 0;

    Divisor = BaseFrequency / TargetFrequency;
    if (Divisor == 0) Divisor = 1;

    if (SpecVersion > SDHC_SPEC_VERSION_3) {
        //
        // Calculate the fastest available clock frequency which is <=
        // tthe requested frequency.
        //

        Divisor = 1;
        while (((BaseFrequency / Divisor) > TargetFrequency) &&
               (Divisor < SDHC_MAX_CLOCK_DIVISOR)) {

            Divisor <<= 1;
        } // while (...)

        *ActualFrequency = BaseFrequency / Divisor;
        Divisor >>= 1;
        ClockControl = ((USHORT)Divisor << 8);
    } else {
        //
        // Host controller version 3.0 supports the 10-bit divided clock mode.
        //

        Divisor = BaseFrequency / TargetFrequency;
        Divisor >>= 1;
        if ((TargetFrequency < BaseFrequency) &&
            (TargetFrequency * 2 * Divisor != BaseFrequency)) {

            Divisor += 1;
        } // if

        if (Divisor > SDHC_MAX_CLOCK_DIVISOR_SPEC_3 / 2) {
            Divisor = SDHC_MAX_CLOCK_DIVISOR_SPEC_3 / 2;
        } // if

        if (Divisor == 0) {
            *ActualFrequency = BaseFrequency;
        } else {
            *ActualFrequency = BaseFrequency / Divisor;
            *ActualFrequency >>= 1;
        } // iff

        ClockControl = ((USHORT)Divisor & 0xFF) << 8;
        Divisor >>= 8;
        ClockControl |= ((USHORT)Divisor & 0x03) << 6;
    } // iff

    NT_ASSERT((BaseFrequency <= TargetFrequency) ? (Divisor == 0) : TRUE);

    TraceMessage(TRACE_LEVEL_INFORMATION,
                 DRVR_LVL_FUNC,
                 (__FUNCTION__ ": BaseFrequency: %d, TargetFrequency: %d, Divisor: %d, ClockControl: %08x",
                  BaseFrequency,
                  TargetFrequency,
                  Divisor,
                  ClockControl));

    return ClockControl;
} // SdhcCalcClockFrequency (...)

/*++

Routine Description:

    SdhcGetHwUhsMode translates sdport bus speed codes to Arasan's

Arguments:

    BusSpeed - Sdport bus speed code.

Return value:

    The counterpart Arasan bus speed code, or 0 if not supported or invalid.

--*/
_Use_decl_annotations_
ULONG
SdhcGetHwUhsMode (
    SDPORT_BUS_SPEED BusSpeed
    )
{
    switch (BusSpeed) {
    case SdBusSpeedSDR12:
        return SDHC_HC2_SDR12;

    case SdBusSpeedSDR25:
        return SDHC_HC2_SDR25;

    case SdBusSpeedSDR50:
        return SDHC_HC2_SDR50;

    case SdBusSpeedDDR50:
        return SDHC_HC2_SDR50;

    case SdBusSpeedSDR104:
        return SDHC_HC2_SDR50;

    //
    // PCI controllers don't support the higher speed eMMC modes.
    //
    case SdBusSpeedHS200:
    case SdBusSpeedHS400:

    case SdBusSpeedNormal:
    case SdBusSpeedHigh:

    default:
        NT_ASSERT(!"SDHC - Invalid bus speed selected");
        break;
    } // switch (BusSpeed)

    return 0;
} // SdhcGetHwUhsMode (...)

/*++

Routine Description:

    SdhcConvertErrorToStatus translates Arasan error codes to NTSTATUS.

Arguments:

    Error - Arasan error code.

Return value:

    The counterpart NTSTATUS.

--*/
_Use_decl_annotations_
NTSTATUS
SdhcConvertErrorToStatus (
    USHORT Error
    )
{
    if (Error == 0) {
        return STATUS_SUCCESS;
    } // if

    if (Error & (SDHC_ES_CMD_TIMEOUT | SDHC_ES_DATA_TIMEOUT)) {
        return STATUS_IO_TIMEOUT;
    } // if

    if (Error & (SDHC_ES_CMD_CRC_ERROR | SDHC_ES_DATA_CRC_ERROR)) {
        return STATUS_CRC_ERROR;
    } // if

    if (Error & (SDHC_ES_CMD_END_BIT_ERROR | SDHC_ES_DATA_END_BIT_ERROR)) {
        return STATUS_DEVICE_DATA_ERROR;
    } // if

    if (Error & SDHC_ES_CMD_INDEX_ERROR) {
        return STATUS_DEVICE_PROTOCOL_ERROR;
    } // if

    if (Error & SDHC_ES_BUS_POWER_ERROR) {
        return STATUS_DEVICE_POWER_FAILURE;
    } // if

    return STATUS_IO_DEVICE_ERROR;
} // SdhcConvertErrorToStatus (...)

/*++

Routine Description:

    Return the number of bytes associated with a given response type.

Arguments:

    ResponseType

Return value:

    Length of response.

--*/
_Use_decl_annotations_
UCHAR
SdhcGetResponseLength (
    const SDPORT_COMMAND* Command
    )
{
    UCHAR Length;

    switch (Command->ResponseType) {
    case SdResponseTypeR1:
    case SdResponseTypeR3:
    case SdResponseTypeR4:
    case SdResponseTypeR5:
    case SdResponseTypeR6:
    case SdResponseTypeR1B:
    case SdResponseTypeR5B:
        Length = 4;
        break;

    case SdResponseTypeR2:
        Length = 16;
        break;

    case SdResponseTypeNone:
        Length = 0;
        break;

    default:
        NT_ASSERT(!"Invalid response type");
        Length = 0;
        break;
    } // switch (Command->ResponseType)

    return Length;
} // SdhcGetResponseLength (...)

/*++

Routine Description:

    SdhcCompleteRequest clears the miniport run times variables associated
    with the request processing, and completes the request with SDPORT.

Arguments:

    SdhcExtension - The miniport extension.

    Request - The request to completed.

    Status - The completion status.

Return value:

--*/
_Use_decl_annotations_
VOID
SdhcCompleteRequest(
    PSDHC_EXTENSION SdhcExtension,
    PSDPORT_REQUEST Request,
    NTSTATUS Status
    )
{
    const SDPORT_REQUEST* CurRequest;
	const SDPORT_COMMAND* Command = &Request->Command;
    BOOLEAN IsCommandCompleted = TRUE;

    if (Request == &SdhcExtension->UnalignedRequest) {
        return;
    }

    //
    // Data commands are done after all data has been
    // transfered.
    //

    if ((Command->TransferType != SdTransferTypeNone) &&
        (Command->TransferType != SdTransferTypeUndefined)) {
        IsCommandCompleted = Command->BlockCount == 0;
    } // if

    if (IsCommandCompleted) {
        CurRequest = (const SDPORT_REQUEST*)
            InterlockedExchangePointer(&SdhcExtension->OutstandingRequest,
                                       NULL);
        if (CurRequest != Request) {
            NT_ASSERT(FALSE);
        } // if
    } // if

    InterlockedIncrement(&SdhcExtension->CmdCompleted);
    SdPortCompleteRequest(Request, Status);
} // SdhcCompleteRequest (...)

_Use_decl_annotations_
NTSTATUS
SdhcPowerControlCallback(
    _In_ PSD_MINIPORT Miniport,
    _In_ LPCGUID PowerControlCode,
    _In_reads_bytes_opt_(InputBufferSize) PVOID InputBuffer,
    _In_ SIZE_T InputBufferSize,
    _Out_writes_bytes_opt_(OutputBufferSize) PVOID OutputBuffer,
    _In_ SIZE_T OutputBufferSize,
    _Out_opt_ PSIZE_T BytesReturned
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
VOID
SdhcCleanup(
    _In_ PSD_MINIPORT Miniport
    )
{
    UNREFERENCED_PARAMETER(Miniport);
#ifdef WPP_TRACING
    WPP_CLEANUP(NULL);
#endif
}
