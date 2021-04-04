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
#include "driver.h"
#include "SlotExtension.h"
#include "trace.h"

// Global variables

// BRCME88C: RPIQ driver handle for controlling regulator voltage.
static FILE_OBJECT* g_rpiqFileObject;

TRACELOGGING_DEFINE_PROVIDER(
    g_trace,
    "SDHostBRCME88C",
    // *SDHostBRCME88C = {f2782ab9-d1a5-5f95-240a-ac933a6937e2}
    (0xf2782ab9, 0xd1a5, 0x5f95, 0x24, 0x0a, 0xac, 0x93, 0x3a, 0x69, 0x37, 0xe2));

// Forward declarations to ensure the SDPORT-defined prototypes match our definitions.

extern "C" DRIVER_INITIALIZE DriverEntry;
static SDPORT_CLEANUP DriverCleanupNormal;
static SDPORT_CLEANUP DriverCleanupCrashdump;
static SDPORT_GET_SLOT_COUNT DriverGetSlotCount;
static SDPORT_PO_FX_POWER_CONTROL_CALLBACK DriverPoFxPowerControlCallback;

static SDPORT_INITIALIZE SlotInitialize;
static SDPORT_GET_SLOT_CAPABILITIES SlotGetSlotCapabilities;
static SDPORT_ISSUE_BUS_OPERATION SlotIssueBusOperation;
static SDPORT_GET_CARD_DETECT_STATE SlotGetCardDetectState;
static SDPORT_GET_WRITE_PROTECT_STATE SlotGetWriteProtectState;
static SDPORT_INTERRUPT SlotInterrupt;
static SDPORT_ISSUE_REQUEST SlotIssueRequest;
static SDPORT_GET_RESPONSE SlotGetResponse;
static SDPORT_TOGGLE_EVENTS SlotToggleEvents;
static SDPORT_CLEAR_EVENTS SlotClearEvents;
static SDPORT_REQUEST_DPC SlotRequestDpc;
static SDPORT_SAVE_CONTEXT SlotSaveContext;
static SDPORT_RESTORE_CONTEXT SlotRestoreContext;

// Driver-global functions

static
void CODE_SEG_PAGE
DriverExitNormal()
{
    PAGED_CODE();

    // This is the code that should run for normal driver exit but
    // NOT for crashdump-mode driver exit.

    if (g_rpiqFileObject != nullptr)
    {
        ObDereferenceObject(g_rpiqFileObject);
        g_rpiqFileObject = nullptr;
    }

    TraceLoggingUnregister(g_trace);
}

_Use_decl_annotations_ static // = PASSIVE_LEVEL
void CODE_SEG_PAGE
DriverCleanupNormal(
    SD_MINIPORT* miniport)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(miniport);

    LogInfo("DriverCleanup");
    DriverExitNormal();
}

_Use_decl_annotations_ static // = PASSIVE_LEVEL
void CODE_SEG_PAGE
DriverCleanupCrashdump(
    SD_MINIPORT* miniport)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(miniport);
}

