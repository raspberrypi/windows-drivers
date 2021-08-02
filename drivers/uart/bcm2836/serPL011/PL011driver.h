//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//
// Module Name: 
//
//    PL011driver.h
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

#ifndef _PL011_DRIVER_H_
#define _PL011_DRIVER_H_

WDF_EXTERN_C_START


//
// Driver configuration parameters registry value names
//
#define MAX_BAUD_RATE__REG_VAL_NAME         L"MaxBaudRateBPS"
#define UART_CLOCK___REG_VAL_NAME           L"UartClockHz"
#define UART_FLOW_CTRL__REG_VAL_NAME        L"UartFlowControl"
#define UART_CTRL_LINES__REG_VAL_NAME       L"UartControlLines"


typedef struct _PL011_DRIVER_CONFIG_PARAMETER
{
    //
    // Config parameter value
    //
    ULONG Value;

    //
    // FALSE -> if the value was successfully obtained from the registry
    // TRUE  -> if a fallback (default) value was set
    //
    BOOLEAN IsFallbackValue;

} PL011_DRIVER_CONFIG_PARAMETER;

//
// PL011_DRIVER_EXTENSION.
//  Contains all The PL011 driver configuration parameters.
//  It is associated with the WDFDRIVER object.
//
typedef struct _PL011_DRIVER_EXTENSION
{
    //
    // Driver configuration parameters
    // We read these parameters from registry,
    // since three is no standard way to get it
    // from UEFI.
    //
    //  The configuration parameters reside at:
    //  HKLM\System\CurrentControlSet\Services\SerPL011\Parameters
    //

    //
    // Max baud rate
    //
    PL011_DRIVER_CONFIG_PARAMETER   MaxBaudRateBPS;

    //
    // UART clock.
    //
    PL011_DRIVER_CONFIG_PARAMETER   UartClockHz;

    //
    // Flow control supported by the 
    // board
    //
    PL011_DRIVER_CONFIG_PARAMETER   UartFlowControl;

    //
    // Control line exposed by the 
    // board
    //
    PL011_DRIVER_CONFIG_PARAMETER   UartControlLines;

} PL011_DRIVER_EXTENSION;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PL011_DRIVER_EXTENSION, PL011DriverGetExtension);


//
// PL011driver WDF Driver event handlers
//
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_UNLOAD PL011EvtDriverUnload;


//
// PL011driver private methods
//
#ifdef _PL011_DRIVER_CPP_

    _IRQL_requires_max_(PASSIVE_LEVEL)
    NTSTATUS
    PL011pDriverReadConfig();

#endif //_PL011_DRIVER_CPP_


WDF_EXTERN_C_END

#endif // !_PL011_DRIVER_H_
