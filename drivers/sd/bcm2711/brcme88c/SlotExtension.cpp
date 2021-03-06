/*
Copyright 2021 Douglas Cook

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "precomp.h"
#include "SlotExtension.h"
#include "driver.h"
#include "trace.h"
#include "SdRegisters.h"

static unsigned constexpr GpioExpanderOffset = 128;
static unsigned constexpr SignalingVoltageGpio = 4 + GpioExpanderOffset;

static unsigned constexpr RetryMaxCount = 100u;
static unsigned constexpr RetryWaitMicroseconds = 1000u;

static unsigned constexpr ClockWaitMicroseconds = 10u * 1000u;
static unsigned constexpr SignallingWaitMicroseconds = 5u * 1000u;

// 14 = slowest possible timeout clock.
static unsigned constexpr DataTimeoutCounterValue = 14u;

static UINT8 constexpr MaximumOutstandingRequests = 1;

_IRQL_requires_max_(HIGH_LEVEL)
static constexpr __forceinline
SdRegNormalInterrupts
EventMaskToInterruptMask(ULONG eventMask)
{
    return SdRegNormalInterrupts(static_cast<UINT16>(eventMask & 0xFFFF));
}

_IRQL_requires_max_(DISPATCH_LEVEL)
static __forceinline
void
FreeDmaMdl(_Pre_notnull_ __drv_freesMem(Mem) MDL* pDmaMdl)
{
    MmFreePagesFromMdl(pDmaMdl);
    ExFreePool(pDmaMdl);
}

_IRQL_requires_max_(HIGH_LEVEL)
static __forceinline
UINT32
PfnToLegacyMasterAddress(PFN_NUMBER pfn, UINT32 dmaTranslation)
{
    NT_ASSERT(pfn < 0x3FFFFFFF / PAGE_SIZE);
    return static_cast<UINT32>(pfn) * PAGE_SIZE + dmaTranslation;
}

_IRQL_requires_max_(HIGH_LEVEL)
static constexpr __forceinline
LARGE_INTEGER
MakeLargeInteger(ULONG value32)
{
    LARGE_INTEGER li = {};
    li.LowPart = value32;
    return li;
}

enum ChipModel : UINT16
{
    ChipModelBrcm2711 = 0x2711,
};

enum Brcm2711Stepping : UINT8
{
    Brcm2711SteppingB0 = 0x10,
    Brcm2711SteppingC0 = 0x20,
};

union ChipRev
{
    ULONG U32;
    struct
    {
        Brcm2711Stepping Stepping;
        UINT8 Padding;
        ChipModel Model;
    };
};

_IRQL_requires_max_(PASSIVE_LEVEL)
static ChipRev CODE_SEG_PAGE
GetChipRev()
{
    static ULONG constexpr ID_CHIPREV = 0x7c404000 + 0x80000000;

    ChipRev chipRev;

    auto const pChipRev = (ULONG*)MmMapIoSpaceEx(
        MakeLargeInteger(ID_CHIPREV),
        sizeof(ULONG),
        PAGE_READONLY | PAGE_NOCACHE);
    if (!pChipRev)
    {
        chipRev.U32 = 0;
        LogError("GetChipRev-MmMapIoSpaceEx");
    }
    else
    {
        chipRev.U32 = ReadULongNoFence(pChipRev);
        MmUnmapIoSpace(pChipRev, sizeof(ULONG));
        LogInfo("GetChipRev",
            TraceLoggingHexInt32(chipRev.U32, "ChipRev"));
    }

    return chipRev;
}

static constexpr LARGE_INTEGER LargeInteger0 = MakeLargeInteger(0x0);
static constexpr LARGE_INTEGER LargeIntegerPageSize = MakeLargeInteger(PAGE_SIZE);
static constexpr LARGE_INTEGER LargeInteger3FFFFFFF = MakeLargeInteger(0x3FFFFFFF);

_Use_decl_annotations_ // = PASSIVE_LEVEL
void CODE_SEG_PAGE
SlotExtension::SlotCleanup()
{
    PAGED_CODE();
    NT_ASSERT(m_magic == MagicValid);

    FreeDmaBuffers();

    LogInfo("SlotCleanup",
        TraceLoggingPointer((void*)m_registers, "Registers"));
    m_magic = MagicPostCleanup;
}

_Use_decl_annotations_ // = PASSIVE_LEVEL
NTSTATUS CODE_SEG_PAGE
SlotExtension::SlotInitialize(
    PHYSICAL_ADDRESS physicalBase,
    void* virtualBase,
    ULONG length,
    BOOLEAN crashdumpMode)
{
    // Similar sample code: SdhcSlotInitialize
    PAGED_CODE();
    UNREFERENCED_PARAMETER(physicalBase);
    NT_ASSERT(m_magic == MagicPreInitialize);

    enum GpioState : ULONG
    {
        GpioStateOff = 0,
        GpioStateOn = 1,
        GpioStateUnavailable = 7, // Arbitrary magic value for logging purposes.
        GpioStateQueryFailed = 9, // Arbitrary magic value for logging purposes.
    };

    NTSTATUS status;

    memset(this, 0, sizeof(*this)); // Probably already zeroed, but just to be sure.

    if (length < sizeof(SdRegisters))
    {
        LogError("SlotInitialize-BadLength",
            TraceLoggingValue(length));
        return STATUS_NOT_SUPPORTED;
    }

    // BRCME88C: DMA behavior depends on chip revision.
    ChipRev chipRev = GetChipRev();
    if (chipRev.Model != ChipModelBrcm2711)
    {
        LogWarning("SlotInitialize-BadChipModel",
            TraceLoggingHexInt32(chipRev.U32, "ChipRev"));
        m_dmaTranslation = 0xC0000000; // Fall back to Stepping-B0 behavior.
    }
    else if (chipRev.Stepping < Brcm2711SteppingC0)
    {
        m_dmaTranslation = 0xC0000000; // Stepping-B0.
    }
    else
    {
        // TODO: Use normal ScatterGatherDma for stepping C0.
        m_dmaTranslation = 0;
    }

    // Preallocate DMA buffers based on expected maximum transfer size.
    // In crashdump mode, transfers are currently limited to 64KB.
    // In normal mode, transfers are currently limited to 1MB.
    status = AllocateDmaBuffers(crashdumpMode ? 0x10000 : 0x100000);
    if (!NT_SUCCESS(status))
    {
        LogError("SlotInitialize-AllocateDmaBuffers",
            TraceLoggingNTStatus(status));
        return status;
    }

    m_registers = static_cast<SdRegisters*>(virtualBase);
    m_magic = MagicValid;
    m_crashDumpMode = crashdumpMode != 0;
    m_regulatorVoltage1_8 = false;

    // BRCME88C: Voltage regulator control.
    bool const driverRpiqIsAvailable = DriverRpiqIsAvailable();
    SdRegHostControl2 const oldHostControl2(READ_REGISTER_USHORT(&m_registers->HostControl2.U16));
    ULONG oldSignalingVoltageGpioState = GpioStateUnavailable;
    if (driverRpiqIsAvailable)
    {
        // For informational purposes, get existing voltage state.
        MAILBOX_GET_SET_GPIO_EXPANDER info;
        INIT_MAILBOX_GET_GPIO_EXPANDER(&info, SignalingVoltageGpio);
        info.GpioState = GpioStateQueryFailed;
        DriverRpiqProperty(&info.Header);
        oldSignalingVoltageGpioState = info.GpioState;

        // Try to reset voltage regulator to 3.3.
        SetRegulatorVoltage1_8(false);
    }

    SdRegSpecVersion const specVersion =
        (SdRegSpecVersion)READ_REGISTER_UCHAR((UCHAR*)&m_registers->SpecVersion);
    ULONG const maxCurrents = READ_REGISTER_ULONG((ULONG*)m_registers->MaximumCurrentVdd1);

    SdRegCapabilities const regCaps(
        READ_REGISTER_ULONG(&m_registers->Capabilities.U32s[0]),
        READ_REGISTER_ULONG(&m_registers->Capabilities.U32s[1]));

    NT_ASSERT(regCaps.BaseFrequencyForSdClock != 0);
    NT_ASSERT(regCaps.MaxBlockLength < 3);
    NT_ASSERT(regCaps.TimerCountForReTuning < 0x0f);
    NT_ASSERT(regCaps.RetuningModes < 3);

    m_capabilities.SpecVersion = (UCHAR)specVersion;

    m_capabilities.MaximumOutstandingRequests = MaximumOutstandingRequests;

    m_capabilities.MaximumBlockSize =
        static_cast<USHORT>(512 << regCaps.MaxBlockLength);

    m_capabilities.MaximumBlockCount = 0xffff;

    m_capabilities.BaseClockFrequencyKhz =
        regCaps.BaseFrequencyForSdClock * 1000;

    m_capabilities.TuningTimerCountInSeconds =
        regCaps.TimerCountForReTuning == 0
        ? 0
        : 1 << (regCaps.TimerCountForReTuning - 1);

    m_capabilities.DmaDescriptorSize = sizeof(SdRegDma32);

    m_capabilities.AlignmentRequirement =
        regCaps.SystemAddress64BitV3 ? 7 : 3;

    m_capabilities.PioTransferMaxThreshold = 64;

    bool const signalingVoltage18V = regCaps.DDR50 != 0
        && driverRpiqIsAvailable; // BRCME88C: 1.8v requires RPIQ driver.

    // BRCME88C: The Raspberry Pi's "ACPI bus" doesn't play nice with the Windows
    // DMA system. Setting ScatterGatherDma = true means sdport will try to set up
    // Windows-style DMA for us which is just a waste of time. Setting
    // ScatterGatherDma = false means sdport will skip that setup and always
    // request a "PIO" transfer. We can then implement the "PIO" transfer as PIO
    // or DMA as appropriate.
    m_capabilities.Supported.ScatterGatherDma = false; // regCaps.Adma2 != 0;

    m_capabilities.Supported.Address64Bit = regCaps.SystemAddress64BitV3 != 0;

    // BRCME88C: Capabilities are wrong - we only have 4 data pins.
    m_capabilities.Supported.BusWidth8Bit = false; // regCaps.DeviceBus8Bit != 0;

    m_capabilities.Supported.HighSpeed = regCaps.HighSpeed != 0;
    m_capabilities.Supported.SignalingVoltage18V = signalingVoltage18V;
    m_capabilities.Supported.SDR50 = signalingVoltage18V && false; // TUNE-TODO: regCaps.SDR50 != 0;
    m_capabilities.Supported.DDR50 = signalingVoltage18V;
    m_capabilities.Supported.SDR104 = signalingVoltage18V && regCaps.SDR104 != 0;
    m_capabilities.Supported.HS200 = false;
    m_capabilities.Supported.HS400 = false;
    m_capabilities.Supported.Reserved = 0;
    m_capabilities.Supported.DriverTypeA = regCaps.DriverTypeA != 0;
    m_capabilities.Supported.DriverTypeB = true;
    m_capabilities.Supported.DriverTypeC = regCaps.DriverTypeC != 0;
    m_capabilities.Supported.DriverTypeD = regCaps.DriverTypeD != 0;
    m_capabilities.Supported.TuningForSDR50 = regCaps.UseTuningForSDR50 != 0;
    m_capabilities.Supported.SoftwareTuning = regCaps.RetuningModes == 0;
    m_capabilities.Supported.AutoCmd12 = true;
    m_capabilities.Supported.AutoCmd23 = specVersion >= SdRegSpecVersion3;

    // BRCME88C: Capabilities are wrong. It only supports 1.8v for signaling,
    // not for VDD1.
    m_capabilities.Supported.Voltage18V = false; // regCaps.Voltage1_8 != 0;

    m_capabilities.Supported.Voltage30V = regCaps.Voltage3_0 != 0;
    m_capabilities.Supported.Voltage33V = regCaps.Voltage3_3 != 0;

    // Assume that current is most restricted at the highest voltage.
    unsigned const currentLimit = // In 4-milliamp increments
        regCaps.Voltage3_3 ? (UCHAR)maxCurrents
        : regCaps.Voltage3_0 ? (UCHAR)(maxCurrents >> 8)
        : regCaps.Voltage1_8 ? (UCHAR)(maxCurrents >> 16)
        : 0; // 0 is unexpected.
    m_capabilities.Supported.Limit200mA = currentLimit >= 50;
    m_capabilities.Supported.Limit400mA = currentLimit >= 100;
    m_capabilities.Supported.Limit600mA = currentLimit >= 150;
    m_capabilities.Supported.Limit800mA = currentLimit >= 200;
    m_capabilities.Supported.SaveContext = false; // TODO?
    m_capabilities.Supported.Reserved1 = 0;

    m_capabilities.Flags.UsePioForRead = true;
    m_capabilities.Flags.UsePioForWrite = true;
    m_capabilities.Flags.Reserved = 0;

    // Start with interrupts disabled.
    SlotToggleEvents(0xFFFF, false);

    LogInfo("SlotInitialize",
        TraceLoggingHexInt32(chipRev.U32, "ChipRev"),
        TraceLoggingUInt16(m_capabilities.SpecVersion, "SpecVersion"),
        TraceLoggingHexUInt8Array((UINT8*)&maxCurrents, 4, "Vdd1Max"),
        TraceLoggingHexInt64(regCaps.U64, "Capabilities"),
        TraceLoggingHexInt16(oldHostControl2.U16, "OldHostControl2"),
        TraceLoggingUInt32(oldSignalingVoltageGpioState, "OldRegulator"),
        TraceLoggingPointer((void*)m_registers, "Registers"));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ // = PASSIVE_LEVEL
void CODE_SEG_PAGE
SlotExtension::SlotGetSlotCapabilities(
    SDPORT_CAPABILITIES* capabilities)
{
    PAGED_CODE();
    *capabilities = m_capabilities;
    return;
}

_Use_decl_annotations_ // <= APC_LEVEL
NTSTATUS CODE_SEG_PAGE
SlotExtension::SlotIssueBusOperation(
    SDPORT_BUS_OPERATION const& busOperation)
{
    // Similar sample code: SdhcSlotIssueBusOperation
    PAGED_CODE();

    NTSTATUS status;

    switch (busOperation.Type)
    {
    case SdResetHw:
        status = ResetHw();
        break;

    case SdResetHost:
        status = ResetHost(busOperation.Parameters.ResetType);
        break;

    case SdSetClock:
        status = SetClock(busOperation.Parameters.FrequencyKhz);
        break;

    case SdSetVoltage:
        status = SetVoltage(busOperation.Parameters.Voltage);
        break;

    case SdSetPower:
        // Not actually used - sdport seems to only use SdSetVoltage.
        LogError("SetPower-NotSupported",
            TraceLoggingBoolean(busOperation.Parameters.PowerEnabled, "PowerEnabled"));
        status = STATUS_NOT_SUPPORTED;
        break;

    case SdSetBusWidth:
        status = SetBusWidth(busOperation.Parameters.BusWidth);
        break;

    case SdSetBusSpeed:
        status = SetBusSpeed(busOperation.Parameters.BusSpeed);
        break;

    case SdSetSignalingVoltage:
        status = SetSignalingVoltage(busOperation.Parameters.SignalingVoltage);
        break;

    case SdSetDriveStrength:
        // Not actually used - sdport seems to only use SdSetDriverType.
        LogError("SetDriveStrength-NotSupported",
            TraceLoggingUInt32(busOperation.Parameters.DriveStrength, "DriveStrength"));
        status = STATUS_NOT_SUPPORTED;
        break;

    case SdSetDriverType:
        status = SetDriverType(busOperation.Parameters.DriverType);
        break;

    case SdSetPresetValue:
        status = SetPresetValue(busOperation.Parameters.PresetValueEnabled != 0);
        break;

    case SdSetBlockGapInterrupt:
        status = SetBlockGapInterrupt(busOperation.Parameters.BlockGapIntEnabled != 0);
        break;

    case SdExecuteTuning:
        status = ExecuteTuning();
        break;

    default:
        LogError("IssueBusOperation-BadType",
            TraceLoggingUInt32(busOperation.Type, "Type"),
            TraceLoggingHexInt32(busOperation.Parameters.FrequencyKhz, "Parameters"));
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    return status;
}

_Use_decl_annotations_ // = PASSIVE_LEVEL
BOOLEAN CODE_SEG_PAGE
SlotExtension::SlotGetCardDetectState()
{
    // Similar sample code: SdhcIsCardInserted
    PAGED_CODE();

    SdRegPresentState presentState(READ_REGISTER_ULONG(&m_registers->PresentState.U32));
    bool const cardInserted = presentState.CardInserted != 0;

    LogVerbose("GetCardDetectState",
        TraceLoggingValue(cardInserted, "CardInserted"));
    return cardInserted;
}

_Use_decl_annotations_ // <= DISPATCH_LEVEL
BOOLEAN CODE_SEG_TEXT
SlotExtension::SlotGetWriteProtectState()
{
    // Similar sample code: SdhcIsWriteProtected

    SdRegPresentState presentState(READ_REGISTER_ULONG(&m_registers->PresentState.U32));
    bool const writeProtected = presentState.WriteEnabled == 0;

    LogVerbose("GetWriteProtectState",
        TraceLoggingValue(writeProtected));
    return writeProtected;
}

_Use_decl_annotations_ // <= HIGH_LEVEL
BOOLEAN CODE_SEG_TEXT
SlotExtension::SlotInterrupt(
    ULONG* events,
    ULONG* errors,
    BOOLEAN* notifyCardChange,
    BOOLEAN* notifySdioInterrupt,
    BOOLEAN* notifyTuning)
{
    // Similar sample code: SdhcSlotInterrupt

    bool clearedAnything;

    SdRegNormalInterrupts const interrupts(
        READ_REGISTER_USHORT(&m_registers->NormalInterruptStatus.U16));

    if (interrupts.U16 == 0 || interrupts.U16 == 0xFFFF)
    {
        // 0xFFFF means controller is offline and interrupts are not real.
        *events = 0;
        *errors = 0;
        *notifyCardChange = false;
        *notifySdioInterrupt = false;
        *notifyTuning = false;
        clearedAnything = false;
    }
    else
    {
        // Make a copy of interrupts, but clear flags that will be reported
        // in errors, notifyCardChange, notifySdioInterrupt, or notifyTuning.
        SdRegNormalInterrupts unreported = interrupts;
        unreported.CardInsertion = false;
        unreported.CardRemoval = false;
        unreported.CardInterrupt = false;
        unreported.ReTuningEvent = false;
        unreported.ErrorInterrupt = false;

        *events = unreported.U16;
        *errors = interrupts.ErrorInterrupt
            ? READ_REGISTER_USHORT(&m_registers->ErrorInterruptStatus.U16)
            : 0;
        *notifyCardChange = interrupts.CardInsertion || interrupts.CardRemoval;
        *notifySdioInterrupt = interrupts.CardInterrupt != 0;
        *notifyTuning = interrupts.ReTuningEvent != 0;

        AcknowledgeInterrupts(interrupts);
        clearedAnything = true;
    }

    return clearedAnything;
}

_Use_decl_annotations_ // = DISPATCH_LEVEL
NTSTATUS CODE_SEG_TEXT
SlotExtension::SlotIssueRequest(
    _Inout_ SDPORT_REQUEST* request)
{
    // Similar sample code: SdhcSlotIssueRequest

    NTSTATUS status;

    switch (request->Type)
    {
    case SdRequestTypeCommandNoTransfer:
    case SdRequestTypeCommandWithTransfer:
        status = SendCommand(request);
        break;

    case SdRequestTypeStartTransfer:
        switch (request->Command.TransferMethod)
        {
        case SdTransferMethodPio:
            status = m_dmaInProgress != SdTransferDirectionUndefined
                ? StartDmaTransfer(request)
                : StartPioTransfer(request);
            break;
        case SdTransferMethodSgDma:
            status = StartSgDmaTransfer(request);
            break;
        default:
            LogError("IssueRequest-BadMethod",
                TraceLoggingUInt32(request->Command.TransferMethod, "TransferMethod"));
            status = STATUS_NOT_SUPPORTED;
            break;
        }
        break;

    default:
        LogError("IssueRequest-BadType",
            TraceLoggingUInt32(request->Command.TransferMethod, "Type"));
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    return status;
}

_Use_decl_annotations_ // <= DISPATCH_LEVEL
void CODE_SEG_TEXT
SlotExtension::SlotGetResponse(
    SDPORT_COMMAND const& command,
    void* responseBuffer)
{
    // Similar sample code: SdhcGetResponse

    // TODO? AutoCmd responses are in high registers. Do we care?

    UINT16 responseLength;
    switch (command.ResponseType)
    {
    case SdResponseTypeNone:
        responseLength = 0;
        break;

    case SdResponseTypeR1:
    case SdResponseTypeR1B:
    case SdResponseTypeR3:
    case SdResponseTypeR4:
    case SdResponseTypeR5:
    case SdResponseTypeR5B:
    case SdResponseTypeR6:
        // 4-byte response:
        responseLength = 4;
        break;

    case SdResponseTypeR2:
        // 16-byte response:
        responseLength = 16;
        break;

    default:
        LogError("GetResponse-BadType",
            TraceLoggingUInt32(command.ResponseType, "ResponseType"));
        return;
    }

    READ_REGISTER_BUFFER_ULONG(m_registers->Response32s, (ULONG*)responseBuffer, responseLength / 4);

    LogVerboseInfo("GetResponse",
        TraceLoggingUInt32(command.ResponseType, "ResponseType"),
        TraceLoggingHexInt32Array((INT32*)responseBuffer, responseLength / 4, "Data"));
    return;
}

_Use_decl_annotations_ // <= HIGH_LEVEL
void CODE_SEG_TEXT
SlotExtension::SlotToggleEvents(
    ULONG eventMask,
    BOOLEAN enable)
{
    // Similar sample code: SdhcSlotToggleEvents, SdhcEnableInterrupt, SdhcDisableInterrupt

    auto const interruptMask = EventMaskToInterruptMask(eventMask);

    auto const oldEnable = SdRegNormalInterrupts(
        READ_REGISTER_USHORT(&m_registers->NormalInterruptSignalEnable.U16));

    if (enable)
    {
        SdRegNormalInterrupts const newEnable(oldEnable.U16 | interruptMask.U16);
        if (!m_crashDumpMode)
        {
            // Enable signals to OS.

            WRITE_REGISTER_USHORT(&m_registers->NormalInterruptSignalEnable.U16, newEnable.U16);
            WRITE_REGISTER_USHORT(&m_registers->ErrorInterruptSignalEnable.U16, 0xffff);
        }

        // Enable signals on controller.

        WRITE_REGISTER_USHORT(&m_registers->NormalInterruptStatusEnable.U16, newEnable.U16);
        WRITE_REGISTER_USHORT(&m_registers->ErrorInterruptStatusEnable.U16, 0xffff);
    }
    else
    {
        SdRegNormalInterrupts const newEnable(oldEnable.U16 & ~interruptMask.U16);

        // Disable signals on controller.

        WRITE_REGISTER_USHORT(&m_registers->NormalInterruptStatusEnable.U16, newEnable.U16);
        WRITE_REGISTER_USHORT(&m_registers->ErrorInterruptStatusEnable.U16, 0);

        // Disable signals to OS.

        WRITE_REGISTER_USHORT(&m_registers->NormalInterruptSignalEnable.U16, newEnable.U16);
        WRITE_REGISTER_USHORT(&m_registers->ErrorInterruptSignalEnable.U16, 0);
    }

    LogVerboseInfo("ToggleEvents",
        TraceLoggingBoolean(enable, "Enable"),
        TraceLoggingHexInt32(eventMask, "EventMask"),
        TraceLoggingHexInt16(READ_REGISTER_USHORT(&m_registers->NormalInterruptSignalEnable.U16), "NormalSignal"));
    return;
}

_Use_decl_annotations_ // <= HIGH_LEVEL
void CODE_SEG_TEXT
SlotExtension::SlotClearEvents(
    ULONG eventMask)
{
    // Similar sample code: SdhcSlotClearEvents

    auto const interruptMask = EventMaskToInterruptMask(eventMask);
    AcknowledgeInterrupts(interruptMask);
    return;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
static NTSTATUS CODE_SEG_TEXT
ErrorInterruptToStatus(SdRegErrorInterrupts errorInterrupts)
{
    NTSTATUS status;

    if (errorInterrupts.U16 == 0)
    {
        status = STATUS_SUCCESS;
    }
    else if (errorInterrupts.CommandTimeout || errorInterrupts.DataTimeout)
    {
        status = STATUS_IO_TIMEOUT;
    }
    else if (errorInterrupts.CommandCRC || errorInterrupts.DataCRC)
    {
        status = STATUS_CRC_ERROR;
    }
    else if (errorInterrupts.CommandEndBit || errorInterrupts.DataEndBit)
    {
        status = STATUS_DEVICE_DATA_ERROR;
    }
    else if (errorInterrupts.CommandIndex)
    {
        status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    else if (errorInterrupts.CurrentLimit)
    {
        status = STATUS_DEVICE_POWER_FAILURE;
    }
    else
    {
        status = STATUS_IO_DEVICE_ERROR;
    }

    return status;
}

_Use_decl_annotations_ // = DISPATCH_LEVEL
void CODE_SEG_TEXT
SlotExtension::SlotRequestDpc(
    SDPORT_REQUEST* request,
    ULONG events,
    ULONG errors)
{
    // Similar sample code: SdhcRequestDpc

    auto const originalRequired =
        InterlockedAndNoFence((long*)&request->RequiredEvents, ~events);

    if (errors != 0)
    {
        SdRegErrorInterrupts const errorInterrupts(static_cast<UINT16>(errors));
        NTSTATUS const status = ErrorInterruptToStatus(errorInterrupts);

        LogWarning("RequestDpc-Error",
            TraceLoggingPointer(request),
            TraceLoggingNTStatus(status),
            TraceLoggingHexInt32(originalRequired, "RequiredEvents"),
            TraceLoggingHexInt32(events, "Events"),
            TraceLoggingHexInt32(errors, "Errors"));
        InterlockedAndNoFence((long*)&request->RequiredEvents, 0);
        SdPortCompleteRequest(request, status);
    }
    else if ((originalRequired & ~events) == 0)
    {
        NTSTATUS status = request->Status;
        if (status != STATUS_MORE_PROCESSING_REQUIRED)
        {
            status = STATUS_SUCCESS;
            request->Status = status;
        }

        LogVerboseInfo("RequestDpc-Complete",
            TraceLoggingPointer(request),
            TraceLoggingNTStatus(status),
            TraceLoggingHexInt32(events, "Events"));
        SdPortCompleteRequest(request, status);
    }
    else
    {
        LogVerboseInfo("RequestDpc-Partial",
            TraceLoggingPointer(request),
            TraceLoggingHexInt32(originalRequired, "RequiredEvents"),
            TraceLoggingHexInt32(events, "Events"));
    }

    return;
}

_Use_decl_annotations_ // <= DISPATCH_LEVEL
void CODE_SEG_TEXT
SlotExtension::SlotSaveContext()
{
    LogError("SaveContext"); // Unexpected
    return;
}

_Use_decl_annotations_ // <= DISPATCH_LEVEL
void CODE_SEG_TEXT
SlotExtension::SlotRestoreContext()
{
    LogError("SlotRestoreContext"); // Unexpected
    return;
}

_Use_decl_annotations_ // <= DISPATCH_LEVEL
void CODE_SEG_TEXT
SlotExtension::FreeDmaBuffers()
{
    if (m_pDmaDataMdl)
    {
        FreeDmaMdl(m_pDmaDataMdl);
        m_pDmaDataMdl = nullptr;
    }

    if (m_pDmaDescriptorMdl)
    {
        FreeDmaMdl(m_pDmaDescriptorMdl);
        m_pDmaDescriptorMdl = nullptr;
    }

    LogInfo("FreeDmaBuffers");
}

_Use_decl_annotations_ // <= DISPATCH_LEVEL
NTSTATUS CODE_SEG_TEXT
SlotExtension::AllocateDmaBuffers(ULONG cbDmaData)
{
    NT_ASSERT(cbDmaData > 0);
    NT_ASSERT(cbDmaData < 0x10000000);

    NTSTATUS status;
    MDL* pDmaDataMdl = nullptr;
    MDL* pDmaDescriptorMdl = nullptr;
    ULONG const cDmaDataPages = (cbDmaData + PAGE_SIZE - 1) / PAGE_SIZE;
    SdRegDma32* pDescriptors;
    PFN_NUMBER* pPfns;

    // DMA bounce buffer:
    // - Must be in low 1GB of physical memory.
    // - We need all of the pages we're asking for. Partial success is not ok.
    // - We don't need them to be contiguous. Scattered pages are fine.
    //   (Note: MM_ALLOCATE_PREFER_CONTIGUOUS is confusingly-named.)
    // - After allocating the physical memory, map it into system address space.
    pDmaDataMdl = MmAllocatePagesForMdlEx(
        LargeInteger0,        // LowAddress
        LargeInteger3FFFFFFF, // HighAddress
        LargeIntegerPageSize, // SkipBytes
        cDmaDataPages * static_cast<size_t>(PAGE_SIZE),
        MmCached,
        MM_ALLOCATE_FULLY_REQUIRED | MM_ALLOCATE_PREFER_CONTIGUOUS | MM_DONT_ZERO_ALLOCATION);
    if (!pDmaDataMdl ||
        !MmGetSystemAddressForMdlSafe(pDmaDataMdl, HighPagePriority | MdlMappingNoExecute))
    {
        LogError("AllocateDmaBuffers-Data",
            TraceLoggingUInt32(cbDmaData));
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    // DMA descriptor buffer:
    // - Usually just one page (one page of descriptors can cover a 2MB transfer,
    //   and sdport is currently limited to 1MB transfers).
    // - Must be in low 1GB of physical memory.
    // - We need all of the pages we're asking for. Partial success is not ok.
    // - We need the pages to be in contiguous physical memory.
    // - After allocating the physical memory, map it into system address space.
    pDmaDescriptorMdl = MmAllocatePagesForMdlEx(
        LargeInteger0,        // LowAddress
        LargeInteger3FFFFFFF, // HighAddress
        LargeInteger0,        // SkipBytes
        ROUND_TO_PAGES(cDmaDataPages * sizeof(SdRegDma32)),
        MmCached,
        MM_ALLOCATE_FULLY_REQUIRED | MM_ALLOCATE_REQUIRE_CONTIGUOUS_CHUNKS);
    if (!pDmaDescriptorMdl ||
        !MmGetSystemAddressForMdlSafe(pDmaDescriptorMdl, HighPagePriority | MdlMappingNoExecute))
    {
        LogError("AllocateDmaBuffers-Descriptor",
            TraceLoggingUInt32(cbDmaData));
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    NT_ASSERT(MmGetMdlByteOffset(pDmaDataMdl) == 0);
    NT_ASSERT(MmGetSystemAddressForMdlSafe(pDmaDataMdl, 0) == pDmaDataMdl->MappedSystemVa);
    NT_ASSERT(MmGetMdlByteOffset(pDmaDescriptorMdl) == 0);
    NT_ASSERT(MmGetSystemAddressForMdlSafe(pDmaDescriptorMdl, 0) == pDmaDescriptorMdl->MappedSystemVa);

    // Initialize our descriptor array to point at the pages in the DMA bounce buffer.
    pDescriptors = static_cast<SdRegDma32*>(pDmaDescriptorMdl->MappedSystemVa);
    pPfns = MmGetMdlPfnArray(pDmaDataMdl);
    for (ULONG i = 0; i != cDmaDataPages; i += 1)
    {
        NT_ASSERT(pPfns[i] < (0x40000000 / PAGE_SIZE));
        auto& desc = pDescriptors[i];
        desc.Valid = true;
        desc.End = false;
        desc.Int = false;
        desc.Action = SdRegDmaActionAdma2Tran;
        desc.LengthHigh = 0;
        desc.Length = PAGE_SIZE;
        desc.Address = PfnToLegacyMasterAddress(pPfns[i], m_dmaTranslation);
    }

    pDescriptors[0].End = true; // Set descriptor #0 as end-of-transfer.
    m_iDmaEndDescriptor = 0;    // Remember that we set descriptor #0 as end-of-transfer.

    // Free any previously-allocated buffers.
    FreeDmaBuffers();

    m_pDmaDataMdl = pDmaDataMdl;
    m_pDmaDescriptorMdl = pDmaDescriptorMdl;
    pDmaDataMdl = nullptr;
    pDmaDescriptorMdl = nullptr;

    LogInfo("AllocateDmaBuffers",
        TraceLoggingUInt32(cbDmaData));
    status = STATUS_SUCCESS;

Done:

    if (pDmaDataMdl)
    {
        FreeDmaMdl(pDmaDataMdl);
    }

    if (pDmaDescriptorMdl)
    {
        FreeDmaMdl(pDmaDescriptorMdl);
    }

    return status;
}

_Use_decl_annotations_ // <= DISPATCH_LEVEL
bool CODE_SEG_TEXT
SlotExtension::CommandShouldUseDma(SDPORT_COMMAND const& command)
{
    bool useDma;

    // PIO is slower per-byte, but DMA takes more time to set up each transfer.
    // For small transfers, it's more efficient to use PIO.

    // PERF-TODO: Measure overhead of PIO versus DMA to determine the transfer
    // size where DMA becomes more efficient. Threshold is probably different for
    // reads and writes.

    if (command.Length <= m_capabilities.PioTransferMaxThreshold)
    {
        useDma = false;
    }
    else if (command.Length <= MmGetMdlByteCount(m_pDmaDataMdl))
    {
        useDma = true;
    }
    else
    {
        // Unusual - unexpectedly large transfer request.
        // Try to reallocate. Use PIO if reallocation fails.
        useDma = NT_SUCCESS(AllocateDmaBuffers(command.Length));
    }

    return useDma;
}

_Use_decl_annotations_ // = PASSIVE_LEVEL
NTSTATUS CODE_SEG_PAGE
SlotExtension::SetRegulatorVoltage1_8(bool regulatorVoltage1_8)
{
    PAGED_CODE();

    NTSTATUS status;
    MAILBOX_GET_SET_GPIO_EXPANDER info;

    INIT_MAILBOX_SET_GPIO_EXPANDER(&info, SignalingVoltageGpio, regulatorVoltage1_8);
    status = DriverRpiqProperty(&info.Header);
    if (NT_SUCCESS(status))
    {
        m_regulatorVoltage1_8 = regulatorVoltage1_8;
    }

    LogInfo("SetRegulatorVoltage1_8",
        TraceLoggingUInt8(regulatorVoltage1_8, "requested"),
        TraceLoggingNTStatus(status));

    return status;
}

// Parameter for InvokeSetRegulatorVoltageWorker.
struct SetRegulatorVoltageContext
{
    SlotExtension* const slotExtension;
    bool const regulatorVoltage1_8;
    NTSTATUS status;
    KEVENT event;
};

_Use_decl_annotations_
void CODE_SEG_PAGE
SlotExtension::InvokeSetRegulatorVoltageWorker(void* parameter)
{
    PAGED_CODE();

    auto const context = static_cast<SetRegulatorVoltageContext*>(parameter);
    context->status = context->slotExtension->SetRegulatorVoltage1_8(
        context->regulatorVoltage1_8);
    KeSetEvent(&context->event, 0, false);
}

_Use_decl_annotations_ // <= APC_LEVEL
NTSTATUS CODE_SEG_PAGE
SlotExtension::InvokeSetRegulatorVoltage1_8(bool regulatorVoltage1_8)
{
    PAGED_CODE();

    NTSTATUS status;

    if (KeGetCurrentIrql() == PASSIVE_LEVEL)
    {
        status = SetRegulatorVoltage1_8(regulatorVoltage1_8);
    }
    else
    {
        /*
        RPIQ requires caller to be running at PASSIVE_LEVEL.

        Some of the callers of this function could, in theory, be called at APC_LEVEL.
        (It doesn't happen in practice right now because SetSignalingVoltage is only
        called at PASSIVE_LEVEL, but perhaps that might change in the future.)

        If this function is ever called at APC_LEVEL, we marshal the call onto a worker
        thread running at PASSIVE_LEVEL. Waiting at APC for PASSIVE work to complete is
        technically a priority inversion, but I don't think that's a major problem,
        especially since this code only gets called during device configuration.
        */

        SetRegulatorVoltageContext context = { this, regulatorVoltage1_8 };
        KeInitializeEvent(&context.event, NotificationEvent, false);
        m_rpiqWorkItem = { {}, &InvokeSetRegulatorVoltageWorker, &context };

        // ExQueueWorkItem is deprecated because IoAllocateWorkItem does better diagnostic
        // tracking. We don't care about the tracking and this has lower overhead.
