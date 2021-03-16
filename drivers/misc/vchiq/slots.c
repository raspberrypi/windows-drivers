//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//
// Module Name:
//
//      slots.c
//
// Abstract:
//
//     Vchiq slots operation implementation.
//

#include "precomp.h"

#include "trace.h"
#include "slots.tmh"

#include "slotscommon.h"
#include "device.h"
#include "file.h"
#include "memory.h"
#include "transfer.h"
#include "slots.h"

VCHIQ_PAGED_SEGMENT_BEGIN

/*++

Routine Description:

     Initialize VCHIQ, setup master and slave slot

Arguments:

     DeviceContextPtr - A pointer to the device context.

Return Value:

     NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqInit (
    DEVICE_CONTEXT* DeviceContextPtr
    )
{
    NTSTATUS status;
    OBJECT_ATTRIBUTES objectAttributes;
    ULONG slotMemorySize = (VCHIQ_DEFAULT_TOTAL_SLOTS * VCHIQ_SLOT_SIZE);
    // 2 * (cache line size) * (max fragments)
    // cache line is based on cache-line-size = <32> at bcm2835-rpi.dtsi
    ULONG fragMemorySize = 2 * 32 * VCHIQ_MAX_FRAGMENTS;
    ULONG totalMemorySize = slotMemorySize + fragMemorySize;

    PAGED_CODE();

    // Allocated slot memory
    status = VchiqAllocPhyContiguous(
        DeviceContextPtr,
        totalMemorySize,
        &DeviceContextPtr->SlotZeroPtr);
    if (!NT_SUCCESS(status)) {
        VCHIQ_LOG_ERROR("Fail to allocate slot memory");
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto End;
    }

    RtlZeroMemory(DeviceContextPtr->SlotZeroPtr, totalMemorySize);

    // Get the slot physical memory
    DeviceContextPtr->SlotMemoryPhy =
        MmGetPhysicalAddress(DeviceContextPtr->SlotZeroPtr);
    ULONG slotMemoryPhy = DeviceContextPtr->SlotMemoryPhy.LowPart
        + OFFSET_DIRECT_SDRAM;

    // Initialize slot
    ULONG memAlign =
        (VCHIQ_SLOT_SIZE - (UINT_PTR)DeviceContextPtr->SlotZeroPtr)
        & VCHIQ_SLOT_MASK;
    VCHIQ_SLOT_ZERO* slotZeroPtr = (VCHIQ_SLOT_ZERO*)
        ((UCHAR*)DeviceContextPtr->SlotZeroPtr + memAlign);
    ULONG numSlots = (totalMemorySize - memAlign) / VCHIQ_SLOT_SIZE;
    ULONG firstDataSlot = VCHIQ_SLOT_ZERO_SLOTS;

    numSlots -= firstDataSlot;

    slotZeroPtr->Magic = VCHIQ_MAGIC;
    slotZeroPtr->Version = VCHIQ_VERSION;
    slotZeroPtr->VersionMin = VCHIQ_VERSION_MIN;
    slotZeroPtr->SlotZeroSize = sizeof(VCHIQ_SLOT_ZERO);
    slotZeroPtr->SlotSize = VCHIQ_SLOT_SIZE;
    slotZeroPtr->MaxSlots = VCHIQ_MAX_SLOTS;
    slotZeroPtr->MaxSlotsPerSide = VCHIQ_MAX_SLOTS_PER_SIDE;

    slotZeroPtr->Master.SlotSync = firstDataSlot;
    slotZeroPtr->Master.SlotFirst = firstDataSlot + 1;
    slotZeroPtr->Master.SlotLast = firstDataSlot + (numSlots / 2) - 1;
    slotZeroPtr->Slave.SlotSync = firstDataSlot + (numSlots / 2);
    slotZeroPtr->Slave.SlotFirst = firstDataSlot + (numSlots / 2) + 1;
    slotZeroPtr->Slave.SlotLast = firstDataSlot + numSlots - 1;

    // Enable trigger and recycle event
    VCHIQ_ENABLE_EVENT_INTERRUPT(&slotZeroPtr->Slave.Trigger);
    VCHIQ_ENABLE_EVENT_INTERRUPT(&slotZeroPtr->Slave.Recycle);
    // Currently do not support synchronous message operation with the firmware
#ifdef SUPPORT_SYNC_OPERATION
    VCHIQ_ENABLE_EVENT_INTERRUPT(&slotZeroPtr->Slave.SyncTrigger);
    VCHIQ_ENABLE_EVENT_INTERRUPT(&slotZeroPtr->Slave.SyncRelease);
#endif

    // Now that we have setup the slave slot we can mark it as initialized
    slotZeroPtr->Slave.Initialised = 1;

    // Initialize the circular buffer slot queue
    {
        ULONG i = 0;
        for (ULONG slotCount = slotZeroPtr->Slave.SlotFirst;
        slotCount <= slotZeroPtr->Slave.SlotLast;
            ++slotCount, ++i) {
            slotZeroPtr->Slave.SlotQueue[i] = slotCount;
        }

        ULONG totalTxSlot = i - 1;
        KeInitializeSemaphore(
            &DeviceContextPtr->AvailableTxSlot,
            totalTxSlot,
            totalTxSlot);
        DeviceContextPtr->RecycleTxSlotIndex = totalTxSlot;
        slotZeroPtr->Slave.SlotQueueRecycle = totalTxSlot;

        InterlockedExchange(
            &DeviceContextPtr->AvailableTxSlotCount,
            totalTxSlot);
    }

    DeviceContextPtr->SlotZeroPtr = slotZeroPtr;

    // Setup fragment 
    slotZeroPtr->PlatformData[VCHIQ_PLATFORM_FRAGMENTS_OFFSET_IDX] =
        slotMemoryPhy + slotMemorySize;
    slotZeroPtr->PlatformData[VCHIQ_PLATFORM_FRAGMENTS_COUNT_IDX] =
        VCHIQ_MAX_FRAGMENTS;
    {
        UCHAR* fragmentBasePtr =
            ((UCHAR*)DeviceContextPtr->SlotZeroPtr + slotMemorySize);

        ULONG i;
        for (i = 0; i < (VCHIQ_MAX_FRAGMENTS - 1); ++i) {
            *(UCHAR **)&fragmentBasePtr[i * 2 * 32] =
                &fragmentBasePtr[(i + 1) * (2 * 32)];
        }
        *(char **)&fragmentBasePtr[i * (2 * 32)] = NULL;
    }

    // Initialize all the slot processing threads and locks
    ExInitializeFastMutex(&DeviceContextPtr->TxSlotMutex);
    ExInitializeFastMutex(&DeviceContextPtr->RecycleSlotMutex);

    // Initialize event and thread objects
    KeInitializeEvent(
        &DeviceContextPtr->VchiqThreadEventStop,
        NotificationEvent,
        FALSE);

    InitializeObjectAttributes(
        &objectAttributes,
        NULL,
        OBJ_KERNEL_HANDLE,
        NULL,
        NULL);

    PKSTART_ROUTINE startRoutine[] =
        {
            VchiqTriggerThreadRoutine,
            VchiqRecycleThreadRoutine,
            VchiqSyncThreadRoutine,
            VchiqSyncReleaseThreadRoutine,
        };

    for (ULONG threadCount = 0; 
         threadCount < THREAD_MAX_SUPPORTED;
         ++threadCount) {

        KeInitializeEvent(
            &DeviceContextPtr->VchiqThreadEvent[threadCount],
            SynchronizationEvent,
            FALSE);

        status = PsCreateSystemThread(
            &DeviceContextPtr->VchiqThreadHandle[threadCount],
            THREAD_ALL_ACCESS,
            &objectAttributes,
            NULL,
            NULL,
            startRoutine[threadCount],
            (VOID*)DeviceContextPtr);
        if (!NT_SUCCESS(status)) {
            VCHIQ_LOG_ERROR(
                "Failed to start PsCreateSystemThread (%d) %!STATUS!",
                threadCount,
                status);
            goto End;
        }

        status = ObReferenceObjectByHandleWithTag (
            DeviceContextPtr->VchiqThreadHandle[threadCount],
            THREAD_ALL_ACCESS,
            *PsThreadType,
            KernelMode,
            VCHIQ_ALLOC_TAG_GLOBAL_OBJ,
            &DeviceContextPtr->VchiqThreadObj[threadCount],
            NULL);
        if (!NT_SUCCESS(status)) {
            VCHIQ_LOG_ERROR(
                "ObReferenceObjectByHandle (%d) failed %!STATUS!",
                threadCount,
                status);
            goto End;
        }

        ZwClose(DeviceContextPtr->VchiqThreadHandle[threadCount]);
        DeviceContextPtr->VchiqThreadHandle[threadCount] = NULL;
    }

End:
    return status;
}

/*++

Routine Description:

     Release VCHIQ related resource.

Arguments:

     DeviceContextPtr - A pointer to the device context.

Return Value:

     NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqRelease (
    DEVICE_CONTEXT* DeviceContextPtr
    )
{
    PAGED_CODE();

    VchiqFreePhyContiguous(
        DeviceContextPtr,
        &DeviceContextPtr->SlotZeroPtr);

    for (ULONG threadCount = 0;
         threadCount < THREAD_MAX_SUPPORTED;
         ++threadCount) {

        if (DeviceContextPtr->VchiqThreadObj[threadCount]) {
            NTSTATUS status;
            LARGE_INTEGER timeout;

            (void)KeSetEvent(&DeviceContextPtr->VchiqThreadEventStop, 0, FALSE);

            timeout.QuadPart = WDF_REL_TIMEOUT_IN_MS(1000);

            status = KeWaitForSingleObject(
                DeviceContextPtr->VchiqThreadObj[threadCount],
                Executive,
                KernelMode,
                FALSE, 
                &timeout);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "KeWaitForSingleObject for thread (%d) failed %!STATUS!",
                    threadCount,
                    status);
            }

            ObDereferenceObject(DeviceContextPtr->VchiqThreadObj[threadCount]);
            DeviceContextPtr->VchiqThreadObj[threadCount] = NULL;
        }

        if (DeviceContextPtr->VchiqThreadHandle[threadCount]) {
            ZwClose(DeviceContextPtr->VchiqThreadHandle[threadCount]);
            DeviceContextPtr->VchiqThreadHandle[threadCount] = NULL;
        }
    }

    return STATUS_SUCCESS;
}

/*++

Routine Description:

     Signals VC that there is a pending slot to be processed.

Arguments:

     DeviceContextPtr - A pointer to the device context.

     EventPtr - Pointer to the event to be signalled

Return Value:

     NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqSignalVC (
    DEVICE_CONTEXT* DeviceContextPtr,
    VCHIQ_REMOTE_EVENT* EventPtr
    )
{
    PAGED_CODE();

    // Indicate that we to VC side that the event has been triggered
    EventPtr->Fired = 1;

    if (EventPtr->Armed) {
        WRITE_REGISTER_NOFENCE_ULONG(
            (ULONG*)(DeviceContextPtr->VchiqRegisterPtr + BELL2), 0);
    }

    return STATUS_SUCCESS;
}


/*++

Routine Description:

     Acquire next location to insert a new message. Depending on size
     and current location a new slot maybe used. This function should
     be called within TxSlotMutex.

Arguments:

     DeviceContextPtr - A pointer to the device context.

     VchiqFileContextPtr - File context pointer returned to caller

     RequestSize - Message Suze

     SyncAcquire - Determine if the message would be sent synchronously

     HeaderPPtr - Pointer that would assigned to the next available slot
          location within the transfer slot.

Return Value:

     NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqAcquireTxSpace (
    DEVICE_CONTEXT* DeviceContextPtr,
    VCHIQ_FILE_CONTEXT* VchiqFileContextPtr,
    ULONG RequestSize,
    BOOLEAN SyncAcquire,
    VCHIQ_HEADER** HeaderPPtr
    )
{
    ULONG availSlotSpace;
    ULONG actualBufferSize;
    NTSTATUS status;

    PAGED_CODE();

    actualBufferSize = VCHIQ_GET_SLOT_ALIGN_SIZE(RequestSize);

    availSlotSpace =
        VCHIQ_SLOT_SIZE - (DeviceContextPtr->CurrentTxPos & VCHIQ_SLOT_MASK);

    // Slot messages are expected to be 8 byte align so there should always be
    // enough space to fit a header information
    NT_ASSERT(availSlotSpace >= sizeof(VCHIQ_HEADER));

    // If the current slot does not have sufficient space fill it up with 
    // padding and proceed to the next slot
    if (actualBufferSize > availSlotSpace) {
        VCHIQ_HEADER* tempHeaderPtr =
            VCHIQ_GET_CURRENT_TX_HEADER(DeviceContextPtr);

        tempHeaderPtr->MsgId = VCHIQ_MSGID_PADDING;
        tempHeaderPtr->Size = availSlotSpace - sizeof(VCHIQ_HEADER);
        DeviceContextPtr->CurrentTxPos += availSlotSpace;
    }

    // Process the next available slot if previous slot is all fill up
    if ((DeviceContextPtr->CurrentTxPos & VCHIQ_SLOT_MASK) == 0) {
        LARGE_INTEGER waitAvailableTxSlotTimeout;
        waitAvailableTxSlotTimeout.QuadPart = -1 * 1000000;

        status = VchiqWaitForEvents(
            (VOID*)&DeviceContextPtr->AvailableTxSlot,
            &VchiqFileContextPtr->FileEventStop,
            (SyncAcquire) ? NULL : &waitAvailableTxSlotTimeout);

        switch (status)
        {
        case STATUS_TIMEOUT:
            *HeaderPPtr = NULL;
            status = STATUS_INSUFFICIENT_RESOURCES;
            VCHIQ_LOG_WARNING(
                "No slot available size  %d. Slot count %d",
                RequestSize,
                DeviceContextPtr->AvailableTxSlotCount);
            goto End;
        case STATUS_WAIT_1:
            *HeaderPPtr = NULL;
            status = STATUS_UNSUCCESSFUL;
            VCHIQ_LOG_WARNING(
                "File handle not active anymore %d",
                RequestSize);
            goto End;
        }

        InterlockedDecrement(&DeviceContextPtr->AvailableTxSlotCount);

        ULONG slotIndex = VCHIQ_GET_NEXT_TX_SLOT_INDEX(
            DeviceContextPtr);

        DeviceContextPtr->SlaveCurrentSlot =
            (UCHAR*)VCHIQ_GET_HEADER_BY_GLOBAL_INDEX(
                DeviceContextPtr,
                slotIndex);
    }

    *HeaderPPtr = VCHIQ_GET_CURRENT_TX_HEADER(DeviceContextPtr);
    DeviceContextPtr->CurrentTxPos += actualBufferSize;
    status = STATUS_SUCCESS;

End:

    return status;

}

/*++

Routine Description:

     Process the slot when VC fires a trigger interrupt

Arguments:

     DeviceContextPtr - A pointer to the device context.

Return Value:

     NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqProcessRxSlot (
    DEVICE_CONTEXT* DeviceContextPtr
    )
{
    NTSTATUS status;
    VCHIQ_SLOT_ZERO* slotZeroPtr;
    VCHIQ_FILE_CONTEXT* vchiqFileContextPtr;

    PAGED_CODE();
    
    slotZeroPtr = DeviceContextPtr->SlotZeroPtr;

    // Attempt to parse updated received messages
    while (DeviceContextPtr->CurrentRxPos < slotZeroPtr->Master.TxPos) {
        if (DeviceContextPtr->MasterCurrentSlot == NULL) {
            DeviceContextPtr->MasterCurrentSlotIndex =
                VCHIQ_GET_NEXT_RX_SLOT_INDEX(
                    DeviceContextPtr);
            DeviceContextPtr->MasterCurrentSlot =
                (UCHAR*)VCHIQ_GET_HEADER_BY_GLOBAL_INDEX(
                    DeviceContextPtr,
                    DeviceContextPtr->MasterCurrentSlotIndex);
            SLOT_INFO* currentSlotPtr = &DeviceContextPtr->RxSlotInfo[
                DeviceContextPtr->MasterCurrentSlotIndex];
            currentSlotPtr->SlotInUse = TRUE;
        }
        VCHIQ_HEADER* rxHeader =
            VCHIQ_GET_CURRENT_RX_HEADER(DeviceContextPtr);
        ULONG armPortNum = VCHIQ_MSG_DSTPORT(rxHeader->MsgId);
        vchiqFileContextPtr = DeviceContextPtr->ArmPortHandles[armPortNum];

        switch (VCHIQ_MSG_TYPE(rxHeader->MsgId))
        {
        case VCHIQ_MSG_OPEN:
            {
                VCHIQ_LOG_WARNING(
                    "Unsupported message 0x%08x (%S) size 0x%08x",
                    rxHeader->MsgId,
                    VCHIQ_MESSAGE_NAME(rxHeader->MsgId),
                    rxHeader->Size);
            }
            break;
        case VCHIQ_MSG_OPENACK:
            {
                if (vchiqFileContextPtr == NULL) {
                    VCHIQ_LOG_ERROR(
                        "Unknown VCHIQ_MSG_OPENACK 0x%08x (%S) size 0x%08x",
                        rxHeader->MsgId,
                        VCHIQ_MESSAGE_NAME(rxHeader->MsgId),
                        rxHeader->Size);
                    break;
                }
                vchiqFileContextPtr->VCHIQPortNumber =
                    VCHIQ_MSG_SRCPORT(rxHeader->MsgId);

                {
                    WDFREQUEST nextRequest;
                    status = WdfIoQueueRetrieveNextRequest(
                        vchiqFileContextPtr->FileQueue[FILE_QUEUE_CREATE_SERVICE],
                        &nextRequest);
                    if (!NT_SUCCESS(status)) {
                        VCHIQ_LOG_WARNING(
                            "WdfIoQueueRetrieveNextRequest failed  %!STATUS!",
                            status);
                        break;
                    }
                    
                    InterlockedExchange(
                        &vchiqFileContextPtr->State,
                        SERVICE_STATE_OPEN);

                    WdfRequestComplete(nextRequest, STATUS_SUCCESS);
                }
            }
            break;
        case VCHIQ_MSG_CLOSE:
            {
                if (vchiqFileContextPtr == NULL) {
                    VCHIQ_LOG_WARNING(
                        "Unknown VCHIQ_MSG_CLOSE 0x%08x (%S) size 0x%08x",
                        rxHeader->MsgId,
                        VCHIQ_MESSAGE_NAME(rxHeader->MsgId),
                        rxHeader->Size);
                    break;
                }

                WDFREQUEST nextRequest;
                status = WdfIoQueueRetrieveNextRequest(
                    vchiqFileContextPtr->FileQueue[FILE_QUEUE_CLOSE_SERVICE],
                    &nextRequest);
                if (!NT_SUCCESS(status)) {
                    VCHIQ_LOG_WARNING(
                        "WdfIoQueueRetrieveNextRequest failed  %!STATUS!",
                        status);
                } else {

                    InterlockedExchange(
                        &vchiqFileContextPtr->State,
                        SERVICE_STATE_CLOSE);

                    WdfRequestComplete(nextRequest, STATUS_SUCCESS);
                }

            }
            break;
        case VCHIQ_MSG_DATA:
            {
                if (vchiqFileContextPtr == NULL) {
                    VCHIQ_LOG_ERROR(
                        "Unknown VCHIQ_MSG_DATA 0x%08x (%S) size 0x%08x",
                        rxHeader->MsgId,
                        VCHIQ_MESSAGE_NAME(rxHeader->MsgId),
                        rxHeader->Size);
                    break;
                }

                // Ignore zero length data
                if (rxHeader->Size == 0) {
                    break;
                }

                status = VchiqProcessNewRxMsg(
                    DeviceContextPtr,
                    vchiqFileContextPtr,
                    rxHeader);
                if (!NT_SUCCESS(status)) {
                    VCHIQ_LOG_ERROR(
                        "VchiqProcessNewRxMsg failed  %!STATUS!",
                        status);
                    break;
                }
            }
            break;
        case VCHIQ_MSG_CONNECT:
            {
                DeviceContextPtr->VCConnected = TRUE;

                // Now that the we are connected with the firmware, go ahead 
                // and enable the device interface if it isnt already enabled
                if (DeviceContextPtr->DeviceInterfaceEnabled == FALSE) {

                    status = WdfDeviceCreateDeviceInterface(
                        DeviceContextPtr->Device,
                        &VCHIQ_INTERFACE_GUID,
                        NULL);
                    if (!NT_SUCCESS(status)) {
                        VCHIQ_LOG_ERROR(
                            "Fail to register device interface %!STATUS!",
                            status);
                        break;
                    }

                    WdfDeviceSetDeviceInterfaceState(
                        DeviceContextPtr->Device,
                        &VCHIQ_INTERFACE_GUID,
                        NULL,
                        TRUE);

                    DeviceContextPtr->DeviceInterfaceEnabled = TRUE;
                }
            }
            break;
        case VCHIQ_MSG_BULK_RX:
            {
                VCHIQ_LOG_WARNING(
                    "Unsupported message 0x%08x (%S) size 0x%08x",
                    rxHeader->MsgId,
                    VCHIQ_MESSAGE_NAME(rxHeader->MsgId),
                    rxHeader->Size);
            }
            break;
        case VCHIQ_MSG_BULK_TX:
            {
                VCHIQ_LOG_WARNING(
                    "Unsupported message 0x%08x (%S) size 0x%08x",
                    rxHeader->MsgId,
                    VCHIQ_MESSAGE_NAME(rxHeader->MsgId),
                    rxHeader->Size);
            }
            break;
        case VCHIQ_MSG_BULK_RX_DONE:
            {
                if (vchiqFileContextPtr == NULL) {
                    VCHIQ_LOG_ERROR(
                        "Unknown VCHIQ_MSG_BULK_RX_DONE 0x%08x (%S) size 0x%08x",
                        rxHeader->MsgId,
                        VCHIQ_MESSAGE_NAME(rxHeader->MsgId),
                        rxHeader->Size);
                    break;
                }

                {
                    WDFREQUEST nextRequest;
                    status = WdfIoQueueRetrieveNextRequest(
                        vchiqFileContextPtr->FileQueue[FILE_QUEUE_RX_DATA],
                        &nextRequest);
                    if (!NT_SUCCESS(status)) {
                        VCHIQ_LOG_WARNING(
                            "WdfIoQueueRetrieveNextRequest failed  %!STATUS!",
                            status);
                        break;
                    }

                    ULONG* respondMsg = (ULONG*)(rxHeader + 1);
                    if (*respondMsg == 0xFFFFFFFF) {
                        WdfRequestComplete(nextRequest, STATUS_UNSUCCESSFUL);
                    } else {

                        VCHIQ_TX_REQUEST_CONTEXT* vchiqTxRequestContextPtr =
                            VchiqGetTxRequestContext(nextRequest);
                        if (vchiqTxRequestContextPtr != NULL) {

                            DMA_ADAPTER* dmaAdapterPtr =
                                vchiqFileContextPtr->DmaAdapterPtr;

                            dmaAdapterPtr->DmaOperations->FreeAdapterObject(
                                vchiqFileContextPtr->DmaAdapterPtr,
                                DeallocateObjectKeepRegisters);

                            dmaAdapterPtr->DmaOperations->PutScatterGatherList(
                                vchiqFileContextPtr->DmaAdapterPtr,
                                vchiqTxRequestContextPtr->ScatterGatherListPtr,
                                FALSE);

                            vchiqTxRequestContextPtr->ScatterGatherListPtr = NULL;

                            WdfRequestCompleteWithInformation(
                                nextRequest,
                                STATUS_SUCCESS,
                                MmGetMdlByteCount(vchiqTxRequestContextPtr->BufferMdlPtr));
                        } else {
                            WdfRequestComplete(nextRequest, STATUS_UNSUCCESSFUL);
                        }
                    }
                }

                status = VchiqProcessNewRxMsg(
                    DeviceContextPtr,
                    vchiqFileContextPtr,
                    rxHeader);
                if (!NT_SUCCESS(status)) {
                    VCHIQ_LOG_ERROR(
                        "VchiqProcessNewRxMsg failed  %!STATUS!",
                        status);
                    break;
                }
            }
            break;
        case VCHIQ_MSG_BULK_TX_DONE:
            {
                if (vchiqFileContextPtr == NULL) {
                    VCHIQ_LOG_ERROR(
                        "Unknown VCHIQ_MSG_BULK_TX_DONE 0x%08x (%S) size 0x%08x",
                        rxHeader->MsgId,
                        VCHIQ_MESSAGE_NAME(rxHeader->MsgId),
                        rxHeader->Size);
                    break;
                }

                {
                    WDFREQUEST nextRequest;
                    status = WdfIoQueueRetrieveNextRequest(
                        vchiqFileContextPtr->FileQueue[FILE_QUEUE_TX_DATA],
                        &nextRequest);
                    if (!NT_SUCCESS(status)) {
                        VCHIQ_LOG_WARNING(
                            "WdfIoQueueRetrieveNextRequest failed  %!STATUS!",
                            status);
                        break;
                    }

                    ULONG* respondMsg = (ULONG*)(rxHeader + 1);
                    if (*respondMsg == 0xFFFFFFFF) {
                        WdfRequestComplete(nextRequest, STATUS_UNSUCCESSFUL);
                    } else {

                        VCHIQ_TX_REQUEST_CONTEXT* vchiqTxRequestContextPtr =
                            VchiqGetTxRequestContext(nextRequest);
                        if (vchiqTxRequestContextPtr != NULL) {

                            DMA_ADAPTER* dmaAdapterPtr =
                                vchiqFileContextPtr->DmaAdapterPtr;

                            dmaAdapterPtr->DmaOperations->FreeAdapterObject(
                                vchiqFileContextPtr->DmaAdapterPtr,
                                DeallocateObjectKeepRegisters);

                            dmaAdapterPtr->DmaOperations->PutScatterGatherList(
                                vchiqFileContextPtr->DmaAdapterPtr,
                                vchiqTxRequestContextPtr->ScatterGatherListPtr,
                                TRUE);

                            vchiqTxRequestContextPtr->ScatterGatherListPtr = NULL;

                            WdfRequestCompleteWithInformation(
                                nextRequest,
                                STATUS_SUCCESS,
                                MmGetMdlByteCount(vchiqTxRequestContextPtr->BufferMdlPtr));
                        } else {
                            WdfRequestComplete(nextRequest, STATUS_UNSUCCESSFUL);
                        }
                    }
                }

                status = VchiqProcessNewRxMsg(
                    DeviceContextPtr,
                    vchiqFileContextPtr,
                    rxHeader);
                if (!NT_SUCCESS(status)) {
                    VCHIQ_LOG_ERROR(
                        "VchiqProcessNewRxMsg failed  %!STATUS!",
                        status);
                    break;
                }
            }
            break;
        case VCHIQ_MSG_PADDING:
            break;
        case VCHIQ_MSG_PAUSE:
            break;
        case VCHIQ_MSG_RESUME:
            break;
        case VCHIQ_MSG_REMOTE_USE:
            break;
        case VCHIQ_MSG_REMOTE_RELEASE:
            break;
        case VCHIQ_MSG_REMOTE_USE_ACTIVE:
            break;
        default:
            VCHIQ_LOG_WARNING(
                "Invalid RX message 0x%08x (%S) size 0x%08x",
                rxHeader->MsgId,
                VCHIQ_MESSAGE_NAME(rxHeader->MsgId),
                rxHeader->Size);
        }

        VCHIQ_LOG_INFORMATION(
            "Process RX message 0x%08x (%S) size 0x%08x",
            rxHeader->MsgId,
            VCHIQ_MESSAGE_NAME(rxHeader->MsgId),
            rxHeader->Size);

        DeviceContextPtr->CurrentRxPos +=
            VCHIQ_GET_SLOT_ALIGN_SIZE(rxHeader->Size + sizeof(VCHIQ_HEADER));

        // Attempt to release the slot once we process the last message
        if ((DeviceContextPtr->CurrentRxPos & VCHIQ_SLOT_MASK) == 0) {
            ULONG slotNumber = DeviceContextPtr->MasterCurrentSlotIndex;
            VchiqRecycleSlot(
                DeviceContextPtr,
                slotZeroPtr,
                slotNumber,
                TRUE);
            DeviceContextPtr->MasterCurrentSlot = NULL;
        }

        VCHIQ_RESET_EVENT_SIGNAL(&slotZeroPtr->Slave.Trigger);
    }

    VCHIQ_ENABLE_EVENT_INTERRUPT(&slotZeroPtr->Slave.Trigger);

    return STATUS_SUCCESS;
}

/*++

Routine Description:

     Process queue that is freed by VC gpu. A recycle interrupt would be
     generated when a queue becomes available

Arguments:

     DeviceContextPtr - A pointer to the device context.

Return Value:

     NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqProcessRecycleTxSlot (
    DEVICE_CONTEXT* DeviceContextPtr
    )
{
    VCHIQ_SLOT_ZERO* slotZeroPtr;
    ULONG currentAvailableSlot;

    PAGED_CODE();
    
    currentAvailableSlot = DeviceContextPtr->RecycleTxSlotIndex;
    slotZeroPtr = DeviceContextPtr->SlotZeroPtr;

    // Make available slots that is being recycle. We keep a local counter
    // so we can figure the total slot to be recycled
    while (currentAvailableSlot != slotZeroPtr->Slave.SlotQueueRecycle) {

        // If required slot quota update can be implemented here
        ULONG semaphoreSignal = KeReleaseSemaphore(
            &DeviceContextPtr->AvailableTxSlot,
            0,
            1,
            FALSE);
        if (!semaphoreSignal) {
            VCHIQ_LOG_INFORMATION("Tx slot now available");
        }
        ++currentAvailableSlot;
        InterlockedIncrement(&DeviceContextPtr->AvailableTxSlotCount);

        VCHIQ_RESET_EVENT_SIGNAL(&slotZeroPtr->Slave.Recycle);
    }

    DeviceContextPtr->RecycleTxSlotIndex = currentAvailableSlot;

    VCHIQ_ENABLE_EVENT_INTERRUPT(&slotZeroPtr->Slave.Recycle);

    return STATUS_SUCCESS;
}

/*++

Routine Description:

     Attempt to send message to VC asynchronously

Arguments:

     DeviceContextPtr - A pointer to the device context.

     VchiqFileContextPtr - File context pointer returned to caller

     MessageId - Slot message id

     BufferPtr - Pointer to message that would be dispatch to VC GPU

     BufferSize - The buffer size of BufferPtr

Return Value:

     NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqQueueMessageAsync (
    DEVICE_CONTEXT* DeviceContextPtr,
    VCHIQ_FILE_CONTEXT* VchiqFileContextPtr,
    ULONG MessageId,
    VOID* BufferPtr,
    ULONG BufferSize
    )
{
    NTSTATUS status;
    VCHIQ_HEADER* msgHeaderPtr;

    PAGED_CODE();

    ExAcquireFastMutex(&DeviceContextPtr->TxSlotMutex);

    status = VchiqAcquireTxSpace(
        DeviceContextPtr,
        VchiqFileContextPtr,
        sizeof(*msgHeaderPtr) + BufferSize,
        FALSE,
        &msgHeaderPtr);
    if (!NT_SUCCESS(status)) {
        VCHIQ_LOG_ERROR(
            "Fail to acquire a transfer slot %!STATUS!",
            status);
        goto End;
    }

    msgHeaderPtr->MsgId = MessageId;
    msgHeaderPtr->Size = BufferSize;

    VCHIQ_LOG_INFORMATION(
        "Queue message id 0x%08x (%S) size 0x%08x",
        msgHeaderPtr->MsgId,
        VCHIQ_MESSAGE_NAME(msgHeaderPtr->MsgId),
        msgHeaderPtr->Size);

    if (BufferPtr != NULL && BufferSize != 0) {
        ++msgHeaderPtr;
        RtlCopyMemory(msgHeaderPtr, BufferPtr, BufferSize);
    }

    // Safe to release mutex once we copied all the data over
    ExReleaseFastMutex(&DeviceContextPtr->TxSlotMutex);

    // Update transfer position and signal VC
    {
        VCHIQ_SLOT_ZERO* slotZeroPtr = DeviceContextPtr->SlotZeroPtr;
        slotZeroPtr->Slave.TxPos = DeviceContextPtr->CurrentTxPos;

        status = VchiqSignalVC(
            DeviceContextPtr,
            &slotZeroPtr->Master.Trigger);
        if (!NT_SUCCESS(status)) {
            VCHIQ_LOG_ERROR(
                "Fail to signal VC %!STATUS!",
                status);
        }
    }

    return status;

End:
    ExReleaseFastMutex(&DeviceContextPtr->TxSlotMutex);

    return status;
}

/*++

Routine Description:

     Attempt to send multi element messate to VC asynchronously

Arguments:

     DeviceContextPtr - A pointer to the device context.

     VchiqFileContextPtr - File context pointer returned to caller

     MessageId - Slot message id

     ElementsPtr - Pointer to the multi element that would be
        dispatch to VC GPU

     Count - Total element to be sent

Return Value:

     NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqQueueMultiElementAsync (
    DEVICE_CONTEXT* DeviceContextPtr,
    VCHIQ_FILE_CONTEXT* VchiqFileContextPtr,
    ULONG MessageId,
    VCHIQ_ELEMENT* ElementsPtr,
    ULONG Count
    )
{
    NTSTATUS status;
    ULONG totalMsgSize = 0;
    VCHIQ_HEADER* msgHeaderPtr;

    PAGED_CODE();

    for (ULONG elementIndex = 0; elementIndex < Count; ++elementIndex) {
        if (ElementsPtr[elementIndex].Size) {
            ElementsPtr[elementIndex].Data = WdfMemoryGetBuffer(
                ElementsPtr[elementIndex].WdfMemoryData,
                NULL);
            if (ElementsPtr[elementIndex].Data == NULL) {
                VCHIQ_LOG_ERROR("Invalid element data pointer");
                status = STATUS_INVALID_PARAMETER;
                goto EndNoMutexLock;
            }
            totalMsgSize += ElementsPtr[elementIndex].Size;
        }
    }

    ExAcquireFastMutex(&DeviceContextPtr->TxSlotMutex);

    status = VchiqAcquireTxSpace(
        DeviceContextPtr,
        VchiqFileContextPtr,
        sizeof(*msgHeaderPtr) + totalMsgSize,
        FALSE,
        &msgHeaderPtr);
    if (!NT_SUCCESS(status)) {
        VCHIQ_LOG_ERROR(
            "Fail to acquire a transfer slot %!STATUS!",
            status);
        goto End;
    }

    msgHeaderPtr->MsgId = MessageId;
    msgHeaderPtr->Size = totalMsgSize;

    VCHIQ_LOG_INFORMATION(
        "Queue message id 0x%08x (%S) size 0x%08x",
        msgHeaderPtr->MsgId,
        VCHIQ_MESSAGE_NAME(msgHeaderPtr->MsgId),
        msgHeaderPtr->Size);

    ++msgHeaderPtr;

    for (ULONG elementIndex = 0; elementIndex < Count; ++elementIndex) {
        if (ElementsPtr[elementIndex].Size) {
            RtlCopyMemory(
                msgHeaderPtr,
                ElementsPtr[elementIndex].Data,
                ElementsPtr[elementIndex].Size);
            msgHeaderPtr += ElementsPtr[elementIndex].Size;
        }
    }

    // Safe to release mutex once we copied all the data over
    ExReleaseFastMutex(&DeviceContextPtr->TxSlotMutex);

    // Update transfer position and signal VC
    {
        VCHIQ_SLOT_ZERO* slotZeroPtr = DeviceContextPtr->SlotZeroPtr;
        slotZeroPtr->Slave.TxPos = DeviceContextPtr->CurrentTxPos;

        status = VchiqSignalVC(
            DeviceContextPtr,
            &slotZeroPtr->Master.Trigger);
        if (!NT_SUCCESS(status)) {
            VCHIQ_LOG_ERROR(
                "Fail to signal VC %!STATUS!",
                status);
        }
    }

EndNoMutexLock:
    return status;

End:
    ExReleaseFastMutex(&DeviceContextPtr->TxSlotMutex);

    return status;
}

/*++

Routine Description:

    VchiqProcessBulkTransfer would setup the necassary intermediate
       state and memory and proceeds to perform the bulk transaction

Arguments:

    DeviceContextPtr - A pointer to the device context.

    VchiqFileContextPtr - File context pointer returned to caller

    WdfRequest - Request framework object tied to this bulk transfer

    BulkTransferPtr - Pointer to the current bulk transfer info

    MsgDirection - Specify the direction of the bulk transfer

    BufferMdl - Mdl pointer structer of the buffer

    BufferSize - Size of data that would be transfered

Return Value:

     NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqProcessBulkTransfer (
    DEVICE_CONTEXT* DeviceContextPtr,
    VCHIQ_FILE_CONTEXT* VchiqFileContextPtr,
    WDFREQUEST WdfRequest,
    VCHIQ_QUEUE_BULK_TRANSFER* BulkTransferPtr,
    ULONG MsgDirection,
    MDL* BufferMdl,
    ULONG BufferSize
    )
{
    NTSTATUS status;
    MSG_BULK_TYPE bulkType =
        (MsgDirection == VCHIQ_MSG_BULK_TX) ?
        MSG_BULK_TX : MSG_BULK_RX;
    ULONG transactionType =
        (MsgDirection == VCHIQ_MSG_BULK_TX) ?
        FILE_QUEUE_TX_DATA : FILE_QUEUE_RX_DATA;

    PAGED_CODE();

    // Acquire a mutex here so we can serialize tracking of bulk transfer.
    // On the firmware side it is gurantee to process all bulk in a serialize
    // FIFO fashion. As long as we track the order correctly here we would not
    // be out of sync.
    ExAcquireFastMutex(&VchiqFileContextPtr->PendingBulkMsgMutex[bulkType]);

    status = VchiqAddPendingBulkMsg(
        VchiqFileContextPtr,
        BulkTransferPtr,
        (MsgDirection == VCHIQ_MSG_BULK_TX) ?
            MSG_BULK_TX : MSG_BULK_RX);
    if (!NT_SUCCESS(status)) {
        VCHIQ_LOG_ERROR(
            "VchiqAddPendingBulkMsg failed (%!STATUS!)",
            status);
        goto End;
    }

    // Request needs to remain valid until memory has been succesfully DMA
    // over to or from the firmware. Forward to a queue to be completed later
    // so memory remains lock in physical memory. Premature completing the
    // request could result in corruption (ie:jpeg decode).
    status = WdfRequestForwardToIoQueue(
        WdfRequest,
        VchiqFileContextPtr->FileQueue[transactionType]);
    if (!NT_SUCCESS(status)) {
        VCHIQ_LOG_ERROR(
            "WdfRequestForwardToIoQueue failed (%!STATUS!)",
            status);
        NTSTATUS tempStatus;
        tempStatus = VchiqRemovePendingBulkMsg(
            VchiqFileContextPtr,
            NULL,
            bulkType,
            FALSE,
            NULL);
        if (!NT_SUCCESS(tempStatus)) {
            VCHIQ_LOG_ERROR(
                "VchiqRemovePendingBulkMsg failed (%!STATUS!)",
                tempStatus);
        }
        NT_ASSERT(NT_SUCCESS(tempStatus));
        goto End;
    }

    status = VchiqBulkTransfer(
        DeviceContextPtr,
        VchiqFileContextPtr,
        WdfRequest,
        MsgDirection,
        BufferMdl,
        BufferSize,
        VchiqFileContextPtr->ArmPortNumber,
        VchiqFileContextPtr->VCHIQPortNumber);
    if (!NT_SUCCESS(status)) {
        NTSTATUS tempStatus;
        WDFREQUEST removeRequest;

        VCHIQ_LOG_ERROR(
            "VchiqBulkTransfer failed (%!STATUS!)",
            status);

        tempStatus = VchiqRemovePendingBulkMsg(
            VchiqFileContextPtr,
            NULL,
            bulkType,
            FALSE,
            NULL);
        if (!NT_SUCCESS(tempStatus)) {
            VCHIQ_LOG_ERROR(
                "VchiqRemovePendingBulkMsg failed (%!STATUS!)",
                tempStatus);
            NT_ASSERT(NT_SUCCESS(tempStatus));
        }

        // Remove the request that was just inserted
        tempStatus = WdfIoQueueRetrieveFoundRequest(
            VchiqFileContextPtr->FileQueue[transactionType],
            WdfRequest,
            &removeRequest);
        if (tempStatus == STATUS_NOT_FOUND) {
            // Request not found, framework has has cancel the request just
            // return success and request would not be completed
            status = STATUS_SUCCESS;
        } else if (!NT_SUCCESS(tempStatus)) {
            VCHIQ_LOG_ERROR(
                "WdfIoQueueRetrieveFoundRequest failed (%!STATUS!)",
                tempStatus);
            NT_ASSERT(NT_SUCCESS(tempStatus));
        }
    }

End:
    ExReleaseFastMutex(&VchiqFileContextPtr->PendingBulkMsgMutex[bulkType]);

    return status;
}

/*++

Routine Description:

    Implementation of bulk transaction for both transmit and receive

Arguments:

    DeviceContextPtr - A pointer to the device context.

    VchiqFileContextPtr - File context pointer returned to caller

    WdfRequest - Request framework object tied to this bulk transfer

    MsgDirection - Specify the direction of the bulk transfer

    BufferMdl - Mdl pointer structer of the buffer

    BufferSize - Size of data that would be transfered

    ArmPortNumber - Slave port number of the transfer

    VchiqPortNumber - Master port number of the transfer

Return Value:

    NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqBulkTransfer (
    DEVICE_CONTEXT* DeviceContextPtr,
    VCHIQ_FILE_CONTEXT* VchiqFileContextPtr,
    WDFREQUEST WdfRequest,
    ULONG MsgDirection,
    MDL* BufferMdl,
    ULONG BufferSize,
    ULONG ArmPortNumber,
    ULONG VchiqPortNumber
    )
{
    NTSTATUS status;
    VCHIQ_PAGELIST* pageListPtr = NULL;
    VCHIQ_TX_REQUEST_CONTEXT* vchiqTxRequestContextPtr;
    DMA_ADAPTER* dmaAdapterPtr = VchiqFileContextPtr->DmaAdapterPtr;
    ULONG scatterGatherListSize, numberOfMapRegisters;
    WDFMEMORY scatterGatherWdfMemory = NULL;
    WDFMEMORY dmaTransferContextPtr = NULL;
    VOID* scatterGatherBufferPtr = NULL;
    VOID* dmaTransferContextBufferPtr;
    ULONG pageListSize = 0;
    PHYSICAL_ADDRESS pageListPhyAddress = { 0 };

    PAGED_CODE();

    // Use the DMA api to get the right buffer list for DMA transfer as the
    // recommended approach. 
    status = dmaAdapterPtr->DmaOperations->CalculateScatterGatherList(
        dmaAdapterPtr,
        BufferMdl,
        MmGetMdlVirtualAddress(BufferMdl),
        BufferSize,
        &scatterGatherListSize,
        &numberOfMapRegisters);
    if (!NT_SUCCESS(status)) {
        VCHIQ_LOG_ERROR(
            "CalculateScatterGatherList failed (%!STATUS!)",
            status);
        goto End;
    }

    // Allocate memory to hold the scatter gather list and transfer context
    {
        WDF_OBJECT_ATTRIBUTES attributes;
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

        // Let the framework release the memory when request is completed
        attributes.ParentObject = WdfRequest;

        status = WdfMemoryCreate(
            &attributes,
            PagedPool,
            VCHIQ_ALLOC_TAG_WDF,
            scatterGatherListSize,
            &scatterGatherWdfMemory,
            &scatterGatherBufferPtr);
        if (!NT_SUCCESS(status)) {
            VCHIQ_LOG_ERROR(
                "WdfMemoryCreate (scatter gather list) failed (%!STATUS!)",
                status);
            goto End;
        }

        status = WdfMemoryCreate(
            &attributes,
            PagedPool,
            VCHIQ_ALLOC_TAG_WDF,
            DMA_TRANSFER_CONTEXT_SIZE_V1,
            &dmaTransferContextPtr,
            &dmaTransferContextBufferPtr);
        if (!NT_SUCCESS(status)) {
            VCHIQ_LOG_ERROR(
                "WdfMemoryCreate for transfer context failed (%!STATUS!)",
                status);
            goto End;
        }

        status = dmaAdapterPtr->DmaOperations->InitializeDmaTransferContext(
            dmaAdapterPtr,
            dmaTransferContextBufferPtr);
        if (!NT_SUCCESS(status)) {
            VCHIQ_LOG_ERROR(
                "InitializeDmaTransferContext failed (%!STATUS!)",
                status);
            goto End;
        }
    }

    SCATTER_GATHER_LIST* scatterGatherListOutPtr;

    status = dmaAdapterPtr->DmaOperations->BuildScatterGatherListEx(
        dmaAdapterPtr,
        DeviceContextPtr->PhyDeviceObjectPtr,
        dmaTransferContextBufferPtr,
        BufferMdl,
        0,
        BufferSize,
        DMA_SYNCHRONOUS_CALLBACK,
        NULL,
        NULL,
        (MsgDirection == VCHIQ_MSG_BULK_TX) ? TRUE : FALSE,
        scatterGatherBufferPtr,
        scatterGatherListSize,
        NULL,
        NULL,
        &scatterGatherListOutPtr); // API requirement
    if (!NT_SUCCESS(status)) {
        VCHIQ_LOG_ERROR(
            "BuildScatterGatherListEx failed (%!STATUS!)",
            status);
        goto End;
    }

    // Now allocate and setup a page list for buffer transmission
    {
        SCATTER_GATHER_LIST* scatterGatherListPtr = scatterGatherBufferPtr;

        ULONG numPages = scatterGatherListPtr->NumberOfElements;
        pageListSize = (numPages * sizeof(ULONG)) + sizeof(VCHIQ_PAGELIST);

        status = VchiqAllocateCommonBuffer(
            VchiqFileContextPtr,
            pageListSize,
            &pageListPtr,
            &pageListPhyAddress);
        if (!NT_SUCCESS(status)) {
            VCHIQ_LOG_ERROR("Fail to alloc page list memory");
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto End;
        }

        pageListPtr->Length = BufferSize;
        pageListPtr->Type =
            (MsgDirection == VCHIQ_MSG_BULK_TX) ?
            PAGELIST_WRITE : PAGELIST_READ;
        pageListPtr->Offset =
            (USHORT)(scatterGatherListPtr->Elements[0].Address.LowPart
                & (PAGE_SIZE - 1));

        // Fill up page information for transfer with page address as required
        // by the firmware. Firmware does not expect actual physical address.
        // Running continuous pages is determined from individual element
        // length value.
        ULONG* pageListAddrPtr = pageListPtr->Addrs;
        ULONG i;

        for ( i = 0; i < scatterGatherListPtr->NumberOfElements; ++i) {

            // Firmware does not support DMA transaction more than 16MB in 
            // running pages. This is an unlikely path so adding an assert
            // to catch this
            ASSERT(scatterGatherListPtr->Elements[i].Length <= 0x1000000);

            *pageListAddrPtr =
                (scatterGatherListPtr->Elements[i].Address.LowPart &
                    ~(PAGE_SIZE - 1)) |
                OFFSET_DIRECT_SDRAM |
                BYTES_TO_PAGES(scatterGatherListPtr->Elements[i].Length) - 1;
            ++pageListAddrPtr;
        }
    }

    status = VchiqAllocateTransferRequestObjContext(
        DeviceContextPtr,
        VchiqFileContextPtr,
        WdfRequest,
        BufferMdl,
        pageListPtr,
        pageListSize,
        pageListPhyAddress,
        (SCATTER_GATHER_LIST* )scatterGatherBufferPtr,
        &vchiqTxRequestContextPtr);
    if (!NT_SUCCESS(status)) {
        VCHIQ_LOG_ERROR(
            "VchiqAllocateTransferRequestObjContext failed (%!STATUS!)",
            status);
        goto End;
    }

    // Buffer will be free when request object is released
    pageListPtr = NULL;

    // Dispatch message
    {
        ULONG bulkData[2];

        bulkData[0] = pageListPhyAddress.LowPart | OFFSET_DIRECT_SDRAM;
        bulkData[1] = BufferSize;

        status = VchiqQueueMessageAsync(
            DeviceContextPtr,
            VchiqFileContextPtr,
            VCHIQ_MAKE_MSG(MsgDirection, ArmPortNumber, VchiqPortNumber),
            &bulkData,
            sizeof(bulkData));
        if (!NT_SUCCESS(status)) {
            VCHIQ_LOG_ERROR(
                "VchiqQueueMessageAsync failed with status %!STATUS!",
                status);
            goto End;
        }
    }

End:
    if (!NT_SUCCESS(status)) {
        if (pageListPtr) {
            VchiqFreeCommonBuffer(
                VchiqFileContextPtr,
                pageListSize,
                pageListPhyAddress,
                pageListPtr);
        }
    }

    return status;
}

/*++

Routine Description:

     Process pending message when pending message request is sent from the client

Arguments:

     DeviceContextPtr - Pointer to device context

     VchiqFileContextPtr - File context pointer returned to caller

Return Value:

     NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqProcessPendingMsg (
    DEVICE_CONTEXT* DeviceContextPtr,
    VCHIQ_FILE_CONTEXT* VchiqFileContextPtr
    )
{
    NTSTATUS status;
    WDFREQUEST nextRequest;

    PAGED_CODE();
    
    if (IsListEmpty(&VchiqFileContextPtr->PendingDataMsgList)) {
        status = STATUS_SUCCESS;
        goto End;
    }

    status = WdfIoQueueRetrieveNextRequest(
        VchiqFileContextPtr->FileQueue[FILE_QUEUE_PENDING_MSG],
        &nextRequest);
    if (status == STATUS_NO_MORE_ENTRIES) {
        // Ok to return success here as userland might not have
        // have a chance to request for more completion msg yet.
        status = STATUS_SUCCESS;
        goto End;
    } else if (!NT_SUCCESS(status)) {
        VCHIQ_LOG_WARNING(
            "WdfIoQueueRetrieveNextRequest failed  %!STATUS!",
            status);
        goto End;
    }

    status = VchiqRemovePendingMsg(
        DeviceContextPtr,
        VchiqFileContextPtr,
        nextRequest);
    if (!NT_SUCCESS(status)) {
        VCHIQ_LOG_WARNING(
            "VchiqRemovePendingMsg failed  %!STATUS!",
            status);
        goto End;
    }

End:
    return status;
}

/*++

Routine Description:

     Process pending vchi message when a dequeue request from client
        is received

Arguments:

     DeviceContextPtr - Pointer to device context

     VchiqFileContextPtr - File context pointer returned to caller

Return Value:

     NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqProcessPendingVchiMsg (
    DEVICE_CONTEXT* DeviceContextPtr,
    VCHIQ_FILE_CONTEXT* VchiqFileContextPtr
    )
{
    NTSTATUS status;
    WDFREQUEST nextRequest;

    PAGED_CODE();
    
    if (IsListEmpty(&VchiqFileContextPtr->PendingVchiMsgList)) {
        status = STATUS_SUCCESS;
        goto End;
    }

    do {
        status = WdfIoQueueRetrieveNextRequest(
            VchiqFileContextPtr->FileQueue[FILE_QUEUE_PENDING_VCHI_MSG],
            &nextRequest);
        if (status == STATUS_NO_MORE_ENTRIES) {
            // Ok to return success here as userland might not have
            // have a chance to request to dequeue any msg yet.
            status = STATUS_SUCCESS;
            break;
        } else if (!NT_SUCCESS(status)) {
            VCHIQ_LOG_WARNING(
                "WdfIoQueueRetrieveNextRequest failed  %!STATUS!",
                status);
            break;
        }

        status = VchiqRemovePendingVchiMsg(
            DeviceContextPtr,
            VchiqFileContextPtr,
            nextRequest);
        if (!NT_SUCCESS(status)) {
            VCHIQ_LOG_WARNING(
                "VchiqRemovePendingVchiMsg failed  %!STATUS!",
                status);
            break;
        }

        if (IsListEmpty(&VchiqFileContextPtr->PendingVchiMsgList)) {
            status = STATUS_SUCCESS;
            break;
        }
    } while (nextRequest != NULL);

End:
    return status;
}

/*++

Routine Description:

     VchiqAddRefMsg adds the ref count to a slot.

Arguments:

     DeviceContextPtr - Pointer to device context

     SlotNumber - Increment msg ref count for this slot number

Return Value:

     NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqAddRefMsg (
    DEVICE_CONTEXT* DeviceContextPtr,
    ULONG SlotNumber
    )
{
    SLOT_INFO* slotPtr = &DeviceContextPtr->RxSlotInfo[SlotNumber];

    PAGED_CODE();

    InterlockedIncrement(&slotPtr->RefCount);

    return STATUS_SUCCESS;
}

/*++

Routine Description:

     VchiqReleaseMsg decrease the ref count to a slot and attempt
        to recycle the slot.

Arguments:

     DeviceContextPtr - Pointer to device context

     SlotNumber - Decrrease msg ref count for this slot number

Return Value:

     NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqReleaseMsg (
    DEVICE_CONTEXT* DeviceContextPtr,
    ULONG SlotNumber
    )
{
    SLOT_INFO* slotPtr = &DeviceContextPtr->RxSlotInfo[SlotNumber];

    PAGED_CODE();

    InterlockedDecrement(&slotPtr->RefCount);

    // Check to see if the slot is available to be recycled
    VchiqRecycleSlot(
        DeviceContextPtr,
        DeviceContextPtr->SlotZeroPtr,
        SlotNumber,
        FALSE);

    return STATUS_SUCCESS;
}

/*++

Routine Description:

    VchiqProcessNewRxMsg would take the latest rx message and add it
        to the queue. It would then attempt to dispatch the message if
        the client is waiting for a message completion.

Arguments:

    DeviceContextPtr - Pointer to device context

    VchiqFileContextPtr - File context pointer returned to caller

    Msg - Pointer to the message from master

    SlotNumber - Slot number that would be attempted to be recycled

Return Value:

    NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqProcessNewRxMsg (
    DEVICE_CONTEXT* DeviceContextPtr,
    VCHIQ_FILE_CONTEXT* VchiqFileContextPtr,
    VCHIQ_HEADER* RxMsg
    )
{
    NTSTATUS status;

    PAGED_CODE();

    ExAcquireFastMutex(&VchiqFileContextPtr->PendingDataMsgMutex);

    status = VchiqAddPendingMsg(
        DeviceContextPtr,
        VchiqFileContextPtr,
        RxMsg,
        DeviceContextPtr->MasterCurrentSlotIndex);
    if (!NT_SUCCESS(status)) {
        VCHIQ_LOG_ERROR(
            "VchiqAddPendingMsg failed  %!STATUS!",
            status);
        goto End;
    }

    status = VchiqProcessPendingMsg(
        DeviceContextPtr,
        VchiqFileContextPtr);
    if (!NT_SUCCESS(status)) {
        VCHIQ_LOG_ERROR(
            "VchiqAddPendingMsg failed  %!STATUS!",
            status);
        goto End;
    }

End:
    ExReleaseFastMutex(&VchiqFileContextPtr->PendingDataMsgMutex);

    return status;
}

/*++

Routine Description:

     VchiqAddPendingMsg keeps track of pending message to a port number
        and also adds a reference to the current slot.

Arguments:

     DeviceContextPtr - Pointer to device context

     VchiqFileContextPtr - File context pointer returned to caller

     Msg - Pointer to the message from master

     SlotNumber - Slot number of the current message

Return Value:

     NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqAddPendingMsg (
    DEVICE_CONTEXT* DeviceContextPtr,
    VCHIQ_FILE_CONTEXT* VchiqFileContextPtr,
    VCHIQ_HEADER* Msg,
    ULONG SlotNumber
    )
{
    NTSTATUS status;
    WDFMEMORY wdfMemoryNewPendingMsg = NULL;

    PAGED_CODE();

    NT_ASSERT(VchiqFileContextPtr->PendingMsgLookAsideMemory != NULL);

    status = WdfMemoryCreateFromLookaside(
        VchiqFileContextPtr->PendingMsgLookAsideMemory,
        &wdfMemoryNewPendingMsg);
    if (!NT_SUCCESS(status)) {
        VCHIQ_LOG_ERROR(
            "WdfMemoryCreateFromLookaside failed %!STATUS!)",
            status);
        goto End;
    }

    size_t bufferSize;
    VCHIQ_PENDING_MSG* newPendingMsgPtr;
    newPendingMsgPtr = WdfMemoryGetBuffer(wdfMemoryNewPendingMsg, &bufferSize);
    if (!NT_SUCCESS(status) || (bufferSize != sizeof(*newPendingMsgPtr))) {
        VCHIQ_LOG_ERROR(
            "WdfMemoryGetBuffer failed %!STATUS! size %lld)",
            status,
            bufferSize);
        goto End;
    }

    newPendingMsgPtr->Msg = Msg;
    newPendingMsgPtr->SlotNumber = SlotNumber;
    newPendingMsgPtr->WdfMemory = wdfMemoryNewPendingMsg;

    InsertTailList(
        &VchiqFileContextPtr->PendingDataMsgList,
        &newPendingMsgPtr->ListEntry);
    VchiqAddRefMsg(DeviceContextPtr, SlotNumber);

End:
    if (!NT_SUCCESS(status)) {
        if (wdfMemoryNewPendingMsg != NULL) {
            WdfObjectDelete(wdfMemoryNewPendingMsg);
        }
    }

    return status;
}


/*++

Routine Description:

     VchiqRemovePendingMsg - Will remove pending message for a port number
        and completes the request if a request object is provided.

Arguments:

     DeviceContextPtr - Pointer to device context

     VchiqFileContextPtr - File context pointer returned to caller

     WdfRequest - Optional wdf request object. If provided pending messages
        woudl be copied over to the output buffer.

Return Value:

     NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqRemovePendingMsg (
    DEVICE_CONTEXT* DeviceContextPtr,
    VCHIQ_FILE_CONTEXT* VchiqFileContextPtr,
    WDFREQUEST WdfRequest
    )
{
    NTSTATUS status;
    LIST_ENTRY *nextListEntryPtr;

    PAGED_CODE();

    // Remove all pending message if a request object is not provided
    if (WdfRequest == NULL) {
        do {
            nextListEntryPtr = RemoveTailList(
                &VchiqFileContextPtr->PendingDataMsgList);
            if (nextListEntryPtr ==
                &VchiqFileContextPtr->PendingDataMsgList) {
                break;
            }
            ULONG nextMsgSlotNumber =
                CONTAINING_RECORD(
                    nextListEntryPtr, VCHIQ_PENDING_MSG, ListEntry)->SlotNumber;
            VchiqReleaseMsg(DeviceContextPtr, nextMsgSlotNumber);
            WdfObjectDelete(CONTAINING_RECORD(
                nextListEntryPtr, VCHIQ_PENDING_MSG, ListEntry)->WdfMemory);
        } while (nextListEntryPtr != NULL);
        status = STATUS_SUCCESS;
        goto End;
    }

    ULONG* totalMsgPtr;
    size_t bufSize;
    status = WdfRequestRetrieveOutputBuffer(
        WdfRequest,
        sizeof(*totalMsgPtr),
        &totalMsgPtr,
        &bufSize);
    if (!NT_SUCCESS(status)) {
        VCHIQ_LOG_ERROR(
            "WdfRequestRetrieveOutputBuffer failed %!STATUS! \
            bufSize(%lld)",
            status,
            bufSize);
        goto CompleteRequest;
    }

    VCHIQ_AWAIT_COMPLETION* awaitCompletionPtr;
    status = WdfRequestRetrieveInputBuffer(
        WdfRequest,
        sizeof(*awaitCompletionPtr),
        &awaitCompletionPtr,
        &bufSize);
    if (!NT_SUCCESS(status)) {
        VCHIQ_LOG_ERROR(
            "WdfRequestRetrieveInputBuffer failed %!STATUS! \
            bufSize(%lld)",
            status,
            bufSize);
        goto CompleteRequest;
    }

    *totalMsgPtr = 0;

    VCHIQ_COMPLETION_DATA* completionDataPtr =
        WdfMemoryGetBuffer(
            awaitCompletionPtr->WdfMemoryCompletion,
            NULL);

    do {
        nextListEntryPtr = RemoveHeadList(
            &VchiqFileContextPtr->PendingDataMsgList);
        if (nextListEntryPtr ==
            &VchiqFileContextPtr->PendingDataMsgList) {

            break;
        }

        // Only copy over the message header is we have sufficient output buffer
        VCHIQ_HEADER* nextMsgHeaderPtr =
            CONTAINING_RECORD(
                nextListEntryPtr, VCHIQ_PENDING_MSG, ListEntry)->Msg;
        ULONG pendingMsgSize = nextMsgHeaderPtr->Size + sizeof(*nextMsgHeaderPtr);
        if (pendingMsgSize > awaitCompletionPtr->MsgBufSize) {
            InsertHeadList(
                &VchiqFileContextPtr->PendingDataMsgList,
                nextListEntryPtr);
            break;
        }

        ULONG nextMsgSlotNumber =
            CONTAINING_RECORD(
                nextListEntryPtr, VCHIQ_PENDING_MSG, ListEntry)->SlotNumber;

        // Return the reason we receive the message. This is redundant but 
        // userland expects this information. We return VCHIQ defined reason
        // even for vchi service as userland would be responsible to translate
        // it to equivalent vchi defined reason.
        NTSTATUS tempStatus;
        VCHIQ_BULK_MODE_T bulkMode;
        BOOLEAN trackMsgForVchiService = FALSE;
        BOOLEAN returnMsgToVchiService = TRUE;
        switch (VCHIQ_MSG_TYPE(nextMsgHeaderPtr->MsgId))
        {
        case VCHIQ_MSG_DATA:
            {
                completionDataPtr[*totalMsgPtr].Reason = 
                    VCHIQ_MESSAGE_AVAILABLE;
                trackMsgForVchiService = TRUE;
            }
            break;
        case VCHIQ_MSG_BULK_TX_DONE:
            {
                completionDataPtr[*totalMsgPtr].Reason = 
                    VCHIQ_BULK_TRANSMIT_DONE;

                ExAcquireFastMutex(
                    &VchiqFileContextPtr->PendingBulkMsgMutex[MSG_BULK_TX]);

                // Suppress warning as OACR seem confused. Lock is acquired
                // above but OACR still flags a warning. Warning does not occur
                // in a Visual Studio build.
                #pragma warning(suppress: 26110)
                tempStatus = VchiqRemovePendingBulkMsg(
                    VchiqFileContextPtr,
                    &completionDataPtr[*totalMsgPtr],
                    MSG_BULK_TX,
                    FALSE,
                    &bulkMode);

                ExReleaseFastMutex(
                    &VchiqFileContextPtr->PendingBulkMsgMutex[MSG_BULK_TX]);

                if (!NT_SUCCESS(tempStatus)) {
                    VCHIQ_LOG_ERROR(
                        "VchiqRemovePendingBulkMsg failed %!STATUS!",
                        tempStatus);
                    returnMsgToVchiService = FALSE;
                    break;
                }

               

                // Do not return message if bulk transfer mode is blocking
                // or no callback
                if ((bulkMode == VCHIQ_BULK_MODE_BLOCKING) ||
                    (bulkMode == VCHIQ_BULK_MODE_NOCALLBACK)) {

                    returnMsgToVchiService = FALSE;
                }
            }
            break;
        case VCHIQ_MSG_BULK_RX_DONE:
            {
                completionDataPtr[*totalMsgPtr].Reason = 
                    VCHIQ_BULK_RECEIVE_DONE;

                ExAcquireFastMutex(
                    &VchiqFileContextPtr->PendingBulkMsgMutex[MSG_BULK_RX]);

                tempStatus = VchiqRemovePendingBulkMsg(
                    VchiqFileContextPtr,
                    &completionDataPtr[*totalMsgPtr],
                    MSG_BULK_RX,
                    FALSE,
                    &bulkMode);

                ExReleaseFastMutex(
                    &VchiqFileContextPtr->PendingBulkMsgMutex[MSG_BULK_RX]);

                if (!NT_SUCCESS(tempStatus)) {
                    VCHIQ_LOG_ERROR(
                        "VchiqRemovePendingBulkMsg failed %!STATUS!",
                        tempStatus);
                    returnMsgToVchiService = FALSE;
                    break;
                }

                // Do not return message if bulk transfer mode is blocking
                // or no callback
                if ((bulkMode == VCHIQ_BULK_MODE_BLOCKING) ||
                    (bulkMode == VCHIQ_BULK_MODE_NOCALLBACK)) {

                    returnMsgToVchiService = FALSE;
                }
            }
            break;
        default:
            VCHIQ_LOG_WARNING("Processing unknown message back to user");
            break;
        }

        if (returnMsgToVchiService == TRUE) {
            // Return the service pointer back to userland
            VOID* msgBufferPtr =
                WdfMemoryGetBuffer(
                    completionDataPtr[*totalMsgPtr].WdfMemoryBuffer,
                    NULL);

            RtlCopyMemory(
                msgBufferPtr,
                nextMsgHeaderPtr,
                pendingMsgSize);

            completionDataPtr[*totalMsgPtr].ServiceUserData =
                VchiqFileContextPtr->ServiceUserData;

            ++*totalMsgPtr;
        }

        if ((VchiqFileContextPtr->IsVchi) && (trackMsgForVchiService == TRUE)) {

            ExAcquireFastMutex(&VchiqFileContextPtr->PendingVchiMsgMutex);

            // For vchi based service we need to keep track of all message so it could
            // be dequeue by the service in a separate IOCTL
            status = VchiqAddPendingVchiMsg(
                DeviceContextPtr,
                VchiqFileContextPtr,
                nextMsgHeaderPtr,
                nextMsgSlotNumber);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "VchiqAddPendingVchiMsg failed %!STATUS!)",
                    status);
            }

            ExReleaseFastMutex(&VchiqFileContextPtr->PendingVchiMsgMutex);
        }

        WdfObjectDelete(CONTAINING_RECORD(
            nextListEntryPtr, VCHIQ_PENDING_MSG, ListEntry)->WdfMemory);

        VchiqReleaseMsg(DeviceContextPtr, nextMsgSlotNumber);

    } while (*totalMsgPtr < awaitCompletionPtr->MsgBufCount);

    if (*totalMsgPtr == 0) {

        // Requeue the request if no completion msg to return back to caller
        status = WdfRequestRequeue(WdfRequest);
        if (!NT_SUCCESS(status)) {
            VCHIQ_LOG_ERROR(
                "WdfRequestRequeue failed  %!STATUS!",
                status);
        }
    } else {
        WdfRequestCompleteWithInformation(
            WdfRequest,
            STATUS_SUCCESS,
            sizeof(ULONG));

        ExAcquireFastMutex(&VchiqFileContextPtr->PendingVchiMsgMutex);

        status = VchiqProcessPendingVchiMsg(
            DeviceContextPtr,
            VchiqFileContextPtr);
        if (!NT_SUCCESS(status)) {
            VCHIQ_LOG_ERROR(
                "VchiqProcessPendingVchiMsg failed  %!STATUS!",
                status);
        }

        ExReleaseFastMutex(&VchiqFileContextPtr->PendingVchiMsgMutex);
    }

   

End:

    return status;

CompleteRequest:
    WdfRequestComplete(
        WdfRequest,
        status);

    return status;
}

/*++

Routine Description:

     VchiqAddPendingBulkMsg keeps track of bulk message. This function
        needs to be lock so the order of bulk transmit is maintain.

Arguments:

     VchiqFileContextPtr - File context pointer returned to caller

     BulkTransferPtr - Pointer to the next pending bulk message to be added
        to the list.

     BulkType - Specify the type of bulk transfer

Return Value:

     NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqAddPendingBulkMsg (
    VCHIQ_FILE_CONTEXT* VchiqFileContextPtr,
    VCHIQ_QUEUE_BULK_TRANSFER* BulkTransferPtr,
    MSG_BULK_TYPE BulkType
    )
{
    NTSTATUS status;
    WDFMEMORY wdfMemoryNewPendingMsg = NULL;        

    PAGED_CODE();

    NT_ASSERT(VchiqFileContextPtr->PendingBulkMsgLookAsideMemory != NULL);

    status = WdfMemoryCreateFromLookaside(
        VchiqFileContextPtr->PendingBulkMsgLookAsideMemory,
        &wdfMemoryNewPendingMsg);
    if (!NT_SUCCESS(status)) {
        VCHIQ_LOG_ERROR(
            "WdfMemoryCreateFromLookaside failed %!STATUS!)",
            status);
        goto End;
    }

    size_t bufferSize;
    VCHIQ_PENDING_BULK_MSG* newPendingBulkTransferPtr;
    newPendingBulkTransferPtr =
        WdfMemoryGetBuffer(wdfMemoryNewPendingMsg, &bufferSize);
    if (!NT_SUCCESS(status) || (bufferSize != sizeof(*newPendingBulkTransferPtr))) {
        VCHIQ_LOG_ERROR(
            "WdfMemoryGetBuffer failed %!STATUS! size %lld)",
            status,
            bufferSize);
        goto End;
    }

    newPendingBulkTransferPtr->WdfMemory = wdfMemoryNewPendingMsg;
    newPendingBulkTransferPtr->Mode = BulkTransferPtr->Mode;
    newPendingBulkTransferPtr->BulkUserData =
        BulkTransferPtr->UserData;

    InsertTailList(
        &VchiqFileContextPtr->PendingBulkMsgList[BulkType],
        &newPendingBulkTransferPtr->ListEntry);

End:
    if (!NT_SUCCESS(status)) {
        if (wdfMemoryNewPendingMsg != NULL) {
            WdfObjectDelete(wdfMemoryNewPendingMsg);
        }
    }

    return status;
}

/*++

Routine Description:

    VchiqARemovePendingBulkMsg remove pending bulk message from the list.
        Caller should acquire the appropriate PendingBulkMsgMutex resource.

Arguments:

    VchiqFileContextPtr - File context pointer returned to caller

    CompletionDataPtr - Pointer to a completion ddata structure which would be
        populated with the next pending bulk transaction information. If this
        pointer is NULL then the caller is   requesting that one or all entry
        would need to be remove based on RemoveAll value.

    BulkType - Specify the type of bulk transfer

    RemoveAll - A zero value means remove the last entry in tail. A non-zero
        value means remove all entries.

    Mode - Returns the bulk transder back to the caller if a pointer is provided

Return Value:

    NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqRemovePendingBulkMsg (
    VCHIQ_FILE_CONTEXT* VchiqFileContextPtr,
    VCHIQ_COMPLETION_DATA* CompletionDataPtr,
    MSG_BULK_TYPE BulkType,
    ULONG RemoveAll,
    VCHIQ_BULK_MODE_T* BulkMode
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    LIST_ENTRY* nextListEntryPtr;

    PAGED_CODE();

    if (BulkMode) {
        *BulkMode = VCHIQ_BULK_MODE_WAITING;
    }

    // Remove the last inserted bulk list
    if (CompletionDataPtr == NULL) {
        do {
            nextListEntryPtr = RemoveTailList(
                &VchiqFileContextPtr->PendingBulkMsgList[BulkType]);
            if (nextListEntryPtr ==
                &VchiqFileContextPtr->PendingBulkMsgList[BulkType]) {
                break;
            }

            WdfObjectDelete(CONTAINING_RECORD(
                nextListEntryPtr, VCHIQ_PENDING_BULK_MSG, ListEntry)->WdfMemory);
        } while (nextListEntryPtr != NULL && RemoveAll);

        status = STATUS_SUCCESS;
        goto End;
    }

    nextListEntryPtr = RemoveHeadList(
        &VchiqFileContextPtr->PendingBulkMsgList[BulkType]);
    if (nextListEntryPtr ==
        &VchiqFileContextPtr->PendingBulkMsgList[BulkType]) {

        status = STATUS_NOT_FOUND;
        VCHIQ_LOG_WARNING("No pending bulk transfer available");
        goto End;
    }

    // Userland expects the bulk user data pointer to be returned. I
    CompletionDataPtr->BulkUserData = CONTAINING_RECORD(
        nextListEntryPtr, VCHIQ_PENDING_BULK_MSG, ListEntry)->BulkUserData;

    if (BulkMode) {
        *BulkMode = CONTAINING_RECORD(
            nextListEntryPtr, VCHIQ_PENDING_BULK_MSG, ListEntry)->Mode;
    }

End:

    return status;
}

/*++

Routine Description:

     VchiqAddPendingVchiMsg keeps track of pending message for vchi serice.

Arguments:

     DeviceContextPtr - Pointer to device context

     VchiqFileContextPtr - File context pointer returned to caller

     Msg - Pointer to the message from master

     SlotNumber - Slot number that would be attempted to be recycled

Return Value:

     NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqAddPendingVchiMsg (
    DEVICE_CONTEXT* DeviceContextPtr,
    VCHIQ_FILE_CONTEXT* VchiqFileContextPtr,
    VCHIQ_HEADER* Msg,
    ULONG SlotNumber
    )
{
    NTSTATUS status;
    WDFMEMORY wdfMemoryNewPendingMsg = NULL;

    PAGED_CODE();
    
    // Reuse the same look aside for pending message
    status = WdfMemoryCreateFromLookaside(
        VchiqFileContextPtr->PendingMsgLookAsideMemory,
        &wdfMemoryNewPendingMsg);
    if (!NT_SUCCESS(status)) {
        VCHIQ_LOG_ERROR(
            "WdfMemoryCreateFromLookaside failed %!STATUS!)",
            status);
        goto End;
    }

    size_t bufferSize;
    VCHIQ_PENDING_MSG* newPendingMsgPtr;
    newPendingMsgPtr = WdfMemoryGetBuffer(wdfMemoryNewPendingMsg, &bufferSize);
    if (!NT_SUCCESS(status) || (bufferSize != sizeof(*newPendingMsgPtr))) {
        VCHIQ_LOG_ERROR(
            "WdfMemoryGetBuffer failed %!STATUS! size %lld)",
            status,
            bufferSize);
        goto End;
    }

    newPendingMsgPtr->Msg = Msg;
    newPendingMsgPtr->SlotNumber = SlotNumber;
    newPendingMsgPtr->WdfMemory = wdfMemoryNewPendingMsg;

    InsertTailList(
        &VchiqFileContextPtr->PendingVchiMsgList,
        &newPendingMsgPtr->ListEntry);
    VchiqAddRefMsg(DeviceContextPtr, SlotNumber);

End:
    if (!NT_SUCCESS(status)) {
        if (wdfMemoryNewPendingMsg != NULL) {
            WdfObjectDelete(wdfMemoryNewPendingMsg);
        }
    }

    return status;
}


/*++

Routine Description:

     VchiqRemovePendingVchiMsg - Will remove pending message for vchi service.

Arguments:

     DeviceContextPtr - Pointer to device context

     VchiqFileContextPtr - File context pointer returned to caller

     WdfRequest - Optional wdf request object. If provided pending messages
        woudl be copied over to the output buffer.

Return Value:

     NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqRemovePendingVchiMsg (
    DEVICE_CONTEXT* DeviceContextPtr,
    VCHIQ_FILE_CONTEXT* VchiqFileContextPtr,
    WDFREQUEST WdfRequest
    )
{
    NTSTATUS status;
    LIST_ENTRY *nextListEntryPtr;

    PAGED_CODE();

    // Just like pending data message remove all pending vchi message if a 
    // request WDF object is not provided
    if (WdfRequest == NULL) {
        do {
            nextListEntryPtr = RemoveTailList(
                &VchiqFileContextPtr->PendingVchiMsgList);
            if (nextListEntryPtr ==
                &VchiqFileContextPtr->PendingVchiMsgList) {
                break;
            }
            ULONG nextMsgSlotNumber =
                CONTAINING_RECORD(
                    nextListEntryPtr, VCHIQ_PENDING_MSG, ListEntry)->SlotNumber;
            VchiqReleaseMsg(DeviceContextPtr, nextMsgSlotNumber);
            WdfObjectDelete(CONTAINING_RECORD(
                nextListEntryPtr, VCHIQ_PENDING_MSG, ListEntry)->WdfMemory);
        } while (nextListEntryPtr != NULL);
        status = STATUS_SUCCESS;
        goto End;
    }

    VCHIQ_DEQUEUE_MESSAGE* dequeueMsgPtr;
    size_t bufSize;
    status = WdfRequestRetrieveInputBuffer(
        WdfRequest,
        sizeof(*dequeueMsgPtr),
        &dequeueMsgPtr,
        &bufSize);
    if (!NT_SUCCESS(status)) {
        VCHIQ_LOG_ERROR(
            "WdfRequestRetrieveInputBuffer failed %!STATUS! \
            bufSize(%lld)",
            status,
            bufSize);
        goto CompleteRequest;
    }

    ULONG* totalMsgSizePtr;
    status = WdfRequestRetrieveOutputBuffer(
        WdfRequest,
        sizeof(*totalMsgSizePtr),
        &totalMsgSizePtr,
        &bufSize);
    if (!NT_SUCCESS(status)) {
        VCHIQ_LOG_ERROR(
            "WdfRequestRetrieveOutputBuffer failed %!STATUS! \
            bufSize(%lld)",
            status,
            bufSize);
        goto CompleteRequest;
    }
    
    nextListEntryPtr = RemoveHeadList(
        &VchiqFileContextPtr->PendingVchiMsgList);
    if (nextListEntryPtr ==
        &VchiqFileContextPtr->PendingVchiMsgList) {

        VCHIQ_LOG_ERROR(
            "No more vchi message available!");
        status = STATUS_UNSUCCESSFUL;
        goto CompleteRequest;
    }

    // Only copy over the message header is we have sufficient output buffer
    VCHIQ_HEADER* nextMsgHeaderPtr =
        CONTAINING_RECORD(
            nextListEntryPtr, VCHIQ_PENDING_MSG, ListEntry)->Msg;
    ULONG pendingVchiMsgSize = nextMsgHeaderPtr->Size + sizeof(*nextMsgHeaderPtr);
    if (pendingVchiMsgSize > dequeueMsgPtr->BufSize) {
        InsertHeadList(
            &VchiqFileContextPtr->PendingVchiMsgList,
            nextListEntryPtr);
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto CompleteRequest;
    }

    ULONG nextMsgSlotNumber =
        CONTAINING_RECORD(
            nextListEntryPtr, VCHIQ_PENDING_MSG, ListEntry)->SlotNumber;

    VOID* msgBufferPtr =
        WdfMemoryGetBuffer(
            dequeueMsgPtr->WdfMemoryBuffer,
            NULL);

    RtlCopyMemory(
        msgBufferPtr,
        nextMsgHeaderPtr,
        pendingVchiMsgSize);

    WdfObjectDelete(CONTAINING_RECORD(
        nextListEntryPtr, VCHIQ_PENDING_MSG, ListEntry)->WdfMemory);

    *totalMsgSizePtr = pendingVchiMsgSize;

    VchiqReleaseMsg(DeviceContextPtr, nextMsgSlotNumber);

    WdfRequestCompleteWithInformation(
        WdfRequest, 
        status, 
        sizeof(*totalMsgSizePtr));

    return status;

CompleteRequest:
    WdfRequestComplete(WdfRequest, status);

End:
    return status;
}
/*++

Routine Description:

     VchiqRecycleSlot attempt to recycle slot back to vchiq if
        all messages within the slot are not needed anymore and the
        caller has already specify slot is ready to be recyled.

Arguments:

     DeviceContextPtr - Pointer to device context

     SlotZeroPtr - Pointer to slot zero

     SlotNumber - Slot number that would be attempted to be recycled

     ReleaseSlot - Caller would specify if slot can be release for recycle

Return Value:

     VOID

--*/
_Use_decl_annotations_
VOID VchiqRecycleSlot (
    DEVICE_CONTEXT* DeviceContextPtr,
    VCHIQ_SLOT_ZERO* SlotZeroPtr,
    ULONG SlotNumber,
    BOOLEAN ReleaseSlot
    )
{
    ULONG curSlotRefCount;
    SLOT_INFO* currentSlotPtr = &DeviceContextPtr->RxSlotInfo[SlotNumber];

    PAGED_CODE();

    // Only release slot when caller specifies it isnt used anymore
    if (ReleaseSlot) {
        currentSlotPtr->SlotInUse = FALSE;
    }

    curSlotRefCount = InterlockedExchange(
        (volatile LONG *)&currentSlotPtr->RefCount,
        currentSlotPtr->RefCount);

    // Only recycle the slot if there is no more reference to the slot
    if ((curSlotRefCount == 0) && (currentSlotPtr->SlotInUse == FALSE)) {
        NTSTATUS status;

        // Lock here because we want to serialize recycle notification
        ExAcquireFastMutex(&DeviceContextPtr->RecycleSlotMutex);

        SlotZeroPtr->Master.SlotQueue[SlotZeroPtr->Master.SlotQueueRecycle
            & VCHIQ_SLOT_QUEUE_MASK] =
            SlotNumber;

        ++SlotZeroPtr->Master.SlotQueueRecycle;

        status = VchiqSignalVC(
            DeviceContextPtr,
            &SlotZeroPtr->Master.Recycle);
        if (!NT_SUCCESS(status)) {
            VCHIQ_LOG_ERROR(
                "Fail to signal VC %!STATUS!",
                status);
        }

        ExReleaseFastMutex(&DeviceContextPtr->RecycleSlotMutex);
    }

    return;
}

