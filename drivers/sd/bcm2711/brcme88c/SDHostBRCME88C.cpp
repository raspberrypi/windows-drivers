// Adapted from sample code: Copyright (c) Microsoft Corporation
// https://github.com/microsoft/Windows-driver-samples/tree/master/sd/miniport/sdhc

#include <wdm.h>
#include <TraceLoggingProvider.h>
#include <evntrace.h>
extern "C" {
#include <sdport.h>
}

#include "SDHostBRCME88C.h"

TRACELOGGING_DEFINE_PROVIDER(
    g_trace,
    "SDHostBRCME88C",
    // *SDHostBRCME88C = {f2782ab9-d1a5-5f95-240a-ac933a6937e2}
    (0xf2782ab9, 0xd1a5, 0x5f95, 0x24, 0x0a, 0xac, 0x93, 0x3a, 0x69, 0x37, 0xe2));

__forceinline constexpr ULONG
Min(ULONG x, ULONG y)
{
    return x < y ? x : y;
}

__forceinline constexpr ULONG
Max(ULONG x, ULONG y)
{
    return x < y ? y : x;
}

//-----------------------------------------------------------------------------
// SlotExtension routines.
//-----------------------------------------------------------------------------

_Use_decl_annotations_
extern "C"
NTSTATUS
__declspec(code_seg("INIT"))
DriverEntry(
    _In_ DRIVER_OBJECT* DriverObject,
    _In_ UNICODE_STRING* RegistryPath)
{
    SDPORT_INITIALIZATION_DATA InitializationData;

    RtlZeroMemory(&InitializationData, sizeof(InitializationData));
    InitializationData.StructureSize = sizeof(InitializationData);

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
    InitializationData.PowerControlCallback = SdhcPoFxPowerControlCallback;
    InitializationData.Cleanup = SdhcCleanup;

    InitializationData.PrivateExtensionSize = sizeof(SDHC_EXTENSION);
    InitializationData.CrashdumpSupported = TRUE;

    NTSTATUS Status = SdPortInitialize(DriverObject, RegistryPath, &InitializationData);

    TraceLoggingRegister(g_trace);

    LogInfo("DriverEntry", TraceLoggingNTStatus(Status));

    if (!NT_SUCCESS(Status))
    {
        TraceLoggingUnregister(g_trace);
    }

    return Status;
}

void
SdhcCleanup(
    _In_ SD_MINIPORT* Miniport
)

/*++

Routine Description:

    Cleanup any memory allocations done during the lifetime of the driver.

Arguments:

    Miniport - Miniport interface for the controller.

Return value:

    NTSTATUS

--*/

{
    UNREFERENCED_PARAMETER(Miniport);
    LogInfo("Cleanup");
    TraceLoggingUnregister(g_trace);
}

NTSTATUS
SdhcGetSlotCount(
    _In_ SD_MINIPORT* Miniport,
    _Out_ UCHAR* SlotCount
)

/*++

Routine Description:

    Return the number of slots present on this controller.

Arguments:

    Argument - Functional device object for this controller.

Return value:

    STATUS_UNSUCCESSFUL - PCI config space was unable to be queried.

    STATUS_INVALID_PARAMETER - Invalid underlying bus type for the miniport.

    STATUS_SUCCESS - SlotCount returned properly.

--*/

{

    SDPORT_BUS_TYPE BusType;
    UCHAR Data;
    NTSTATUS Status;

    *SlotCount = 0;
    BusType = Miniport->ConfigurationInfo.BusType;
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
        Data = 0;
        Status = SdPortGetPciConfigSpace(Miniport,
            SDHC_PCICFG_SLOT_INFORMATION,
            &Data,
            sizeof(Data));

        if (!NT_SUCCESS(Status)) {
            return Status;
        }

        *SlotCount = (Data >> 4) + 1;
        if ((*SlotCount) > 6) {

            NT_ASSERT((*SlotCount) <= 6);

            Status = STATUS_UNSUCCESSFUL;
        }

        SdhcInitializePciConfigSpace(Miniport);
        Status = STATUS_SUCCESS;
        break;

    default:

        NT_ASSERT((BusType == SdBusTypeAcpi) || (BusType == SdBusTypePci));

        *SlotCount = 1;
        Status = STATUS_INVALID_PARAMETER;
        break;
    }

    return Status;
}

void
SdhcGetSlotCapabilities(
    _In_ void* PrivateExtension,
    _Out_ SDPORT_CAPABILITIES* Capabilities
)

/*++

Routine Description:

    Override for miniport to provide host register mapping information if the
    memory range provideed by the underlying bus isn't sufficient.

Arguments:

    SlotExtension - SlotExtension interface between sdhost and this miniport.

    Capabilities - Miniport provided capabilities.

Return value:

    STATUS_SUCCESS - Capabilities returned successfully.

--*/

{

    SDHC_EXTENSION* SdhcExtension;

    SdhcExtension = (SDHC_EXTENSION*)PrivateExtension;
    RtlCopyMemory(Capabilities,
        &SdhcExtension->Capabilities,
        sizeof(SdhcExtension->Capabilities));
}

NTSTATUS
SdhcSlotInitialize(
    _In_ void* PrivateExtension,
    _In_ PHYSICAL_ADDRESS PhysicalBase,
    _In_ void* VirtualBase,
    _In_ ULONG Length,
    _In_ BOOLEAN CrashdumpMode
)

/*++

Routine Description:

    Initialize the miniport for standard host controllers.

Arguments:

    SlotExtension - SlotExtension interface between sdhost and this miniport.

    PhysicalBase - Physical base address of the host controller register space.

    VirtualBase - Mapped register space.

    Length - Length of the host register set in bytes.

    CrashdumpMode - whether this miniport is being used for crashdump.

Return value:

    NTSTATUS

--*/