#pragma warning(suppress:4996)
        ExQueueWorkItem(&m_rpiqWorkItem, CriticalWorkQueue);

        KeWaitForSingleObject(&context.event, Executive, KernelMode, false, nullptr);
        status = context.status;
        LogInfo("InvokeSetRegulatorVoltage1_8",
            TraceLoggingUInt8(regulatorVoltage1_8, "requested"),
            TraceLoggingNTStatus(status));
    }

    return status;
}

_Use_decl_annotations_ // <= HIGH_LEVEL
void CODE_SEG_TEXT
SlotExtension::AcknowledgeInterrupts(SdRegNormalInterrupts interruptMask)
{
    if (interruptMask.ErrorInterrupt)
    {
        WRITE_REGISTER_USHORT(&m_registers->ErrorInterruptStatus.U16, 0xffff);
    }

    WRITE_REGISTER_USHORT(&m_registers->NormalInterruptStatus.U16, interruptMask.U16);
    return;
}

_Use_decl_annotations_ // = DISPATCH_LEVEL
NTSTATUS CODE_SEG_TEXT
SlotExtension::SendCommand(_Inout_ SDPORT_REQUEST* request)
{
    // Similar sample code: SdhcSendCommand

    SdRegCommand command(0);
    SdRegTransferMode transferMode(0);
    SdRegNormalInterrupts requiredEvents(0);

    m_dmaInProgress = SdTransferDirectionUndefined;

    NT_ASSERT(
        request->Type == SdRequestTypeCommandNoTransfer ||
        request->Type == SdRequestTypeCommandWithTransfer);

    // Determine value for command:

    command.CommandIndex = request->Command.Index;
    command.DataPresent = SdTransferTypeNone != request->Command.TransferType;

    switch (request->Command.ResponseType)
    {
    case SdResponseTypeNone:
        break;

    case SdResponseTypeR1:
    case SdResponseTypeR5:
    case SdResponseTypeR6:
        command.ResponseType = SdRegResponse48;
        command.CommandCrcCheck = true;
        command.CommandIndexCheck = true;
        break;

    case SdResponseTypeR1B:
    case SdResponseTypeR5B:
        command.ResponseType = SdRegResponse48CheckBusy;
        command.CommandCrcCheck = true;
        command.CommandIndexCheck = true;
        requiredEvents.TransferComplete = true;
        break;

    case SdResponseTypeR2:
        command.ResponseType = SdRegResponse136;
        command.CommandCrcCheck = true;
        break;

    case SdResponseTypeR3:
    case SdResponseTypeR4:
        command.ResponseType = SdRegResponse48;
        break;

    default:
        LogError("SendCommand-BadResponseType",
            TraceLoggingUInt32(request->Command.ResponseType, "ResponseType"));
        return STATUS_NOT_SUPPORTED;
    }

    switch (request->Command.Type)
    {
    case SdCommandTypeUndefined: // Normal
    case SdCommandTypeSuspend:
    case SdCommandTypeResume:
    case SdCommandTypeAbort:
        command.CommandType = (SdRegCommandType)request->Command.Type;
        break;
    default:
        LogError("SendCommand-BadCommandType",
            TraceLoggingUInt32(request->Command.Type, "Type"));
        return STATUS_NOT_SUPPORTED;
    }

    // Determine value for transferMode:

    switch (request->Command.TransferType)
    {
    case SdTransferTypeNone:
        NT_ASSERT(request->Command.BlockCount == 0);
        break;

    case SdTransferTypeSingleBlock:
        NT_ASSERT(request->Command.BlockCount == 1);
        __fallthrough;

    case SdTransferTypeMultiBlock:
    case SdTransferTypeMultiBlockNoStop:

        NT_ASSERT(request->Command.BlockCount != 0);
        transferMode.DataTransferDirectionRead =
            (SdTransferDirectionRead == request->Command.TransferDirection);

        switch (request->Command.TransferMethod)
        {
        case SdTransferMethodPio:

            if (CommandShouldUseDma(request->Command))
            {
                auto const pDescriptors = static_cast<SdRegDma32*>(m_pDmaDescriptorMdl->MappedSystemVa);

                // Clean up the transfer-end descriptor from previous DMA.
                pDescriptors[m_iDmaEndDescriptor].End = false;
                pDescriptors[m_iDmaEndDescriptor].Length = PAGE_SIZE;

                // Set up the transfer-end descriptor for this DMA.
                m_iDmaEndDescriptor = (request->Command.Length - 1) / PAGE_SIZE;
                pDescriptors[m_iDmaEndDescriptor].End = true;
                pDescriptors[m_iDmaEndDescriptor].Length = request->Command.Length - (m_iDmaEndDescriptor * PAGE_SIZE);

                transferMode.DmaEnable = true;
                requiredEvents.TransferComplete = true;

                switch (request->Command.TransferDirection)
                {
                case SdTransferDirectionRead:
                    m_dmaInProgress = SdTransferDirectionRead;
                    break;
                case SdTransferDirectionWrite:
                    m_dmaInProgress = SdTransferDirectionWrite;
                    memcpy(m_pDmaDataMdl->MappedSystemVa, request->Command.DataBuffer, request->Command.Length);
                    break;
                default:
                    LogError("SendCommand-BadTransferDirection",
                        TraceLoggingUInt32(request->Command.TransferDirection, "TransferDirection"));
                    return STATUS_NOT_SUPPORTED;
                }

                // TODO: Are these actually needed?
                KeFlushIoBuffers(m_pDmaDescriptorMdl, true, true);
                KeFlushIoBuffers(m_pDmaDataMdl, transferMode.DataTransferDirectionRead, true);

                auto pfn = MmGetMdlPfnArray(m_pDmaDescriptorMdl)[0];
                WRITE_REGISTER_NOFENCE_ULONG(&m_registers->AdmaSystemAddress,
                    PfnToLegacyMasterAddress(pfn, m_dmaTranslation));
            }
            else
            {
                switch (request->Command.TransferDirection)
                {
                case SdTransferDirectionRead:
                    requiredEvents.BufferReadReady = true;
                    break;
                case SdTransferDirectionWrite:
                    requiredEvents.BufferWriteReady = true;
                    break;
                default:
                    LogError("SendCommand-BadTransferDirection",
                        TraceLoggingUInt32(request->Command.TransferDirection, "TransferDirection"));
                    return STATUS_NOT_SUPPORTED;
                }
            }

            break;

        case SdTransferMethodSgDma:
            // BRCME88C: DMA transfers are done with TransferMethod == SdTransferMethodPio.
            //requiredEvents.TransferComplete = true;

        default:
            LogError("SendCommand-BadTransferMethod",
                TraceLoggingUInt32(request->Command.TransferMethod, "TransferMethod"));
            return STATUS_NOT_SUPPORTED;
        }
        break;

    default:

        LogError("SendCommand-BadTransferType",
            TraceLoggingUInt32(request->Command.TransferType, "TransferType"));
        return STATUS_NOT_SUPPORTED;
    }

    if (request->Command.BlockCount > 1)
    {
        transferMode.BlockCountEnable = true;
        transferMode.MultipleBlock = true;
        transferMode.AutoCmdEnable = SdRegAutoCmd12Enable;
    }

    // Start the command:

    requiredEvents.CommandComplete = true;
    InterlockedExchangeNoFence((long*)&request->RequiredEvents, requiredEvents.U16);

    WRITE_REGISTER_NOFENCE_ULONG(&m_registers->SdmaSystemAddress, 0);
    WRITE_REGISTER_NOFENCE_USHORT(&m_registers->BlockSize, request->Command.BlockSize);
    WRITE_REGISTER_NOFENCE_USHORT(&m_registers->BlockCount16, request->Command.BlockCount);
    WRITE_REGISTER_NOFENCE_ULONG(&m_registers->Argument, request->Command.Argument);
    WRITE_REGISTER_NOFENCE_USHORT(&m_registers->TransferMode.U16, transferMode.U16);

    LogVerboseInfo("SendCommand",
        TraceLoggingUInt8(request->Command.Index, "CMD"),
        TraceLoggingHexInt32(request->Command.Argument, "ARG"),
        TraceLoggingHexInt16(command.U16, "CommandReg"),
        TraceLoggingHexInt16(transferMode.U16, "TransferMode"),
        TraceLoggingHexInt16(requiredEvents.U16, "RequiredEvents"));

    // Need fence before starting command.
    WRITE_REGISTER_USHORT(&m_registers->Command.U16, command.U16);

    return STATUS_PENDING;
}