/*++

Routine Description:

    Running thread that process the trigger interrupt

Arguments:

    Param - A caller-supplied pointer to driver-defined context information
    where in our case it is the device context

Return Value:

    VOID

--*/
_Use_decl_annotations_
VOID VchiqTriggerThreadRoutine (
    VOID* Param
    )
{
    NTSTATUS status;
    ULONG threadActive = 1;
    DEVICE_CONTEXT* deviceContextPtr = Param;

    PAGED_CODE();

    while (threadActive) {

        if (VCHIQ_IS_EVENT_SIGNAL(
                &deviceContextPtr->SlotZeroPtr->Slave.Trigger)) {
            status = STATUS_WAIT_0;
        } else {
            status = VchiqWaitForEvents(
                &deviceContextPtr->VchiqThreadEvent[THREAD_TRIGGER],
                &deviceContextPtr->VchiqThreadEventStop,
                NULL);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "Unexpected failure on trigger thread %!STATUS!",
                    status);
                continue;
            }
        }

        switch (status)
        {
        case STATUS_WAIT_0:
            VchiqProcessRxSlot(deviceContextPtr);
            break;
        case STATUS_WAIT_1:
            threadActive = 0;
            break;
        default:
            VCHIQ_LOG_ERROR(
                "Unexpected response for trigger thread %!STATUS!",
                status);
            break;
        }
    }

    (void)PsTerminateSystemThread(STATUS_SUCCESS);

    return;
}