{
    SDPORT_CAPABILITIES* Capabilities;
    SDHC_CAPABILITIES_REGISTER CapabilitiesReg;
    SDHC_CAPABILITIES2_REGISTER Capabilities2Reg;
    ULONG CurrentLimits;
    ULONG CurrentLimitMax;
    ULONG CurrentLimitMask;
    ULONG CurrentLimitShift;
    UCHAR Index;
    SDHC_EXTENSION* SdhcExtension;
    USHORT SpecVersion;

    SdhcExtension = (SDHC_EXTENSION*)PrivateExtension;

    //
    // Initialize the SDHC_EXTENSION register space.
    //

    SdhcExtension->PhysicalBaseAddress = PhysicalBase;
    SdhcExtension->BaseAddress = VirtualBase;
    SdhcExtension->BaseAddressSpaceSize = Length;
    SdhcExtension->BaseAddressDebug =
        (SD_HOST_CONTROLLER_REGISTERS*)VirtualBase;

    //
    // Check whether the driver is in crashdump mode.
    //

    SdhcExtension->CrashdumpMode = CrashdumpMode;

    //
    // Initialize host capabilities.
    //

    Capabilities = (SDPORT_CAPABILITIES*)&SdhcExtension->Capabilities;
    CapabilitiesReg.AsUlong =
        SdhcReadRegisterUlong(SdhcExtension, SDHC_CAPABILITIES);

    Capabilities2Reg.AsUlong =
        SdhcReadRegisterUlong(SdhcExtension, SDHC_CAPABILITIES2);

    // BRCME88C capabilities register is not accurate.
    CapabilitiesReg.Adma2Support = 0;
    CapabilitiesReg.DmaSupport = 0;
    Capabilities2Reg.DDR50Support = 0;

    SpecVersion = SdhcReadRegisterUshort(SdhcExtension, SDHC_VERSION);
    Capabilities->SpecVersion = SpecVersion & 0xFF;
    Capabilities->MaximumOutstandingRequests = SDHC_MAX_OUTSTANDING_REQUESTS;
    Capabilities->MaximumBlockSize =
        (USHORT)(512 << CapabilitiesReg.MaxBlockLength);

    Capabilities->MaximumBlockCount = 0xFFFF;
    Capabilities->BaseClockFrequencyKhz =
        CapabilitiesReg.BaseClockFrequency * 1000;

    Capabilities->DmaDescriptorSize =
        sizeof(SDHC_ADMA2_DESCRIPTOR_TABLE_ENTRY) +
        (CapabilitiesReg.SystemBus64Support ?
            sizeof(ULONGLONG) : sizeof(ULONG));

    Capabilities->AlignmentRequirement =
        (CapabilitiesReg.SystemBus64Support ?
            sizeof(ULONGLONG) : sizeof(ULONG)) - 1;

    //
    // For small transfers (SDIO) we want to use PIO for performance,
    // for both reads and writes <= 64 bytes.
    //

    Capabilities->PioTransferMaxThreshold = 64;
    Capabilities->Flags.UsePioForRead = TRUE;
    Capabilities->Flags.UsePioForWrite = TRUE;

    if (CapabilitiesReg.Adma2Support) {
        Capabilities->Supported.ScatterGatherDma = 1;
    }

    if (CapabilitiesReg.SystemBus64Support) {
        Capabilities->Supported.Address64Bit = 1;
    }

    if (CapabilitiesReg.Support8BitBus) {
        Capabilities->Supported.BusWidth8Bit = 1;
    }

    if (CapabilitiesReg.HighSpeedSupport) {
        Capabilities->Supported.HighSpeed = 1;
    }

    if (Capabilities2Reg.SDR50Support) {
        Capabilities->Supported.SDR50 = 1;
        Capabilities->Supported.SignalingVoltage18V = 1;
    }

    if (Capabilities2Reg.DDR50Support) {
        Capabilities->Supported.DDR50 = 1;
        Capabilities->Supported.SignalingVoltage18V = 1;
    }

    if (Capabilities2Reg.SDR104Support) {
        Capabilities->Supported.SDR104 = 1;
        Capabilities->Supported.SignalingVoltage18V = 1;
    }

    Capabilities->Supported.HS200 = 0;
    Capabilities->Supported.HS400 = 0;

    if (Capabilities2Reg.DriverTypeA) {
        Capabilities->Supported.DriverTypeA = 1;
    }

    if (Capabilities2Reg.DriverTypeC) {
        Capabilities->Supported.DriverTypeC = 1;
    }

    if (Capabilities2Reg.DriverTypeD) {
        Capabilities->Supported.DriverTypeD = 1;
    }

    Capabilities->Supported.DriverTypeB = 1;

    if (Capabilities2Reg.UseTuningForSDR50) {
        Capabilities->Supported.TuningForSDR50 = 1;
    }

    if (Capabilities2Reg.RetuningTimerCount != 0) {
        Capabilities->TuningTimerCountInSeconds =
            (1 << (Capabilities2Reg.RetuningTimerCount - 1));
    }

    if (Capabilities2Reg.RetuningModes == 0) {
        Capabilities->Supported.SoftwareTuning = 1;
    }

    Capabilities->Supported.AutoCmd12 = 1;
    if (SpecVersion >= SDHC_SPEC_VERSION_3) {
        Capabilities->Supported.AutoCmd23 = 1;
    }

    if (CapabilitiesReg.Voltage18) {
        Capabilities->Supported.Voltage18V;
    }

    if (CapabilitiesReg.Voltage30) {
        Capabilities->Supported.Voltage30V;
    }

    if (CapabilitiesReg.Voltage33) {
        Capabilities->Supported.Voltage33V;
    }

    //
    // Find the current limits suppported by the controller.
    //

    CurrentLimitMask = 0;
    CurrentLimitShift = 0;
    if (Capabilities->Supported.Voltage33V) {
        CurrentLimitMask = 0xFF;
        CurrentLimitShift = 0;

    }
    else if (Capabilities->Supported.Voltage30V) {
        CurrentLimitMask = 0xFF00;
        CurrentLimitShift = 8;

    }
    else if (Capabilities->Supported.Voltage18V) {
        CurrentLimitMask = 0xFF0000;
        CurrentLimitShift = 16;
    }

    CurrentLimits = SdhcReadRegisterUlong(SdhcExtension, SDHC_MAXIMUM_CURRENT);
    CurrentLimitMax =
        ((CurrentLimits & CurrentLimitMask) >> CurrentLimitShift) * 4;

    if (CurrentLimitMax >= 800) {
        Capabilities->Supported.Limit800mA;
    }

    if (CurrentLimitMax >= 600) {
        Capabilities->Supported.Limit600mA;
    }

    if (CurrentLimitMax >= 400) {
        Capabilities->Supported.Limit400mA;
    }

    if (CurrentLimitMax >= 200) {
        Capabilities->Supported.Limit200mA;
    }

    //
    // Initialize our array of outstanding requests to keep track
    // of what operations are in flight.
    //

    for (Index = 0;
        Index < Capabilities->MaximumOutstandingRequests;
        Index += 1) {

        RtlZeroMemory(&SdhcExtension->OutstandingRequests[Index],
            sizeof(SDPORT_REQUEST*));
    }

    return STATUS_SUCCESS;
}

_IRQL_requires_max_(APC_LEVEL)
NTSTATUS
SdhcSlotIssueBusOperation(
    _In_ void* PrivateExtension,
    _In_ SDPORT_BUS_OPERATION* BusOperation
)

/*++

Routine Description:

    Issue host bus operation specified by BusOperation.

Arguments:

    SlotExtension - SlotExtension interface between sdhost and this miniport.

    BusOperation - Bus operation to perform.

Return value:

    NTSTATUS - Return value of the bus operation performed.

--*/

{
    SDHC_EXTENSION* SdhcExtension;
    NTSTATUS Status;

    Status = STATUS_INVALID_PARAMETER;
    SdhcExtension = (SDHC_EXTENSION*)PrivateExtension;
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
        Status = SdhcSetSpeed(SdhcExtension, BusOperation->Parameters.BusSpeed);
        break;

    case SdSetSignalingVoltage:
        Status =
            SdhcSetSignaling(
                SdhcExtension,
                (BOOLEAN)(BusOperation->Parameters.SignalingVoltage ==
                    SdSignalingVoltage18));

        break;

    case SdSetDriveStrength:
        //Status = SdhcSetDriveStrength(SdhcExtension,
        //                            BusOperation->Parameters.DriveStrength);
        break;

    case SdSetDriverType:
        //Status = SdhcSetDriverType(SdhcExtension,
        //                         BusOperation->Parameters.DriverType);

        break;

    case SdSetPresetValue:
        Status = SdhcSetPresetValue(
            SdhcExtension,
            BusOperation->Parameters.PresetValueEnabled);

        break;

    case SdSetBlockGapInterrupt:
        Status = SdhcEnableBlockGapInterrupt(
            SdhcExtension,
            BusOperation->Parameters.BlockGapIntEnabled);

        break;

    case SdExecuteTuning:
        Status = SdhcExecuteTuning(SdhcExtension);
        break;

    default:
        Status = STATUS_INVALID_PARAMETER;
        break;
    }

    LogInfo("IssueBusOperation",
        TraceLoggingUInt32(BusOperation->Type, "Type"),
        TraceLoggingHexInt32(BusOperation->Parameters.FrequencyKhz, "Parameters"),
        TraceLoggingNTStatus(Status));
    return Status;
}

BOOLEAN
SdhcSlotGetCardDetectState(
    _In_ void* PrivateExtension
)

/*++

Routine Description:

    Determine whether a card is inserted in the slot.

Arguments:

    SlotExtension - SlotExtension interface between sdhost and this miniport.

Return value:

    TRUE - Card is inserted.

    FALSE - Slot is empty.

--*/

{

    SDHC_EXTENSION* SdhcExtension;

    SdhcExtension = (SDHC_EXTENSION*)PrivateExtension;
    return SdhcIsCardInserted(SdhcExtension);
}

BOOLEAN
SdhcSlotGetWriteProtectState(
    _In_ void* PrivateExtension
)

/*++

Routine Description:

    Determine whether the slot write protection is engaged.

Arguments:

    SlotExtension - SlotExtension interface between sdhost and this miniport.

Return value:

    TRUE - Slot is write protected.

    FALSE - Slot is writable.

--*/

{

    SDHC_EXTENSION* SdhcExtension;

    SdhcExtension = (SDHC_EXTENSION*)PrivateExtension;
    return SdhcIsWriteProtected(SdhcExtension);
}

BOOLEAN
SdhcSlotInterrupt(
    _In_ void* PrivateExtension,
    _Out_ ULONG* Events,
    _Out_ ULONG* Errors,
    _Out_ BOOLEAN* CardChange,
    _Out_ BOOLEAN* SdioInterrupt,
    _Out_ BOOLEAN* Tuning
)

/*++

Routine Description:

    Level triggered DIRQL interrupt handler (ISR) for this controller.

Arguments:

    SlotExtension - SlotExtension interface between sdhost and this miniport.

Return value:

    Whether the interrupt is handled.

--*/

