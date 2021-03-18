//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//
// Module Name:
//
//     driver.c
//
// Abstract:
//
//     This file contains the driver entry points and callbacks.
//

#include "precomp.h"

#include "trace.h"
#include "driver.tmh"

#include "driver.h"
#include "register.h"
#include "device.h"

RPIQ_INIT_SEGMENT_BEGIN

/*++
Routine Description:

    Create the driver object

Parameters Description:

    DriverObject - represents the instance of the function driver that is loaded
    into memory. DriverEntry must initialize members of DriverObject before it
    returns to the caller. DriverObject is allocated by the system before the
    driver is loaded, and it is released by the system after the system unloads
    the function driver from memory.

    RegistryPath - represents the driver specific path in the Registry.
    The function driver can use the path to store driver related data between
    reboots. The path does not store hardware instance specific data.

Return Value:

    STATUS_SUCCESS if successful,
    STATUS_UNSUCCESSFUL otherwise.

--*/
_Use_decl_annotations_
NTSTATUS DriverEntry(
    PDRIVER_OBJECT  DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;

    ExInitializeDriverRuntime(0);

    // Initialize WPP Tracing
    {
        WPP_INIT_TRACING(DriverObject, RegistryPath);
        RECORDER_CONFIGURE_PARAMS recorderConfigureParams;
        RECORDER_CONFIGURE_PARAMS_INIT(&recorderConfigureParams);
        WppRecorderConfigure(&recorderConfigureParams);
    }

    // Register a cleanup callback so that we can call WPP_CLEANUP when
    // the framework driver object is deleted during driver unload.
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

    WDF_DRIVER_CONFIG_INIT(&config, RpiqOnDeviceAdd);
    config.EvtDriverUnload = RpiqOnDriverUnload;

    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        &attributes,
        &config,
        WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        RPIQ_LOG_ERROR(
            "WdfDriverCreate failed %!STATUS!", 
            status);
        WPP_CLEANUP(DriverObject);
    }
    
    return status;
}

RPIQ_INIT_SEGMENT_END

RPIQ_PAGED_SEGMENT_BEGIN

/*++

Routine Description:

    Called when driver is unloaded. Clean up WPP.

Arguments:

    Driver - Handle to a framework driver object created in DriverEntry

Return Value:

    NTSTATUS

--*/
_Use_decl_annotations_
VOID RpiqOnDriverUnload (  
    WDFDRIVER Driver
    )
{
    PDRIVER_OBJECT driverObject;

    PAGED_CODE();

    RPIQ_LOG_INFORMATION("Driver unloaded");

    driverObject = WdfDriverWdmGetDriverObject(Driver); 

    WPP_CLEANUP(driverObject);
}

/*++

Routine Description:

    RpiqOnDeviceAdd is called by the framework in response to AddDevice
    call from the PnP manager. We create and initialize a device object to
    represent a new instance of the device.

Arguments:

    Driver - Handle to a framework driver object created in DriverEntry

    DeviceInit - Pointer to a framework-allocated WDFDEVICE_INIT structure.

Return Value:

    NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS RpiqOnDeviceAdd (
    WDFDRIVER       Driver,
    PWDFDEVICE_INIT DeviceInit
    )
{
    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    return RpiqCreateDevice(Driver, DeviceInit);
}

RPIQ_PAGED_SEGMENT_END