_Use_decl_annotations_ // = DISPATCH_LEVEL
NTSTATUS CODE_SEG_TEXT
SlotExtension::StartPioTransfer(SDPORT_REQUEST* request)
{
    // Similar sample code: SdhcStartPioTransfer

    NT_ASSERT(request->Type == SdRequestTypeStartTransfer);
    NT_ASSERT(request->Command.TransferMethod == SdTransferMethodPio);
    NT_ASSERT(request->Command.BlockCount != 0);

    unsigned const LongSize = sizeof(ULONG);
    unsigned const LongMask = LongSize - 1;
    auto const blockSize = request->Command.BlockSize;
    auto const blockCount = blockSize / LongSize;

    switch (request->Command.TransferDirection)
    {
    case SdTransferDirectionRead:

        if (blockCount != 0)
        {
            // Only need a fence before the first read.
            *(ULONG*)&request->Command.DataBuffer[0] =
                READ_REGISTER_ULONG(&m_registers->BufferDataPort);

            for (unsigned i = 1; i < blockCount; i += 1)
            {
                ((ULONG*)request->Command.DataBuffer)[i] =
                    READ_REGISTER_NOFENCE_ULONG(&m_registers->BufferDataPort);
            }
        }

        if (blockSize & LongMask)
        {
            READ_REGISTER_BUFFER_UCHAR((UCHAR*)&m_registers->BufferDataPort,
                &request->Command.DataBuffer[blockCount * LongSize],
                blockSize & LongMask);
        }

        break;

    case SdTransferDirectionWrite:

        if (blockCount != 0)
        {
            for (unsigned i = 0; i < blockCount - 1; i += 1)
            {
                WRITE_REGISTER_NOFENCE_ULONG(&m_registers->BufferDataPort,
                    ((ULONG*)request->Command.DataBuffer)[i]);
            }

            // Only need a fence after the last write.
            WRITE_REGISTER_ULONG(&m_registers->BufferDataPort,
                ((ULONG*)request->Command.DataBuffer)[blockCount - 1]);
        }

        if (blockSize & LongMask)
        {
            WRITE_REGISTER_BUFFER_UCHAR((UCHAR*)&m_registers->BufferDataPort,
                &request->Command.DataBuffer[blockCount * LongSize],
                blockSize & LongMask);
        }

        break;

    default:

        LogError("StartPioTransfer-BadTransferDirection",
            TraceLoggingUInt32(request->Command.TransferDirection, "TransferDirection"));
        return STATUS_NOT_SUPPORTED;
    }

    request->Command.BlockCount -= 1;
    request->Command.DataBuffer += request->Command.BlockSize;

    SdRegNormalInterrupts requiredEvents(0);
    if (request->Command.BlockCount == 0)
    {
        requiredEvents.TransferComplete = true;
        request->Status = STATUS_SUCCESS;
    }
    else
    {
        if (request->Command.TransferDirection == SdTransferDirectionRead)
        {
            requiredEvents.BufferReadReady = true;
        }
        else
        {
            requiredEvents.BufferWriteReady = true;
        }

        request->Status = STATUS_MORE_PROCESSING_REQUIRED;
    }

    InterlockedOrNoFence((long*)&request->RequiredEvents, requiredEvents.U16);

    LogVerboseInfo("StartPioTransfer",
        TraceLoggingUInt16(blockSize, "BlockSize"));
    return STATUS_PENDING;
}