{

    SDHC_EXTENSION* SdhcExtension;

    SdhcExtension = (SDHC_EXTENSION*)PrivateExtension;
    *Events = (ULONG)SdhcGetInterruptStatus(SdhcExtension);
    *Errors = 0;
    *CardChange = 0;
    *SdioInterrupt = 0;
    *Tuning = 0;

    //
    // If there aren't any events to handle, then we don't need to
    // process anything.
    //

    if (*Events == 0) {
        return FALSE;
    }

    if (*Events & SDHC_IS_ERROR_INTERRUPT) {
        *Errors = (ULONG)SdhcGetErrorStatus(SdhcExtension);
    }

    //
    // If a card has changed, notify the port driver.
    //

    if (*Events & SDHC_IS_CARD_DETECT) {
        *CardChange = TRUE;
    }

    //
    // If we have an external SDIO interrupt, notify the port driver.
    //

    if (*Events & SDHC_IS_CARD_INTERRUPT) {
        *SdioInterrupt = TRUE;
    }

    //
    // If there's a tuning request, notify the port driver.
    //

    if (*Events & SDHC_IS_TUNING_INTERRUPT) {
        *Tuning = TRUE;
    }

    //
    // Acknowledge/clear interrupt status. Request completions will occur in
    // the port driver's slot completion DPC. We need to make the members of 
    // SDPORT_REQUEST that only the port driver uses opaque to the miniport.
    // See how Storport does this (if it does).
    //

    SdhcAcknowledgeInterrupts(SdhcExtension, (USHORT)*Events);
    *Events &= ~(SDHC_IS_CARD_DETECT |
        SDHC_IS_CARD_INTERRUPT |
        SDHC_IS_TUNING_INTERRUPT);

    return (*Events != 0) || (*CardChange) || (*SdioInterrupt) || (*Tuning);
}

NTSTATUS
SdhcSlotIssueRequest(
    _In_ void* PrivateExtension,
    _In_ SDPORT_REQUEST* Request
)

/*++

Routine Description:

    Issue hardware request specified by Request.

Arguments:

    PrivateExtension - This driver's device extension (SdhcExtension).

    Request - Request operation to perform.

Return value:

    NTSTATUS - Return value of the request performed.

--*/

{

    ULONG Index;
    SDHC_EXTENSION* SdhcExtension;
    NTSTATUS Status;

    SdhcExtension = (SDHC_EXTENSION*)PrivateExtension;

    //
    // Insert the request onto the outstanding requests list.
    //

    for (Index = 0;
        Index < SdhcExtension->Capabilities.MaximumOutstandingRequests;
        Index += 1) {

        if (SdhcExtension->OutstandingRequests[Index] == NULL) {
            SdhcExtension->OutstandingRequests[Index] = Request;
            break;
        }
    }

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
        break;

    default:
        Status = STATUS_NOT_SUPPORTED;
        break;
    }

    return Status;
}

void
SdhcSlotGetResponse(
    _In_ void* PrivateExtension,
    _In_ SDPORT_COMMAND* Command,
    _Out_ void* ResponseBuffer
)

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

{

    SDHC_EXTENSION* SdhcExtension;
    NTSTATUS Status;

    SdhcExtension = (SDHC_EXTENSION*)PrivateExtension;
    Status = SdhcGetResponse(SdhcExtension, Command, ResponseBuffer);
    NT_ASSERT(NT_SUCCESS(Status));
}

void
SdhcRequestDpc(
    _In_ void* PrivateExtension,
    _Inout_ SDPORT_REQUEST* Request,
    _In_ ULONG Events,
    _In_ ULONG Errors
)

/*++

Routine Description:

    DPC for interrupts associated with the given request.

Arguments:

    PrivateExtension - This driver's device extension (SdhcExtension).

    Request - Request operation to perform.

Return value:

    NTSTATUS - Return value of the request performed.

--*/

{

    ULONG Index;
    SDHC_EXTENSION* SdhcExtension;
    NTSTATUS Status;

    //
    // Clear the request's required events if they have completed.
    //

    Request->RequiredEvents &= ~Events;

    //
    // Find the index of the request in the outstanding request list.
    //

    SdhcExtension = (SDHC_EXTENSION*)PrivateExtension;
    for (Index = 0; Index < SDHC_MAX_OUTSTANDING_REQUESTS; Index += 1) {
        if (SdhcExtension->OutstandingRequests[Index] == Request) {
            break;
        }
    }

    //
    // If there are errors, we need to fail whatever outstanding request
    // was on the bus. Otherwise, the request succeeded.
    // 
    // TODO: There's a race condition here--it's possible for a request
    // to get a transfer complete interrupt and then later receive an
    // error interrupt for the same transfer. It might be mitigated by
    // handling request completion in a DPC for ISR.
    //

    if (Errors) {
        Request->RequiredEvents = 0;
        SdhcExtension->OutstandingRequests[Index] = NULL;
        Status = SdhcConvertErrorToStatus((USHORT)Errors);
        SdPortCompleteRequest(Request, Status);

    }
    else if (Request->RequiredEvents == 0) {
        if (Request->Status != STATUS_MORE_PROCESSING_REQUIRED) {
            Request->Status = STATUS_SUCCESS;
        }

        SdhcExtension->OutstandingRequests[Index] = NULL;
        SdPortCompleteRequest(Request, Request->Status);
    }
}

void
SdhcSlotToggleEvents(
    _In_ void* PrivateExtension,
    _In_ ULONG EventMask,
    _In_ BOOLEAN Enable
)

/*++

Routine Description:

    Enable or disable the given event mask.

Arguments:

    SlotExtension - SlotExtension interface between sdhost and this miniport.

    Events - The event mask to toggle.

    Enable - TRUE to enable, FALSE to disable.

Return value:

    None.

--*/

{
    USHORT InterruptMask;
    SDHC_EXTENSION* SdhcExtension;

    LogInfo("ToggleEvents",
        TraceLoggingBoolean(Enable),
        TraceLoggingHexInt32(EventMask));

    InterruptMask = SdhcConvertEventsToHwMask(EventMask);
    SdhcExtension = (SDHC_EXTENSION*)PrivateExtension;
    if (Enable) {
        SdhcEnableInterrupt(SdhcExtension,
            InterruptMask);

    }
    else {
        SdhcDisableInterrupt(SdhcExtension,
            InterruptMask);
    }
}

void
SdhcSlotClearEvents(
    _In_ void* PrivateExtension,
    _In_ ULONG EventMask
)

{

    USHORT Interrupts;
    SDHC_EXTENSION* SdhcExtension;

    SdhcExtension = (SDHC_EXTENSION*)PrivateExtension;
    Interrupts = SdhcConvertEventsToHwMask(EventMask);
    SdhcAcknowledgeInterrupts(SdhcExtension, Interrupts);
}

void
SdhcSaveContext(
    _In_ void* PrivateExtension
)

/*++

Routine Description:

    Save slot register context.

Arguments:

    SlotExtension - SlotExtension interface between sdhost and this miniport.

Return value:

    None.

--*/

{
    UNREFERENCED_PARAMETER(PrivateExtension);
}

void
SdhcRestoreContext(
    _In_ void* PrivateExtension
)

/*++

Routine Description:

    Restore slot register context from a previously saved context.

Arguments:

    SlotExtension - SlotExtension interface between sdhost and this miniport.

Return value:

    None.

--*/

{
    UNREFERENCED_PARAMETER(PrivateExtension);
}

NTSTATUS
SdhcPoFxPowerControlCallback(
    _In_ SD_MINIPORT* Miniport,
    _In_ GUID const* PowerControlCode,
    _In_reads_bytes_opt_(InputBufferSize) void* InputBuffer,
    _In_ SIZE_T InputBufferSize,
    _Out_writes_bytes_opt_(OutputBufferSize) void* OutputBuffer,
    _In_ SIZE_T OutputBufferSize,
    _Out_opt_ SIZE_T* BytesReturned
)

/*++

Routine Description:

    Handle any PoFxPowerControl callbacks.

Arguments:

    Miniport - Miniport interface for the controller.

    PowerControlCode - GUID defining a platform-specific PoFxPowerControl
                       method.

    InputBuffer - Buffer containing any input arguments.

    InputBufferSize - Size of InputBuffer in bytes.

    OutputBuffer - Buffer containing any output results.

    OutputBufferSize - Size of OutputBuffer in bytes.

    BytesReturned - Number of bytes returned.

Return value:

    NTSTATUS

--*/

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

//-----------------------------------------------------------------------------
// Host routine implementations.
//-----------------------------------------------------------------------------

_IRQL_requires_max_(APC_LEVEL)
NTSTATUS
SdhcResetHost(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ SDPORT_RESET_TYPE ResetType
)

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

