#ifndef RPIUXFLT_H
#define RPIUXFLT_H

#include <ntddk.h>
#include <initguid.h>
#include <wdmguid.h>

typedef struct _FILTER_DEVICE_DATA
{
    PDEVICE_OBJECT Self;
    PDEVICE_OBJECT TopOfStack;
    BUS_INTERFACE_STANDARD AttachedBusInterface;
} FILTER_DEVICE_DATA, *PFILTER_DEVICE_DATA;

VOID
Bus_WrapBusInterface(
    IN PFILTER_DEVICE_DATA DeviceData,
    IN OUT PBUS_INTERFACE_STANDARD BusInterface
    );

PDMA_ADAPTER
Dma_CreateDmaAdapter(
    IN PFILTER_DEVICE_DATA DeviceData,
    IN PDEVICE_DESCRIPTION DeviceDescriptor,
    OUT PULONG NumberOfMapRegisters
    );

#endif /* RPIUXFLT_H */
