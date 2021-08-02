//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//
// Module Name: 
//
//    PL011driver.cpp
//
// Abstract:
//
//    This module contains the ARM PL011 UART controller driver entry functions.
//    This controller driver uses the Serial WDF class extension (SerCx2).
//
// Environment:
//
//    kernel-mode only
//
#include "precomp.h"
#pragma hdrstop

#define _PL011_DRIVER_CPP_

// Logging header files
#include "PL011logging.h"
#include "PL011driver.tmh"

// Common driver header files
#include "PL011common.h"

// Module specific header files
#include "PL011driver.h"


#ifdef ALLOC_PRAGMA
    #pragma alloc_text(INIT, DriverEntry)
    #pragma alloc_text(PAGE, PL011EvtDriverUnload)
    #pragma alloc_text(PAGE, PL011EvtDeviceAdd)

    #pragma alloc_text(PAGE, PL011pDriverReadConfig)
#endif


//
// Routine Description:
//
//  Installable driver initialization entry point.
//  This entry point is called directly by the I/O system.
//
// Arguments:
//
//  DriverObjectPtr - pointer to the driver object
//
//  RegistryPathPtr - pointer to a Unicode string representing the path,
//      to driver-specific key in the registry.
//
// Return Value:
//
//    STATUS_SUCCESS, or appropriate error code
//
_Use_decl_annotations_
NTSTATUS
DriverEntry(
    DRIVER_OBJECT* DriverObjectPtr,
    UNICODE_STRING* RegistryPathPtr
    )
{
    NTSTATUS status;

    PAGED_CODE();

    //
    // Initialize tracing
    //
    {
        WPP_INIT_TRACING(DriverObjectPtr, RegistryPathPtr);

        RECORDER_CONFIGURE_PARAMS recorderConfigureParams;
        RECORDER_CONFIGURE_PARAMS_INIT(&recorderConfigureParams);

        WppRecorderConfigure(&recorderConfigureParams);
#if DBG
        WPP_RECORDER_LEVEL_FILTER(PL011_TRACING_VERBOSE) = TRUE;
#endif // DBG

    } // Tracing

    //
    // Create the PL011 WDF driver object
    //
    {
        WDF_OBJECT_ATTRIBUTES attributes;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, PL011_DRIVER_EXTENSION);

        WDF_DRIVER_CONFIG wdfDriverConfig;
        WDF_DRIVER_CONFIG_INIT(&wdfDriverConfig, PL011EvtDeviceAdd);
        wdfDriverConfig.EvtDriverUnload = PL011EvtDriverUnload;

        //
        // Specify a pool tag for allocations WDF makes on our behalf
        //
        wdfDriverConfig.DriverPoolTag = ULONG(PL011_ALLOC_TAG::PL011_ALLOC_TAG_WDF);

        status = WdfDriverCreate(
            DriverObjectPtr,
            RegistryPathPtr,
            &attributes,
            &wdfDriverConfig,
            WDF_NO_HANDLE
            );
        if (!NT_SUCCESS(status)) {
            PL011_LOG_ERROR(
                "WdfDriverCreate failed, (status = %!STATUS!)", status
                );
            goto done;
        }

    } // WDF driver 

    //
    // Read driver configuration parameters from registry.
    //
    status = PL011pDriverReadConfig();
    if (!NT_SUCCESS(status)) {
        PL011_LOG_ERROR(
            "PL011DriverReadConfig failed, (status = %!STATUS!)", status
            );
        goto done;
    }

done:

    //
    // Cleanup...
    //
    if (!NT_SUCCESS(status)) {

        WPP_CLEANUP(DriverObjectPtr);
    }

    return status;
}


//
// Routine Description:
//
//  PL011EvtDriverUnload is called by the framework just before the driver 
//  is unloaded. We use it to call WPP_CLEANUP.
//
// Arguments:
//
//  WdfDriver - Handle to a framework driver object created in DriverEntry
//
//
// Return Value:
//
_Use_decl_annotations_
VOID
PL011EvtDriverUnload(
    WDFDRIVER WdfDriver
    )
{
    PAGED_CODE();

    WPP_CLEANUP(WdfDriverWdmGetDriverObject(WdfDriver));
}


