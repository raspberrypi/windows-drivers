//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//
// Module Name:
//
//     Mailbox.c
//
// Abstract:
//
//    Mailbox Interface.
//

#include "precomp.h"

#include "trace.h"
#include "Mailbox.tmh"

#include "register.h"
#include "device.h"
#include "mailbox.h"

RPIQ_PAGED_SEGMENT_BEGIN

/*++

Routine Description:

    Initialize Mailbox.

Arguments:

    Device - A handle to a framework device object.

Return Value:

    NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS RpiqMailboxInit (
    WDFDEVICE Device
    )
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;
    DEVICE_CONTEXT *deviceContextPtr = RpiqGetContext(Device);

    PAGED_CODE();

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;

    // Serialize write to mailbox
    status = WdfWaitLockCreate(
        &attributes,
        &deviceContextPtr->WriteLock);
    if (!NT_SUCCESS(status)) {
        RPIQ_LOG_ERROR(
            "Failed to allocate lock resources for mailbox status = %!STATUS!",
            status);
        return status;
    }

    return status;
}

/*++

Routine Description:

    Write to mail box in a serialize manner

Arguments:

    DeviceContextPtr - Pointer to device context

    Channel - Mailbox Channel

    Value - Value to be written

    Request - Optional WDF request object associated with this mailbox 
        transaction

Return Value:

    NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS RpiqMailboxWrite (
    DEVICE_CONTEXT* DeviceContextPtr,
    ULONG Channel,
    ULONG Value,
    WDFREQUEST Request
    )
{
    NTSTATUS status;
    ULONG count = 0, reg;
    LARGE_INTEGER timeOut = { 0 };

    PAGED_CODE();
    
    WdfWaitLockAcquire(DeviceContextPtr->WriteLock, NULL);

    timeOut.QuadPart = WDF_REL_TIMEOUT_IN_MS(1);

    reg = READ_REGISTER_NOFENCE_ULONG(&DeviceContextPtr->Mailbox->Status);

    // Poll until mailbox is available. It doesn't seem like
    // the mailbox is full often so polling is sufficient for now
    // rather than enable mailbox empty interrupt
    while (reg & MAILBOX_STATUS_FULL) {
        if (count > MAX_POLL) {
            RPIQ_LOG_ERROR(
                "Exit Fail Status 0x%08x", 
                DeviceContextPtr->Mailbox->Status);
            status = STATUS_IO_TIMEOUT;
            goto End;
        }

        KeDelayExecutionThread(KernelMode, FALSE, &timeOut);
        reg = READ_REGISTER_NOFENCE_ULONG(&DeviceContextPtr->Mailbox->Status);
        ++count;
    }

    if (Request) {
        status = WdfRequestForwardToIoQueue(
            Request,
            DeviceContextPtr->ChannelQueue[Channel]);
        if (!NT_SUCCESS(status)) {
            RPIQ_LOG_ERROR(
                "WdfRequestForwardToIoQueue failed ( %!STATUS!)",
                status);
            goto End;
        }
    }

    WRITE_REGISTER_NOFENCE_ULONG(
        &DeviceContextPtr->Mailbox->Write, 
        (Value & ~MAILBOX_CHANNEL_MASK) | Channel);

    status = STATUS_SUCCESS;

End:
    WdfWaitLockRelease(DeviceContextPtr->WriteLock);

    return status;
}

/*++

Routine Description:

    Process mailbox property request.

Arguments:

    DeviceContextPtr - Pointer to device context

    DataInPtr - Pointer to property data

    DataSize - Data size for input and output is the expected to be the same

    Request - WDF request object associated with this mailbox
        transaction

Return Value:

    NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS RpiqMailboxProperty (
    DEVICE_CONTEXT* DeviceContextPtr,
    const VOID* DataInPtr,
    ULONG DataSize,
    ULONG Channel,
    WDFREQUEST Request
    )
{
    NTSTATUS status;
    PHYSICAL_ADDRESS highAddress;
    PHYSICAL_ADDRESS lowAddress = { 0 };
    PHYSICAL_ADDRESS boundaryAddress = { 0 };
    PHYSICAL_ADDRESS addrProperty;
    RPIQ_REQUEST_CONTEXT* requestContextPtr;

    PAGED_CODE();
    
    highAddress.QuadPart = HEX_1_G - 1;

    if (DataInPtr == NULL ||
        DataSize < sizeof(MAILBOX_HEADER)) {
        status = STATUS_INVALID_PARAMETER;
        goto End;
    }    

    {
        WDF_OBJECT_ATTRIBUTES wdfObjectAttributes;

        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
            &wdfObjectAttributes,
            RPIQ_REQUEST_CONTEXT);
        wdfObjectAttributes.EvtCleanupCallback =
            RpiqRequestContextCleanup;

        status = WdfObjectAllocateContext(
            Request,
            &wdfObjectAttributes,
            &requestContextPtr);
        if (!NT_SUCCESS(status)) {
            RPIQ_LOG_WARNING(
                "WdfObjectAllocateContext() failed %!STATUS!)",
                status);
            goto End;
        }
    }

    // Firmware expects mailbox request to be in contiguous memory
    requestContextPtr->PropertyMemory = MmAllocateContiguousNodeMemory(
        DataSize,
        lowAddress,
        highAddress,
        boundaryAddress,
        PAGE_NOCACHE | PAGE_READWRITE,
        MM_ANY_NODE_OK);
    if (requestContextPtr->PropertyMemory == NULL) {
        RPIQ_LOG_ERROR("RpiqMailboxProperty fail to allocate memory");
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto End;
    }

    requestContextPtr->PropertyMemorySize = DataSize;
    addrProperty = MmGetPhysicalAddress(requestContextPtr->PropertyMemory);

    RtlCopyMemory(requestContextPtr->PropertyMemory, DataInPtr, DataSize);
    
    status = RpiqMailboxWrite(
        DeviceContextPtr, 
        Channel,
        addrProperty.LowPart + OFFSET_DIRECT_SDRAM,
        Request);
    if (!NT_SUCCESS(status)) {
        RPIQ_LOG_ERROR("RpiqMailboxWrite failed %!STATUS!", status);
        goto End;
    }

End:
    return status;
}

RPIQ_PAGED_SEGMENT_END

RPIQ_NONPAGED_SEGMENT_BEGIN

/*++

Routine Description:

    RpiqRequestContextCleanup would perform cleanup
        when request object is delete.

Arguments:

    WdfObject - A handle to a framework object in this case
         a WDFRequest

Return Value:

    VOID

--*/
_Use_decl_annotations_
VOID RpiqRequestContextCleanup (
    WDFOBJECT WdfObject
    )
{
    RPIQ_REQUEST_CONTEXT* requestContextPtr =
        RpiqGetRequestContext(WdfObject);
    
    if (requestContextPtr->PropertyMemory) {
        MmFreeContiguousMemory(requestContextPtr->PropertyMemory);
    }
}

RPIQ_NONPAGED_SEGMENT_END