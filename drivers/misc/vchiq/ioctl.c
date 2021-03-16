//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//
// Module Name:
//
//      ioctl.c
//
// Abstract:
//
//        Ioctl implementation of vchiq driver
//

#include "precomp.h"

#include "trace.h"
#include "ioctl.tmh"

#include "slotscommon.h"
#include "device.h"
#include "file.h"
#include "slots.h"
#include "ioctl.h"

VCHIQ_PAGED_SEGMENT_BEGIN

/*++

Routine Description:

     VchiqIoDeviceControl handles VCHIQ IOCTL.

Arguments:

     DeviceContextPtr - Pointer to device context

     WdfRequest - A handle to a framework request object.

     OutputBufferLength - The length, in bytes, of the request's output buffer,
          if an output buffer is available.

     InputBufferLength - The length, in bytes, of the request's input buffer,
          if an input buffer is available.

     IoControlCode - The driver-defined or system-defined I/O control code
          (IOCTL) that is associated with the request.

Return Value:

     None

--*/
_Use_decl_annotations_
VOID VchiqIoDeviceControl (
    WDFQUEUE Queue,
    WDFREQUEST WdfRequest,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode
    )
{
    NTSTATUS status;
    VCHIQ_FILE_CONTEXT* vchiqFileContextPtr;
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    DEVICE_CONTEXT* deviceContextPtr = VchiqGetDeviceContext(device);

    PAGED_CODE();

    if (deviceContextPtr->VCConnected == FALSE) {
        VCHIQ_LOG_WARNING("VCHIQ interface not ready");
        status = STATUS_DEVICE_NOT_READY;
        goto CompleteRequest;
    }

    // First process IOCTL that does not require a file context
    switch (IoControlCode)
    {
    case IOCTL_VCHIQ_CONNECT:
        {
            // Connect is a simple ioctl to ensure that we have establish 
            // connection with VC firmware. Connection is establish by 
            // initiating the slots which if we reach this point has 
            // already been initialize. We just send a message to notify
            // the firmware too
            WDFFILEOBJECT wdfFileObject = WdfRequestGetFileObject(WdfRequest);
            if (wdfFileObject == NULL) {
                WDF_REQUEST_PARAMETERS wdfRequestParameters;
                WdfRequestGetParameters(WdfRequest, &wdfRequestParameters);
                VCHIQ_LOG_ERROR(
                    "Fail to retrieve file object. \
                         (WdfRequest = 0x%p, Type = 0x%lx)",
                    WdfRequest,
                    (ULONG)wdfRequestParameters.Type);
                status = STATUS_INTERNAL_ERROR;
                goto CompleteRequest;
            }

            // Create a file context here as vchiq_arm would immediately begin
            // to send IOCTL to wait for completion message
            vchiqFileContextPtr = VchiqGetFileContext(wdfFileObject);
            if (vchiqFileContextPtr != NULL) {
                VCHIQ_LOG_ERROR(
                    "Caller has already connected to a service");
                status = STATUS_UNSUCCESSFUL;
                goto CompleteRequest;
            }

            status = VchiqAllocateFileObjContext(
                deviceContextPtr,
                wdfFileObject,
                &vchiqFileContextPtr);
            if (!NT_SUCCESS (status)) {
                VCHIQ_LOG_ERROR(
                    "VchiqAllocateFileObjContext failed (%!STATUS!)",
                    status);
                goto CompleteRequest;
            }

            status = VchiqQueueMessageAsync(
                deviceContextPtr,
                vchiqFileContextPtr,
                VCHIQ_MAKE_MSG(VCHIQ_MSG_CONNECT, 0, 0),
                NULL,
                0);
            if (!NT_SUCCESS (status)) {
                VCHIQ_LOG_ERROR(
                    "VchiqQueueMessageAsync failed (%!STATUS!)",
                    status);
                goto CompleteRequest;
            }

            goto CompleteRequest;
        }
    case IOCTL_VCHIQ_GET_CONFIG:
        {
            VCHIQ_CONFIG vchiqCurrentConfig =
                {
                    VCHIQ_MAX_MSG_SIZE,
                    VCHIQ_MAX_MSG_SIZE,
                    VCHIQ_NUM_SERVICE_BULKS,
                    VCHIQ_MAX_SERVICES,
                    VCHIQ_VERSION,
                    VCHIQ_VERSION_MIN
                };

            VCHIQ_GET_CONFIG* clientConfigPtr;
            status = WdfRequestRetrieveInputBuffer(
                WdfRequest,
                sizeof(*clientConfigPtr),
                &clientConfigPtr,
                NULL);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "WdfRequestRetrieveInputBuffer failed (%!STATUS!)",
                    status);
                goto CompleteRequest;
            }

            // Ensure that the buffer provided is not too big.
            if (clientConfigPtr->ConfigSize > sizeof(VCHIQ_CONFIG)) {
                VCHIQ_LOG_WARNING("Config buffer too big");
                status = STATUS_INVALID_PARAMETER;
                goto CompleteRequest;
            }

            VCHIQ_CONFIG* configurationPtr;
            size_t bufferSize;
            configurationPtr = WdfMemoryGetBuffer(
                clientConfigPtr->WdfMemoryConfiguration,
                &bufferSize);
            if ((configurationPtr == NULL) ||
                (bufferSize != sizeof(*configurationPtr))) {
                VCHIQ_LOG_ERROR(
                    "Caller provided invalid VCHIQ_CONFIG buffer 0x%p %lld",
                    configurationPtr,
                    bufferSize);
                status = STATUS_INVALID_PARAMETER;
                goto CompleteRequest;
            }

            RtlCopyMemory(
                configurationPtr,
                &vchiqCurrentConfig, 
                sizeof(*clientConfigPtr->PConfig));

            status = STATUS_SUCCESS;
        }
        goto CompleteRequest;
    case IOCTL_VCHIQ_LIB_VERSION:
        {
            ULONG* libVersion;
            status = WdfRequestRetrieveInputBuffer(
                WdfRequest,
                sizeof(*libVersion),
                &libVersion,
                NULL);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "WdfRequestRetrieveInputBuffer failed (%!STATUS!)",
                    status);
                goto CompleteRequest;
            }

            if (*libVersion < VCHIQ_VERSION_MIN) {
                VCHIQ_LOG_ERROR(
                    "Library version %d unsupported",
                    *libVersion);
                status = STATUS_NOT_SUPPORTED;
                goto CompleteRequest;
            }
            status = STATUS_SUCCESS;
        }
        goto CompleteRequest;
    case IOCTL_VCHIQ_CREATE_SERVICE:
    default:
        {
            WDFFILEOBJECT wdfFileObject = WdfRequestGetFileObject(WdfRequest);
            if (wdfFileObject == NULL) {
                WDF_REQUEST_PARAMETERS wdfRequestParameters;
                WdfRequestGetParameters(WdfRequest, &wdfRequestParameters);
                VCHIQ_LOG_ERROR(
                    "Fail to retrieve file object. \
                         (WdfRequest = 0x%p, Type = 0x%lx)",
                    WdfRequest,
                    (ULONG)wdfRequestParameters.Type);
                status = STATUS_INTERNAL_ERROR;
                goto CompleteRequest;
            }

            vchiqFileContextPtr = VchiqGetFileContext(wdfFileObject);
            if ((vchiqFileContextPtr == NULL) &&
                (IoControlCode == IOCTL_VCHIQ_CREATE_SERVICE)) {

                // Functional test does not call connect prior to a create
                // service call, so we allocate a file context here instead
                status = VchiqAllocateFileObjContext(
                    deviceContextPtr,
                    wdfFileObject,
                    &vchiqFileContextPtr);
                if (!NT_SUCCESS (status)) {
                    VCHIQ_LOG_ERROR(
                        "VchiqAllocateFileObjContext failed (%!STATUS!)",
                        status);
                    goto CompleteRequest;
                }
            } else if (vchiqFileContextPtr == NULL) {
                VCHIQ_LOG_ERROR(
                    "Caller has not connected to a service %d",
                    ((IoControlCode >> 2) & 0x0FFF));
                status = STATUS_UNSUCCESSFUL;
                goto CompleteRequest;
            }
        }
    }

    // The following IOCTL are process for client that has already allocated
    // a file context. Which also means it has call a IOCTL_VCHIQ_CONNECT ioctl
    switch (IoControlCode)
    {
    case IOCTL_VCHIQ_CREATE_SERVICE:
        {
            VCHIQ_CREATE_SERVICE* createServicePtr;
            status = WdfRequestRetrieveInputBuffer(
                WdfRequest,
                sizeof(*createServicePtr),
                &createServicePtr,
                NULL);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "WdfRequestRetrieveInputBuffer failed (%!STATUS!)",
                    status);
                goto CompleteRequest;
            }

            vchiqFileContextPtr->IsVchi = createServicePtr->IsVchi;

            // Internal structure used by vchiq driver but not declared
            // in common headers.
            VCHIQ_OPEN_PAYLOAD createServicePayload;

            createServicePayload.FourCC = 
                createServicePtr->Params.FourCC;
            createServicePayload.ClientId = 
                vchiqFileContextPtr->ArmPortNumber;
            createServicePayload.Version = 
                createServicePtr->Params.Version;
            createServicePayload.VersionMin = 
                createServicePtr->Params.VersionMin;
            vchiqFileContextPtr->ServiceUserData =
                createServicePtr->Params.UserData;

            status = VchiqUpdateQueueDispatchMessage(
                deviceContextPtr,
                vchiqFileContextPtr,
                WdfRequest,
                vchiqFileContextPtr->FileQueue[FILE_QUEUE_CREATE_SERVICE],
                VCHIQ_MAKE_MSG(
                    VCHIQ_MSG_OPEN,
                    vchiqFileContextPtr->ArmPortNumber,
                    0),
                &createServicePayload,
                sizeof(createServicePayload));
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "VchiqUpdateQueueDispatchMessage failed ( %!STATUS!)",
                    status);
                goto CompleteRequest;
            }
        }
        goto End;
    case IOCTL_VCHIQ_SHUTDOWN:
        {
            WdfIoQueuePurge(
                vchiqFileContextPtr->FileQueue[FILE_QUEUE_PENDING_MSG],
                WDF_NO_EVENT_CALLBACK,
                WDF_NO_CONTEXT);

            status = STATUS_SUCCESS;
            goto CompleteRequest;
        }
        goto End;
    case IOCTL_VCHIQ_REMOVE_SERVICE:
    case IOCTL_VCHIQ_CLOSE_SERVICE:
        {
            // The current VCHIQ driver doesn't manage any service state. This 
            // would be an ideal time to clean up service and close pending
            // request. Although when a file handle is close all queues are 
            // purged at that point.
            status = VchiqUpdateQueueDispatchMessage(
                deviceContextPtr,
                vchiqFileContextPtr,
                WdfRequest,
                vchiqFileContextPtr->FileQueue[FILE_QUEUE_CLOSE_SERVICE],
                VCHIQ_MAKE_MSG(
                    VCHIQ_MSG_CLOSE,
                    vchiqFileContextPtr->ArmPortNumber,
                    vchiqFileContextPtr->VCHIQPortNumber),
                NULL,
                0);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "VchiqUpdateQueueDispatchMessage failed ( %!STATUS!)",
                    status);
                goto CompleteRequest;
            }
        }
        goto End;
    case IOCTL_VCHIQ_QUEUE_MSG:
        {
            VCHIQ_QUEUE_MESSAGE* messageBufferPtr;

            status = WdfRequestRetrieveInputBuffer(
                WdfRequest,
                sizeof(*messageBufferPtr),
                &messageBufferPtr,
                NULL);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "WdfRequestRetrieveInputBuffer failed (%!STATUS!)",
                    status);
                goto CompleteRequest;
            }

            VCHIQ_ELEMENT* elementsPtr = WdfMemoryGetBuffer(
                messageBufferPtr->WdfMemoryElementBuffer,
                NULL);
            if (messageBufferPtr->Count == 1) {
                VOID* elementDataPtr = WdfMemoryGetBuffer(
                    elementsPtr->WdfMemoryData,
                    NULL);

                status = VchiqQueueMessageAsync(
                    deviceContextPtr,
                    vchiqFileContextPtr,
                    VCHIQ_MAKE_MSG(
                        VCHIQ_MSG_DATA,
                        vchiqFileContextPtr->ArmPortNumber,
                        vchiqFileContextPtr->VCHIQPortNumber),
                    elementDataPtr,
                    elementsPtr->Size);
                if (!NT_SUCCESS(status)) {
                    VCHIQ_LOG_ERROR(
                        "VchiqQueueMessageAsync failed ( %!STATUS!)",
                        status);
                }
            } else {
                status = VchiqQueueMultiElementAsync(
                    deviceContextPtr,
                    vchiqFileContextPtr,
                    VCHIQ_MAKE_MSG(
                        VCHIQ_MSG_DATA,
                        vchiqFileContextPtr->ArmPortNumber,
                        vchiqFileContextPtr->VCHIQPortNumber),
                    elementsPtr,
                    messageBufferPtr->Count);
                if (!NT_SUCCESS(status)) {
                    VCHIQ_LOG_ERROR(
                        "VchiqQueueMessageAsync failed ( %!STATUS!)",
                        status);
                }
            }

            goto CompleteRequest;
        }
        goto End;
    case IOCTL_VCHIQ_BULK_TRANSMIT:
        {
            VCHIQ_QUEUE_BULK_TRANSFER* bulkTransferPtr;

            if (InputBufferLength == 0) {
                VCHIQ_LOG_WARNING("No input buffer for bulk transmit");
                status = STATUS_INVALID_PARAMETER;
                goto CompleteRequest;
            }

            if (OutputBufferLength < sizeof(*bulkTransferPtr)) {
                VCHIQ_LOG_WARNING("No input buffer for bulk transmit");
                status = STATUS_INVALID_PARAMETER;
                goto CompleteRequest;
            }

            // Input buffer contains the buffer to be transfer for both
            // transfer and receive. This allow us to simplify obtaining
            // MDL for the buffer as we can request WDF to provide it
            MDL* bufferMdl;
            status = WdfRequestRetrieveInputWdmMdl(WdfRequest, &bufferMdl);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "WdfRequestRetrieveInputWdmMdl failed (%!STATUS!)",
                    status);
                goto CompleteRequest;
            }

            // Output buffer holds the bulk transfer structure
            status = WdfRequestRetrieveOutputBuffer(
                WdfRequest,
                sizeof(*bulkTransferPtr),
                &bulkTransferPtr,
                NULL);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "WdfRequestRetrieveOutputBuffer failed %!STATUS!",
                    status);
                goto CompleteRequest;
            }

            // Currently only support blocking call. Consider adding
            // non blocking call support.
            status = VchiqProcessBulkTransfer(
                deviceContextPtr,
                vchiqFileContextPtr,
                WdfRequest,
                bulkTransferPtr,
                VCHIQ_MSG_BULK_TX,
                bufferMdl,
                MmGetMdlByteCount(bufferMdl));
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "VchiqProcessBulkTransfer failed (%!STATUS!)",
                    status);
                goto CompleteRequest;
            }
        }
        goto End;
    case IOCTL_VCHIQ_BULK_RECEIVE:
        {
            VCHIQ_QUEUE_BULK_TRANSFER* bulkTransferPtr;

            if (OutputBufferLength == 0) {
                VCHIQ_LOG_WARNING("No output buffer for bulk transmit");
                status = STATUS_INVALID_PARAMETER;
                goto CompleteRequest;
            }

            if (InputBufferLength < sizeof(*bulkTransferPtr)) {
                VCHIQ_LOG_WARNING("No input buffer for bulk transmit");
                status = STATUS_INVALID_PARAMETER;
                goto CompleteRequest;
            }

            MDL* bufferMdl;
            status = WdfRequestRetrieveOutputWdmMdl(WdfRequest, &bufferMdl);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "WdfRequestRetrieveOutputWdmMdl failed (%!STATUS!)",
                    status);
                goto CompleteRequest;
            }

            // Input buffer holds the bulk transfer structure
            status = WdfRequestRetrieveInputBuffer(
                WdfRequest,
                sizeof(*bulkTransferPtr),
                &bulkTransferPtr,
                NULL);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "WdfRequestRetrieveInputBuffer failed %!STATUS!",
                    status);
                goto CompleteRequest;
            }

            status = VchiqProcessBulkTransfer(
                deviceContextPtr,
                vchiqFileContextPtr,
                WdfRequest,
                bulkTransferPtr,
                VCHIQ_MSG_BULK_RX,
                bufferMdl,
                MmGetMdlByteCount(bufferMdl));
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "VchiqProcessBulkTransfer failed (%!STATUS!)",
                    status);
                goto CompleteRequest;
            }
        }
        goto End;
    case IOCTL_VCHIQ_AWAIT_COMPLETION:
        {
            status = WdfRequestForwardToIoQueue(
                WdfRequest,
                vchiqFileContextPtr->FileQueue[FILE_QUEUE_PENDING_MSG]);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_WARNING(
                    "WdfRequestForwardToIoQueue failed (%!STATUS!)",
                    status);
                goto CompleteRequest;
            }

            ExAcquireFastMutex(&vchiqFileContextPtr->PendingDataMsgMutex);

            status = VchiqProcessPendingMsg(
                deviceContextPtr,
                vchiqFileContextPtr);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "VchiqProcessPendingMsg failed  %!STATUS!",
                    status);
                ExReleaseFastMutex(&vchiqFileContextPtr->PendingDataMsgMutex);
                goto CompleteRequest;
            }

            ExReleaseFastMutex(&vchiqFileContextPtr->PendingDataMsgMutex);
        }
        goto End;
    case IOCTL_DEQUEUE_MESSAGE:
        {
            VCHIQ_DEQUEUE_MESSAGE* dequeueMsgPtr;
            
            status = WdfRequestRetrieveInputBuffer(
                WdfRequest,
                sizeof(*dequeueMsgPtr),
                &dequeueMsgPtr,
                NULL);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "WdfRequestRetrieveInputBuffer failed %!STATUS!",
                    status);
                goto CompleteRequest;
            }

            ExAcquireFastMutex(&vchiqFileContextPtr->PendingVchiMsgMutex);

            // If caller did not request blocking and no entries are available
            // return immediately with an error.
            if (!dequeueMsgPtr->Blocking) {
                if (IsListEmpty(&vchiqFileContextPtr->PendingVchiMsgList)) {
                    WdfRequestComplete(WdfRequest, STATUS_NO_MORE_ENTRIES);
                    ExReleaseFastMutex(&vchiqFileContextPtr->PendingVchiMsgMutex);
                    goto End;
                }
            }

            status = WdfRequestForwardToIoQueue(
                WdfRequest,
                vchiqFileContextPtr->FileQueue[FILE_QUEUE_PENDING_VCHI_MSG]);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "WdfRequestForwardToIoQueue failed (%!STATUS!)",
                    status);
                WdfRequestComplete(WdfRequest, status);
                ExReleaseFastMutex(&vchiqFileContextPtr->PendingVchiMsgMutex);
                goto End;
            }

            status = VchiqProcessPendingVchiMsg(
                deviceContextPtr,
                vchiqFileContextPtr);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "VchiqProcessPendingVchiMsg failed  %!STATUS!",
                    status);
                WdfRequestComplete(WdfRequest, status);
                ExReleaseFastMutex(&vchiqFileContextPtr->PendingVchiMsgMutex);
                goto End;
            }

            ExReleaseFastMutex(&vchiqFileContextPtr->PendingVchiMsgMutex);
        }
        goto End;
    case IOCTL_VCHIQ_CLOSE_DELIVERED:
        {
            // This is vchi specific IOCTL, for now just complete as success.
            status = STATUS_SUCCESS;
            goto CompleteRequest;
        }
    case IOCTL_VCHIQ_USE_SERVICE:
    case IOCTL_VCHIQ_RELEASE_SERVICE:
        {
            // VCHIQ does not support service related management. For now
            // just successfully complete any service related IOCTL.
            status = STATUS_SUCCESS;
            goto CompleteRequest;
        }
    case IOCTL_VCHIQ_SET_SERVICE_OPTION:
        {
            VCHIQ_SET_SERVICE_OPTION* serviceOptionPtr;

            // Only perform parameter checking for now
            status = WdfRequestRetrieveInputBuffer(
                WdfRequest,
                sizeof(*serviceOptionPtr),
                &serviceOptionPtr,
                NULL);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "WdfRequestRetrieveInputBuffer failed %!STATUS!",
                    status);
                goto CompleteRequest;
            }

            if ((serviceOptionPtr->Option < VCHIQ_SERVICE_OPTION_AUTOCLOSE) ||
                (serviceOptionPtr->Option > VCHIQ_SERVICE_OPTION_TRACE)) {
                VCHIQ_LOG_WARNING("Invalid service option");
                goto CompleteRequest;
            }

            status = STATUS_SUCCESS;
            goto CompleteRequest;
        }
        goto End;
    default:
        {
            VCHIQ_LOG_ERROR(
                "Unsupported VCHIQ IOCTL %d",
                ((IoControlCode >> 2) & 0x0FFF));
            status = STATUS_NOT_SUPPORTED;
            goto CompleteRequest;
        }
        goto End;
    }