/*++

Routine Description:

    Running thread that process the recyle interrupt

Arguments:

    Param - A caller-supplied pointer to driver-defined context information
    where in our case it is the device context

Return Value:

    VOID

--*/
_Use_decl_annotations_
VOID VchiqRecycleThreadRoutine (
    VOID* Param
    )
{
    NTSTATUS status;
    ULONG threadActive = 1;
    DEVICE_CONTEXT* deviceContextPtr = Param;

    PAGED_CODE();

    while (threadActive) {

        if (VCHIQ_IS_EVENT_SIGNAL(
                &deviceContextPtr->SlotZeroPtr->Slave.Recycle)) {
            status = STATUS_WAIT_0;
        } else {
            status = VchiqWaitForEvents(
                &deviceContextPtr->VchiqThreadEvent[THREAD_RECYCLE],
                &deviceContextPtr->VchiqThreadEventStop,
                NULL);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "Unexpected failure on recycle thread %!STATUS!",
                    status);
                continue;
            }
        }

        switch (status)
        {
            case STATUS_WAIT_0:
                VchiqProcessRecycleTxSlot(deviceContextPtr);
                break;
            case STATUS_WAIT_1:
                threadActive = 0;
                break;
            default:
                VCHIQ_LOG_ERROR(
                    "Unexpected response for recycle thread %!STATUS!",
                    status);
                break;
        }
    }

    (void)PsTerminateSystemThread(STATUS_SUCCESS);

    return;
}