{

    UCHAR HostControl;
    UCHAR Reset;
    UCHAR Retries;
    UCHAR Mask;

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
    }

    //
    // Reset the host controller.
    //

    Retries = 100;
    SdhcWriteRegisterUchar(SdhcExtension, SDHC_RESET, Mask);
    do {
        Retries -= 1;
        if (Retries == 0) {
            return STATUS_IO_TIMEOUT;
        }

        Reset = SdhcReadRegisterUchar(SdhcExtension, SDHC_RESET);

        if ((Reset & Mask) != 0) {
            SdPortWait(1000);
        }

    } while ((Reset & Mask) != 0);

    //
    // Set the max HW timeout for bus operations.
    //

    SdhcWriteRegisterUchar(SdhcExtension,
        SDHC_TIMEOUT_CONTROL,
        SDHC_TC_MAX_DATA_TIMEOUT);

    //
    // Clear detection interrupt after reset, we will pick up the
    // status from the present state register
    //

    SdhcWriteRegisterUshort(SdhcExtension,
        SDHC_INTERRUPT_STATUS,
        0xFFFF);

    //
    // Initialize DMA if the controller supports it.
    //

    HostControl = SdhcReadRegisterUchar(SdhcExtension, SDHC_HOST_CONTROL);
    HostControl &= ~SDHC_HC_DMA_SELECT_MASK;
    if (SdhcExtension->Capabilities.Supported.ScatterGatherDma) {
        if (SdhcExtension->Capabilities.Supported.Address64Bit) {
            HostControl |= SDHC_HC_DMA_SELECT_ADMA64;

        }
        else {
            HostControl |= SDHC_HC_DMA_SELECT_ADMA32;
        }
    }

    SdhcWriteRegisterUchar(SdhcExtension, SDHC_HOST_CONTROL, HostControl);
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(APC_LEVEL)
NTSTATUS
SdhcSetClock(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ ULONG Frequency
)

/*++

Routine Description:

    Set the clock to a given frequency.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Frequency - The target frequency.

Return value:

    STATUS_SUCCESS - The clock was successfuly set.

    STATUS_IO_TIMEOUT - The clock did not stabilize in time.

--*/

{

    ULONG ActualFrequency;
    USHORT ClockControl;
    ULONG Delay;
    USHORT Mask;
    UCHAR Retries;

    UNREFERENCED_PARAMETER(Frequency);

    ClockControl = SdhcReadRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL);
    ClockControl &= ~(SDHC_CC_CLOCK_ENABLE | SDHC_CC_INTERNAL_CLOCK_ENABLE);
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL, ClockControl);
    ClockControl = SdhcCalcClockFrequency(SdhcExtension,
        Frequency,
        &ActualFrequency);

    ClockControl |= SDHC_CC_INTERNAL_CLOCK_ENABLE;
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL, ClockControl);

    //
    // Now the frequency is set, delay a few times to wait for it to become
    // stable.
    //

    Retries = 100;
    Mask = SDHC_CC_CLOCK_STABLE;
    do {
        Retries -= 1;
        if (Retries == 0) {
            return STATUS_IO_TIMEOUT;
        }

        ClockControl =
            SdhcReadRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL);

        if ((ClockControl & Mask) == 0) {
            SdPortWait(1000);
        }

    } while ((ClockControl & Mask) == 0);

    //
    // Clock is now stable, now enable it.
    //

    ClockControl |= SDHC_CC_CLOCK_ENABLE;
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL, ClockControl);

    //
    // Some hardware need more time here to stabilize, but minimize latency
    // for fixed eMMC devices during runtime Dx transitions.
    //

    Delay = (SdhcExtension->Removable) ? (10 * 1000) : 50;
    SdPortWait(Delay);
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(APC_LEVEL)
NTSTATUS
SdhcSetVoltage(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ SDPORT_BUS_VOLTAGE Voltage
)

/*++

Routine Description:

    Set the slot's voltage profile.

Arguments:

    SdhcExtension - Host controller specific driver context.

    VoltageProfile - Indicates which power voltage to use. If 0, turn off the
                     power

Return value:

    STATUS_SUCCESS - The bus voltage was successfully switched.

    STATUS_INVALID_PARAMETER - The voltage profile provided was invalid.

    STATUS_IO_TIMEOUT - The bus voltage did not stabilize in time.

--*/

{

    ULONG Delay;
    UCHAR Mask;
    UCHAR PowerControl;
    UCHAR PowerControlCheck;
    UCHAR Retries;

    //
    // Wait 10ms if the slot is removable; otherwise, only wait 50us.
    //

    Delay = (SdhcExtension->Removable) ? (10 * 1000) : 50;
    SdPortWait(Delay);

    //
    // Use the highest voltage capable.
    //

    switch (Voltage) {
    case SdBusVoltage33:
        PowerControl = SDHC_PC_3_3V;
        break;

    case SdBusVoltage30:
        PowerControl = SDHC_PC_3_0V;
        break;

    case SdBusVoltage18:
        PowerControl = SDHC_PC_1_8V;
        break;

    case SdBusVoltageOff:
        PowerControl = 0;
        break;

    default:

        NT_ASSERTMSG("SDHC - Voltage profile invalid.", FALSE);

        return STATUS_INVALID_PARAMETER;
    }

    Retries = 100;
    Mask = SDHC_PC_VOLTAGE_MASK;
    do {
        Retries -= 1;
        if (Retries == 0) {
            return STATUS_IO_TIMEOUT;
        }

        SdhcWriteRegisterUchar(SdhcExtension, SDHC_POWER_CONTROL, PowerControl);
        PowerControlCheck =
            SdhcReadRegisterUchar(SdhcExtension, SDHC_POWER_CONTROL);

        if ((PowerControlCheck & Mask) != PowerControl) {
            SdPortWait(1000);
        }

    } while ((PowerControlCheck & Mask) != PowerControl);

    PowerControl |= SDHC_PC_BUS_POWER;
    SdhcWriteRegisterUchar(SdhcExtension, SDHC_POWER_CONTROL, PowerControl);
    SdPortWait(Delay);

    Retries = 100;
    Mask = SDHC_PC_VOLTAGE_MASK | SDHC_PC_BUS_POWER;
    do {
        Retries -= 1;
        if (Retries == 0) {
            return STATUS_IO_TIMEOUT;
        }

        SdhcWriteRegisterUchar(SdhcExtension, SDHC_POWER_CONTROL, PowerControl);
        PowerControlCheck =
            SdhcReadRegisterUchar(SdhcExtension, SDHC_POWER_CONTROL);

        if ((PowerControlCheck & Mask) != PowerControl) {
            SdPortWait(1000);
        }

    } while ((PowerControlCheck & Mask) != PowerControl);

    return STATUS_SUCCESS;
}

_IRQL_requires_max_(APC_LEVEL)
NTSTATUS
SdhcSetBusWidth(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ SDPORT_BUS_WIDTH Width
)

/*++

Routine Description:

    Set bus width for host controller.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Width - The data bus width of the slot.

Return value:

    STATUS_SUCCESS - The bus width was properly changed.

--*/

{

    UCHAR HostControl;

    HostControl = SdhcReadRegisterUchar(SdhcExtension, SDHC_HOST_CONTROL);
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

        NT_ASSERTMSG("SDHC - Provided bus width is invalid", FALSE);

        break;
    }

    SdhcWriteRegisterUchar(SdhcExtension, SDHC_HOST_CONTROL, HostControl);
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(APC_LEVEL)
NTSTATUS
SdhcSetSpeed(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ SDPORT_BUS_SPEED Speed
)

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

{

    NTSTATUS Status;
    USHORT UhsMode;

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

        NT_ASSERTMSG("SDHC - Invalid speed mode selected.", FALSE);

        Status = STATUS_INVALID_PARAMETER;
        break;
    }

    return Status;
}

_IRQL_requires_max_(APC_LEVEL)
NTSTATUS
SdhcSetHighSpeed(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ BOOLEAN Enable
)

/*++

Routine Description:

    Based on the capabilities of card and host, enables or disables high speed.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Enable - TRUE to enable high speed, FALSE to disable.

Return value:

    STATUS_SUCCESS - High speed was successfully enabled.

--*/

{

    UCHAR HostControl;

    HostControl = SdhcReadRegisterUchar(SdhcExtension, SDHC_HOST_CONTROL);
    HostControl &= ~SDHC_HC_ENABLE_HIGH_SPEED;
    if (Enable) {
        HostControl |= SDHC_HC_ENABLE_HIGH_SPEED;
    }

    SdhcWriteRegisterUchar(SdhcExtension, SDHC_HOST_CONTROL, HostControl);
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(APC_LEVEL)
NTSTATUS
SdhcSetUhsMode(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ USHORT Mode
)

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

{

    USHORT ClockControl;
    USHORT HostControl2;

    HostControl2 = SdhcReadRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2);

    //
    // If we're already in the requested mode, return.
    //

    if ((HostControl2 & SDHC_HC2_UHS_MODES) == Mode) {
        return STATUS_SUCCESS;
    }

    ClockControl = SdhcReadRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL);
    ClockControl &= ~SDHC_CC_CLOCK_ENABLE;
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL, ClockControl);
    SdPortWait(10 * 1000);

    //
    // Set the UHS mode.
    //

    HostControl2 &= ~SDHC_HC2_UHS_MODES;
    HostControl2 |= Mode;
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2, HostControl2);
    ClockControl = SdhcReadRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL);
    ClockControl |= SDHC_CC_CLOCK_ENABLE;
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL, ClockControl);
    SdPortWait(10 * 1000);
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(APC_LEVEL)
NTSTATUS
SdhcSetSignaling(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ BOOLEAN Enable
)

