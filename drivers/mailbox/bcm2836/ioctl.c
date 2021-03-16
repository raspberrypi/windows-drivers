//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//
// Module Name:
//
//     ioctl.c
//
// Abstract:
//
//      Ioctl implementation of rpiq driver
//

#include "precomp.h"

#include "trace.h"
#include "ioctl.tmh"

#include "register.h"
#include "ioctl.h"
#include "device.h"
#include "mailbox.h"

RPIQ_PAGED_SEGMENT_BEGIN

/*++

Routine Description:

    RpiqDeviceControl main routine to complete mailbox related operation.

Arguments:

    Queue - A handle to the framework queue object that is associated with the
        I/O request.

    Request - A handle to a framework request object.

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
VOID RpiqProcessChannel (
    WDFQUEUE Queue,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode
    )
{
    NTSTATUS status;
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    DEVICE_CONTEXT *deviceContextPtr = RpiqGetContext(device);
    size_t sizeInput = 0, sizeOutput; 

    PAGED_CODE();

    RPIQ_LOG_INFORMATION(
        "Queue 0x%p, Request 0x%p OutputBufferLength %d"
        "InputBufferLength %d IoControlCode %d\n",
        Queue,
        Request,
        (int)OutputBufferLength,
        (int)InputBufferLength,
        IoControlCode);

    switch (IoControlCode)
    {
    case IOCTL_MAILBOX_VCHIQ:
        {
            ULONG* inputBufferPtr;

            status = WdfRequestRetrieveInputBuffer(
                Request,
                IOCTL_MAILBOX_VCHIQ_INPUT_BUFFER_SIZE,
                &inputBufferPtr,
                &sizeInput);
            if (!NT_SUCCESS(status)) {
                RPIQ_LOG_ERROR(
                    "WdfRequestRetrieveInputBuffer failed %!STATUS!\n",
                    status);
                goto CompleteRequest;
            }

            status = RpiqMailboxWrite(
                deviceContextPtr,
                inputBufferPtr[0],
                inputBufferPtr[1],
                NULL); // Vchiq ioctl is unique, refer comment below
            if (!NT_SUCCESS(status)) {
                RPIQ_LOG_ERROR(
                    "WdfRequestRetrieveInputBuffer failed %!STATUS!\n",
                    status);
                goto CompleteRequest;
            }

            // Interrupts are not generated for vchiq channel write so complete
            // the request here instead of in DPC.
            WdfRequestComplete(Request, STATUS_SUCCESS);
            break;
        }
    case IOCTL_MAILBOX_PROPERTY:
        {
            MAILBOX_HEADER* inputBufferPtr;
            MAILBOX_HEADER* outputBufferPtr;

            status = WdfRequestRetrieveInputBuffer(
                Request,
                sizeof(*inputBufferPtr),
                &inputBufferPtr,
                &sizeInput);
            if (!NT_SUCCESS(status)) {
                RPIQ_LOG_ERROR(
                    "WdfRequestRetrieveInputBuffer failed: %!STATUS!\n",
                    status);
                goto CompleteRequest;
            }

            if(inputBufferPtr->TotalBuffer != sizeInput) {
                RPIQ_LOG_ERROR("Input buffer mismatch \n");
                status = STATUS_INVALID_PARAMETER;
                goto CompleteRequest;
            }

            status = WdfRequestRetrieveOutputBuffer(
                Request,
                inputBufferPtr->TotalBuffer,
                &outputBufferPtr,
                &sizeOutput);
            if(!NT_SUCCESS(status)) {
                RPIQ_LOG_ERROR(
                    "WdfRequestRetrieveOutputBuffer failed : %!STATUS!\n",
                    status);
                goto CompleteRequest;
            }

            // Input and output buffer should be the same size
            if (sizeOutput != sizeInput) {
                RPIQ_LOG_ERROR("Input and output buffer mismatch\n");
                status = STATUS_INVALID_PARAMETER;
                goto CompleteRequest;
            }

            status = RpiqMailboxProperty(
                deviceContextPtr,
                inputBufferPtr,
                (ULONG)sizeInput,
                MAILBOX_CHANNEL_PROPERTY_ARM_VC,
                Request);
            if(!NT_SUCCESS(status)) {
                RPIQ_LOG_ERROR(
                    "RpiqMailboxProperty failed %!STATUS!\n",
                    status);
                goto CompleteRequest;
            }
        }
        break;
    // Currently no support for unused mailbox channel
    case IOCTL_MAILBOX_POWER_MANAGEMENT:
    case IOCTL_MAILBOX_FRAME_BUFFER:
    case IOCTL_MAILBOX_VIRT_UART:
    case IOCTL_MAILBOX_LED:
    case IOCTL_MAILBOX_BUTTONS:
    case IOCTL_MAILBOX_TOUCH_SCREEN:
    default:
        RPIQ_LOG_WARNING("Unsupported IOCTL");
        status = STATUS_NOT_SUPPORTED;
        goto CompleteRequest;
    }

    return;

CompleteRequest:
    if (!NT_SUCCESS(status)) {
        RPIQ_LOG_ERROR(
            "RpiqProcessChannel failed ( %!STATUS!)",
            status);
    }

    WdfRequestComplete(Request, status);

    return;
}

RPIQ_PAGED_SEGMENT_END