/*++

Routine Description:

    Running thread that handles sync trigger

Arguments:

    Param - A caller-supplied pointer to driver-defined context information
    where in our case it is the device context

Return Value:

    VOID

--*/
_Use_decl_annotations_
VOID VchiqSyncThreadRoutine (
    VOID* Param
    )
{
    NTSTATUS status;
    ULONG threadActive = 1;
    DEVICE_CONTEXT* deviceContextPtr = Param;

    PAGED_CODE();

    while (threadActive) {
        if (VCHIQ_IS_EVENT_SIGNAL(
                &deviceContextPtr->SlotZeroPtr->Slave.SyncTrigger)) {
            status = STATUS_WAIT_0;
        } else {
            status = VchiqWaitForEvents(
                &deviceContextPtr->VchiqThreadEvent[THREAD_SYNC],
                &deviceContextPtr->VchiqThreadEventStop,
                NULL);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "Unexpected failure on trigger sync thread %!STATUS!",
                    status);
                continue;
            }
        }

        switch (status)
        {
        case STATUS_WAIT_0:
            // Currently unsupported
            break;
        case STATUS_WAIT_1:
            threadActive = 0;
            break;
        default:
            VCHIQ_LOG_ERROR(
                "Unexpected response for trigger sync thread %!STATUS!",
                status);
            break;
        }
    }

    (void)PsTerminateSystemThread(STATUS_SUCCESS);

    return;
}