/*++

Routine Description:

    Set signaling voltage (1.8V or 3.3V).

Arguments:

    SdhcExtension - Host controller specific driver context.

    Enable - TRUE for 1.8V signaling, FALSE for default 3.3V.

Return value:

    STATUS_SUCCESS - Signaling voltage switch successful.

    STATUS_UNSUCCESSFUL - Signaling voltage switch unsuccessful.

--*/

{

    USHORT ClockControl;
    ULONG DatLines;
    USHORT HostControl2;
    USHORT Mask;

    //
    // Disable the bus clock.
    //

    ClockControl = SdhcReadRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL);
    ClockControl &= ~SDHC_CC_CLOCK_ENABLE;
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL, ClockControl);
    SdPortWait(10000);

    //
    // DAT[3:0] must be all zeroes.
    //

    DatLines = 0;
    DatLines =
        SDHC_PS_DAT_3_0 &
        SdhcReadRegisterUlong(SdhcExtension, SDHC_PRESENT_STATE);

    if (DatLines != 0) {
        return STATUS_UNSUCCESSFUL;
    }

    //
    // Set the signaling voltage.
    //

    HostControl2 = SdhcReadRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2);
    Mask = SDHC_HC2_1_8V_SIGNALING;
    if (Enable) {
        HostControl2 |= Mask;

    }
    else {
        HostControl2 &= ~Mask;
    }

    SdhcWriteRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2, HostControl2);
    SdPortWait(5000);
    HostControl2 = SdhcReadRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2);

    //
    // Make sure the signaling voltage latched.
    //

    if (Enable) {
        if ((HostControl2 & Mask) == 0) {
            return STATUS_UNSUCCESSFUL;
        }

    }
    else {
        if ((HostControl2 & Mask) != 0) {
            return STATUS_UNSUCCESSFUL;
        }
    }

    //
    // Reenable the bus clock.
    //

    ClockControl = SdhcReadRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL);
    ClockControl |= SDHC_CC_CLOCK_ENABLE;
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL, ClockControl);
    SdPortWait(10000);

    //
    // DAT[3:0] must be all ones.
    //

    DatLines = 0;
    DatLines =
        SDHC_PS_DAT_3_0 &
        SdhcReadRegisterUlong(SdhcExtension, SDHC_PRESENT_STATE);

    if (DatLines != SDHC_PS_DAT_3_0) {
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

_IRQL_requires_max_(APC_LEVEL)
NTSTATUS
SdhcExecuteTuning(
    _In_ SDHC_EXTENSION* SdhcExtension
)

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

{

    USHORT HostControl2;
    //UCHAR Retries;
    SDPORT_REQUEST TuningRequest;

    HostControl2 = SdhcReadRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2);

    NT_ASSERT((HostControl2 & SDHC_HC2_EXECUTE_TUNING) == 0);

    //
    // Disable controller events
    //
    // Technically, all controller events should be disabled at tuning execute
    // time, but some controllers do not follow this requirement.
    //

    if ((HostControl2 & SDHC_HC2_EXECUTE_TUNING) == 0) {
        HostControl2 |= SDHC_HC2_EXECUTE_TUNING;
        SdhcWriteRegisterUshort(SdhcExtension,
            SDHC_HOST_CONTROL2,
            HostControl2);
    }

    RtlZeroMemory(&TuningRequest, sizeof(TuningRequest));
    TuningRequest.Command.TransferType = SdTransferTypeSingleBlock;
    TuningRequest.Command.TransferDirection = SdTransferDirectionRead;
    TuningRequest.Command.Class = SdCommandClassStandard;
    TuningRequest.Command.ResponseType = SdResponseTypeR1;
    if (SdhcExtension->SpeedMode == SdhcSpeedModeSDR104) {
        TuningRequest.Command.Index = 19;
        TuningRequest.Command.BlockSize = 64;

    }
    else {
        TuningRequest.Command.Index = 21;
        TuningRequest.Command.BlockSize = 128;
    }

    //
    // TODO: Implement rest of tuning logic as described in
    // SD Host Controller Specification. This is needed for
    // SDR104, HS200, and HS400 modes, and sometimes for SDR50.
    //

    //
    // Execute the data transfer.
    //

    //Status = SdhcBuildTransfer()

    return STATUS_SUCCESS;
}

void
SdhcSetLed(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ BOOLEAN Enable
)

/*++

Routine Description:

    Turn the controller activity LED on/off.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Enable - Indicate whether to enable or disable the LED.

Return value:

    None.

--*/

{

    UCHAR HostControl;

    HostControl = SdhcReadRegisterUchar(SdhcExtension, SDHC_HOST_CONTROL);
    if (Enable) {
        HostControl |= SDHC_HC_LED_POWER;

    }
    else {
        HostControl &= ~SDHC_HC_LED_POWER;
    }

    SdhcWriteRegisterUchar(SdhcExtension, SDHC_HOST_CONTROL, HostControl);
}

_IRQL_requires_max_(APC_LEVEL)
NTSTATUS
SdhcSetPresetValue(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ BOOLEAN Enable
)

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

{

    USHORT HostControl2;

    HostControl2 = SdhcReadRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2);

    NT_ASSERT((HostControl2 & SDHC_HC2_ENABLE_PRESET_VALUE) != 0);

    HostControl2 &= ~SDHC_HC2_ENABLE_PRESET_VALUE;
    if (Enable) {
        HostControl2 |= SDHC_HC2_ENABLE_PRESET_VALUE;
    }

    SdhcWriteRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2, HostControl2);
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(APC_LEVEL)
NTSTATUS
SdhcEnableBlockGapInterrupt(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ BOOLEAN Enable
)

/*++

Routine Description:

    Enables block gap interrupts for SDIO cards in 4-bit mode.
    Caller has responsibility to make sure this is only called for
    appropriate devices.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Enable - Enable or disable block gap interrupts.

Return value:

    None.

--*/

{

    UCHAR Control;

    Control = SdhcReadRegisterUchar(SdhcExtension, SDHC_BLOCKGAP_CONTROL);
    if (Enable) {
        Control |= SDHC_BGC_INTERRUPT_ENABLE;

    }
    else {
        Control &= ~SDHC_BGC_INTERRUPT_ENABLE;
    }

    SdhcWriteRegisterUchar(SdhcExtension, SDHC_BLOCKGAP_CONTROL, Control);
    return STATUS_SUCCESS;
}

void
SdhcSetBlockGapControl(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ BOOLEAN Continue,
    _In_ BOOLEAN RequestStop
)

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

{

    UCHAR BlockGapControl;

    BlockGapControl = SdhcReadRegisterUchar(SdhcExtension,
        SDHC_BLOCKGAP_CONTROL);

    BlockGapControl &= ~SDHC_BGC_CONTINUE;
    BlockGapControl &= ~SDHC_BGC_STOP_NEXT_GAP;
    if (Continue) {
        BlockGapControl |= SDHC_BGC_CONTINUE;
    }

    if (RequestStop) {
        BlockGapControl |= SDHC_BGC_STOP_NEXT_GAP;
    }
}

void
SdhcEnableInterrupt(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ ULONG NormalInterruptMask
)

/*++

Routine Description:

    Set the host event mask to the new value specified.

Arguments:

    SdhcExtension - Host controller specific driver context.

    NormalInterruptMask - The new normal events to set.

Return value:

    None.

--*/

{

    USHORT InterruptEnable;

    InterruptEnable =
        SdhcReadRegisterUshort(SdhcExtension, SDHC_INTERRUPT_SIGNAL_ENABLE);

    InterruptEnable |= NormalInterruptMask;

    //
    // Enable the interrupt signals from controller to OS.
    //

    if (!SdhcExtension->CrashdumpMode) {
        SdhcWriteRegisterUshort(SdhcExtension,
            SDHC_INTERRUPT_SIGNAL_ENABLE,
            InterruptEnable);

        SdhcWriteRegisterUshort(SdhcExtension,
            SDHC_ERROR_SIGNAL_ENABLE,
            0xFFFF);
    }

    //
    // Enable the interrupt signals on the controller.
    //

    SdhcWriteRegisterUshort(SdhcExtension,
        SDHC_INTERRUPT_STATUS_ENABLE,
        InterruptEnable);

    SdhcWriteRegisterUshort(SdhcExtension,
        SDHC_ERROR_STATUS_ENABLE,
        0xFFFF);
}

void
SdhcDisableInterrupt(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ ULONG NormalInterruptMask
)

/*++

Routine Description:

    Set the host event mask to the new value specified.

Arguments:

    SdhcExtension - Host controller specific driver context.

    NormalInterruptMask - Normal Interrupts to disable.

Return value:

    None.

--*/

