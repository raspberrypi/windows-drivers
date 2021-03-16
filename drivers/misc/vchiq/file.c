//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//
// Module Name:
//
//      file.c
//
// Abstract:
//
//        File handle related implementation
//

#include "precomp.h"

#include "trace.h"
#include "file.tmh"

#include "slotscommon.h"
#include "device.h"
#include "file.h"
#include "slots.h"

VCHIQ_PAGED_SEGMENT_BEGIN

/*++

Routine Description:

     VchiqAllocateFileObjContext would allocated a file context.

Arguments:

     DeviceContextPtr - Pointer to device context

     WdfFileObject - A handle to a framework file object

     VchiqFileContextPPtr - File context pointer returned to caller

Return Value:

     NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqAllocateFileObjContext (
    DEVICE_CONTEXT* DeviceContextPtr,
    WDFFILEOBJECT WdfFileObject,
    VCHIQ_FILE_CONTEXT** VchiqFileContextPPtr
    )
{
    ULONG i;
    WDF_OBJECT_ATTRIBUTES wdfObjectAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
        &wdfObjectAttributes,
        VCHIQ_FILE_CONTEXT);

    PAGED_CODE();

    NTSTATUS status = WdfObjectAllocateContext(
        WdfFileObject,
        &wdfObjectAttributes,
        VchiqFileContextPPtr);
    if (!NT_SUCCESS(status)) {
        VCHIQ_LOG_ERROR(
            "WdfObjectAllocateContext() failed %!STATUS!)",
            status);
        goto End;
    }

    for (i = ARM_PORT_START; i < MAX_ARM_PORTS; i++) {
        if (InterlockedCompareExchangePointer((PVOID*)&DeviceContextPtr->ArmPortHandles[i],
            (PVOID)*VchiqFileContextPPtr,
            0) == 0) {
            // found a unused port number;
            (*VchiqFileContextPPtr)->ArmPortNumber = i;
            break;
        }
    }

    if (i >= MAX_ARM_PORTS) {
        // no more ports fail the file open call.
        status = STATUS_NO_MORE_FILES;
        goto End;
    }

    for (ULONG queueCount = 0; queueCount < FILE_QUEUE_MAX; ++queueCount) {
        WDF_IO_QUEUE_CONFIG ioQueueConfig;
        WDF_OBJECT_ATTRIBUTES ioQueueConfigAttributes;
        WDF_IO_QUEUE_CONFIG_INIT(
            &ioQueueConfig,
            WdfIoQueueDispatchManual);

        WDF_OBJECT_ATTRIBUTES_INIT(&ioQueueConfigAttributes);
        ioQueueConfigAttributes.ParentObject = WdfFileObject;
        status = WdfIoQueueCreate(
            DeviceContextPtr->Device,
            &ioQueueConfig,
            &ioQueueConfigAttributes,
            &(*VchiqFileContextPPtr)->FileQueue[queueCount]);
        if (!NT_SUCCESS(status)) {
            VCHIQ_LOG_ERROR(
                "WdfIoQueueCreate (%d) failed %!STATUS!)",
                queueCount,
                status);
            goto End;
        }
    }

    // Initialize lookaside memory for pending data message
    // and pending bulk done message
    {
        WDF_OBJECT_ATTRIBUTES_INIT(&wdfObjectAttributes);
        wdfObjectAttributes.ParentObject = WdfFileObject;
        status = WdfLookasideListCreate(
            &wdfObjectAttributes,
            sizeof(VCHIQ_PENDING_MSG),
            PagedPool,
            &wdfObjectAttributes,
            VCHIQ_ALLOC_TAG_PENDING_MSG,
            &(*VchiqFileContextPPtr)->PendingMsgLookAsideMemory);
        if (!NT_SUCCESS(status)) {
            VCHIQ_LOG_ERROR(
                "WdfLookasideListCreate failed %!STATUS!)",
                status);
            goto End;
        }

        WDF_OBJECT_ATTRIBUTES_INIT(&wdfObjectAttributes);
        wdfObjectAttributes.ParentObject = WdfFileObject;
        status = WdfLookasideListCreate(
            &wdfObjectAttributes,
            sizeof(VCHIQ_PENDING_BULK_MSG),
            PagedPool,
            &wdfObjectAttributes,
            VCHIQ_ALLOC_TAG_PENDING_BULK_MSG,
            &(*VchiqFileContextPPtr)->PendingBulkMsgLookAsideMemory);
        if (!NT_SUCCESS(status)) {
            VCHIQ_LOG_ERROR(
                "WdfLookasideListCreate failed %!STATUS!)",
                status);
            goto End;
        }
    }

    InitializeListHead(&(*VchiqFileContextPPtr)->PendingDataMsgList);
    ExInitializeFastMutex(&(*VchiqFileContextPPtr)->PendingDataMsgMutex);

    // Even though this might not be a vchi service keep all file context
    // initialization in one place for easy tracking
    InitializeListHead(&(*VchiqFileContextPPtr)->PendingVchiMsgList);
    ExInitializeFastMutex(&(*VchiqFileContextPPtr)->PendingVchiMsgMutex);

    for (ULONG bulkCount = 0; bulkCount < MSG_BULK_MAX; ++bulkCount) {
        InitializeListHead(
            &(*VchiqFileContextPPtr)->PendingBulkMsgList[bulkCount]);
        ExInitializeFastMutex(
            &(*VchiqFileContextPPtr)->PendingBulkMsgMutex[bulkCount]);
    }

    KeInitializeEvent(
        &(*VchiqFileContextPPtr)->FileEventStop,
        NotificationEvent,
        FALSE);

    // Initialize DMA adapter
    {
        ULONG numberOfMapRegister;
        DEVICE_DESCRIPTION dmaDeviceDescription = { 0 };



        dmaDeviceDescription.Version = DEVICE_DESCRIPTION_VERSION3;
        dmaDeviceDescription.Master = TRUE;
        dmaDeviceDescription.ScatterGather = TRUE;
        dmaDeviceDescription.IgnoreCount = TRUE;
        dmaDeviceDescription.DmaChannel = 1;
        dmaDeviceDescription.InterfaceType = ACPIBus;
        dmaDeviceDescription.MaximumLength = (ULONG)-1;
        dmaDeviceDescription.DmaAddressWidth = 32;

        (*VchiqFileContextPPtr)->DmaAdapterPtr = IoGetDmaAdapter(
            DeviceContextPtr->PhyDeviceObjectPtr,
            &dmaDeviceDescription,
            &numberOfMapRegister);
        if ((*VchiqFileContextPPtr)->DmaAdapterPtr == NULL) {
            status = STATUS_UNSUCCESSFUL;
            VCHIQ_LOG_ERROR("IoGetDmaAdapter failed");
            goto End;
        }
    }
End:

    return status;
}

/*++

Routine Description:

     VchiqFileClose would deallocated a file context.

Arguments:

     WdfFileObject - A handle to a framework file object

Return Value:

     VOID

--*/
_Use_decl_annotations_
VOID VchiqFileClose (
    WDFFILEOBJECT WdfFileObject
    )
{
    ULONG portNumber;
    WDFDEVICE device = WdfFileObjectGetDevice(WdfFileObject);
    DEVICE_CONTEXT* deviceContextPtr = VchiqGetDeviceContext(device);
    VCHIQ_FILE_CONTEXT* vchiqFileContextPtr =
        VchiqGetFileContext(WdfFileObject);

    PAGED_CODE();

    if (vchiqFileContextPtr == NULL) {
        goto End;
    }

    // Send a close message if service caller has done so yet.
    if (InterlockedCompareExchange(
            &vchiqFileContextPtr->State,
            SERVICE_STATE_CLOSE,
            SERVICE_STATE_CLOSE) != SERVICE_STATE_CLOSE) {

        NTSTATUS status = VchiqQueueMessageAsync(
            deviceContextPtr,
            vchiqFileContextPtr,
            VCHIQ_MAKE_MSG(
                VCHIQ_MSG_CLOSE,
                vchiqFileContextPtr->ArmPortNumber,
                vchiqFileContextPtr->VCHIQPortNumber),
            NULL,
            0); 
        if (!NT_SUCCESS(status)) {
            VCHIQ_LOG_WARNING(
                "VchiqUpdateQueueDispatchMessage failed ( %!STATUS!)",
                status);
        }
    }

    // Release the port number
    portNumber = vchiqFileContextPtr->ArmPortNumber;
	PVOID temp = deviceContextPtr->ArmPortHandles[portNumber];
    if (InterlockedCompareExchangePointer(
        (PVOID*)&deviceContextPtr->ArmPortHandles[portNumber],
        0,
        temp)
        != temp) {
        ASSERT(FALSE);
    }

    (void)KeSetEvent(&vchiqFileContextPtr->FileEventStop, 0, FALSE);

    for (ULONG queueCount = 0; queueCount < FILE_QUEUE_MAX; ++queueCount) {
        if (vchiqFileContextPtr->FileQueue[queueCount]) {
            WdfIoQueuePurgeSynchronously(
                vchiqFileContextPtr->FileQueue[queueCount]);
        }
    }

    ExAcquireFastMutex(&vchiqFileContextPtr->PendingDataMsgMutex);

    (VOID)VchiqRemovePendingMsg(
        deviceContextPtr,
        vchiqFileContextPtr,
        NULL);

    ExReleaseFastMutex(&vchiqFileContextPtr->PendingDataMsgMutex);

    for (ULONG bulkCount = 0; bulkCount < MSG_BULK_MAX; ++bulkCount) {
        NTSTATUS tempStatus;

        ExAcquireFastMutex(&vchiqFileContextPtr->PendingBulkMsgMutex[bulkCount]);

        tempStatus = VchiqRemovePendingBulkMsg(
            vchiqFileContextPtr,
            NULL,
            bulkCount,
            TRUE,
            NULL);
        if (!NT_SUCCESS(tempStatus)) {
            VCHIQ_LOG_WARNING(
                "VchiqRemovePendingBulkMsg failed %!STATUS!",
                tempStatus);
        }

        ExReleaseFastMutex(&vchiqFileContextPtr->PendingBulkMsgMutex[bulkCount]);
    }

    ExAcquireFastMutex(&vchiqFileContextPtr->PendingVchiMsgMutex);

    (VOID)VchiqRemovePendingVchiMsg(
        deviceContextPtr,
        vchiqFileContextPtr,
        NULL);

    ExReleaseFastMutex(&vchiqFileContextPtr->PendingVchiMsgMutex);

    if (vchiqFileContextPtr->DmaAdapterPtr) {
        vchiqFileContextPtr->DmaAdapterPtr->DmaOperations->PutDmaAdapter(
            vchiqFileContextPtr->DmaAdapterPtr);
        vchiqFileContextPtr->DmaAdapterPtr = NULL;
    }

End:

    return;
}

VCHIQ_PAGED_SEGMENT_END