/*++

Routine Description:

    Running thread that handles sync release

Arguments:

    Param - A caller-supplied pointer to driver-defined context information
    where in our case it is the device context

Return Value:

    VOID

--*/
_Use_decl_annotations_
VOID VchiqSyncReleaseThreadRoutine (
    VOID* Param
    )
{
    NTSTATUS status;
    ULONG threadActive = 1;
    DEVICE_CONTEXT* deviceContextPtr = Param;

    PAGED_CODE();

    while (threadActive) {
        if (VCHIQ_IS_EVENT_SIGNAL(
                &deviceContextPtr->SlotZeroPtr->Slave.SyncRelease)) {
            status = STATUS_WAIT_0;
        } else {
            status = VchiqWaitForEvents(
                &deviceContextPtr->VchiqThreadEvent[THREAD_SYNC_RELEASE],
                &deviceContextPtr->VchiqThreadEventStop,
                NULL);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "Unexpected failure on sync release thread %!STATUS!",
                    status);
                continue;
            }
        }

        switch (status)
        {
        case STATUS_WAIT_0:
            // Currently unsupported
            break;
        case STATUS_WAIT_1:
            threadActive = 0;
            break;
        default:
            VCHIQ_LOG_ERROR(
                "Unexpected response for sync release thread %!STATUS!",
                status);
            break;
        }
    }

    (void)PsTerminateSystemThread(STATUS_SUCCESS);

    return;
}
VCHIQ_PAGED_SEGMENT_END