{

    USHORT InterruptDisable;

    InterruptDisable =
        SdhcReadRegisterUshort(SdhcExtension, SDHC_INTERRUPT_SIGNAL_ENABLE);

    InterruptDisable &= ~NormalInterruptMask;

    //
    // Disable the interrupt signals on the controller.
    //

    SdhcWriteRegisterUshort(SdhcExtension,
        SDHC_INTERRUPT_STATUS_ENABLE,
        InterruptDisable);

    SdhcWriteRegisterUshort(SdhcExtension,
        SDHC_ERROR_STATUS_ENABLE,
        0);

    //
    // Disable the interrupt signals from controller to OS.
    //

    SdhcWriteRegisterUshort(SdhcExtension,
        SDHC_INTERRUPT_SIGNAL_ENABLE,
        InterruptDisable);

    SdhcWriteRegisterUshort(SdhcExtension,
        SDHC_ERROR_SIGNAL_ENABLE,
        0);
}

__forceinline
USHORT
SdhcGetInterruptStatus(
    _In_ SDHC_EXTENSION* SdhcExtension
)

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

{

    USHORT InterruptStatus;

    InterruptStatus =
        SdhcReadRegisterUshort(SdhcExtension, SDHC_INTERRUPT_STATUS);

    //
    // 0xFFFF means HC is no longer accessible. This interrupt does not belong
    // to us.
    //

    if (InterruptStatus == 0xFFFF) {
        return 0;
    }

    return InterruptStatus;
}

__forceinline
USHORT
SdhcGetErrorStatus(
    _In_ SDHC_EXTENSION* SdhcExtension
)

/*++

Routine Description:

    This routine returns the current error status, if any.

Arguments:

    SdhcExtension - Host controller specific driver context.

Return value:

    The error interrupt status.

--*/

{

    return SdhcReadRegisterUshort(SdhcExtension, SDHC_ERROR_STATUS);
}

__forceinline
USHORT
SdhcGetAutoCmd12ErrorStatus(
    _In_ SDHC_EXTENSION* SdhcExtension
)

/*++

Routine Description:

    This routine returns the current Auto CMD12 error status, if any.

Arguments:

    SdhcExtension - Host controller specific driver context.

Return value:

    The Auto CMD12 error status.

--*/

{

    return SdhcReadRegisterUshort(SdhcExtension, SDHC_AUTO_CMD12_ERROR_STATUS);
}

USHORT
SdhcGetAdmaErrorStatus(
    _In_ SDHC_EXTENSION* SdhcExtension
)

/*++

Routine Description:

    This routine returns the current ADMA error status, if any.

Arguments:

    SdhcExtension - Host controller specific driver context.

Return value:

    The ADMA error status.

--*/

{

    return SdhcReadRegisterUshort(SdhcExtension, SDHC_ADMA_ERROR_STATUS);
}

void
SdhcAcknowledgeInterrupts(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ USHORT Interrupts
)

/*++

Routine Description:

    Acknowlege the interrupts specified.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Interrupts - event mask to acknowledge (single event only)

Return value:

    None.

--*/

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

        SdhcWriteRegisterUshort(SdhcExtension,
            SDHC_AUTO_CMD12_ERROR_STATUS,
            0xFFFF);

        SdhcWriteRegisterUshort(SdhcExtension,
            SDHC_AUTO_CMD12_ERROR_STATUS,
            0x0);

        //
        // Clear the error interrupt by writing all-ones.
        //

        SdhcWriteRegisterUshort(SdhcExtension, SDHC_ERROR_STATUS, 0xFFFF);
        Interrupts &= ~SDHC_IS_ERROR_INTERRUPT;
    }

    //
    // Clear other interrupts in the interrupt status register.
    //

    SdhcWriteRegisterUshort(SdhcExtension, SDHC_INTERRUPT_STATUS, Interrupts);
}

BOOLEAN
SdhcIsCardInserted(
    _In_ SDHC_EXTENSION* SdhcExtension
)

/*++

Routine Description:

    To detect whether there is a card in the socket.

Arguments:

    SdhcExtension - Host controller specific driver context.

Return value:

    BOOLEAN value to indicate whether there is a card in the socket.

--*/

{

    ULONG PresentState;

    PresentState = SdhcReadRegisterUlong(SdhcExtension, SDHC_PRESENT_STATE);
    return (BOOLEAN)((PresentState & SDHC_PS_CARD_INSERTED) != 0);
}

BOOLEAN
SdhcIsWriteProtected(
    _In_ SDHC_EXTENSION* SdhcExtension
)

/*++

Routine Description:

    To detect whether the card is write protected.

Arguments:

    SdhcExtension - Host controller specific driver context.

Return value:

    BOOLEAN value to indicate whether the card is write protected.

--*/

{

    ULONG PresentState;
    BOOLEAN WriteProtected;

    //
    // Write protect is active low.
    //

    PresentState = SdhcReadRegisterUlong(SdhcExtension, SDHC_PRESENT_STATE);
    WriteProtected = (BOOLEAN)((PresentState & SDHC_PS_WRITE_PROTECT) == 0);
    return WriteProtected;
}

NTSTATUS
SdhcSendCommand(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ SDPORT_REQUEST* Request
)

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

{

    SDPORT_COMMAND* Command;
    USHORT CommandType;
    USHORT CommandReg;
    NTSTATUS Status;
    USHORT TransferMode;

    //
    // Initialize transfer parameters if this command is a data command.
    //

    Command = &Request->Command;
    if ((Command->TransferType != SdTransferTypeNone) &&
        (Command->TransferType != SdTransferTypeUndefined)) {

        Status = SdhcBuildTransfer(SdhcExtension, Request);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    //
    // Set the response parameters based off the given response type.
    //

    SdhcWriteRegisterUlong(SdhcExtension, SDHC_ARGUMENT, Command->Argument);

    CommandReg = Command->Index << 8;
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
    }

    if (Request->Command.TransferType != SdTransferTypeNone) {
        TransferMode = 0;
        CommandReg |= SDHC_CMD_DATA_PRESENT;

    }
    else {
        TransferMode =
            SdhcReadRegisterUshort(SdhcExtension, SDHC_TRANSFER_MODE);

        TransferMode &= ~SDHC_TM_DMA_ENABLE;
        TransferMode &= ~SDHC_TM_AUTO_CMD12_ENABLE;
        TransferMode &= ~SDHC_TM_AUTO_CMD23_ENABLE;
        SdhcWriteRegisterUshort(SdhcExtension,
            SDHC_TRANSFER_MODE,
            TransferMode);
    }

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
    }

    //
    // Set the bitmask for the required events that will fire after
    // writing to the command register. Depending on the response
    // type or whether the command involves data transfer, we will need
    // to wait on a number of different events.
    //

    Request->RequiredEvents = SDHC_IS_CMD_COMPLETE;
    if ((Command->ResponseType == SdResponseTypeR1B) ||
        (Command->ResponseType == SdResponseTypeR5B)) {

        Request->RequiredEvents |= SDHC_IS_TRANSFER_COMPLETE;
    }

    if (Request->Command.TransferMethod == SdTransferMethodSgDma) {
        Request->RequiredEvents |= SDHC_IS_TRANSFER_COMPLETE;

    }
    else if (Request->Command.TransferMethod == SdTransferMethodPio) {
        if (Request->Command.TransferDirection == SdTransferDirectionRead) {
            Request->RequiredEvents |= SDHC_IS_BUFFER_READ_READY;

        }
        else {
            Request->RequiredEvents |= SDHC_IS_BUFFER_WRITE_READY;
        }
    }

    //
    // Issue the actual command.
    //

    CommandReg |= CommandType;
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_COMMAND, CommandReg);

    //
    // We must wait until the request completes.
    //

    return STATUS_PENDING;
}

NTSTATUS
SdhcGetResponse(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ SDPORT_COMMAND* Command,
    _Out_ void* ResponseBuffer
)

/*++

Routine Description:

    This routine reads the response of the SD card which is sent over
    the command line, and stores it in the specified buffer

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - Supplies the descriptor for this SD command.

Return value:

    STATUS_SUCCESS - Response successfully returned.

    STATUS_INVALID_PARAMETER - Invalid response length.

--*/