//
// Routine Description:
//
//  PL011pDriverReadConfig is called by DriverEntry to
//  read driver configuration parameters from registry.
//  These configuration parameters are read from registry since there
//  is no standard way to get them through UEFI.
//
//  The configuration parameters reside at:
//  HKLM\System\CurrentControlSet\Services\SerPL011\Parameters
//
// Arguments:
//
// Return Value:
//
//  Registry open/read status code.
//
_Use_decl_annotations_
NTSTATUS
PL011pDriverReadConfig()
{
    PAGED_CODE();

    //
    // Destination base address, the driver extension.
    //
    PL011_DRIVER_EXTENSION* drvExtPtr = 
        PL011DriverGetExtension(WdfGetDriver());

    //
    // The list of registry value descriptors
    //
    struct PL011_REG_VALUE_DESCRIPTOR
    {
        // Value name
        const WCHAR* ValueNameWsz;

        // Address of parameter
        _Field_size_bytes_(ParamSize)
        VOID*        ParamAddrPtr;

        // Size of parameters
        size_t       ParamSize;

        // Default value to use
        ULONG        DefaultValue;

    } regValues[] = {

        {
            MAX_BAUD_RATE__REG_VAL_NAME,
            &drvExtPtr->MaxBaudRateBPS,
            FIELD_SIZE(PL011_DRIVER_EXTENSION, MaxBaudRateBPS),
            PL011_MAX_BAUD_RATE_BPS,

        }, // MaxBaudRateBPS

        {
            UART_CLOCK___REG_VAL_NAME,
            &drvExtPtr->UartClockHz,
            FIELD_SIZE(PL011_DRIVER_EXTENSION, UartClockHz),
            PL011_DEAFULT_UART_CLOCK,

        }, // UartClockHz

        {
            UART_FLOW_CTRL__REG_VAL_NAME,
            &drvExtPtr->UartFlowControl,
            FIELD_SIZE(PL011_DRIVER_EXTENSION, UartFlowControl),
            UART_SERIAL_FLAG_FLOW_CTL_NONE,

        }, // UartFlowControl

        {
            UART_CTRL_LINES__REG_VAL_NAME,
            &drvExtPtr->UartControlLines,
            FIELD_SIZE(PL011_DRIVER_EXTENSION, UartControlLines),
            0,

        }, // UartControlLines

    }; // regValues

    NTSTATUS status;
    WDFKEY driverRegkey = NULL;

    status = WdfDriverOpenParametersRegistryKey(
        WdfGetDriver(),
        KEY_READ,
        WDF_NO_OBJECT_ATTRIBUTES,
        &driverRegkey
        );
    if (!NT_SUCCESS(status)) {

        PL011_LOG_ERROR(
            "WdfDriverOpenParametersRegistryKey failed, (status = %!STATUS!)", 
            status
            );
        goto done;
    }

    for (ULONG descInx = 0; descInx < ULONG(ARRAYSIZE(regValues)); ++descInx) {

        PL011_REG_VALUE_DESCRIPTOR* regValueDescPtr = &regValues[descInx];
        
        UNICODE_STRING valueName;
        RtlInitUnicodeString(&valueName, regValueDescPtr->ValueNameWsz);

        PL011_DRIVER_CONFIG_PARAMETER configParam;
        configParam.IsFallbackValue = FALSE;

        status = WdfRegistryQueryULong(driverRegkey, &valueName, &configParam.Value);
        if (status == STATUS_OBJECT_NAME_NOT_FOUND) {
            //
            // Value not found, not an error, use default value.
            //
            configParam.Value = regValueDescPtr->DefaultValue;
            configParam.IsFallbackValue = TRUE;
            status = STATUS_SUCCESS;

        } else if (!NT_SUCCESS(status)) {

            PL011_LOG_ERROR(
                "WdfRegistryQueryULong failed, (status = %!STATUS!)",
                status
                );
            goto done;
        }

        RtlCopyMemory(
            regValueDescPtr->ParamAddrPtr,
            &configParam,
            min(sizeof(PL011_DRIVER_CONFIG_PARAMETER), regValueDescPtr->ParamSize)
            );

    } // for (descriptors)

    status = STATUS_SUCCESS;

done:

    if (driverRegkey != NULL) {

        WdfRegistryClose(driverRegkey);
    }

    return status;
}


#undef _PL011_DRIVER_CPP_