VCHIQ_NONPAGED_SEGMENT_BEGIN

/*++

Routine Description:

    Utility function to wait for dispatcher object to be signalled or stop 
        event. This funtion is in nonpaged as the array of events needs to be
        in nonpaged system memory.

Arguments:

    MainEventPtr - Pointer to main dispatcher object

    StopEventPtr - Pointer to stop event object

    Timeout - Optional timeout value for wait events

Return Value:

    NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS VchiqWaitForEvents (
    VOID* MainEventPtr,
    KEVENT* StopEventPtr,
    LARGE_INTEGER* TimeoutPtr
    )
{
    NTSTATUS status;
    VOID* waitEvents[2];

    NT_ASSERT(KeGetCurrentIrql() <= APC_LEVEL);

    waitEvents[0] = MainEventPtr;
    waitEvents[1] = (VOID*)StopEventPtr;

    status = KeWaitForMultipleObjects(
        ARRAYSIZE(waitEvents),
        waitEvents,
        WaitAny,
        Executive,
        KernelMode,
        FALSE,
        TimeoutPtr,
        NULL);
    if (!NT_SUCCESS(status)) {
        VCHIQ_LOG_ERROR(
            "Unexpected failure on trigger thread %!STATUS!",
            status);
    }

    return status;
}


VCHIQ_NONPAGED_SEGMENT_END