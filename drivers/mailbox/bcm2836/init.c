//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//
// Module Name:
//
//     init.c
//
// Abstract:
//
//     This file contain early initialization related definition
//     such as setting up MAC address.
//

#include "precomp.h"

#include "trace.h"
#include "init.tmh"

#include "register.h"
#include "device.h"
#include "interrupt.h"
#include "mailbox.h"
#include "init.h"

RPIQ_PAGED_SEGMENT_BEGIN

/*++

Routine Description:

    RpiqInitOperation is responsible to run initialize any mailbox
    related operation before the rest of the OS is booted.

Arguments:

    Device - A handle to a framework device object.

Return Value:

    NTSTATUS value

--*/
_Use_decl_annotations_
NTSTATUS RpiqInitOperation (
    WDFDEVICE Device,
    WDF_POWER_DEVICE_STATE PreviousState
    )
{
    NTSTATUS status;

    PAGED_CODE();

    // Only perform initialize if this is the first boot
    if (PreviousState != WdfPowerDeviceD3Final) {
        RPIQ_LOG_INFORMATION(
            "This is not first boot %d do nothing",
            PreviousState);
        status = STATUS_SUCCESS;
        goto End;
    }

    DEVICE_CONTEXT* deviceContextPtr = RpiqGetContext(Device);

    // Proceed to boot even if we fail to set MAC address
    status = RpiSetDeviceMacAddress(deviceContextPtr);
    if(!NT_SUCCESS(status)) {
        RPIQ_LOG_ERROR(
            "Failed to set initialize MAC Address %!STATUS!", 
            status);
    }
    status = STATUS_SUCCESS;
    
    // Finally enable interrupts
    status = RpiqEnableInterrupts(deviceContextPtr);
    if (!NT_SUCCESS(status)) {
        RPIQ_LOG_ERROR(
            "Failed to intialize interrupt status = %!STATUS!",
            status);
        goto End;
    }

End:
    
    return status;
}

/*++

Routine Description:

    RpiSetDeviceMacAddress would query the mailbox interface for the MAC 
    address and save it into registry. The GUID and device ID is based on RPi.
    Setting this early in boot time before network driver is loaded and before
    enabling mailbox interrupt.

Arguments:

    Device - A handle to a framework device object.

Return Value:
    
    NTSTATUS value

--*/

WCHAR macAddrStrGlobal[13] = { 0 };

_Use_decl_annotations_
NTSTATUS RpiSetDeviceMacAddress (
    DEVICE_CONTEXT* DeviceContextPtr
    )
{
    NTSTATUS status;
    PHYSICAL_ADDRESS HighestAcceptableAddress = { 0 };
    PHYSICAL_ADDRESS LowestAcceptableAddress = { 0 };
    PHYSICAL_ADDRESS BoundaryAddress = { 0 };
    PHYSICAL_ADDRESS addrProperty;
    MAILBOX_GET_MAC_ADDRESS* macAddrProperty;
    LARGE_INTEGER timeOut = { 0 };
    ULONG retries;

    PAGED_CODE();

    HighestAcceptableAddress.QuadPart = HEX_1_G - 1;

    // Firmware expects mailbox request to be in contiguous memory
    macAddrProperty = MmAllocateContiguousNodeMemory(
        sizeof(MAILBOX_GET_MAC_ADDRESS),
        LowestAcceptableAddress,
        HighestAcceptableAddress,
        BoundaryAddress,
        PAGE_NOCACHE | PAGE_READWRITE,
        MM_ANY_NODE_OK);
    if (macAddrProperty == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto End;
    }

    addrProperty = MmGetPhysicalAddress(macAddrProperty);

    INIT_MAILBOX_GET_BOARD_MAC_ADDRESS(macAddrProperty);
    status = RpiqMailboxWrite(
        DeviceContextPtr,
        MAILBOX_CHANNEL_PROPERTY_ARM_VC,
        addrProperty.LowPart + OFFSET_DIRECT_SDRAM,
        NULL); // We are polling so no WDF request to send down
    if (!NT_SUCCESS(status)) {
        RPIQ_LOG_ERROR(
            "Failed to set query MAC address %!STATUS!",
            status);
        goto End;
    }

    retries = 10;
    timeOut.QuadPart = WDF_REL_TIMEOUT_IN_MS(1);

    while (retries > 0){
        ULONG reg;
        // read the status to ack
        reg = READ_REGISTER_NOFENCE_ULONG(&DeviceContextPtr->Mailbox->Status);
        reg = READ_REGISTER_NOFENCE_ULONG(&DeviceContextPtr->Mailbox->Read) & ~MAILBOX_CHANNEL_MASK;
        if ((reg == (addrProperty.LowPart + OFFSET_DIRECT_SDRAM))) {
            // we check the if the request was a success
            if (macAddrProperty->Header.RequestResponse & RESPONSE_SUCCESS) {
                break;
            } else {
                status = STATUS_UNSUCCESSFUL;
                goto End;
            }
        }
        KeDelayExecutionThread(KernelMode, FALSE, &timeOut);
        --retries;
    }

    if (retries == 0) {
        status = STATUS_UNSUCCESSFUL;
        goto End;
    }

    // The next step is to save this mac address into global.
    // When got a notification of NDIS interface ready, this mac address will 
    // be write into registry.
    status = RtlStringCchPrintfW(
        macAddrStrGlobal,
        ARRAYSIZE(macAddrStrGlobal),
        L"%02X%02X%02X%02X%02X%02X",
        macAddrProperty->MACAddress[0],
        macAddrProperty->MACAddress[1],
        macAddrProperty->MACAddress[2],
        macAddrProperty->MACAddress[3],
        macAddrProperty->MACAddress[4],
        macAddrProperty->MACAddress[5]);
    if (!NT_SUCCESS(status)) {
        RPIQ_LOG_ERROR(
            "Failed to save MAC address global %!STATUS!",
            status);
        goto End;
    }

	RPIQ_LOG_INFORMATION("Save MAC addres %S in global", macAddrStrGlobal);

End:
    if (macAddrProperty){
        MmFreeContiguousMemory(macAddrProperty);
    }

    return status;
}

RPIQ_PAGED_SEGMENT_END