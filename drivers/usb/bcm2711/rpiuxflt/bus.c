#include "rpiuxflt.h"

static
VOID
Bus_InterfaceReferenceNop(
    IN PVOID Context
    )
{
    UNREFERENCED_PARAMETER(Context);
}

static
VOID
Bus_InterfaceDereferenceNop(
    IN PVOID Context
    )
{
    UNREFERENCED_PARAMETER(Context);
}

static
BOOLEAN
Bus_TranslateBusAddress(
    IN OUT PVOID Context,
    IN PHYSICAL_ADDRESS BusAddress,
    IN ULONG Length,
    OUT PULONG AddressSpace,
    OUT PPHYSICAL_ADDRESS TranslatedAddress
    )
{
    PFILTER_DEVICE_DATA pDeviceData;

    pDeviceData = (PFILTER_DEVICE_DATA)Context;

    return pDeviceData->AttachedBusInterface.TranslateBusAddress(
        pDeviceData->AttachedBusInterface.Context,
        BusAddress,
        Length,
        AddressSpace,
        TranslatedAddress);
}

static
PDMA_ADAPTER
Bus_GetDmaAdapter(
    IN OUT PVOID Context,
    IN PDEVICE_DESCRIPTION DeviceDescriptor,
    OUT PULONG NumberOfMapRegisters
    )
{
    PFILTER_DEVICE_DATA pDeviceData;

    pDeviceData = (PFILTER_DEVICE_DATA)Context;

    return Dma_CreateDmaAdapter(pDeviceData,
                                DeviceDescriptor,
                                NumberOfMapRegisters);
}

static
ULONG
Bus_SetBusData(
    IN OUT PVOID Context,
    IN ULONG DataType,
    IN OUT PVOID Buffer,
    IN ULONG Offset,
    IN ULONG Length
    )
{
    PFILTER_DEVICE_DATA pDeviceData;

    pDeviceData = (PFILTER_DEVICE_DATA)Context;

    return pDeviceData->AttachedBusInterface.SetBusData(
        pDeviceData->AttachedBusInterface.Context,
        DataType,
        Buffer,
        Offset,
        Length);
}

static
ULONG
Bus_GetBusData(
    IN OUT PVOID Context,
    IN ULONG DataType,
    IN OUT PVOID Buffer,
    IN ULONG Offset,
    IN ULONG Length
    )
{
    PFILTER_DEVICE_DATA pDeviceData;

    pDeviceData = (PFILTER_DEVICE_DATA)Context;

    return pDeviceData->AttachedBusInterface.GetBusData(
        pDeviceData->AttachedBusInterface.Context,
        DataType,
        Buffer,
        Offset,
        Length);
}

VOID
Bus_WrapBusInterface(
    IN PFILTER_DEVICE_DATA DeviceData,
    IN OUT PBUS_INTERFACE_STANDARD BusInterface
    )
{
    if (!DeviceData->AttachedBusInterface.Size) {
        RtlCopyMemory(&DeviceData->AttachedBusInterface,
                BusInterface,
                sizeof(BUS_INTERFACE_STANDARD));
    }

    BusInterface->Context = (PVOID)DeviceData;
    BusInterface->InterfaceReference = Bus_InterfaceReferenceNop;
    BusInterface->InterfaceDereference = Bus_InterfaceDereferenceNop;
    BusInterface->TranslateBusAddress = Bus_TranslateBusAddress;
    BusInterface->GetDmaAdapter = Bus_GetDmaAdapter;
    BusInterface->SetBusData = Bus_SetBusData;
    BusInterface->GetBusData = Bus_GetBusData;
}