_Use_decl_annotations_ // = DISPATCH_LEVEL
NTSTATUS CODE_SEG_TEXT
SlotExtension::StartDmaTransfer(SDPORT_REQUEST* request)
{
    NT_ASSERT(request->Type == SdRequestTypeStartTransfer);
    NT_ASSERT(request->Command.TransferMethod == SdTransferMethodPio);

    // BRCME88C: DMA transfers are done with TransferMethod == SdTransferMethodPio.
    // We actually started the DMA transfer during SendCommand.
    // Sdport will call StartDmaTransfer when the SendCommand completes,
    // at which point the DMA transfer is also complete.

    if (m_dmaInProgress == SdTransferDirectionRead)
    {
        memcpy(request->Command.DataBuffer, m_pDmaDataMdl->MappedSystemVa, request->Command.Length);
    }

    m_dmaInProgress = SdTransferDirectionUndefined;
    request->Status = STATUS_SUCCESS;
    SdPortCompleteRequest(request, STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ // = DISPATCH_LEVEL
NTSTATUS CODE_SEG_TEXT
SlotExtension::StartSgDmaTransfer(SDPORT_REQUEST* request)
{
    // Similar sample code: SdhcStartAdmaTransfer

    // BRCME88C: We don't use SdTransferMethodSgDma.
    // DMA transfers are done with TransferMethod == SdTransferMethodPio.
    LogError("StartSgDmaTransfer",
        TraceLoggingUInt32(request->Command.TransferDirection, "TransferDirection"));
    return STATUS_NOT_IMPLEMENTED;
}

_Use_decl_annotations_ // <= APC_LEVEL
NTSTATUS CODE_SEG_PAGE
SlotExtension::ResetHw()
{
    // Distinction between ResetHw and ResetHost is unclear.
    // We don't do anything here - it's all in ResetHost.
    PAGED_CODE();
    LogInfo("ResetHw");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ // <= APC_LEVEL
NTSTATUS CODE_SEG_PAGE
SlotExtension::ResetHost(SDPORT_RESET_TYPE resetType)
{
    // Similar sample code: SdhcResetHost
    PAGED_CODE();

    SdRegSoftwareReset resetReg(0);
    switch (resetType)
    {
    case SdResetTypeAll:
        resetReg.ResetForAll = true;
        break;

    case SdResetTypeCmd:
        resetReg.ResetForCmdLine = true;
        break;

    case SdResetTypeDat:
        resetReg.ResetForDatLine = true;
        break;

    default:
        LogError("ResetHost-BadType",
            TraceLoggingUInt32(resetType, "ResetType"));
        return STATUS_NOT_SUPPORTED;
    }

    WRITE_REGISTER_UCHAR(&m_registers->SoftwareReset.U8, resetReg.U8);

    // Wait until the requested operation is no longer in progress.
    for (unsigned i = 0;; i += 1)
    {
        if (0 == (resetReg.U8 & READ_REGISTER_UCHAR(&m_registers->SoftwareReset.U8)))
        {
            break;
        }

        if (i == RetryMaxCount)
        {
            LogError("ResetHost-Timeout",
                TraceLoggingUInt32(resetType, "ResetType"));
            return STATUS_IO_TIMEOUT;
        }

        SdPortWait(RetryWaitMicroseconds);
    }

    WRITE_REGISTER_UCHAR(&m_registers->TimeoutControl, DataTimeoutCounterValue);

    // BRCME88C: Host's Signaling1_8 register should match voltage regulator state.
    // Note that sdport currently seems to assume that the card and the controller will
    // maintain the signaling voltage state across a ResetHw and ResetHost.
    if (m_regulatorVoltage1_8)
    {
        SdRegHostControl2 hostControl2(READ_REGISTER_USHORT(&m_registers->HostControl2.U16));
        hostControl2.Signaling1_8 = m_regulatorVoltage1_8;
        WRITE_REGISTER_USHORT(&m_registers->HostControl2.U16, hostControl2.U16);
    }

    // BRCME88C: Always use 32-bit ADMA2.
    SdRegHostControl1 hostControl1(READ_REGISTER_UCHAR(&m_registers->HostControl1.U8));
    hostControl1.DmaSelect = SdRegDma32BitAdma2;
    WRITE_REGISTER_UCHAR(&m_registers->HostControl1.U8, hostControl1.U8);

    LogInfo("ResetHost",
        TraceLoggingUInt32(resetType, "ResetType"),
        TraceLoggingHexInt8(READ_REGISTER_UCHAR(&m_registers->SoftwareReset.U8), "SoftwareReset"),
        TraceLoggingHexInt32(READ_REGISTER_ULONG(&m_registers->PresentState.U32), "PresentState"),
        TraceLoggingHexInt32(READ_REGISTER_UCHAR(&m_registers->HostControl1.U8), "HostControl1"),
        TraceLoggingHexInt32(READ_REGISTER_USHORT(&m_registers->HostControl2.U16), "HostControl2"));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ // <= APC_LEVEL
NTSTATUS CODE_SEG_PAGE
SlotExtension::SetClock(ULONG frequencyKhz)
{
    // Similar sample code: SdhcSetClock
    PAGED_CODE();

    SdRegClockControl clockControl(READ_REGISTER_USHORT(&m_registers->ClockControl.U16));

    if (frequencyKhz == 0)
    {
        // Stop clock.
        clockControl.SdClockEnable = false;
        WRITE_REGISTER_USHORT(&m_registers->ClockControl.U16, clockControl.U16);
    }
    else
    {
        // Compute new clock divisor.
        unsigned clockDivisor;
        if (m_capabilities.BaseClockFrequencyKhz <= frequencyKhz)
        {
            clockDivisor = 1;
        }
        else if (m_capabilities.SpecVersion < SdRegSpecVersion3)
        {
            // SdHost 1.0 and 2.0 can divide by powers of 2 from 2..256.
            // Could do fancy bit manipulation, but easier to just try 8 possibilities
            // and stop at the first one that meets our requirements.

            UINT8 clockShift;
            for (clockShift = 1; clockShift != 9; clockShift += 1)
            {
                if ((m_capabilities.BaseClockFrequencyKhz >> clockShift) <= frequencyKhz)
                {
                    // Found a divisor that results in actual <= target.
                    break;
                }
            }

            if (clockShift == 9)
            {
                LogError("SetClock-BadFreqV2",
                    TraceLoggingUInt32(frequencyKhz, "FrequencyKhz"));
                return STATUS_NOT_SUPPORTED;
            }

            clockDivisor = 1u << clockShift;
        }
        else
        {
            // SdHost 3.0 and later can divide by any even value 2..2046.

            clockDivisor = m_capabilities.BaseClockFrequencyKhz / frequencyKhz;
            clockDivisor &= ~1u; // Only even values.

            // The above rounded down, but we wanted to round up.
            if (m_capabilities.BaseClockFrequencyKhz > frequencyKhz * clockDivisor)
            {
                clockDivisor += 2;
            }

            if (clockDivisor > 2046)
            {
                LogError("SetClock-BadFreqV3",
                    TraceLoggingUInt32(frequencyKhz, "FrequencyKhz"));
                return STATUS_NOT_SUPPORTED;
            }
        }

        // Stop clock.
        clockControl.InternalClockEnable = false;
        clockControl.SdClockEnable = false;
        WRITE_REGISTER_USHORT(&m_registers->ClockControl.U16, clockControl.U16);

        // Set up a new Clock Control register.
        clockControl = SdRegClockControl(0);
        clockControl.FrequencySelect = (UCHAR)(clockDivisor >> 1);
        clockControl.FrequencySelectUpper = (UCHAR)(clockDivisor >> 9);

        // Start clock.
        clockControl.InternalClockEnable = true;
        WRITE_REGISTER_USHORT(&m_registers->ClockControl.U16, clockControl.U16);

        // Wait until clock is stable.
        for (unsigned i = 0;; i += 1)
        {
            clockControl.U16 = READ_REGISTER_USHORT(&m_registers->ClockControl.U16);
            if (clockControl.InternalClockStable)
            {
                break;
            }

            if (i == RetryMaxCount)
            {
                LogError("SetClock-Timeout",
                    TraceLoggingUInt32(frequencyKhz, "FrequencyKhz"));
                return STATUS_IO_TIMEOUT;
            }

            SdPortWait(RetryWaitMicroseconds);
        }

        clockControl.SdClockEnable = true;
        WRITE_REGISTER_USHORT(&m_registers->ClockControl.U16, clockControl.U16);
    }

    LogInfo("SetClock",
        TraceLoggingUInt32(frequencyKhz, "FrequencyKhz"),
        TraceLoggingHexInt16(READ_REGISTER_USHORT(&m_registers->ClockControl.U16), "ClockControl"));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ // <= APC_LEVEL
NTSTATUS CODE_SEG_PAGE
SlotExtension::SetVoltage(SDPORT_BUS_VOLTAGE voltage)
{
    // Similar sample code: SdhcSetVoltage
    PAGED_CODE();

    SdRegVoltage vdd1Voltage;
    switch (voltage)
    {
    case SdBusVoltage33:
        vdd1Voltage = SdRegVoltage3_3;
        break;

    case SdBusVoltage30:
        vdd1Voltage = SdRegVoltage3_0;
        break;

    case SdBusVoltage18:
        vdd1Voltage = SdRegVoltage1_8;
        break;

    case SdBusVoltageOff:
        vdd1Voltage = SdRegVoltageNone;
        break;

    default:
        LogError("SetVoltage-BadVoltage",
            TraceLoggingUInt32(voltage, "Voltage"));
        return STATUS_NOT_SUPPORTED;
    }

    SdRegPowerControl powerControl(READ_REGISTER_UCHAR(&m_registers->PowerControl.U8));

    if (powerControl.Vdd1Power)
    {
        // Turn off power.
        for (unsigned i = 0;; i += 1)
        {
            powerControl.Vdd1Power = false;
            WRITE_REGISTER_UCHAR(&m_registers->PowerControl.U8, powerControl.U8);

            powerControl = SdRegPowerControl(READ_REGISTER_UCHAR(&m_registers->PowerControl.U8));
            if (!powerControl.Vdd1Power)
            {
                break;
            }

            if (i == RetryMaxCount)
            {
                LogError("SetVoltage-Timeout1",
                    TraceLoggingUInt32(voltage, "Voltage"));
                return STATUS_IO_TIMEOUT;
            }

            SdPortWait(RetryWaitMicroseconds);
        }
    }

    if (voltage != SdBusVoltageOff)
    {
        // Set new voltage.
        powerControl.Vdd1Voltage = vdd1Voltage;
        WRITE_REGISTER_UCHAR(&m_registers->PowerControl.U8, powerControl.U8);

        // Turn on power at new voltage.
        for (unsigned i = 0;; i += 1)
        {
            powerControl.Vdd1Voltage = vdd1Voltage;
            powerControl.Vdd1Power = true;
            WRITE_REGISTER_UCHAR(&m_registers->PowerControl.U8, powerControl.U8);

            powerControl = SdRegPowerControl(READ_REGISTER_UCHAR(&m_registers->PowerControl.U8));
            if (powerControl.Vdd1Power &&
                powerControl.Vdd1Voltage == vdd1Voltage)
            {
                break;
            }

            if (i == RetryMaxCount)
            {
                LogError("SetVoltage-Timeout2",
                    TraceLoggingUInt32(voltage, "Voltage"));
                return STATUS_IO_TIMEOUT;
            }

            SdPortWait(RetryWaitMicroseconds);
        }
    }

    LogInfo("SetVoltage",
        TraceLoggingUInt32(voltage, "Voltage"),
        TraceLoggingHexInt8(READ_REGISTER_UCHAR(&m_registers->PowerControl.U8), "PowerControl"));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ // <= APC_LEVEL
NTSTATUS CODE_SEG_PAGE
SlotExtension::SetBusWidth(SDPORT_BUS_WIDTH busWidth)
{
    // Similar sample code: SdhcSetBusWidth
    PAGED_CODE();

    SdRegHostControl1 hostControl1(READ_REGISTER_UCHAR(&m_registers->HostControl1.U8));
    switch (busWidth)
    {
    case SdBusWidthUndefined:
    case SdBusWidth1Bit:
        hostControl1.DataTransferWidth4 = false;
        hostControl1.DataTransferWidth8 = false;
        break;
    case SdBusWidth4Bit:
        hostControl1.DataTransferWidth4 = true;
        hostControl1.DataTransferWidth8 = false;
        break;
    case SdBusWidth8Bit:
        hostControl1.DataTransferWidth4 = false;
        hostControl1.DataTransferWidth8 = true;
        break;
    default:
        LogError("SetBusWidth-BadWidth",
            TraceLoggingUInt32(busWidth, "BusWidth"));
        return STATUS_NOT_SUPPORTED;
    }

    WRITE_REGISTER_UCHAR(&m_registers->HostControl1.U8, hostControl1.U8);

    LogInfo("SetBusWidth",
        TraceLoggingUInt32(busWidth, "BusWidth"),
        TraceLoggingHexInt8(READ_REGISTER_UCHAR(&m_registers->HostControl1.U8), "HostControl1"));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ // <= APC_LEVEL
NTSTATUS CODE_SEG_PAGE
SlotExtension::SetBusSpeed(SDPORT_BUS_SPEED busSpeed)
{
    // Similar sample code: SdhcSetSpeed
    PAGED_CODE();

    SdRegHostControl1 hostControl1(READ_REGISTER_UCHAR(&m_registers->HostControl1.U8));
    SdRegHostControl2 hostControl2(READ_REGISTER_USHORT(&m_registers->HostControl2.U16));

    switch (busSpeed)
    {
    case SdBusSpeedNormal:
    case SdBusSpeedHigh:
        hostControl1.HighSpeedEnable = busSpeed == SdBusSpeedHigh;
        break;
    case SdBusSpeedSDR12:
        hostControl2.UhsModeSelect = SdRegUhsSDR12;
        break;
    case SdBusSpeedSDR25:
        hostControl2.UhsModeSelect = SdRegUhsSDR25;
        break;
    case SdBusSpeedSDR50:
        hostControl2.UhsModeSelect = SdRegUhsSDR50;
        break;
    case SdBusSpeedDDR50:
        hostControl2.UhsModeSelect = SdRegUhsDDR50;
        break;
    case SdBusSpeedSDR104:
        hostControl2.UhsModeSelect = SdRegUhsSDR104;
        break;
    default:
        LogError("SetBusSpeed-BadSpeed",
            TraceLoggingUInt32(busSpeed, "BusSpeed"));
        return STATUS_NOT_SUPPORTED;
    }

    SdRegClockControl clockControl(READ_REGISTER_USHORT(&m_registers->ClockControl.U16));
    clockControl.SdClockEnable = false;
    WRITE_REGISTER_USHORT(&m_registers->ClockControl.U16, clockControl.U16);
    SdPortWait(ClockWaitMicroseconds);

    WRITE_REGISTER_UCHAR(&m_registers->HostControl1.U8, hostControl1.U8);
    WRITE_REGISTER_USHORT(&m_registers->HostControl2.U16, hostControl2.U16);

    clockControl.SdClockEnable = true;
    WRITE_REGISTER_USHORT(&m_registers->ClockControl.U16, clockControl.U16);
    SdPortWait(ClockWaitMicroseconds);

    LogInfo("SetBusSpeed",
        TraceLoggingUInt32(busSpeed, "BusSpeed"),
        TraceLoggingHexInt8(READ_REGISTER_UCHAR(&m_registers->HostControl1.U8), "HostControl1"),
        TraceLoggingHexInt16(READ_REGISTER_USHORT(&m_registers->HostControl2.U16), "HostControl2"));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ // <= APC_LEVEL
NTSTATUS CODE_SEG_PAGE
SlotExtension::SetSignalingVoltage(SDPORT_SIGNALING_VOLTAGE signalingVoltage)
{
    // Similar sample code: SdhcSetSignaling
    PAGED_CODE();

    bool signaling1_8;

    switch (signalingVoltage)
    {
    case SdSignalingVoltage33:
        signaling1_8 = false;
        break;
    case SdSignalingVoltage18:
        signaling1_8 = true;
        break;
    default:
        LogError("SetSignalingVoltage-BadVoltage",
            TraceLoggingUInt32(signalingVoltage, "Voltage"));
        return STATUS_NOT_SUPPORTED;
    }

    SdRegClockControl clockControl(0);
    SdRegPresentState presentState(0);

    // Stop clock.
    clockControl.U16 = READ_REGISTER_USHORT(&m_registers->ClockControl.U16);
    clockControl.SdClockEnable = false;
    WRITE_REGISTER_USHORT(&m_registers->ClockControl.U16, clockControl.U16);
    SdPortWait(ClockWaitMicroseconds);

    // Verify DAT[3:0] == 0000.
    presentState.U32 = READ_REGISTER_ULONG(&m_registers->PresentState.U32);
    if (presentState.SdDatSignalLevel != 0)
    {
        LogError("SetSignalingVoltage-DatNotLow",
            TraceLoggingUInt32(signalingVoltage, "Voltage"),
            TraceLoggingHexInt32(presentState.U32, "PresentState"));
        return STATUS_UNSUCCESSFUL;
    }

    // BRCME88C: Configure voltage regulator.
    bool const oldRegulatorVoltage1_8 = m_regulatorVoltage1_8;
    if (signaling1_8 != m_regulatorVoltage1_8)
    {
        NTSTATUS status = InvokeSetRegulatorVoltage1_8(signaling1_8);
        if (!NT_SUCCESS(status))
        {
            LogError("SetSignalingVoltage-SetGpio4",
                TraceLoggingUInt32(signalingVoltage, "Voltage"),
                TraceLoggingNTStatus(status));
            return status;
        }
    }

    SdRegHostControl2 hostControl2(READ_REGISTER_USHORT(&m_registers->HostControl2.U16));

    hostControl2.Signaling1_8 = signaling1_8;
    WRITE_REGISTER_USHORT(&m_registers->HostControl2.U16, hostControl2.U16);
    SdPortWait(SignallingWaitMicroseconds);

    hostControl2.U16 = READ_REGISTER_USHORT(&m_registers->HostControl2.U16);
    if (hostControl2.Signaling1_8 != (signalingVoltage == SdSignalingVoltage18))
    {
        LogError("SetSignalingVoltage-NotLatched",
            TraceLoggingUInt32(signalingVoltage, "Voltage"),
            TraceLoggingHexInt16(hostControl2.U16, "HostControl2"));

        // BRCME88C: Restore voltage regulator to original state.
        if (oldRegulatorVoltage1_8 != m_regulatorVoltage1_8)
        {
            InvokeSetRegulatorVoltage1_8(oldRegulatorVoltage1_8);
        }

        return STATUS_UNSUCCESSFUL;
    }

    clockControl.U16 = READ_REGISTER_USHORT(&m_registers->ClockControl.U16);
    clockControl.SdClockEnable = true;
    WRITE_REGISTER_USHORT(&m_registers->ClockControl.U16, clockControl.U16);
    SdPortWait(ClockWaitMicroseconds);

    presentState.U32 = READ_REGISTER_ULONG(&m_registers->PresentState.U32);
    if (presentState.SdDatSignalLevel != 0x0F)
    {
        LogError("SetSignalingVoltage-DatNotHigh",
            TraceLoggingUInt32(signalingVoltage, "Voltage"),
            TraceLoggingHexInt32(presentState.U32, "PresentState"));

        // BRCME88C: Restore voltage regulator to original state.
        if (oldRegulatorVoltage1_8 != m_regulatorVoltage1_8)
        {
            InvokeSetRegulatorVoltage1_8(oldRegulatorVoltage1_8);
        }

        return STATUS_UNSUCCESSFUL;
    }

    LogInfo("SetSignalingVoltage",
        TraceLoggingUInt32(signalingVoltage, "Voltage"),
        TraceLoggingHexInt16(READ_REGISTER_USHORT(&m_registers->HostControl2.U16), "HostControl2"));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ // <= APC_LEVEL
NTSTATUS CODE_SEG_PAGE
SlotExtension::SetDriverType(SDPORT_DRIVER_TYPE driverType)
{
    PAGED_CODE();

    SdRegHostControl2 hostControl2(READ_REGISTER_USHORT(&m_registers->HostControl2.U16));

    switch (driverType)
    {
    case SdDriverTypeB:
        hostControl2.DriverStrength = SdRegDriverStrengthB;
        break;
    case SdDriverTypeA:
        hostControl2.DriverStrength = SdRegDriverStrengthA;
        break;
    case SdDriverTypeC:
        hostControl2.DriverStrength = SdRegDriverStrengthC;
        break;
    case SdDriverTypeD:
        hostControl2.DriverStrength = SdRegDriverStrengthD;
        break;
    default:
        LogError("SetDriverType-BadType",
            TraceLoggingUInt32(driverType, "DriverType"));
        return STATUS_NOT_SUPPORTED;
    }

    WRITE_REGISTER_USHORT(&m_registers->HostControl2.U16, hostControl2.U16);

    LogInfo("SetDriverType",
        TraceLoggingUInt32(driverType, "DriverType"),
        TraceLoggingHexInt16(READ_REGISTER_USHORT(&m_registers->HostControl2.U16), "HostControl2"));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ // <= APC_LEVEL
NTSTATUS CODE_SEG_PAGE
SlotExtension::SetPresetValue(bool enabled)
{
    PAGED_CODE();

    SdRegHostControl2 hostControl2(READ_REGISTER_USHORT(&m_registers->HostControl2.U16));
    hostControl2.PresetValueEnable = enabled;
    WRITE_REGISTER_USHORT(&m_registers->HostControl2.U16, hostControl2.U16);

    LogInfo("SetPresetValue",
        TraceLoggingBoolean(enabled, "Enabled"),
        TraceLoggingHexInt16(READ_REGISTER_USHORT(&m_registers->HostControl2.U16), "HostControl2"));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ // <= APC_LEVEL
NTSTATUS CODE_SEG_PAGE
SlotExtension::SetBlockGapInterrupt(bool enabled)
{
    // Similar sample code: SdhcEnableBlockGapInterrupt
    PAGED_CODE();

    SdRegBlockGapControl blockGapControl(READ_REGISTER_UCHAR(&m_registers->BlockGapControl.U8));
    blockGapControl.InterruptAtBlockGap = enabled;
    WRITE_REGISTER_UCHAR(&m_registers->BlockGapControl.U8, blockGapControl.U8);

    LogInfo("SetBlockGapInterrupt",
        TraceLoggingBoolean(enabled, "Enabled"),
        TraceLoggingHexInt8(READ_REGISTER_UCHAR(&m_registers->BlockGapControl.U8), "BlockGapControl"));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ // <= APC_LEVEL
NTSTATUS CODE_SEG_PAGE
SlotExtension::ExecuteTuning()
{
    PAGED_CODE();

    LogWarning("ExecuteTuning");
    return STATUS_SUCCESS; // TUNE-TODO
}