_Use_decl_annotations_ extern "C" // = PASSIVE_LEVEL
NTSTATUS CODE_SEG_INIT
DriverEntry(
    DRIVER_OBJECT* driverObject,
    UNICODE_STRING* registryPath)
{
    PAGED_CODE();

    NTSTATUS status;

    // If DriverEntry is called with higher IRQL, assume crash-dump mode.
    bool const crashDumpMode = KeGetCurrentIrql() != PASSIVE_LEVEL;

    // Logging and regulator control are unavailable in crash-dump mode.
    if (!crashDumpMode)
    {
        // Activate ETW tracing.
        TraceLoggingRegister(g_trace);

        // BRCME88C: Try to access the RPIQ driver so we can control regulator voltage.
        ULONG const RpiqDesiredAccess = GENERIC_READ | GENERIC_WRITE;
        DECLARE_CONST_UNICODE_STRING(rpiqDeviceName, RPIQ_SYMBOLIC_NAME);
        OBJECT_ATTRIBUTES attributes;
        InitializeObjectAttributes(
            &attributes,
            const_cast<UNICODE_STRING*>(&rpiqDeviceName),
            OBJ_KERNEL_HANDLE,
            NULL,
            NULL);
        HANDLE rpiqHandle = nullptr;
        IO_STATUS_BLOCK statusBlock;
        status = ZwCreateFile(
            &rpiqHandle,
            RpiqDesiredAccess,
            &attributes,
            &statusBlock,
            nullptr,
            FILE_ATTRIBUTE_NORMAL,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            FILE_OPEN,
            FILE_NON_DIRECTORY_FILE,
            nullptr,
            0);
        if (!NT_SUCCESS(status))
        {
            LogWarning("DriverEntry-RPIQ-CreateFileError", TraceLoggingNTStatus(status));
        }
        else
        {
            status = ObReferenceObjectByHandle(
                rpiqHandle,
                RpiqDesiredAccess,
                *IoFileObjectType,
                KernelMode,
                (void**)&g_rpiqFileObject,
                nullptr);
            ZwClose(rpiqHandle);
            if (!NT_SUCCESS(status))
            {
                LogWarning("DriverEntry-RPIQ-ReferenceError", TraceLoggingNTStatus(status));
            }
        }
    }

    SDPORT_INITIALIZATION_DATA initializationData;
    memset(&initializationData, 0, sizeof(initializationData));

    initializationData.StructureSize = sizeof(initializationData);
    initializationData.GetSlotCount = DriverGetSlotCount;
    initializationData.GetSlotCapabilities = SlotGetSlotCapabilities;
    initializationData.Interrupt = SlotInterrupt;
    initializationData.IssueRequest = SlotIssueRequest;
    initializationData.GetResponse = SlotGetResponse;
    initializationData.RequestDpc = SlotRequestDpc;
    initializationData.ToggleEvents = SlotToggleEvents;
    initializationData.ClearEvents = SlotClearEvents;
    initializationData.SaveContext = SlotSaveContext;
    initializationData.RestoreContext = SlotRestoreContext;
    initializationData.Initialize = SlotInitialize;
    initializationData.IssueBusOperation = SlotIssueBusOperation;
    initializationData.GetCardDetectState = SlotGetCardDetectState;
    initializationData.GetWriteProtectState = SlotGetWriteProtectState;
    initializationData.PowerControlCallback = DriverPoFxPowerControlCallback;
    initializationData.Cleanup = crashDumpMode ? DriverCleanupCrashdump : DriverCleanupNormal;
    initializationData.PrivateExtensionSize = sizeof(SlotExtension);
    initializationData.CrashdumpSupported = true;

    status = SdPortInitialize(driverObject, registryPath, &initializationData);

    if (NT_SUCCESS(status))
    {
        LogInfo("DriverEntry");
    }
    else if (!crashDumpMode)
    {
        LogError("DriverEntry-SdPortInitializeError", TraceLoggingNTStatus(status));
        DriverExitNormal();
    }

    return status;
}

_Use_decl_annotations_ static // <= DISPATCH_LEVEL
NTSTATUS CODE_SEG_TEXT
DriverPoFxPowerControlCallback(
    SD_MINIPORT* miniport,
    GUID const* powerControlCode,
    void* inputBuffer,
    size_t inputBufferSize,
    void* outputBuffer,
    size_t outputBufferSize,
    size_t* bytesReturned)
{
    UNREFERENCED_PARAMETER(miniport);
    UNREFERENCED_PARAMETER(powerControlCode);
    UNREFERENCED_PARAMETER(inputBuffer);
    UNREFERENCED_PARAMETER(inputBufferSize);
    UNREFERENCED_PARAMETER(outputBuffer);
    UNREFERENCED_PARAMETER(outputBufferSize);
    UNREFERENCED_PARAMETER(bytesReturned);

    return STATUS_NOT_IMPLEMENTED;
}

_Use_decl_annotations_ static // = PASSIVE_LEVEL
NTSTATUS CODE_SEG_PAGE
DriverGetSlotCount(
    SD_MINIPORT* miniport,
    UCHAR* slotCount)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(miniport);

    // BRCME88C: Hardcoded to 1.
    *slotCount = 1;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ // <= APC_LEVEL
bool CODE_SEG_PAGE
DriverRpiqIsAvailable()
{
    PAGED_CODE();
    return g_rpiqFileObject != nullptr;
}

_Use_decl_annotations_ // <= APC_LEVEL
NTSTATUS CODE_SEG_PAGE
DriverRpiqProperty(
    _Inout_ MAILBOX_HEADER* item)
{
    PAGED_CODE();

    NTSTATUS status;

    if (g_rpiqFileObject == nullptr)
    {
        status = STATUS_NOT_FOUND;
    }
    else
    {
        KEVENT event;
        KeInitializeEvent(&event, NotificationEvent, FALSE);

        DEVICE_OBJECT* deviceObject = IoGetRelatedDeviceObject(g_rpiqFileObject);
        IO_STATUS_BLOCK statusBlock = {};
        IRP* irp = IoBuildDeviceIoControlRequest(
            IOCTL_MAILBOX_PROPERTY,
            deviceObject,
            item,
            item->TotalBuffer,
            item,
            item->TotalBuffer,
            false,
            &event,
            &statusBlock);
        if (irp == nullptr)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            auto irpLocation = IoGetNextIrpStackLocation(irp);
            irpLocation->FileObject = g_rpiqFileObject;

            statusBlock.Status = STATUS_NOT_SUPPORTED;
            status = IoCallDriver(deviceObject, irp);
            if (status == STATUS_PENDING)
            {
                KeWaitForSingleObject(&event, Executive, KernelMode, false, nullptr);
                status = statusBlock.Status;
            }
        }
    }

    return status;
}