{

    UCHAR Index;
    UCHAR* Response;
    UCHAR ResponseLength;

    ResponseLength = SdhcGetResponseLength(Command);
    if (ResponseLength > 16) {
        return STATUS_INVALID_PARAMETER;
    }

    Response = (UCHAR*)ResponseBuffer;
    for (Index = 0; Index < ResponseLength; Index += 1) {

#pragma prefast(suppress: 22103, "Response length is guaranteed to be <= ResponseLength.")

        Response[Index] =
            SdhcReadRegisterUchar(SdhcExtension, SDHC_RESPONSE + Index);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
SdhcSetTransferMode(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ SDPORT_REQUEST* Request
)

/*++

Routine Description:

    This routine sets up the host for a data transfer.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Command - Supplies the descriptor for this SD command.

Return value:

    STATUS_SUCCESS - Transfer mode successfully set.

    STATUS_INVALID_PARAMETER - Invalid command transfer parameters.

--*/

{

    USHORT BlockCount;
    USHORT BlockSize;
    USHORT TransferMode;

    NT_ASSERT(Request->Command.TransferMethod != SdTransferMethodUndefined);

    BlockCount = Request->Command.BlockCount;
    if (Request->Command.BlockSize > 2048) {

        NT_ASSERTMSG("SDHC - Invalid block size for command", FALSE);

        return STATUS_INVALID_PARAMETER;
    }

    if ((Request->Command.TransferDirection != SdTransferDirectionRead) &&
        (Request->Command.TransferDirection != SdTransferDirectionWrite)) {

        return STATUS_INVALID_PARAMETER;
    }

    TransferMode = 0;
    TransferMode &= ~(SDHC_TM_AUTO_CMD12_ENABLE |
        SDHC_TM_AUTO_CMD23_ENABLE |
        SDHC_TM_DMA_ENABLE |
        SDHC_TM_BLKCNT_ENABLE |
        SDHC_TM_MULTIBLOCK);

    // if (Command->TransferType = TransferTypeMultiBlockNoStop) {

    //}

    if (BlockCount > 1) {
        TransferMode |= SDHC_TM_MULTIBLOCK;
        TransferMode |= SDHC_TM_BLKCNT_ENABLE;
        TransferMode |= SDHC_TM_AUTO_CMD12_ENABLE;
    }

    if (Request->Command.TransferMethod == SdTransferMethodSgDma) {
        TransferMode |= SDHC_TM_DMA_ENABLE;

    }
    else {

        NT_ASSERT(Request->Command.TransferMethod == SdTransferMethodPio);

    }

    BlockSize = Request->Command.BlockSize;
    TransferMode &= ~SDHC_TM_TRANSFER_READ;
    if (Request->Command.TransferDirection == SdTransferDirectionRead) {
        TransferMode |= SDHC_TM_TRANSFER_READ;
    }

    SdhcWriteRegisterUlong(SdhcExtension, SDHC_SYSADDR, BlockCount);
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_BLOCK_SIZE, BlockSize);
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_BLOCK_COUNT, BlockCount);
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_TRANSFER_MODE, TransferMode);
    return STATUS_SUCCESS;
}

void
SdhcReadDataPort(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _Out_writes_all_(Length) UCHAR* Buffer,
    _In_ SIZE_T Length
)

/*++

Routine Description:

    The data port must be accessed maintaining DWORD alignment. So for example:

        IN DWORD 130
        IN DWORD 130

    is the same as

        IN USHORT 130
        IN USHORT 132
        IN UCHAR  130
        IN UCHAR  131
        IN UCHAR  132
        IN UCHAR  133

Arguments:

    SdhcExtension - Host controller specific driver context.

    Buffer - Data buffer to read.

    Length - Length of the data buffer in bytes.

Return value:

    None.

--*/

{

    UCHAR* Port;

    Port = (UCHAR*)SdhcExtension->BaseAddress + SDHC_DATA_PORT;
    while (Length >= sizeof(ULONG)) {
        SdhcReadRegisterBufferUlong(SdhcExtension,
            SDHC_DATA_PORT,
            (ULONG*)Buffer,
            1);

        Buffer += sizeof(ULONG);
        Length -= sizeof(ULONG);
    }

    if (Length >= sizeof(USHORT)) {
        SdhcReadRegisterBufferUshort(SdhcExtension,
            SDHC_DATA_PORT,
            (USHORT*)Buffer,
            1);

        Buffer += sizeof(USHORT);
        Length -= sizeof(USHORT);
    }

    if (Length >= sizeof(UCHAR)) {
        SdhcReadRegisterBufferUchar(SdhcExtension,
            SDHC_DATA_PORT + sizeof(USHORT),
            Buffer,
            1);
    }
}

void
SdhcWriteDataPort(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_reads_(Length) UCHAR* Buffer,
    _In_ ULONG Length
)

/*++

Routine Description:

    The data port must be accessed maintaining DWORD alignment. So for example:

        IN DWORD 130
        IN DWORD 130

    is the same as

        IN USHORT 130
        IN USHORT 132
        IN UCHAR  130
        IN UCHAR  131
        IN UCHAR  132
        IN UCHAR  133

Arguments:

    SdhcExtension - Host controller specific driver context.

    Buffer - Data buffer to write.

    Length - Length of the data buffer in bytes.

Return value:

    None.

--*/

{

    while (Length >= sizeof(ULONG)) {
        SdhcWriteRegisterBufferUlong(SdhcExtension,
            SDHC_DATA_PORT,
            (ULONG*)Buffer,
            1);

        Buffer += sizeof(ULONG);
        Length -= sizeof(ULONG);
    }

    if (Length >= sizeof(USHORT)) {
        SdhcWriteRegisterBufferUshort(SdhcExtension,
            SDHC_DATA_PORT,
            (USHORT*)Buffer,
            1);

        Buffer += sizeof(USHORT);
        Length -= sizeof(USHORT);
    }

    if (Length >= sizeof(UCHAR)) {
        SdhcWriteRegisterBufferUchar(SdhcExtension,
            SDHC_DATA_PORT + sizeof(USHORT),
            Buffer,
            1);
    }

    NT_ASSERT((SdhcReadRegisterUshort(SdhcExtension, SDHC_ERROR_STATUS) &
        SDHC_ES_BAD_DATA_SPACE_ACCESS) == 0);
}

NTSTATUS
SdhcBuildTransfer(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ SDPORT_REQUEST* Request
)

/*++

Routine Description:

    Prepare the transfer request.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Command - The command for which we're building the transfer request.

Return value:

    NTSTATUS

--*/

{

    NTSTATUS Status;

    NT_ASSERT(Request->Command.TransferType != SdTransferTypeNone);

    NT_ASSERT(Request->Command.TransferMethod != SdTransferMethodUndefined);

    switch (Request->Command.TransferMethod) {
    case SdTransferMethodPio:
        Status = SdhcBuildPioTransfer(SdhcExtension, Request);
        break;

    case SdTransferMethodSgDma:
        Status = SdhcBuildAdmaTransfer(SdhcExtension, Request);
        break;

    default:
        Status = STATUS_NOT_SUPPORTED;
        break;
    }

    return Status;
}

NTSTATUS
SdhcStartTransfer(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ SDPORT_REQUEST* Request
)

/*++

Routine Description:

    Execute the transfer request.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - The command for which we're building the transfer request.

Return value:

    NTSTATUS

--*/

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
    }

    return Status;
}

NTSTATUS
SdhcBuildPioTransfer(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ SDPORT_REQUEST* Request
)

/*++

Routine Description:

    Prepare the PIO transfer request.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - The command for which we're building the transfer.

Return value:

    NTSTATUS

--*/

{

    return SdhcSetTransferMode(SdhcExtension, Request);
}


NTSTATUS
SdhcBuildAdmaTransfer(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ SDPORT_REQUEST* Request
)

/*++

Routine Description:

    Prepare the ADMA2 transfer request.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - The command for which we're building the transfer.

Return value:

    NTSTATUS

--*/

{

    NTSTATUS Status;
    ULONG TransferLength;

    Status = SdhcSetTransferMode(SdhcExtension, Request);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    NT_ASSERT(Request->Command.ScatterGatherList != NULL);

    //
    // Create the ADMA2 descriptor table in the host's DMA buffer.
    //

    Status =
        SdhcCreateAdmaDescriptorTable(
            Request,
            (BOOLEAN)SdhcExtension->Capabilities.Supported.Address64Bit,
            &TransferLength);

    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    SdhcWriteRegisterUlong(SdhcExtension,
        SDHC_ADMA_SYSADDR_LOW,
        Request->Command.DmaPhysicalAddress.LowPart);

    if (SdhcExtension->Capabilities.Supported.Address64Bit) {
        SdhcWriteRegisterUlong(SdhcExtension,
            SDHC_ADMA_SYSADDR_HIGH,
            Request->Command.DmaPhysicalAddress.HighPart);
    }
    else {

        NT_ASSERT(Request->Command.DmaPhysicalAddress.HighPart == 0);

    }

    return STATUS_SUCCESS;
}

NTSTATUS
SdhcStartPioTransfer(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ SDPORT_REQUEST* Request
)

/*++

Routine Description:

    Execute the PIO transfer request.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - The command for which we're building the transfer.

Return value:

    NTSTATUS

--*/