CompleteRequest:

    WdfRequestComplete(WdfRequest, status);

End:

    return;
}

/*++

Routine Description:

    VchiqUpdateQueueDispatchMessage updates the respective queue
    and dispatch message to the slog.

Arguments:

    DeviceContextPtr - Pointer to device context

    VchiqFileContextPtr - File context pointer returned to caller

    WdfRequest - Framework request associated with this message

    MsgQueue - Message queue to be updated.

    MessageId - Slot message id

    BufferPtr - Slot buffer

    BufferSize - Slot buffer size.

Return Value:

    None

--*/
_Use_decl_annotations_
NTSTATUS VchiqUpdateQueueDispatchMessage (
    DEVICE_CONTEXT* DeviceContextPtr,
    VCHIQ_FILE_CONTEXT* VchiqFileContextPtr,
    WDFREQUEST WdfRequest,
    WDFQUEUE MsgQueue,
    ULONG MessageId,
    VOID* BufferPtr,
    ULONG BufferSize
    )
{
    NTSTATUS status;

    PAGED_CODE();

    if (WdfRequest && MsgQueue) {
        status = WdfRequestForwardToIoQueue(
            WdfRequest,
            MsgQueue);
        if (!NT_SUCCESS(status)) {
            VCHIQ_LOG_ERROR(
                "WdfRequestForwardToIoQueue failed ( %!STATUS!)",
                status);
            goto End;
        }
    }

    status = VchiqQueueMessageAsync(
        DeviceContextPtr,
        VchiqFileContextPtr,
        MessageId,
        BufferPtr,
        BufferSize);
    if (!NT_SUCCESS(status)) {
        NTSTATUS tempStatus;
        WDFREQUEST removeRequest;

        VCHIQ_LOG_ERROR(
            "VchiqQueueMessageAsync failed (%!STATUS!)",
            status);

        if (WdfRequest && MsgQueue) {
            tempStatus = WdfIoQueueRetrieveFoundRequest(
                MsgQueue,
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

        goto End;
    }

End:
    return status;
}

VCHIQ_PAGED_SEGMENT_END

VCHIQ_NONPAGED_SEGMENT_BEGIN
/*++

Routine Description:

    VchiqInCallerContext is the driver EVT_WDF_IO_IN_CALLER_CONTEXT
       callback and is responsible to lock user mode pointer for 
       specific IOCTL. The function is made non-paged to support future 
       request from another driver that could be calling in dispatch level.

Arguments:

    Device - A handle to a framework device object.

    WdfRequest - A handle to a framework request object.

Return Value:

    None

--*/
_Use_decl_annotations_
VOID VchiqInCallerContext (
    WDFDEVICE Device,
    WDFREQUEST WdfRequest
    )
{
    NTSTATUS status;
    WDF_REQUEST_PARAMETERS  requestParams;

    WDF_REQUEST_PARAMETERS_INIT(&requestParams);
    WdfRequestGetParameters(
        WdfRequest,
        &requestParams);

    if (requestParams.Type != WdfRequestTypeDeviceControl) {
        goto End;
    }

    ULONG ioControlCode = 
        requestParams.Parameters.DeviceIoControl.IoControlCode;

    switch (ioControlCode)
    {
    case IOCTL_VCHIQ_GET_CONFIG:
        {
            VCHIQ_GET_CONFIG* clientConfigPtr;
            status = WdfRequestRetrieveInputBuffer(
                WdfRequest,
                sizeof(*clientConfigPtr),
                &clientConfigPtr,
                NULL);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "WdfRequestRetrieveInputBuffer failed (%!STATUS!)",
                    status);
                goto CompleteRequest;
            }

            BOOLEAN isUserMode = 
                (WdfRequestGetRequestorMode(WdfRequest) == UserMode);

            // Lock configuration pointer that would be fill when completed
            WDFMEMORY wdfMemoryBufferLock;
            if (isUserMode) {
                status = WdfRequestProbeAndLockUserBufferForWrite(
                    WdfRequest,
                    clientConfigPtr->PConfig,
                    sizeof(*clientConfigPtr->PConfig),
                    &wdfMemoryBufferLock);
                if (!NT_SUCCESS(status)) {
                    VCHIQ_LOG_ERROR(
                        "WdfRequestProbeAndLockUserBufferForWrite \
                        failed %!STATUS!",
                        status);
                    goto CompleteRequest;
                }
            } else {
                WDF_OBJECT_ATTRIBUTES attributes;
                WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
                attributes.ParentObject = WdfRequest;

                status = WdfMemoryCreatePreallocated(
                    &attributes,
                    clientConfigPtr->PConfig,
                    sizeof(*clientConfigPtr->PConfig), 
                    &wdfMemoryBufferLock);
                if (!NT_SUCCESS(status)) {
                    VCHIQ_LOG_ERROR(
                        "WdfMemoryCreatePreallocated failed %!STATUS!",
                        status);
                    goto CompleteRequest;
                }
            }

            clientConfigPtr->WdfMemoryConfiguration = wdfMemoryBufferLock;
        }
        break;
    case IOCTL_VCHIQ_QUEUE_MSG:
        {
            VCHIQ_QUEUE_MESSAGE* messageBufferPtr;

            status = WdfRequestRetrieveInputBuffer(
                WdfRequest,
                sizeof(*messageBufferPtr),
                &messageBufferPtr,
                NULL);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "WdfRequestRetrieveInputBuffer failed (%!STATUS!)",
                    status);
                goto CompleteRequest;
            }

            BOOLEAN isUserMode =
                (WdfRequestGetRequestorMode(WdfRequest) == UserMode);

            // First lock the element list
            WDFMEMORY wdfMemoryBufferLock;
            VCHIQ_ELEMENT* elementsPtr = messageBufferPtr->Elements;

            if (isUserMode) {
                status = WdfRequestProbeAndLockUserBufferForRead(
                    WdfRequest,
                    (VOID*)elementsPtr,
                    sizeof(*elementsPtr) * messageBufferPtr->Count,
                    &wdfMemoryBufferLock);
                if (!NT_SUCCESS(status)) {
                    VCHIQ_LOG_ERROR(
                        "WdfRequestProbeAndLockUserBufferForWrite \
                        failed %!STATUS!",
                        status);
                    goto CompleteRequest;
                }
            } else {
                WDF_OBJECT_ATTRIBUTES attributes;
                WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
                attributes.ParentObject = WdfRequest;

                if (messageBufferPtr->Count == 0) {
                    status = STATUS_INVALID_PARAMETER;
                    VCHIQ_LOG_WARNING("Incoming buffer count is zero");
                    goto CompleteRequest;
                }

                status = WdfMemoryCreatePreallocated(
                    &attributes,
                    (VOID*)elementsPtr,
                    sizeof(*elementsPtr) * messageBufferPtr->Count,
                    &wdfMemoryBufferLock);
                if (!NT_SUCCESS(status)) {
                    VCHIQ_LOG_ERROR(
                        "WdfMemoryCreatePreallocated failed %!STATUS!",
                        status);
                    goto CompleteRequest;
                }
            }

            messageBufferPtr->WdfMemoryElementBuffer = wdfMemoryBufferLock;

            // Next lock all the element of the user mode data
            for (ULONG elementsCount = 0;
                elementsCount < messageBufferPtr->Count;
                ++elementsCount) {

                if (isUserMode) {
                    status = WdfRequestProbeAndLockUserBufferForWrite(
                        WdfRequest,
                        elementsPtr[elementsCount].Data,
                        elementsPtr[elementsCount].Size,
                        &wdfMemoryBufferLock);
                    if (!NT_SUCCESS(status)) {
                        VCHIQ_LOG_ERROR(
                            "WdfRequestProbeAndLockUserBufferForWrite \
                        failed %!STATUS!. count %d",
                            status,
                            elementsCount);
                        goto CompleteRequest;
                    }
                } else {
                    WDF_OBJECT_ATTRIBUTES attributes;
                    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
                    attributes.ParentObject = WdfRequest;

                    if (elementsPtr[elementsCount].Size == 0) {
                        status = STATUS_INVALID_PARAMETER;
                        VCHIQ_LOG_WARNING("Incoming buffer size is zero");
                        goto CompleteRequest;
                    }

                    status = WdfMemoryCreatePreallocated(
                        &attributes,
                        elementsPtr[elementsCount].Data,
                        elementsPtr[elementsCount].Size,
                        &wdfMemoryBufferLock);
                    if (!NT_SUCCESS(status)) {
                        VCHIQ_LOG_ERROR(
                            "WdfMemoryCreatePreallocated failed %!STATUS!",
                            status);
                        goto CompleteRequest;
                    }
                }

                elementsPtr[elementsCount].WdfMemoryData = wdfMemoryBufferLock;
            }
        }
        break;
    case IOCTL_VCHIQ_AWAIT_COMPLETION:
        {
            VCHIQ_AWAIT_COMPLETION* awaitCompletionPtr;
            status = WdfRequestRetrieveInputBuffer(
                WdfRequest,
                sizeof(*awaitCompletionPtr),
                &awaitCompletionPtr,
                NULL);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "WdfRequestRetrieveInputBuffer failed %!STATUS!",
                    status);
                goto CompleteRequest;
            }

            BOOLEAN isUserMode =
                (WdfRequestGetRequestorMode(WdfRequest) == UserMode);

            // First lock the completion structures
            WDFMEMORY wdfMemoryBufferLock;

            if (isUserMode) {
                status = WdfRequestProbeAndLockUserBufferForWrite(
                    WdfRequest,
                    awaitCompletionPtr->Buf,
                    sizeof(*awaitCompletionPtr->Buf) * 
                        awaitCompletionPtr->Count,
                    &wdfMemoryBufferLock);
                if (!NT_SUCCESS(status)) {
                    VCHIQ_LOG_ERROR(
                        "WdfRequestProbeAndLockUserBufferForWrite \
                        failed %!STATUS!",
                        status);
                    goto CompleteRequest;
                }
            } else {
                WDF_OBJECT_ATTRIBUTES attributes;
                WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
                attributes.ParentObject = WdfRequest;

                if (awaitCompletionPtr->Count == 0) {
                    status = STATUS_INVALID_PARAMETER;
                    VCHIQ_LOG_WARNING("Incoming buffer count is zero");
                    goto CompleteRequest;
                }

                status = WdfMemoryCreatePreallocated(
                    &attributes,
                    awaitCompletionPtr->Buf,
                    sizeof(*awaitCompletionPtr->Buf) * 
                        awaitCompletionPtr->Count,
                    &wdfMemoryBufferLock);
                if (!NT_SUCCESS(status)) {
                    VCHIQ_LOG_ERROR(
                        "WdfMemoryCreatePreallocated failed %!STATUS!",
                        status);
                    goto CompleteRequest;
                }
            }

            awaitCompletionPtr->WdfMemoryCompletion = wdfMemoryBufferLock;

            // Now lock all the buffer pointers
            for (ULONG completionCount = 0; 
                 completionCount < awaitCompletionPtr->Count;
                 ++completionCount) { 

                if (isUserMode) {
                    status = WdfRequestProbeAndLockUserBufferForWrite(
                        WdfRequest,
                        awaitCompletionPtr->MsgBufs[completionCount],
                        awaitCompletionPtr->MsgBufSize,
                        &wdfMemoryBufferLock);
                    if (!NT_SUCCESS(status)) {
                        VCHIQ_LOG_ERROR(
                            "WdfRequestProbeAndLockUserBufferForWrite \
                        failed %!STATUS!. count %d",
                            status,
                            completionCount);
                        goto CompleteRequest;
                    }
                } else {
                    WDF_OBJECT_ATTRIBUTES attributes;
                    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
                    attributes.ParentObject = WdfRequest;

                    if (awaitCompletionPtr->MsgBufSize == 0) {
                        status = STATUS_INVALID_PARAMETER;
                        VCHIQ_LOG_WARNING("Incoming buffer size is zero");
                        goto CompleteRequest;
                    }

                    status = WdfMemoryCreatePreallocated(
                        &attributes,
                        awaitCompletionPtr->MsgBufs[completionCount],
                        awaitCompletionPtr->MsgBufSize,
                        &wdfMemoryBufferLock);
                    if (!NT_SUCCESS(status)) {
                        VCHIQ_LOG_ERROR(
                            "WdfMemoryCreatePreallocated failed %!STATUS!",
                            status);
                        goto CompleteRequest;
                    }
                }

                awaitCompletionPtr->Buf[completionCount].Header =
                    awaitCompletionPtr->MsgBufs[completionCount];
                awaitCompletionPtr->Buf[completionCount].WdfMemoryBuffer =
                    wdfMemoryBufferLock;
            }
        }
        break;
    case IOCTL_DEQUEUE_MESSAGE:
        {
            VCHIQ_DEQUEUE_MESSAGE* dequeueMsgPtr;

            status = WdfRequestRetrieveInputBuffer(
                WdfRequest,
                sizeof(*dequeueMsgPtr),
                &dequeueMsgPtr,
                NULL);
            if (!NT_SUCCESS(status)) {
                VCHIQ_LOG_ERROR(
                    "WdfRequestRetrieveInputBuffer failed %!STATUS!",
                    status);
                goto CompleteRequest;
            }

            BOOLEAN isUserMode =
                (WdfRequestGetRequestorMode(WdfRequest) == UserMode);

            WDFMEMORY wdfMemoryBufferLock;

            if (isUserMode) {
                status = WdfRequestProbeAndLockUserBufferForWrite(
                    WdfRequest,
                    dequeueMsgPtr->Buf,
                    dequeueMsgPtr->BufSize,
                    &wdfMemoryBufferLock);
                if (!NT_SUCCESS(status)) {
                    VCHIQ_LOG_ERROR(
                        "WdfRequestProbeAndLockUserBufferForWrite \
                        failed %!STATUS!",
                        status);
                    goto CompleteRequest;
                }
            } else {
                WDF_OBJECT_ATTRIBUTES attributes;
                WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
                attributes.ParentObject = WdfRequest;

                if (dequeueMsgPtr->BufSize == 0) {
                    status = STATUS_INVALID_PARAMETER;
                    VCHIQ_LOG_WARNING("Incoming buffer size is zero");
                    goto CompleteRequest;
                }

                status = WdfMemoryCreatePreallocated(
                    &attributes,
                    dequeueMsgPtr->Buf,
                    dequeueMsgPtr->BufSize,
                    &wdfMemoryBufferLock);
                if (!NT_SUCCESS(status)) {
                    VCHIQ_LOG_ERROR(
                        "WdfMemoryCreatePreallocated failed %!STATUS!",
                        status);
                    goto CompleteRequest;
                }
            }

            dequeueMsgPtr->WdfMemoryBuffer = wdfMemoryBufferLock;
        }
        break;
    default:
        break;
    }

End:
    status = WdfDeviceEnqueueRequest(Device, WdfRequest);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(WdfRequest, status);
        VCHIQ_LOG_ERROR(
            "WdfDeviceEnqueueRequest failed ( %!STATUS!)",
            status);
    }

    return;

CompleteRequest:
    if (!NT_SUCCESS(status)) {
        VCHIQ_LOG_ERROR(
            "VchiqInCallerContext failed ( %!STATUS!)",
            status);
    }
    WdfRequestComplete(WdfRequest, status);

    return;
}

VCHIQ_NONPAGED_SEGMENT_END