// Per-slot functions forwarded to SlotExtension class

_Use_decl_annotations_ static // = PASSIVE_LEVEL
NTSTATUS CODE_SEG_PAGE
SlotInitialize(
    void* slotExtension,
    PHYSICAL_ADDRESS physicalBase,
    void* virtualBase,
    ULONG length,
    BOOLEAN crashdumpMode)
{
    PAGED_CODE();
    return static_cast<SlotExtension*>(slotExtension)->
        SlotInitialize(physicalBase, virtualBase, length, crashdumpMode);
}

_Use_decl_annotations_ static // = PASSIVE_LEVEL
void CODE_SEG_PAGE
SlotGetSlotCapabilities(
    void* slotExtension,
    SDPORT_CAPABILITIES* capabilities)
{
    PAGED_CODE();
    return static_cast<SlotExtension*>(slotExtension)->
        SlotGetSlotCapabilities(capabilities);
}

_Use_decl_annotations_ static // <= APC_LEVEL
NTSTATUS CODE_SEG_PAGE
SlotIssueBusOperation(
    void* slotExtension,
    SDPORT_BUS_OPERATION* busOperation)
{
    PAGED_CODE();
    return static_cast<SlotExtension*>(slotExtension)->
        SlotIssueBusOperation(*busOperation);
}

_Use_decl_annotations_ static // = PASSIVE_LEVEL
BOOLEAN CODE_SEG_PAGE
SlotGetCardDetectState(
    void* slotExtension)
{
    PAGED_CODE();
    return static_cast<SlotExtension*>(slotExtension)->
        SlotGetCardDetectState();
}

_Use_decl_annotations_ static // <= DISPATCH_LEVEL (sdport.h declaration is WRONG)
BOOLEAN CODE_SEG_TEXT
SlotGetWriteProtectState(
    void* slotExtension)
{
    return static_cast<SlotExtension*>(slotExtension)->
        SlotGetWriteProtectState();
}

_Use_decl_annotations_ static // <= HIGH_LEVEL
BOOLEAN CODE_SEG_TEXT
SlotInterrupt(
    void* slotExtension,
    ULONG* events,
    ULONG* errors,
    BOOLEAN* notifyCardChange,
    BOOLEAN* notifySdioInterrupt,
    BOOLEAN* notifyTuning)
{
    return static_cast<SlotExtension*>(slotExtension)->
        SlotInterrupt(events, errors, notifyCardChange, notifySdioInterrupt, notifyTuning);
}

_Use_decl_annotations_ static // = DISPATCH_LEVEL
NTSTATUS CODE_SEG_TEXT
SlotIssueRequest(
    void* slotExtension,
    SDPORT_REQUEST* request)
{
    return static_cast<SlotExtension*>(slotExtension)->
        SlotIssueRequest(request);
}

_Use_decl_annotations_ static // <= DISPATCH_LEVEL
void CODE_SEG_TEXT
SlotGetResponse(
    void* slotExtension,
    SDPORT_COMMAND* command,
    void* responseBuffer)
{
#pragma warning(suppress:6001) // SDPORT_GET_RESPONSE has incorrect SAL annotation.
    return static_cast<SlotExtension*>(slotExtension)->
        SlotGetResponse(*command, responseBuffer);
}

_Use_decl_annotations_ static // <= HIGH_LEVEL (?)
void CODE_SEG_TEXT
SlotToggleEvents(
    void* slotExtension,
    ULONG eventMask,
    BOOLEAN enable)
{
    return static_cast<SlotExtension*>(slotExtension)->
        SlotToggleEvents(eventMask, enable);
}

_Use_decl_annotations_ static // <= HIGH_LEVEL (?)
void CODE_SEG_TEXT
SlotClearEvents(
    void* slotExtension,
    ULONG eventMask)
{
    return static_cast<SlotExtension*>(slotExtension)->
        SlotClearEvents(eventMask);
}

_Use_decl_annotations_ static // = DISPATCH_LEVEL
void CODE_SEG_TEXT
SlotRequestDpc(
    void* slotExtension,
    SDPORT_REQUEST* request,
    ULONG events,
    ULONG errors)
{
    return static_cast<SlotExtension*>(slotExtension)->
        SlotRequestDpc(request, events, errors);
}

_Use_decl_annotations_ static // <= DISPATCH_LEVEL
void CODE_SEG_TEXT
SlotSaveContext(
    void* slotExtension)
{
    return static_cast<SlotExtension*>(slotExtension)->
        SlotSaveContext();
}

_Use_decl_annotations_ static // <= DISPATCH_LEVEL
void CODE_SEG_TEXT
SlotRestoreContext(
    void* slotExtension)
{
    return static_cast<SlotExtension*>(slotExtension)->
        SlotRestoreContext();
}