{

    NT_ASSERT((Request->Command.TransferDirection == SdTransferDirectionRead) ||
        (Request->Command.TransferDirection == SdTransferDirectionWrite));

    if (Request->Command.TransferDirection == SdTransferDirectionRead) {
        SdhcReadDataPort(SdhcExtension,
            Request->Command.DataBuffer,
            Request->Command.BlockSize);

    }
    else {
        SdhcWriteDataPort(SdhcExtension,
            Request->Command.DataBuffer,
            Request->Command.BlockSize);
    }

    Request->Command.BlockCount -= 1;
    if (Request->Command.BlockCount >= 1) {
        Request->Command.DataBuffer += Request->Command.BlockSize;
        if (Request->Command.TransferDirection == SdTransferDirectionRead) {
            Request->RequiredEvents |= SDHC_IS_BUFFER_READ_READY;

        }
        else {
            Request->RequiredEvents |= SDHC_IS_BUFFER_WRITE_READY;
        }

        Request->Status = STATUS_MORE_PROCESSING_REQUIRED;

    }
    else {

        NT_ASSERT(Request->Command.BlockCount == 0);

        Request->RequiredEvents |= SDHC_IS_TRANSFER_COMPLETE;
        Request->Status = STATUS_SUCCESS;
    }

    return STATUS_PENDING;
}

NTSTATUS
SdhcStartAdmaTransfer(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ SDPORT_REQUEST* Request
)

/*++

Routine Description:

    Execute the ADMA2 transfer request.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - The command for which we're building the transfer.

Return value:

    NTSTATUS

--*/

{

    UNREFERENCED_PARAMETER(SdhcExtension);

    Request->Status = STATUS_SUCCESS;
    SdPortCompleteRequest(Request, Request->Status);
    return STATUS_SUCCESS;
}

USHORT
SdhcCalcClockFrequency(
    _In_ SDHC_EXTENSION* SdhcExtension,
    _In_ ULONG TargetFrequency,
    _Out_ ULONG* ActualFrequency
)

/*++

Routine Description:

    Execute the ADMA2 transfer request.

Arguments:

    SdhcExtension - Host controller specific driver context.

    TargetFrequency - The frequency in kHz to which we want to set the bus.

    ActualFrequency - The actual frequency to which the bus is set.

Return value:

    ClockControl - The value of the Clock Control register to be written.

--*/

{

    ULONG BaseFrequency;
    USHORT ClockControl;
    ULONG Divisor;
    USHORT SpecVersion;

    *ActualFrequency = 0;
    BaseFrequency = SdhcExtension->Capabilities.BaseClockFrequencyKhz;
    Divisor = Max(BaseFrequency / TargetFrequency, 1);

    NT_ASSERT(Divisor > 0);

    SpecVersion = SdhcReadRegisterUshort(SdhcExtension, SDHC_VERSION) & 0xFF;
    if (SpecVersion > SDHC_SPEC_VERSION_3) {

        //
        // Calculate the fastest available clock frequency which is <= 
        // tthe requested frequency.
        //

        Divisor = 1;
        while (((BaseFrequency / Divisor) > TargetFrequency) &&
            (Divisor < SDHC_MAX_CLOCK_DIVISOR)) {

            Divisor <<= 1;
        }

        *ActualFrequency = BaseFrequency / Divisor;
        Divisor >>= 1;
        ClockControl = ((USHORT)Divisor << 8);

    }
    else {

        //
        // Host controller version 3.0 supports the 10-bit divided clock mode.
        //

        Divisor = BaseFrequency / TargetFrequency;
        Divisor >>= 1;
        if ((TargetFrequency < BaseFrequency) &&
            (TargetFrequency * 2 * Divisor != BaseFrequency)) {

            Divisor += 1;
        }

        if (Divisor > SDHC_MAX_CLOCK_DIVISOR_SPEC_3 / 2) {
            Divisor = SDHC_MAX_CLOCK_DIVISOR_SPEC_3 / 2;
        }

        if (Divisor == 0) {
            *ActualFrequency = BaseFrequency;

        }
        else {
            *ActualFrequency = BaseFrequency / Divisor;
            *ActualFrequency >>= 1;
        }

        ClockControl = ((USHORT)Divisor & 0xFF) << 8;
        Divisor >>= 8;
        ClockControl |= ((USHORT)Divisor & 0x03) << 6;
    }

    NT_ASSERT((BaseFrequency <= TargetFrequency) ? (Divisor == 0) : TRUE);

    return ClockControl;
}

NTSTATUS
SdhcCreateAdmaDescriptorTable(
    _In_ SDPORT_REQUEST* Request,
    _In_ BOOLEAN Use64BitDescriptor,
    _Out_ ULONG* TotalTransferLength
)

/*++

Routine Description:

    This routine creates a ADMA descriptor table based on scattor gatther list
    given by Sglist.

Arguments:

    Socket - Supplies the pointer to the socket

    Request - Data transfer request for which to build the descriptor table.

    TotalTransferLength - Supplies the pointer to return the total transfer
                          length of the descriptor table

Return value:

    Whether the table was successfully created.

--*/

{
    UCHAR* Buffer;
    SDHC_ADMA2_DESCRIPTOR_TABLE_ENTRY* Descriptor;
    ULONG NumberOfElements;
    PHYSICAL_ADDRESS NextAddress;
    ULONG NextLength;
    ULONG RemainingLength;
    PSCATTER_GATHER_ELEMENT SglistElement;

    Buffer = static_cast<UCHAR*>(Request->Command.DmaVirtualAddress);
    Descriptor = NULL;
    NumberOfElements = Request->Command.ScatterGatherList->NumberOfElements;
    SglistElement = &Request->Command.ScatterGatherList->Elements[0];
    *TotalTransferLength = 0;

    NT_ASSERT(NumberOfElements > 0);

    //
    // Iterate through each element in the SG list and convert it into the
    // descriptor table required by the controller.
    //

    while (NumberOfElements > 0) {
        RemainingLength = SglistElement->Length;
        NextAddress.QuadPart = SglistElement->Address.QuadPart;

        NT_ASSERT(RemainingLength > 0);

        while (RemainingLength > 0) {
            Descriptor = (SDHC_ADMA2_DESCRIPTOR_TABLE_ENTRY*)Buffer;
            Buffer += sizeof(SDHC_ADMA2_DESCRIPTOR_TABLE_ENTRY);

            NT_ASSERT((ULONG_PTR)Buffer <
                (ULONG_PTR)Request->Command.DmaVirtualAddress +
                Request->Command.Length);

            NextLength = Min(SDHC_ADMA2_MAX_LENGTH_PER_ENTRY, RemainingLength);
            RemainingLength -= NextLength;

            //
            // Set the entry attributes and length.
            //

            Descriptor->AsUlong = 0;
            Descriptor->Action = SDHC_ADMA2_ACTION_TRAN;
            Descriptor->Attribute = SDHC_ADMA2_ATTRIBUTE_VALID;
            Descriptor->Length = NextLength;
            *TotalTransferLength += NextLength;

            //
            // Set the address field.
            //

            if (Use64BitDescriptor) {
                *((ULONGLONG*)Buffer) = NextAddress.QuadPart;
                Buffer += sizeof(LONGLONG);

            }
            else {

                //
                // The HighPart should not be a non-zero value, since in the
                // DMA adapter object, we declared that this device only
                // supports 32-bit addressing.
                //

                NT_ASSERT(NextAddress.HighPart == 0);

                *((ULONG*)Buffer) = NextAddress.LowPart;
                Buffer += sizeof(ULONG);
            }

            NextAddress.QuadPart += NextLength;
        }

        SglistElement += 1;
        NumberOfElements -= 1;

        //
        // Set the END bit if we're at the last element.
        //

        if (NumberOfElements == 0) {
            Descriptor->Attribute |= SDHC_ADMA2_ATTRIBUTE_END;
        }
    }

    return STATUS_SUCCESS;
}

void
SdhcInitializePciConfigSpace(
    _In_ SD_MINIPORT* Miniport
)

/*++

Routine Description:

    This routine updates the PCI configuration space.

Arguments:

    Argument - Pointer to the functional device object extension for the SD host controller

Return value:

    None.

--*/

{
    if (Miniport->ConfigurationInfo.BusType != SdBusTypePci) {
        return;
    }

    //
    // Some PCI based controllers require access to PCI configuration
    // space to enable certain features. In order to do this, sdport
    // provides two APIs, SdPortGetPciConfigSpace and 
    // SdPortSetPciConfigSpace to access various PCI configuration 
    // registers. For example, to access offset 0xA0 and unset bit
    // 31 (hypothetically to turn on or off a feature):
    //  
    // ULONG PciConfig;
    //
    // PciConfig = 0;
    // SdPortGetPciConfigSpace(Miniport,
    //                         0xA0,
    //                         (UCHAR*)&PciConfig,
    //                         sizeof(PciConfig));
    //
    // PciConfig &= ~(1 << 31);
    // SdPortSetPciConfigSpace(Miniport,
    //                         0xA0,
    //                         (UCHAR*)&PciConfig,
    //                         sizeof(PciConfig));
    //
}
