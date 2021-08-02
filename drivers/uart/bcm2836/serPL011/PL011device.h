//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//
// Module Name:
//
//    PL011device.h
//
// Abstract:
//
//    This module contains all the enums, types, and functions related to
//    an single ARM PL011 UART device.
//
// Environment:
//
//    kernel-mode only
//

#ifndef _PL011_DEVICE_H_
#define _PL011_DEVICE_H_

WDF_EXTERN_C_START


//
// PL011_RESOURCE_DATA.
//  Contains all The PL011 resources, and configuration data
//
typedef struct _PL011_RESOURCE_DATA
{
    //
    // PL011 register file
    //
    ULONG               RegsResInx;
    PHYSICAL_ADDRESS    RegsPA;
    ULONG               RegsSpan;

    //
    // Interrupt parameters
    //
    ULONG               IntResInx;
    ULONG               IntVector;
    ULONG               IntLevel;
    KAFFINITY           Intffinity;
    KINTERRUPT_MODE     InterruptMode;

    //
    // DMA channels
    //
    
    //
    // Optional UartSerialBus Connection ID for creating the device interface 
    // reference string.
    //
    LARGE_INTEGER       ConnectionId;
    
    //
    // Optional FunctionConfig Connection ID for reserving pins in the case of
    // a kernel debugger conflict.
    //
    LARGE_INTEGER       FunctionConfigConnectionId;

} PL011_RESOURCE_DATA, *PPL011_RESOURCE_DATA;


//
// PL011_DEVICE_EXTENSION.
//  Contains all The PL011 device runtime parameters.
//  It is associated with the WDFDEVICE object.
//
typedef struct _PL011_DEVICE_EXTENSION
{
    //
    // The WDFDEVICE associated with this instance of the
    // controller driver.
    //
    WDFDEVICE                       WdfDevice;

    //
    // Device lock
    //
    KSPIN_LOCK                      Lock;

    //
    // Device open count, used for initialization/cleanup...
    //
    LONG                           OpenCount;

    //
    // If we are conflicting with the serial debugger.
    // If the driver fails to load due to a debugger conflict,
    // it blocks RHPROXY from loading. Thus we load a stale device,
    // and reject any Create requests.
    //
    BOOLEAN                         IsDebuggerConflict;

    //
    // PL011 parsed resource data
    //
    PL011_RESOURCE_DATA             PL011ResourceData;

    //
    // PL011 mapped resources
    //

    //
    // Device registers lock
    //
    KSPIN_LOCK                      RegsLock;

    //
    // PL011 register file
    //
    volatile ULONG*                 PL011RegsPtr;

    //
    // PL011 UART interrupt object
    //
    WDFINTERRUPT                    WdfUartInterrupt;

    //
    // DMA...
    //

    //
    // This value indicates whether the device is
    // currently idle. It is only set to TRUE after
    // EvtDeviceD0Enty is invoked, but is cleared to
    // FALSE before EvtDeviceD0ExitPreInterruptsDisabled
    // returns.
    //
    BOOLEAN                         IsDeviceActive;

    //
    // Handles for PIO objects
    //
    SERCX2PIOTRANSMIT               SerCx2PioTransmit;
    SERCX2PIORECEIVE                SerCx2PioReceive;

    //
    // Device configuration lock
    //
    EX_SPIN_LOCK                    ConfigLock;

    //
    // HW configuration
    //
    PL011_UART_SERIAL_BUS_DESCRIPTOR CurrentConfiguration;

    //
    // SoC supported UART controls.
    // A combination of UARTCR_??? bits that defines
    // the supported UART controls.
    //
    ULONG                           UartSupportedControlsMask;

    //
    // The settable baud rates supported
    //
    ULONG                           SettableBaud;

    //
    //  Runtime...
    //

    //
    // The encountered UART error types to be reported
    // through
    // IOCTL_SERIAL_GET_MODEMSTATUS::SERIAL_STATUS.Errors
    //
    // Please refer to SERIAL_ERROR_???
    //
    ULONG                           UartErrorTypes;

    //
    // The event mask set by PL011EvtSerCx2SetWaitMask()
    // Does not include the RX/TX relate events.
    //
    ULONG                           WaitEventMask;

    //
    // Interrupt events mask captured in UART ISR
    // that require DPC handling.
    //
    ULONG                           IntEventsForDpc;
    
    //
    // Handle to FunctionConfig() resource used in case of debugger conflict.
    //
    WDFIOTARGET                     FunctionConfigHandle;

} PL011_DEVICE_EXTENSION;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PL011_DEVICE_EXTENSION, PL011DeviceGetExtension);


//
// PL011device WDF device event handlers
//
EVT_WDF_DRIVER_DEVICE_ADD PL011EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE PL011EvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE PL011EvtDeviceReleaseHardware;
EVT_WDF_OBJECT_CONTEXT_DESTROY PL011DeviceEvtDestroy;


//
// PL011device public methods
//
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
PL011DeviceRecordErrors(
    _In_ PL011_DEVICE_EXTENSION* DevExtPtr,
    _In_ ULONG PL011ErrorEventsMask
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
PL011DeviceNotifyEvents(
    _In_ PL011_DEVICE_EXTENSION* DevExtPtr,
    _In_ ULONG PL011EventsMask
    );


//
// PL011device private methods
//
#ifdef _PL011_DEVICE_CPP_

    _IRQL_requires_max_(PASSIVE_LEVEL)
    static NTSTATUS
    PL011pDeviceSerCx2Init(
        _In_ WDFDEVICE WdfDevice
        );

    _IRQL_requires_max_(APC_LEVEL)
    static NTSTATUS
    PL011pReadDeviceSpecificData(
        _In_  WDFDEVICE WdfDevice,
        _Out_ ULONG* ClockFrequencyPtr
        );

    _IRQL_requires_max_(PASSIVE_LEVEL)
    static NTSTATUS
    PL011pDeviceExtensionInit(
        _In_ WDFDEVICE WdfDevice
        );

    _IRQL_requires_max_(PASSIVE_LEVEL)
    static NTSTATUS
    PL011pDeviceParseResources(
        _In_ WDFDEVICE WdfDevice,
        _In_ WDFCMRESLIST ResourcesTranslated
        );
        
    _IRQL_requires_max_(PASSIVE_LEVEL)
    static NTSTATUS
    PL011pDeviceMapResources(
        _In_ WDFDEVICE WdfDevice,
        _In_ WDFCMRESLIST ResourcesRaw,
        _In_ WDFCMRESLIST ResourcesTranslated
        );
        
    _IRQL_requires_max_(PASSIVE_LEVEL)
    static NTSTATUS
    PL011pDeviceCreateDeviceInterface(
        _In_ WDFDEVICE WdfDevice,
        _In_ LARGE_INTEGER ConnectionId
        );
        
    _IRQL_requires_max_(PASSIVE_LEVEL)
    static NTSTATUS
    PL011pDeviceReserveFunctionConfigResource(
        _In_ PL011_DEVICE_EXTENSION* DevExtPtr
        );

    _IRQL_requires_max_(PASSIVE_LEVEL)
    static NTSTATUS
    PL011pDeviceGetSupportedFeatures(
        _In_ WDFDEVICE WdfDevice
        );

#endif // _PL011_DEVICE_CPP_

WDF_EXTERN_C_END

#endif // !_PL011_DEVICE_H_