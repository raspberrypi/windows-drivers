/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Abstract:

    This file contains definitions for DMA interrupt handling.

--*/

#pragma once

#define PREVIOUS_PACKET_INDEX(currentPacket, numPackets) (currentPacket ? currentPacket - 1 : numPackets - 1)
#define FIRST_CB_ADDRESS_OF_PACKET(packet, cbBaseAddressPaLow) (cbBaseAddressPaLow + (2 * packet * sizeof(DMA_CB)))
#define SOURCE_AD_INIT_VALUE_OF_PACKET(packet, cbBaseAddress) (cbBaseAddress[2 * packet].SOURCE_AD)

EVT_WDF_INTERRUPT_ISR DmaIsr;
EVT_WDF_INTERRUPT_DPC DmaDpc;

VOID
ClearDmaErrorAndRequestRestart
(
    _In_ PDEVICE_CONTEXT DeviceContext
);

ULONG
GetNumberOfProcessedPackets
(
    _In_ ULONG CompletedPacket,
    _In_ ULONG LastKnownCompletedPacket,
    _In_ ULONG NumPackets
    );

VOID
HandleUnderflow(
    _In_ PDEVICE_CONTEXT DeviceContext
    );
