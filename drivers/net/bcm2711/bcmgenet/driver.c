#include <ntddk.h>
#include <wdf.h>
#include <ntintsafe.h>
#include <preview/netadaptercx.h>
#include <preview/netadapter.h>
#include <evntrace.h>
#include <TraceLoggingProvider.h>
#include <net/logicaladdress.h>
#include <net/returncontext.h>
#include <net/virtualaddress.h>

#include "registers.h"
#include "trace.h"

#define GENET_MDIO_RETRY 1000
#define GENET_PHY_RESET_RETRY 25000
#define GENET_MAX_LINK_SPEED 1000000000
#define GENET_SUPPORTED_FILTERS                                       \
    (NetPacketFilterFlagDirected | NetPacketFilterFlagMulticast |     \
     NetPacketFilterFlagAllMulticast | NetPacketFilterFlagBroadcast | \
     NetPacketFilterFlagPromiscuous)
#define GENET_MAX_MULTICAST_ADDRESSES (BG_UMAC_MAX_MAC_FILTERS - 2)
#define GENET_RX_BUFFER_SIZE 2048
#define GENET_MAX_MTU_SIZE 1536
#define GENET_RING_DMA_EN \
    ((1 << (BG_DEFAULT_RING + BG_DMA_RING_BUF_EN_SHIFT)) | BG_DMA_EN)

#define GRD(adapter, element) READ_REGISTER_ULONG(&adapter->registers->element)
#define GWR(adapter, element, value) \
    WRITE_REGISTER_ULONG(&adapter->registers->element, value)

typedef struct GENET_RX_BUFFER_MDL {
    MDL mdl;
    PFN_NUMBER pfns[(PAGE_SIZE - 1 + GENET_RX_BUFFER_SIZE + PAGE_SIZE - 1) /
                    PAGE_SIZE];
} GENET_RX_BUFFER_MDL;

typedef struct GenetAdapter GenetAdapter;

typedef struct GenetTxPacket {
    ULONG lastDesc;
    ULONG endFragment;
} GenetTxPacket;

typedef struct GenetRxBuffer {
    void *virtualAddress;
    GENET_RX_BUFFER_MDL rxMdl;
    UINT64 logicalAddress;
} GenetRxBuffer;

typedef struct GenetTxQueue {
    GenetAdapter *adapter;
    NETPACKETQUEUE netTxQueue;
    NET_EXTENSION virtualAddressExtension;
    NET_EXTENSION logicalAddressExtension;
    const NET_RING_COLLECTION *rings;
    ULONG numDescs;
    GenetTxPacket *packetContexts;
    ULONG prodIndex;
    ULONG consIndex;
} GenetTxQueue;

typedef struct GenetRxQueue {
    GenetAdapter *adapter;
    NETPACKETQUEUE netRxQueue;
    ULONG queueId;
    NET_EXTENSION virtualAddressExtension;
    NET_EXTENSION returnContextExtension;
    const NET_RING_COLLECTION *rings;
    ULONG numDescs;
    ULONG numBuffers;
    GenetRxBuffer *buffers;
    GenetRxBuffer **freeBuffers;
    ULONG curFreeBuffer;
    GenetRxBuffer **descBuffers;
    ULONG prodIndex;
    ULONG consIndex;
    BOOLEAN canceled;
} GenetRxQueue;

typedef struct GenetInterrupt {
    GenetAdapter *adapter;
    WDFINTERRUPT wdfInterrupt;
    LONG txNotify;
    LONG rxNotify;
    ULONG savedStatus;
} GenetInterrupt;

typedef struct GenetTimer {
    GenetAdapter *adapter;
    WDFTIMER wdfTimer;
} GenetTimer;

typedef struct GenetAdapter {
    WDFDEVICE wdfDevice;
    NETADAPTER netAdapter;
    NETCONFIGURATION netConfiguration;
    WDFDMAENABLER dmaEnabler;
    WDFSPINLOCK lock;
    GenetRegisters *registers;
    GenetTimer *timer;
    GenetInterrupt *interrupt;
    GenetTxQueue *txQueue;
    GenetRxQueue *rxQueue;
    NET_ADAPTER_LINK_LAYER_ADDRESS permanentMacAddress;
    NET_ADAPTER_LINK_LAYER_ADDRESS currentMacAddress;
    NET_PACKET_FILTER_FLAGS packetFilter;
    ULONG numMulticastAddresses;
    NET_ADAPTER_LINK_LAYER_ADDRESS
    multicastAddresses[GENET_MAX_MULTICAST_ADDRESSES];
} GenetAdapter;

typedef struct GenetDevice {
    GenetAdapter *adapter;
} GenetDevice;

static const NET_ADAPTER_LINK_LAYER_ADDRESS broadcastMacAddress = {
    6, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};

/* {AC94D0B6-8332-4EBD-BD5D-D33C6EC7BD5E} */
TRACELOGGING_DEFINE_PROVIDER(GenetTraceProvider, "BcmGenet",
                             (0xac94d0b6, 0x8332, 0x4ebd, 0xbd, 0x5d, 0xd3,
                              0x3c, 0x6e, 0xc7, 0xbd, 0x5e));

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GenetDevice, GenetGetDeviceContext);
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GenetAdapter, GenetGetAdapterContext);
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GenetTimer, GenetGetTimerContext);
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GenetInterrupt, GenetGetInterruptContext);
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GenetTxQueue, GenetGetTxQueueContext);
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GenetRxQueue, GenetGetRxQueueContext);

DRIVER_INITIALIZE DriverEntry;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#endif

static NTSTATUS GenetPhyRead(GenetAdapter *adapter, UCHAR regAddr,
                             USHORT *regData) {
    int retry;
    ULONG cmdReg;

    GWR(adapter, UMAC.MDIO_Cmd,
        BG_MDIO_START_BUSY | BG_MDIO_READ | (1 << BG_MDIO_ADDR_SHIFT) |
            ((regAddr & BG_MDIO_REG_MASK) << BG_MDIO_REG_SHIFT));
    for (retry = GENET_MDIO_RETRY; retry > 0; --retry) {
        if (((cmdReg = GRD(adapter, UMAC.MDIO_Cmd)) & BG_MDIO_START_BUSY) ==
            0) {
            *regData = cmdReg & BG_MDIO_DATA_MASK;
            break;
        }
        KeStallExecutionProcessor(10);
    }
    if (0 == retry) {
        return STATUS_TRANSACTION_TIMED_OUT;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS GenetPhyWrite(GenetAdapter *adapter, UCHAR regAddr,
                              USHORT regData) {
    int retry;
    ULONG cmdReg;

    GWR(adapter, UMAC.MDIO_Cmd,
        BG_MDIO_START_BUSY | BG_MDIO_WRITE | (1 << BG_MDIO_ADDR_SHIFT) |
            ((regAddr & BG_MDIO_REG_MASK) << BG_MDIO_REG_SHIFT) | regData);
    for (retry = GENET_MDIO_RETRY; retry > 0; --retry) {
        if (((cmdReg = GRD(adapter, UMAC.MDIO_Cmd)) & BG_MDIO_START_BUSY) ==
            0) {
            break;
        }
        KeStallExecutionProcessor(10);
    }
    if (0 == retry) {
        return STATUS_TRANSACTION_TIMED_OUT;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS GenetPhyInitialize(GenetAdapter *adapter) {
    NTSTATUS status;
    USHORT phyReg;
    int pollRetry;

    status = GenetPhyWrite(adapter, BG_MII_BMCR, BG_MII_BMCR_RESET);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    for (pollRetry = 0; pollRetry < GENET_PHY_RESET_RETRY; pollRetry++) {
        status = GenetPhyRead(adapter, BG_MII_BMCR, &phyReg);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        if (!(phyReg & BG_MII_BMCR_RESET)) {
            break;
        }
        KeStallExecutionProcessor(20);
    }
    if (phyReg & BG_MII_BMCR_RESET) {
        return STATUS_TRANSACTION_TIMED_OUT;
    }

    status = GenetPhyWrite(
        adapter, BG_MII_BCM_AUXCTL,
        (BG_MII_BCM_AUXCTL_SHD_MISC << BG_MII_BCM_AUXCTL_SHD_READ_SHIFT) |
            BG_MII_BCM_AUXCTL_SHD_MASK);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = GenetPhyRead(adapter, BG_MII_BCM_AUXCTL, &phyReg);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    phyReg |= BG_MII_BCM_AUXCTL_SHD_MISC_WRITE_EN |
              BG_MII_BCM_AUXCTL_SHD_MISC_RGMII_SKEW_EN |
              BG_MII_BCM_AUXCTL_SHD_MISC;
    status = GenetPhyWrite(adapter, BG_MII_BCM_AUXCTL, phyReg);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = GenetPhyWrite(adapter, BG_MII_BCM_SHD,
                           BG_MII_BCM_SHD_CLK << BG_MII_BCM_SHD_SEL_SHIFT);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = GenetPhyRead(adapter, BG_MII_BCM_SHD, &phyReg);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    phyReg &= ~BG_MII_BCM_SHD_CLK_GTXCLK_EN & BG_MII_BCM_SHD_DATA_MASK;
    phyReg |= BG_MII_BCM_SHD_WRITE_EN |
              (BG_MII_BCM_SHD_CLK << BG_MII_BCM_SHD_SEL_SHIFT);
    status = GenetPhyWrite(adapter, BG_MII_BCM_SHD, phyReg);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS GenetPhyReadLinkState(GenetAdapter *adapter,
                                      NET_ADAPTER_LINK_STATE *linkState) {
    NTSTATUS status;
    USHORT bmsr = 0, auxsts = 0;
    USHORT linkSpeed = 0;
    NET_IF_MEDIA_DUPLEX_STATE duplexState = MediaDuplexStateUnknown;
    NET_ADAPTER_PAUSE_FUNCTION_TYPE pauseFunctions =
        NetAdapterPauseFunctionTypeUnsupported;
    NET_ADAPTER_AUTO_NEGOTIATION_FLAGS autoNegotiationFlags =
        NetAdapterAutoNegotiationFlagXmitLinkSpeedAutoNegotiated |
        NetAdapterAutoNegotiationFlagRcvLinkSpeedautoNegotiated |
        NetAdapterAutoNegotiationFlagDuplexAutoNegotiated;

    status = GenetPhyRead(adapter, BG_MII_BMSR, &bmsr);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (!(bmsr & BG_MII_BMSR_LINK) || !(bmsr & BG_MII_BMSR_ANCOMP)) {
        NET_ADAPTER_LINK_STATE_INIT_DISCONNECTED(linkState);
        return STATUS_SUCCESS;
    }

    status = GenetPhyRead(adapter, BG_MII_BCM_AUXSTS, &auxsts);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    switch (auxsts & BG_MII_BCM_AUXSTS_ANRESULT) {
        case BG_MII_BCM_RESULT_1000FD:
            linkSpeed = 1000;
            duplexState = MediaDuplexStateFull;
            break;
        case BG_MII_BCM_RESULT_1000HD:
            linkSpeed = 1000;
            duplexState = MediaDuplexStateHalf;
            break;
        case BG_MII_BCM_RESULT_100FD:
            linkSpeed = 100;
            duplexState = MediaDuplexStateFull;
            break;
        case BG_MII_BCM_RESULT_100T4:
        case BG_MII_BCM_RESULT_100HD:
            linkSpeed = 100;
            duplexState = MediaDuplexStateHalf;
            break;
        case BG_MII_BCM_RESULT_10FD:
            linkSpeed = 10;
            duplexState = MediaDuplexStateFull;
            break;
        case BG_MII_BCM_RESULT_10HD:
            linkSpeed = 10;
            duplexState = MediaDuplexStateHalf;
            break;
    }

    NET_ADAPTER_LINK_STATE_INIT(linkState, linkSpeed * 1000000,
                                MediaConnectStateConnected, duplexState,
                                pauseFunctions, autoNegotiationFlags);

    return status;
}

static void GenetMacInitialize(GenetAdapter *adapter) {
    ULONG regData;

    regData = GRD(adapter, Sys.RBuf_Flush_Ctrl);
    regData |= BG_SYS_RBUF_FLUSH_RESET;
    GWR(adapter, Sys.RBuf_Flush_Ctrl, regData);
    KeStallExecutionProcessor(10);
    regData &= ~BG_SYS_RBUF_FLUSH_RESET;
    GWR(adapter, Sys.RBuf_Flush_Ctrl, regData);
    KeStallExecutionProcessor(10);
    GWR(adapter, Sys.RBuf_Flush_Ctrl, 0);
    KeStallExecutionProcessor(10);

    GWR(adapter, UMAC.Cmd, 0);
    GWR(adapter, UMAC.Cmd, BG_UMAC_CMD_LCL_LOOP_EN | BG_UMAC_CMD_SW_RESET);
    KeStallExecutionProcessor(10);

    GWR(adapter, Sys.Port_Ctrl, BG_SYS_PORT_MODE_EXT_GPHY);
    KeStallExecutionProcessor(10);

    GWR(adapter, UMAC.Cmd, 0);
    KeStallExecutionProcessor(10);

    GWR(adapter, UMAC.MIB_Ctrl,
        BG_UMAC_MIB_RESET_TX | BG_UMAC_MIB_RESET_RUNT | BG_UMAC_MIB_RESET_RX);
    GWR(adapter, UMAC.MIB_Ctrl, 0);

    GWR(adapter, UMAC.Max_Frame_Len, GENET_MAX_MTU_SIZE);

    regData = GRD(adapter, RBuf.Ctrl);
    regData |= BG_RBUF_ALIGN_2B;
    GWR(adapter, RBuf.Ctrl, regData);

    GWR(adapter, RBuf.TBuf_Size_Ctrl, 1);

    GWR(adapter, INTRL2_0.CPU_Mask_Set, 0xffffffff);
    GWR(adapter, INTRL2_0.CPU_Clear, 0xffffffff);

    GWR(adapter, Sys.Port_Ctrl, BG_SYS_PORT_MODE_EXT_GPHY);

    GWR(adapter, UMAC.MAC0,
        (adapter->currentMacAddress.Address[0] << 24) |
            (adapter->currentMacAddress.Address[1] << 16) |
            (adapter->currentMacAddress.Address[2] << 8) |
            adapter->currentMacAddress.Address[3]);
    GWR(adapter, UMAC.MAC1,
        (adapter->currentMacAddress.Address[4] << 8) |
            adapter->currentMacAddress.Address[5]);
}

static void GenetMacPhyConfigure(GenetAdapter *adapter) {
    ULONG regData;

    regData = GRD(adapter, Ext.RGMII_OOB_Ctrl);
    regData &= ~BG_EXT_RGMII_OOB_ID_MODE_DIS;
    regData |= BG_EXT_RGMII_OOB_MODE_EN;
    GWR(adapter, Ext.RGMII_OOB_Ctrl, regData);
}

static void GenetMacSetLinkState(GenetAdapter *adapter,
                                 NET_ADAPTER_LINK_STATE *linkState) {
    const ULONG stateMask = BG_UMAC_CMD_SPEED_1000 | BG_UMAC_CMD_SPEED_100 |
                            BG_UMAC_CMD_HD_EN | BG_UMAC_CMD_RX_PAUSE_IGNORE |
                            BG_UMAC_CMD_TX_PAUSE_IGNORE;
    ULONG newState = 0, cmdReg = 0;

    if (linkState->MediaConnectState != MediaConnectStateConnected) {
        return;
    }

    switch (linkState->TxLinkSpeed) {
        case 1000 * 1000000:
            newState = BG_UMAC_CMD_SPEED_1000;
            break;
        case 100 * 1000000:
            newState = BG_UMAC_CMD_SPEED_100;
            break;
    }
    if (linkState->MediaDuplexState != MediaDuplexStateFull) {
        newState |= BG_UMAC_CMD_HD_EN;
    }
    newState |= BG_UMAC_CMD_RX_PAUSE_IGNORE | BG_UMAC_CMD_TX_PAUSE_IGNORE;

    cmdReg = GRD(adapter, Ext.RGMII_OOB_Ctrl);
    cmdReg &= ~BG_EXT_RGMII_OOB_DISABLE;
    cmdReg |= BG_EXT_RGMII_OOB_LINK;
    GWR(adapter, Ext.RGMII_OOB_Ctrl, cmdReg);

    cmdReg = GRD(adapter, UMAC.Cmd);
    if (newState != (cmdReg & stateMask)) {
        cmdReg &= ~stateMask;
        cmdReg |= newState;
        TraceInfo("MacSetLinkState", TraceULX(newState, "NewState"),
                  TraceULX(cmdReg, "CmdReg"));
        GWR(adapter, UMAC.Cmd, cmdReg);
    }
}

static void GenetSetOneMacAddressFilter(
    GenetAdapter *adapter, int filterNum,
    const NET_ADAPTER_LINK_LAYER_ADDRESS *macAddress, ULONG *ctrlFlags) {
    int regBase = filterNum * 2;

    GWR(adapter, UMAC.MDF_Addr[regBase],
        macAddress->Address[0] << 8 | macAddress->Address[1]);
    GWR(adapter, UMAC.MDF_Addr[regBase + 1],
        macAddress->Address[2] << 24 | macAddress->Address[3] << 16 |
            macAddress->Address[4] << 8 | macAddress->Address[5]);

    *ctrlFlags |= 1 << (BG_UMAC_MAX_MAC_FILTERS - 1 - filterNum);
}

static void GenetSetMacAddressFilters(GenetAdapter *adapter) {
    ULONG umacCmd;
    ULONG mdfCtrl = 0;
    ULONG curFilter = 0;
    ULONG curMulticast;

    umacCmd = GRD(adapter, UMAC.Cmd);
    if (adapter->packetFilter &
        (NetPacketFilterFlagAllMulticast | NetPacketFilterFlagPromiscuous)) {
        umacCmd |= BG_UMAC_CMD_PROMISC;
    } else {
        umacCmd &= ~BG_UMAC_CMD_PROMISC;

        if (adapter->packetFilter & NetPacketFilterFlagBroadcast) {
            GenetSetOneMacAddressFilter(adapter, curFilter++,
                                        &broadcastMacAddress, &mdfCtrl);
        }

        if (adapter->packetFilter & NetPacketFilterFlagDirected) {
            GenetSetOneMacAddressFilter(adapter, curFilter++,
                                        &adapter->currentMacAddress, &mdfCtrl);
        }

        if (adapter->packetFilter & NetPacketFilterFlagMulticast) {
            for (curMulticast = 0;
                 curMulticast < adapter->numMulticastAddresses;
                 ++curMulticast) {
                GenetSetOneMacAddressFilter(
                    adapter, curFilter++,
                    &adapter->multicastAddresses[curMulticast], &mdfCtrl);
            }
        }
    }
    GWR(adapter, UMAC.Cmd, umacCmd);
    GWR(adapter, UMAC.MDF_Ctrl, mdfCtrl);
}

static void GenetReturnRxBuffer(
    NETADAPTER netAdapter, NET_FRAGMENT_RETURN_CONTEXT_HANDLE returnContext) {
    GenetAdapter *adapter = GenetGetAdapterContext(netAdapter);
    GenetRxQueue *rxQueue = adapter->rxQueue;

    if (returnContext == NULL) {
        return;
    }

    NT_FRE_ASSERT(rxQueue->curFreeBuffer < rxQueue->numBuffers);
    rxQueue->freeBuffers[rxQueue->curFreeBuffer++] =
        (GenetRxBuffer *)returnContext;
}

static void GenetFillRxDesc(GenetAdapter *adapter, ULONG desc) {
    GenetRxQueue *rxQueue = adapter->rxQueue;
    GenetRxBuffer *rxBuffer;

    NT_FRE_ASSERT(desc < rxQueue->numDescs);
    NT_FRE_ASSERT(rxQueue->curFreeBuffer > 0);
    rxBuffer = rxQueue->freeBuffers[--rxQueue->curFreeBuffer];
    GWR(adapter, RDMA.BDs[desc].Address_Lo, (ULONG)rxBuffer->logicalAddress);
    GWR(adapter, RDMA.BDs[desc].Address_Hi,
        (ULONG)(rxBuffer->logicalAddress >> 32));
    rxQueue->descBuffers[desc] = rxBuffer;
}

static void GenetSetPacketFilter(NETADAPTER netAdapter,
                                 NET_PACKET_FILTER_FLAGS packetFilter) {
    GenetAdapter *adapter = GenetGetAdapterContext(netAdapter);

    TraceInfo("Entry", TraceULX((ULONG)packetFilter, "PacketFilter"));
    WdfSpinLockAcquire(adapter->lock);
    adapter->packetFilter = packetFilter;
    GenetSetMacAddressFilters(adapter);
    WdfSpinLockRelease(adapter->lock);
}

static void GenetSetMulticastList(
    NETADAPTER netAdapter, ULONG multicastAddressCount,
    NET_ADAPTER_LINK_LAYER_ADDRESS *multicastAddressList) {
    GenetAdapter *adapter = GenetGetAdapterContext(netAdapter);

    TraceInfo("Entry", TraceULX(multicastAddressCount, "Count"));
    WdfSpinLockAcquire(adapter->lock);
    adapter->numMulticastAddresses = multicastAddressCount;
    RtlZeroMemory(
        adapter->multicastAddresses,
        sizeof(NET_ADAPTER_LINK_LAYER_ADDRESS) * GENET_MAX_MULTICAST_ADDRESSES);
    if (adapter->numMulticastAddresses != 0) {
        RtlCopyMemory(adapter->multicastAddresses, multicastAddressList,
                      sizeof(NET_ADAPTER_LINK_LAYER_ADDRESS) *
                          adapter->numMulticastAddresses);
    }
    GenetSetMacAddressFilters(adapter);
    WdfSpinLockRelease(adapter->lock);
}

static NTSTATUS GenetAdapterStart(GenetAdapter *adapter) {
    NTSTATUS status;
    NET_ADAPTER_LINK_STATE linkState;
    NET_ADAPTER_LINK_LAYER_CAPABILITIES linkLayerCapabilities;
    NET_ADAPTER_DMA_CAPABILITIES dmaCapabilities;
    NET_ADAPTER_TX_CAPABILITIES txCapabilities;
    NET_ADAPTER_RX_CAPABILITIES rxCapabilities;
    NET_ADAPTER_PACKET_FILTER_CAPABILITIES packetFilterCapabilities;
    NET_ADAPTER_MULTICAST_CAPABILITIES multicastCapabilities;

    NET_ADAPTER_LINK_STATE_INIT_DISCONNECTED(&linkState);
    NetAdapterSetLinkState(adapter->netAdapter, &linkState);

    NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(
        &linkLayerCapabilities, GENET_MAX_LINK_SPEED, GENET_MAX_LINK_SPEED);
    NetAdapterSetLinkLayerCapabilities(adapter->netAdapter,
                                       &linkLayerCapabilities);
    NetAdapterSetLinkLayerMtuSize(adapter->netAdapter, 1500);

    NET_ADAPTER_DMA_CAPABILITIES_INIT(&dmaCapabilities, adapter->dmaEnabler);
    NET_ADAPTER_TX_CAPABILITIES_INIT_FOR_DMA(&txCapabilities, &dmaCapabilities,
                                             1);
    txCapabilities.FragmentRingNumberOfElementsHint = BG_NUM_BDS;
    NET_ADAPTER_RX_CAPABILITIES_INIT_DRIVER_MANAGED(
        &rxCapabilities, GenetReturnRxBuffer, GENET_RX_BUFFER_SIZE, 1);
    rxCapabilities.FragmentRingNumberOfElementsHint = BG_NUM_BDS;
    NetAdapterSetDataPathCapabilities(adapter->netAdapter, &txCapabilities,
                                      &rxCapabilities);

    NET_ADAPTER_PACKET_FILTER_CAPABILITIES_INIT(&packetFilterCapabilities,
                                                GENET_SUPPORTED_FILTERS,
                                                GenetSetPacketFilter);
    NetAdapterSetPacketFilterCapabilities(adapter->netAdapter,
                                          &packetFilterCapabilities);

    NET_ADAPTER_MULTICAST_CAPABILITIES_INIT(&multicastCapabilities,
                                            GENET_MAX_MULTICAST_ADDRESSES,
                                            GenetSetMulticastList);
    NetAdapterSetMulticastCapabilities(adapter->netAdapter,
                                       &multicastCapabilities);

    status = NetAdapterStart(adapter->netAdapter);

    return status;
}

static void GenetInterruptSetCommon(GenetAdapter *adapter, LONG *notify,
                                    BOOLEAN enabled) {
    const ULONG interruptMask = BG_INTR_TXDMA_DONE | BG_INTR_RXDMA_DONE;
    ULONG armedInterrupts = 0;

    InterlockedExchange(notify, enabled);

    WdfInterruptAcquireLock(adapter->interrupt->wdfInterrupt);

    if (adapter->interrupt->txNotify) {
        armedInterrupts |= BG_INTR_TXDMA_DONE;
    }

    if (adapter->interrupt->rxNotify) {
        armedInterrupts |= BG_INTR_RXDMA_DONE;
    }

    GWR(adapter, INTRL2_0.CPU_Mask_Set, interruptMask & ~armedInterrupts);
    GWR(adapter, INTRL2_0.CPU_Mask_Clear, armedInterrupts);

    WdfInterruptReleaseLock(adapter->interrupt->wdfInterrupt);

    if (!enabled) {
        KeFlushQueuedDpcs();
    }
}

static void GenetTxInterruptSet(GenetAdapter *adapter, BOOLEAN enabled) {
    GenetInterruptSetCommon(adapter, &adapter->interrupt->txNotify, enabled);
}

static void GenetRxInterruptSet(GenetAdapter *adapter, BOOLEAN enabled) {
    GenetInterruptSetCommon(adapter, &adapter->interrupt->rxNotify, enabled);
}

static void GenetTxQueueAdvance(NETPACKETQUEUE netTxQueue) {
    GenetTxQueue *txQueue = GenetGetTxQueueContext(netTxQueue);
    GenetAdapter *adapter = txQueue->adapter;
    NET_RING *packetRing = NetRingCollectionGetPacketRing(txQueue->rings);
    NET_RING *fragmentRing = NetRingCollectionGetFragmentRing(txQueue->rings);
    BOOLEAN postedDescs = FALSE;
    ULONG prodDesc;
    ULONG consDesc;
    ULONG hwDescs;
    ULONG freeDescs;
    ULONG packetDescNum;
    ULONG packetIndex;
    ULONG fragmentIndex;
    ULONG fragmentEndIndex;
    NET_PACKET *packet;
    GenetTxPacket *txPacket;
    NET_FRAGMENT *fragment;
    ULONG length_status;
    const NET_FRAGMENT_LOGICAL_ADDRESS *logicalAddress;
    UINT64 fragmentAddress;

    prodDesc = txQueue->prodIndex % txQueue->numDescs;
    consDesc = txQueue->consIndex % txQueue->numDescs;
    hwDescs = (prodDesc - consDesc) % txQueue->numDescs;
    freeDescs = txQueue->numDescs - hwDescs - 1;
    packetIndex = packetRing->NextIndex;
    while (packetIndex != packetRing->EndIndex) {
        packet = NetRingGetPacketAtIndex(packetRing, packetIndex);
        txPacket = &txQueue->packetContexts[packetIndex];
        if (!packet->Ignore) {
            if (packet->FragmentCount > freeDescs) {
                break;
            }
            fragmentIndex = packet->FragmentIndex;
            fragmentEndIndex = NetRingIncrementIndex(
                fragmentRing, fragmentIndex + packet->FragmentCount - 1);
            for (packetDescNum = 0; fragmentIndex != fragmentEndIndex;
                 packetDescNum++) {
                postedDescs = TRUE;
                fragment =
                    NetRingGetFragmentAtIndex(fragmentRing, fragmentIndex);
                length_status = (((USHORT)fragment->ValidLength)
                                 << BG_DMA_BD_LENGTH_SHIFT) |
                                BG_DMA_BG_STATUS_TX_QTAG;
                if (packetDescNum == 0) {
                    length_status |=
                        BG_DMA_BG_STATUS_SOP | BG_DMA_BG_STATUS_TX_CRC;
                }
                if (packetDescNum + 1 == packet->FragmentCount) {
                    length_status |= BG_DMA_BG_STATUS_EOP;
                }
                logicalAddress = NetExtensionGetFragmentLogicalAddress(
                    &txQueue->logicalAddressExtension, fragmentIndex);
                fragmentAddress =
                    logicalAddress->LogicalAddress + fragment->Offset;
                GWR(adapter, TDMA.BDs[prodDesc].Length_Status, length_status);
                GWR(adapter, TDMA.BDs[prodDesc].Address_Lo,
                    (ULONG)fragmentAddress);
                GWR(adapter, TDMA.BDs[prodDesc].Address_Hi,
                    (ULONG)(fragmentAddress >> 32));
                txPacket->lastDesc = prodDesc;
                txQueue->prodIndex = (txQueue->prodIndex + 1) & 0xffff;
                prodDesc = txQueue->prodIndex % txQueue->numDescs;
                fragmentIndex =
                    NetRingIncrementIndex(fragmentRing, fragmentIndex);
            }
            NT_FRE_ASSERT(packetDescNum == packet->FragmentCount);
            txPacket->endFragment = fragmentIndex;
            fragmentRing->NextIndex = fragmentIndex;
            freeDescs -= packet->FragmentCount;
        }
        packetIndex = NetRingIncrementIndex(packetRing, packetIndex);
    }
    packetRing->NextIndex = packetIndex;
    if (postedDescs) {
        GWR(adapter, TDMA.Rings[BG_DEFAULT_RING].TDMA_Prod_Index,
            txQueue->prodIndex);
    }

    packetIndex = packetRing->BeginIndex;
    fragmentIndex = fragmentRing->BeginIndex;
    txQueue->consIndex =
        GRD(adapter, TDMA.Rings[BG_DEFAULT_RING].TDMA_Cons_Index);
    consDesc = txQueue->consIndex % txQueue->numDescs;
    hwDescs = (prodDesc - consDesc) % txQueue->numDescs;
    while (packetIndex != packetRing->NextIndex) {
        packet = NetRingGetPacketAtIndex(packetRing, packetIndex);
        txPacket = &txQueue->packetContexts[packetIndex];
        if (!packet->Ignore) {
            if (((prodDesc - txPacket->lastDesc) % txQueue->numDescs) <=
                hwDescs) {
                break;
            }
            fragmentIndex = txPacket->endFragment;
        }
        packetIndex = NetRingIncrementIndex(packetRing, packetIndex);
    }
    packetRing->BeginIndex = packetIndex;
    fragmentRing->BeginIndex = fragmentIndex;
}

static void GenetTxQueueSetNotificationEnabled(NETPACKETQUEUE netTxQueue,
                                               BOOLEAN notificationEnabled) {
    GenetAdapter *adapter = GenetGetTxQueueContext(netTxQueue)->adapter;

    WdfSpinLockAcquire(adapter->lock);
    GenetTxInterruptSet(adapter, notificationEnabled);
    WdfSpinLockRelease(adapter->lock);
}

static void GenetTxQueueCancel(NETPACKETQUEUE netTxQueue) {
    UNREFERENCED_PARAMETER(netTxQueue);

    TraceInfo("Entry");
}

static void GenetTxQueueStart(NETPACKETQUEUE netTxQueue) {
    GenetTxQueue *txQueue = GenetGetTxQueueContext(netTxQueue);
    GenetAdapter *adapter = txQueue->adapter;
    ULONG regData;

    TraceInfo("Entry");
    txQueue->prodIndex = 0;
    txQueue->consIndex = 0;

    GWR(adapter, TDMA.Regs.SCB_Burst_Size, BG_MAX_DMA_BURST);

    GWR(adapter, TDMA.Rings[BG_DEFAULT_RING].TDMA_Read_Ptr, 0x0);
    GWR(adapter, TDMA.Rings[BG_DEFAULT_RING].TDMA_Read_Ptr_Hi, 0x0);
    GWR(adapter, TDMA.Rings[BG_DEFAULT_RING].TDMA_Cons_Index, 0x0);
    GWR(adapter, TDMA.Rings[BG_DEFAULT_RING].TDMA_Prod_Index, 0x0);
    GWR(adapter, TDMA.Rings[BG_DEFAULT_RING].DMA_Ring_Buf_Size,
        (BG_NUM_BDS << BG_DMA_RING_SIZE_SHIFT) | GENET_RX_BUFFER_SIZE);
    GWR(adapter, TDMA.Rings[BG_DEFAULT_RING].DMA_Start_Addr, 0x0);
    GWR(adapter, TDMA.Rings[BG_DEFAULT_RING].DMA_Start_Addr_Hi, 0x0);
    GWR(adapter, TDMA.Rings[BG_DEFAULT_RING].DMA_End_Addr,
        BG_NUM_BDS * sizeof(BG_DMA_DESC) / 4 - 1);
    GWR(adapter, TDMA.Rings[BG_DEFAULT_RING].DMA_End_Addr_Hi, 0x0);
    GWR(adapter, TDMA.Rings[BG_DEFAULT_RING].DMA_MBuf_Done_Thresh, 1);
    GWR(adapter, TDMA.Rings[BG_DEFAULT_RING].TDMA_Flow_Period, 0);
    GWR(adapter, TDMA.Rings[BG_DEFAULT_RING].TDMA_Write_Ptr, 0x0);
    GWR(adapter, TDMA.Rings[BG_DEFAULT_RING].TDMA_Write_Ptr_Hi, 0x0);

    GWR(adapter, TDMA.Regs.Ring_Cfg, 1 << BG_DEFAULT_RING);

    regData = GRD(adapter, TDMA.Regs.Ctrl);
    regData |= GENET_RING_DMA_EN;
    GWR(adapter, TDMA.Regs.Ctrl, regData);

    WdfSpinLockAcquire(adapter->lock);
    regData = GRD(adapter, UMAC.Cmd);
    regData |= BG_UMAC_CMD_TX_EN;
    GWR(adapter, UMAC.Cmd, regData);
    WdfSpinLockRelease(adapter->lock);
}

static void GenetTxQueueStop(NETPACKETQUEUE netTxQueue) {
    GenetTxQueue *txQueue = GenetGetTxQueueContext(netTxQueue);
    GenetAdapter *adapter = txQueue->adapter;
    ULONG regData;

    TraceInfo("Entry");
    regData = GRD(adapter, TDMA.Regs.Ctrl);
    regData &= ~GENET_RING_DMA_EN;
    GWR(adapter, TDMA.Regs.Ctrl, regData);

    GWR(adapter, UMAC.Tx_Flush, 1);
    KeStallExecutionProcessor(10);
    GWR(adapter, UMAC.Tx_Flush, 0);

    WdfSpinLockAcquire(adapter->lock);
    regData = GRD(adapter, UMAC.Cmd);
    regData &= ~BG_UMAC_CMD_TX_EN;
    GWR(adapter, UMAC.Cmd, regData);

    GenetTxInterruptSet(adapter, FALSE);
    WdfSpinLockRelease(adapter->lock);
}

static void GenetRxQueueCleanup(WDFOBJECT wdfRxQueue) {
    GenetRxQueue *rxQueue = GenetGetRxQueueContext((NETPACKETQUEUE)wdfRxQueue);
    ULONG curBuffer;
    void **virtualAddress;

    for (curBuffer = 0; curBuffer < rxQueue->numBuffers; ++curBuffer) {
        virtualAddress = &rxQueue->buffers[curBuffer].virtualAddress;
        if (*virtualAddress) {
            MmFreeContiguousMemory(*virtualAddress);
        }
        *virtualAddress = NULL;
    }
}

static void GenetRxQueueAdvance(NETPACKETQUEUE netRxQueue) {
    GenetRxQueue *rxQueue = GenetGetRxQueueContext(netRxQueue);
    GenetAdapter *adapter = rxQueue->adapter;
    NET_RING *fragmentRing = NetRingCollectionGetFragmentRing(rxQueue->rings);
    NET_RING *packetRing = NetRingCollectionGetPacketRing(rxQueue->rings);
    ULONG fragmentIndex;
    ULONG packetIndex;
    BOOLEAN postedDescs = FALSE;
    ULONG fragmentDesc;
    ULONG length_status;
    NET_FRAGMENT *fragment;
    NET_PACKET *packet;
    GenetRxBuffer *rxBuffer;
    NET_FRAGMENT_RETURN_CONTEXT *returnContext;
    NET_FRAGMENT_VIRTUAL_ADDRESS *virtualAddress;

    fragmentIndex = fragmentRing->BeginIndex;
    packetIndex = packetRing->BeginIndex;
    rxQueue->prodIndex =
        GRD(adapter, RDMA.Rings[BG_DEFAULT_RING].TDMA_Cons_Index) & 0xffff;
    while ((rxQueue->consIndex != rxQueue->prodIndex) &&
           (fragmentIndex != fragmentRing->EndIndex) &&
           (packetIndex != packetRing->EndIndex) && rxQueue->curFreeBuffer) {
        postedDescs = TRUE;
        fragmentDesc = rxQueue->consIndex % rxQueue->numDescs;
        rxQueue->consIndex = (rxQueue->consIndex + 1) & 0xffff;
        length_status = GRD(adapter, RDMA.BDs[fragmentDesc].Length_Status);
        if (!(length_status & BG_DMA_BG_STATUS_EOP) ||
            !(length_status & BG_DMA_BG_STATUS_SOP) ||
            (length_status & BG_DMA_BG_STATUS_RX_ERRORS)) {
            continue;
        }
        fragment = NetRingGetFragmentAtIndex(fragmentRing, fragmentIndex);
        fragment->Capacity = GENET_RX_BUFFER_SIZE;
        fragment->ValidLength = length_status >> BG_DMA_BD_LENGTH_SHIFT;
        fragment->Offset = 2;
        fragment->ValidLength -= 2;
        packet = NetRingGetPacketAtIndex(packetRing, packetIndex);
        packet->FragmentIndex = fragmentIndex;
        packet->FragmentCount = 1;
        rxBuffer = rxQueue->descBuffers[fragmentDesc];
        returnContext = NetExtensionGetFragmentReturnContext(
            &rxQueue->returnContextExtension, fragmentIndex);
        returnContext->Handle = (NET_FRAGMENT_RETURN_CONTEXT_HANDLE)rxBuffer;
        virtualAddress = NetExtensionGetFragmentVirtualAddress(
            &rxQueue->virtualAddressExtension, fragmentIndex);
        virtualAddress->VirtualAddress = rxBuffer->virtualAddress;
        KeFlushIoBuffers(&rxBuffer->rxMdl.mdl, TRUE, TRUE);
        GenetFillRxDesc(adapter, fragmentDesc);
        fragmentIndex = NetRingIncrementIndex(fragmentRing, fragmentIndex);
        packetIndex = NetRingIncrementIndex(packetRing, packetIndex);
    }
    if (rxQueue->canceled) {
        fragmentIndex = fragmentRing->EndIndex;
        while (packetIndex != packetRing->EndIndex) {
            packet = NetRingGetPacketAtIndex(packetRing, packetIndex);
            packet->Ignore = 1;
            packetIndex = NetRingIncrementIndex(packetRing, packetIndex);
        }
    }
    fragmentRing->BeginIndex = fragmentIndex;
    packetRing->BeginIndex = packetIndex;
    if (postedDescs) {
        GWR(adapter, RDMA.Rings[BG_DEFAULT_RING].TDMA_Prod_Index,
            rxQueue->consIndex);
    }
}

static void GenetRxQueueSetNotificationEnabled(NETPACKETQUEUE netRxQueue,
                                               BOOLEAN notificationEnabled) {
    GenetAdapter *adapter = GenetGetRxQueueContext(netRxQueue)->adapter;

    WdfSpinLockAcquire(adapter->lock);
    GenetRxInterruptSet(adapter, notificationEnabled);
    WdfSpinLockRelease(adapter->lock);
}

static void GenetRxQueueCancel(NETPACKETQUEUE netRxQueue) {
    GenetRxQueue *rxQueue = GenetGetRxQueueContext(netRxQueue);
    GenetAdapter *adapter = rxQueue->adapter;
    ULONG regData;

    TraceInfo("Entry");
    WdfSpinLockAcquire(adapter->lock);
    regData = GRD(adapter, UMAC.Cmd);
    regData &= ~BG_UMAC_CMD_RX_EN;
    GWR(adapter, UMAC.Cmd, regData);
    WdfSpinLockRelease(adapter->lock);

    regData = GRD(adapter, RDMA.Regs.Ctrl);
    regData &= ~GENET_RING_DMA_EN;
    GWR(adapter, RDMA.Regs.Ctrl, regData);

    rxQueue->canceled = TRUE;
}

static void GenetRxQueueStart(NETPACKETQUEUE netRxQueue) {
    GenetRxQueue *rxQueue = GenetGetRxQueueContext(netRxQueue);
    GenetAdapter *adapter = rxQueue->adapter;
    ULONG regData;
    ULONG curDesc;

    TraceInfo("Entry");
    rxQueue->prodIndex = 0;
    rxQueue->consIndex = 0;
    rxQueue->canceled = FALSE;

    GWR(adapter, RDMA.Regs.SCB_Burst_Size, BG_MAX_DMA_BURST);

    GWR(adapter, RDMA.Rings[BG_DEFAULT_RING].TDMA_Read_Ptr, 0x0);
    GWR(adapter, RDMA.Rings[BG_DEFAULT_RING].TDMA_Read_Ptr_Hi, 0x0);
    GWR(adapter, RDMA.Rings[BG_DEFAULT_RING].TDMA_Cons_Index, 0x0);
    GWR(adapter, RDMA.Rings[BG_DEFAULT_RING].TDMA_Prod_Index, 0x0);
    GWR(adapter, RDMA.Rings[BG_DEFAULT_RING].DMA_Ring_Buf_Size,
        (BG_NUM_BDS << BG_DMA_RING_SIZE_SHIFT) | GENET_RX_BUFFER_SIZE);
    GWR(adapter, RDMA.Rings[BG_DEFAULT_RING].DMA_Start_Addr, 0x0);
    GWR(adapter, RDMA.Rings[BG_DEFAULT_RING].DMA_Start_Addr_Hi, 0x0);
    GWR(adapter, RDMA.Rings[BG_DEFAULT_RING].DMA_End_Addr,
        BG_NUM_BDS * sizeof(BG_DMA_DESC) / 4 - 1);
    GWR(adapter, RDMA.Rings[BG_DEFAULT_RING].DMA_End_Addr_Hi, 0x0);
    GWR(adapter, RDMA.Rings[BG_DEFAULT_RING].TDMA_Flow_Period,
        (5 << BG_DMA_RING_XON_XOF_SHIFT) | (BG_NUM_BDS >> 4));
    GWR(adapter, RDMA.Rings[BG_DEFAULT_RING].TDMA_Write_Ptr, 0x0);
    GWR(adapter, RDMA.Rings[BG_DEFAULT_RING].TDMA_Write_Ptr_Hi, 0x0);

    GWR(adapter, RDMA.Regs.Ring_Cfg, 1 << BG_DEFAULT_RING);

    NT_FRE_ASSERT(rxQueue->curFreeBuffer >= rxQueue->numDescs);
    for (curDesc = 0; curDesc < rxQueue->numDescs; ++curDesc) {
        GenetFillRxDesc(adapter, curDesc);
    }

    regData = GRD(adapter, RDMA.Regs.Ctrl);
    regData |= GENET_RING_DMA_EN;
    GWR(adapter, RDMA.Regs.Ctrl, regData);

    WdfSpinLockAcquire(adapter->lock);
    regData = GRD(adapter, UMAC.Cmd);
    regData |= BG_UMAC_CMD_RX_EN;
    GWR(adapter, UMAC.Cmd, regData);
    WdfSpinLockRelease(adapter->lock);
}

static void GenetRxQueueStop(NETPACKETQUEUE netRxQueue) {
    GenetRxQueue *rxQueue = GenetGetRxQueueContext(netRxQueue);
    GenetAdapter *adapter = rxQueue->adapter;
    ULONG curDesc;
    GenetRxBuffer *rxBuffer;

    TraceInfo("Entry", TraceULX(rxQueue->curFreeBuffer, "FreeBuffer"),
              TraceULX(rxQueue->numBuffers, "NumBuffers"));
    WdfSpinLockAcquire(adapter->lock);
    GenetRxInterruptSet(adapter, FALSE);
    WdfSpinLockRelease(adapter->lock);

    for (curDesc = 0; curDesc < rxQueue->numDescs; ++curDesc) {
        rxBuffer = rxQueue->descBuffers[curDesc];
        if (rxBuffer) {
            GenetReturnRxBuffer(adapter->netAdapter,
                                (NET_FRAGMENT_RETURN_CONTEXT_HANDLE)rxBuffer);
            rxQueue->descBuffers[curDesc] = NULL;
        }
    }
}

static BOOLEAN GenetInterruptIsr(WDFINTERRUPT wdfInterrupt, ULONG messageId) {
    GenetInterrupt *interrupt = GenetGetInterruptContext(wdfInterrupt);
    GenetAdapter *adapter = interrupt->adapter;
    ULONG intrStatus;

    UNREFERENCED_PARAMETER(messageId);

    intrStatus = GRD(adapter, INTRL2_0.CPU_Status);
    intrStatus &= ~GRD(adapter, INTRL2_0.CPU_Mask_Status);
    GWR(adapter, INTRL2_0.CPU_Clear, intrStatus);
    GWR(adapter, INTRL2_0.CPU_Mask_Set, intrStatus);

    InterlockedOr((LONG volatile *)&interrupt->savedStatus, intrStatus);

    WdfInterruptQueueDpcForIsr(wdfInterrupt);

    return TRUE;
}

static void GenetInterruptDpc(WDFINTERRUPT wdfInterrupt,
                              WDFOBJECT associatedObject) {
    GenetInterrupt *interrupt = GenetGetInterruptContext(wdfInterrupt);
    GenetAdapter *adapter = interrupt->adapter;
    ULONG intrStatus =
        InterlockedExchange((LONG volatile *)&interrupt->savedStatus, 0);

    UNREFERENCED_PARAMETER(associatedObject);

    if (intrStatus & BG_INTR_TXDMA_DONE) {
        if (InterlockedExchange(&interrupt->txNotify, FALSE)) {
            NetTxQueueNotifyMoreCompletedPacketsAvailable(
                adapter->txQueue->netTxQueue);
        }
    }

    if (intrStatus & BG_INTR_RXDMA_DONE) {
        if (InterlockedExchange(&interrupt->rxNotify, FALSE)) {
            NetRxQueueNotifyMoreReceivedPacketsAvailable(
                adapter->rxQueue->netRxQueue);
        }
    }
}

static NTSTATUS GenetInterruptEnable(WDFINTERRUPT wdfInterrupt,
                                     WDFDEVICE wdfDevice) {
    UNREFERENCED_PARAMETER(wdfInterrupt);
    UNREFERENCED_PARAMETER(wdfDevice);

    TraceInfo("Entry");

    return STATUS_SUCCESS;
}

static NTSTATUS GenetInterruptDisable(WDFINTERRUPT wdfInterrupt,
                                      WDFDEVICE wdfDevice) {
    GenetAdapter *adapter = GenetGetInterruptContext(wdfInterrupt)->adapter;

    UNREFERENCED_PARAMETER(wdfDevice);

    TraceInfo("Entry");
    GWR(adapter, INTRL2_0.CPU_Mask_Set, 0xffffffff);
    GWR(adapter, INTRL2_0.CPU_Clear, 0xffffffff);

    return STATUS_SUCCESS;
}

static NTSTATUS GenetPrepareHardware(WDFDEVICE wdfDevice,
                                     WDFCMRESLIST resourcesRaw,
                                     WDFCMRESLIST resourcesTranslated) {
    GenetAdapter *adapter = GenetGetDeviceContext(wdfDevice)->adapter;
    NTSTATUS status;
    ULONG rawCount = WdfCmResourceListGetCount(resourcesRaw);
    CM_PARTIAL_RESOURCE_DESCRIPTOR *rawDescriptor, *translatedDescriptor;
    WDF_OBJECT_ATTRIBUTES interruptAttributes;
    WDF_INTERRUPT_CONFIG interruptConfig;
    WDFINTERRUPT wdfInterrupt;
    ULONG versionReg;
    UCHAR versionMajor, versionMinor;
    USHORT phyIdReg = 0;
    ULONG phyId = 0;

    if (rawCount < 2) {
        return STATUS_RESOURCE_TYPE_NOT_FOUND;
    }

    rawDescriptor = WdfCmResourceListGetDescriptor(resourcesRaw, 0);
    translatedDescriptor =
        WdfCmResourceListGetDescriptor(resourcesTranslated, 0);
    if (rawDescriptor->Type != CmResourceTypeMemory) {
        return STATUS_RESOURCE_TYPE_NOT_FOUND;
    }
    if (translatedDescriptor->u.Memory.Length != sizeof(GenetRegisters)) {
        return STATUS_INVALID_BUFFER_SIZE;
    }
    if ((adapter->registers = (GenetRegisters *)MmMapIoSpaceEx(
             translatedDescriptor->u.Memory.Start, sizeof(GenetRegisters),
             PAGE_READWRITE | PAGE_NOCACHE)) == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    TraceInfo(
        "RegistersPrepare",
        TraceUQX(translatedDescriptor->u.Memory.Start.QuadPart, "LAStart"),
        TraceUQX((ULONG64)adapter->registers, "VAStart"));

    rawDescriptor = WdfCmResourceListGetDescriptor(resourcesRaw, 1);
    translatedDescriptor =
        WdfCmResourceListGetDescriptor(resourcesTranslated, 1);
    if (rawDescriptor->Type != CmResourceTypeInterrupt) {
        return STATUS_RESOURCE_TYPE_NOT_FOUND;
    }
    TraceInfo(
        "InterruptPrepare",
        TraceULX(rawDescriptor->u.Interrupt.Level, "RawLevel"),
        TraceULX(rawDescriptor->u.Interrupt.Vector, "RawVector"),
        TraceULX(translatedDescriptor->u.Interrupt.Level, "TranslatedLevel"),
        TraceULX(translatedDescriptor->u.Interrupt.Vector, "TranslatedVector"));
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&interruptAttributes,
                                            GenetInterrupt);
    WDF_INTERRUPT_CONFIG_INIT(&interruptConfig, GenetInterruptIsr,
                              GenetInterruptDpc);
    interruptConfig.EvtInterruptEnable = GenetInterruptEnable;
    interruptConfig.EvtInterruptDisable = GenetInterruptDisable;
    interruptConfig.InterruptRaw = rawDescriptor;
    interruptConfig.InterruptTranslated = translatedDescriptor;
    status = WdfInterruptCreate(adapter->wdfDevice, &interruptConfig,
                                &interruptAttributes, &wdfInterrupt);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    adapter->interrupt = GenetGetInterruptContext(wdfInterrupt);
    adapter->interrupt->adapter = adapter;
    adapter->interrupt->wdfInterrupt = wdfInterrupt;

    versionReg = GRD(adapter, Sys.Rev_Ctrl);
    versionMajor = (versionReg >> 24) & 0x0f;
    versionMinor = (versionReg >> 16) & 0x0f;
    TraceInfo("HardwareVersion", TraceUCX(versionMajor, "Major"),
              TraceUCX(versionMinor, "Minor"));
    if (versionMajor != BG_MAJOR_V5) {
        return STATUS_NOT_FOUND;
    }

    status = GenetPhyRead(adapter, BG_MII_PHYSID1, &phyIdReg);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    phyId = phyIdReg << 16;
    status = GenetPhyRead(adapter, BG_MII_PHYSID2, &phyIdReg);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    phyId |= phyIdReg;
    TraceInfo("PhyModel", TraceULX(phyId, "Id"));
    if (phyId != BG_PHY_ID_BCM54213PE) {
        return STATUS_NOT_FOUND;
    }

    if (!(GRD(adapter, Sys.RBuf_Flush_Ctrl) & BG_SYS_RBUF_FLUSH_RESET)) {
        adapter->permanentMacAddress.Length = ETHERNET_LENGTH_OF_ADDRESS;
        *((ULONG *)adapter->permanentMacAddress.Address) =
            RtlUlongByteSwap(GRD(adapter, UMAC.MAC0));
        *((USHORT *)(&adapter->permanentMacAddress.Address[sizeof(ULONG)])) =
            RtlUshortByteSwap(GRD(adapter, UMAC.MAC1) & 0xffff);
    } else {
        return STATUS_NOT_FOUND;
    }

    status = NetConfigurationQueryLinkLayerAddress(adapter->netConfiguration,
                                                   &adapter->currentMacAddress);
    if (!NT_SUCCESS(status) ||
        adapter->currentMacAddress.Length != ETH_LENGTH_OF_ADDRESS ||
        ETH_IS_MULTICAST(adapter->currentMacAddress.Address) ||
        ETH_IS_BROADCAST(adapter->currentMacAddress.Address)) {
        RtlCopyMemory(&adapter->currentMacAddress,
                      &adapter->permanentMacAddress,
                      sizeof(adapter->permanentMacAddress));
    }
    TraceInfo("MacAddress",
              TraceB(adapter->permanentMacAddress.Address,
                     adapter->permanentMacAddress.Length, "Permanent"),
              TraceB(adapter->currentMacAddress.Address,
                     adapter->currentMacAddress.Length, "Current"));
    NetAdapterSetPermanentLinkLayerAddress(adapter->netAdapter,
                                           &adapter->permanentMacAddress);
    NetAdapterSetCurrentLinkLayerAddress(adapter->netAdapter,
                                         &adapter->currentMacAddress);

    status = GenetAdapterStart(adapter);

    return status;
}

static NTSTATUS GenetReleaseHardware(WDFDEVICE wdfDevice,
                                     WDFCMRESLIST resourcesTranslated) {
    GenetAdapter *adapter = GenetGetDeviceContext(wdfDevice)->adapter;

    UNREFERENCED_PARAMETER(resourcesTranslated);

    TraceInfo("Entry");
    if (adapter->registers) {
        MmUnmapIoSpace(adapter->registers, sizeof(GenetRegisters));
    }
    adapter->registers = NULL;

    return STATUS_SUCCESS;
}

static NTSTATUS GenetD0Entry(WDFDEVICE wdfDevice,
                             WDF_POWER_DEVICE_STATE previousState) {
    GenetAdapter *adapter = GenetGetDeviceContext(wdfDevice)->adapter;

    UNREFERENCED_PARAMETER(previousState);

    TraceInfo("Entry");
    GenetMacInitialize(adapter);
    GenetSetMacAddressFilters(adapter);
    GenetPhyInitialize(adapter);
    GenetMacPhyConfigure(adapter);
    WdfTimerStart(adapter->timer->wdfTimer, 0);

    return STATUS_SUCCESS;
}

static NTSTATUS GenetD0Exit(WDFDEVICE wdfDevice,
                            WDF_POWER_DEVICE_STATE targetState) {
    GenetAdapter *adapter = GenetGetDeviceContext(wdfDevice)->adapter;

    UNREFERENCED_PARAMETER(targetState);

    TraceInfo("Entry");
    WdfTimerStop(adapter->timer->wdfTimer, TRUE);

    return STATUS_SUCCESS;
}

static NTSTATUS GenetCreateTxQueue(NETADAPTER netAdapter,
                                   NETTXQUEUE_INIT *txQueueInit) {
    GenetAdapter *adapter = GenetGetAdapterContext(netAdapter);
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES txAttributes;
    NET_PACKET_QUEUE_CONFIG txConfig;
    NETPACKETQUEUE netTxQueue;
    GenetTxQueue *txQueue;
    NET_EXTENSION_QUERY extensionQuery;
    NET_RING *packetRing;
    NET_RING *fragmentRing;
    WDF_OBJECT_ATTRIBUTES packetContextAttributes;
    WDFMEMORY wdfPacketContextMemory = NULL;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&txAttributes, GenetTxQueue);
    NET_PACKET_QUEUE_CONFIG_INIT(&txConfig, GenetTxQueueAdvance,
                                 GenetTxQueueSetNotificationEnabled,
                                 GenetTxQueueCancel);
    txConfig.EvtStart = GenetTxQueueStart;
    txConfig.EvtStop = GenetTxQueueStop;
    status =
        NetTxQueueCreate(txQueueInit, &txAttributes, &txConfig, &netTxQueue);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    txQueue = GenetGetTxQueueContext(netTxQueue);
    txQueue->adapter = adapter;
    txQueue->netTxQueue = netTxQueue;
    adapter->txQueue = txQueue;

    NET_EXTENSION_QUERY_INIT(&extensionQuery,
                             NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
                             NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1,
                             NetExtensionTypeFragment);
    NetTxQueueGetExtension(netTxQueue, &extensionQuery,
                           &txQueue->virtualAddressExtension);

    NET_EXTENSION_QUERY_INIT(&extensionQuery,
                             NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_NAME,
                             NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_VERSION_1,
                             NetExtensionTypeFragment);
    NetTxQueueGetExtension(netTxQueue, &extensionQuery,
                           &txQueue->logicalAddressExtension);

    txQueue->rings = NetTxQueueGetRingCollection(netTxQueue);
    txQueue->numDescs = BG_NUM_BDS;
    packetRing = NetRingCollectionGetPacketRing(txQueue->rings);
    fragmentRing = NetRingCollectionGetFragmentRing(txQueue->rings);

    WDF_OBJECT_ATTRIBUTES_INIT(&packetContextAttributes);
    packetContextAttributes.ParentObject = netTxQueue;
    status = WdfMemoryCreate(
        &packetContextAttributes, NonPagedPoolNx, 0,
        sizeof(GenetTxPacket) * packetRing->NumberOfElements,
        &wdfPacketContextMemory, (void **)&txQueue->packetContexts);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS GenetCreateRxQueue(NETADAPTER netAdapter,
                                   NETRXQUEUE_INIT *rxQueueInit) {
    GenetAdapter *adapter = GenetGetAdapterContext(netAdapter);
    NTSTATUS status;
    ULONG queueId;
    WDF_OBJECT_ATTRIBUTES rxAttributes;
    NET_PACKET_QUEUE_CONFIG rxConfig;
    NETPACKETQUEUE netRxQueue;
    GenetRxQueue *rxQueue;
    NET_EXTENSION_QUERY extensionQuery;
    NET_RING *fragmentRing;
    WDF_OBJECT_ATTRIBUTES memoryAttributes;
    WDFMEMORY wdfBuffersMemory = NULL;
    WDFMEMORY wdfFreeBuffersMemory = NULL;
    WDFMEMORY wdfDescBuffersMemory = NULL;
    ULONG curBuffer;
    GenetRxBuffer *curRxBuffer;
    void *virtualAddress;
    const PHYSICAL_ADDRESS zeroAddress = {0};
    const PHYSICAL_ADDRESS maxAddress = {UINT64_MAX};
    MDL *mdl;

    queueId = NetRxQueueInitGetQueueId(rxQueueInit);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&rxAttributes, GenetRxQueue);
    rxAttributes.EvtCleanupCallback = GenetRxQueueCleanup;
    NET_PACKET_QUEUE_CONFIG_INIT(&rxConfig, GenetRxQueueAdvance,
                                 GenetRxQueueSetNotificationEnabled,
                                 GenetRxQueueCancel);
    rxConfig.EvtStart = GenetRxQueueStart;
    rxConfig.EvtStop = GenetRxQueueStop;
    status =
        NetRxQueueCreate(rxQueueInit, &rxAttributes, &rxConfig, &netRxQueue);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    rxQueue = GenetGetRxQueueContext(netRxQueue);
    rxQueue->adapter = adapter;
    rxQueue->netRxQueue = netRxQueue;
    rxQueue->queueId = queueId;
    adapter->rxQueue = rxQueue;

    NET_EXTENSION_QUERY_INIT(&extensionQuery,
                             NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
                             NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1,
                             NetExtensionTypeFragment);
    NetRxQueueGetExtension(netRxQueue, &extensionQuery,
                           &rxQueue->virtualAddressExtension);

    NET_EXTENSION_QUERY_INIT(&extensionQuery,
                             NET_FRAGMENT_EXTENSION_RETURN_CONTEXT_NAME,
                             NET_FRAGMENT_EXTENSION_RETURN_CONTEXT_VERSION_1,
                             NetExtensionTypeFragment);
    NetRxQueueGetExtension(netRxQueue, &extensionQuery,
                           &rxQueue->returnContextExtension);

    rxQueue->rings = NetRxQueueGetRingCollection(netRxQueue);
    rxQueue->numDescs = BG_NUM_BDS;
    fragmentRing = NetRingCollectionGetFragmentRing(rxQueue->rings);
    rxQueue->numBuffers = rxQueue->numDescs * 2;
    if (fragmentRing->NumberOfElements > rxQueue->numBuffers) {
        rxQueue->numBuffers = fragmentRing->NumberOfElements;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&memoryAttributes);
    memoryAttributes.ParentObject = netRxQueue;
    status = WdfMemoryCreate(&memoryAttributes, NonPagedPoolNx, 0,
                             sizeof(GenetRxBuffer) * rxQueue->numBuffers,
                             &wdfBuffersMemory, (void **)&rxQueue->buffers);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status =
        WdfMemoryCreate(&memoryAttributes, NonPagedPoolNx, 0,
                        sizeof(GenetRxBuffer *) * rxQueue->numBuffers,
                        &wdfFreeBuffersMemory, (void **)&rxQueue->freeBuffers);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status =
        WdfMemoryCreate(&memoryAttributes, NonPagedPoolNx, 0,
                        sizeof(GenetRxBuffer *) * rxQueue->numDescs,
                        &wdfDescBuffersMemory, (void **)&rxQueue->descBuffers);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    for (curBuffer = 0; curBuffer < rxQueue->numBuffers; ++curBuffer) {
        curRxBuffer = &rxQueue->buffers[curBuffer];
        virtualAddress = MmAllocateContiguousMemorySpecifyCache(
            GENET_RX_BUFFER_SIZE, zeroAddress, maxAddress, zeroAddress,
            MmCached);
        if (virtualAddress == NULL) {
            TraceError("NoRxBufferMemory");
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        curRxBuffer->virtualAddress = virtualAddress;
        mdl = &curRxBuffer->rxMdl.mdl;
        MmInitializeMdl(mdl, virtualAddress, GENET_RX_BUFFER_SIZE);
        MmBuildMdlForNonPagedPool(mdl);
        curRxBuffer->logicalAddress =
            (MmGetMdlPfnArray(mdl)[0] << PAGE_SHIFT) + MmGetMdlByteOffset(mdl);
        rxQueue->freeBuffers[curBuffer] = curRxBuffer;
    }
    rxQueue->curFreeBuffer = rxQueue->numBuffers;

    return STATUS_SUCCESS;
}

static void GenetTimerFunc(WDFTIMER wdfTimer) {
    GenetAdapter *adapter = GenetGetTimerContext(wdfTimer)->adapter;
    NTSTATUS status;
    NET_ADAPTER_LINK_STATE linkState;

    WdfSpinLockAcquire(adapter->lock);
    status = GenetPhyReadLinkState(adapter, &linkState);
    if (NT_SUCCESS(status)) {
        GenetMacSetLinkState(adapter, &linkState);
    }
    WdfSpinLockRelease(adapter->lock);

    if (!NT_SUCCESS(status)) {
        NET_ADAPTER_LINK_STATE_INIT_DISCONNECTED(&linkState);
    }

    NetAdapterSetLinkState(adapter->netAdapter, &linkState);
}

static NTSTATUS GenetDeviceAdd(WDFDRIVER driver,
                               struct WDFDEVICE_INIT *deviceInit) {
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDFDEVICE wdfDevice;
    WDF_DMA_ENABLER_CONFIG dmaEnablerConfig;
    WDFDMAENABLER dmaEnabler;
    NETADAPTER_INIT *adapterInit;
    NET_ADAPTER_DATAPATH_CALLBACKS datapathCallbacks;
    WDF_OBJECT_ATTRIBUTES adapterAttributes;
    NETADAPTER netAdapter;
    GenetDevice *device;
    GenetAdapter *adapter;
    WDF_OBJECT_ATTRIBUTES lockAttributes;
    WDF_OBJECT_ATTRIBUTES timerAttributes;
    WDF_TIMER_CONFIG timerConfig;
    WDFTIMER wdfTimer;
    GenetTimer *timer;

    UNREFERENCED_PARAMETER(driver);

    status = NetDeviceInitConfig(deviceInit);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, GenetDevice);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = GenetPrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = GenetReleaseHardware;
    pnpPowerCallbacks.EvtDeviceD0Entry = GenetD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit = GenetD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnpPowerCallbacks);
    status = WdfDeviceCreate(&deviceInit, &deviceAttributes, &wdfDevice);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WdfDeviceSetAlignmentRequirement(wdfDevice, FILE_QUAD_ALIGNMENT);
    WDF_DMA_ENABLER_CONFIG_INIT(&dmaEnablerConfig, WdfDmaProfileScatterGather64,
                                GENET_RX_BUFFER_SIZE);
    dmaEnablerConfig.Flags |= WDF_DMA_ENABLER_CONFIG_REQUIRE_SINGLE_TRANSFER;
    dmaEnablerConfig.WdmDmaVersionOverride = 3;
    status = WdfDmaEnablerCreate(wdfDevice, &dmaEnablerConfig,
                                 WDF_NO_OBJECT_ATTRIBUTES, &dmaEnabler);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    adapterInit = NetAdapterInitAllocate(wdfDevice);
    if (!adapterInit) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    NET_ADAPTER_DATAPATH_CALLBACKS_INIT(&datapathCallbacks, GenetCreateTxQueue,
                                        GenetCreateRxQueue);
    NetAdapterInitSetDatapathCallbacks(adapterInit, &datapathCallbacks);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&adapterAttributes, GenetAdapter);
    status = NetAdapterCreate(adapterInit, &adapterAttributes, &netAdapter);
    NetAdapterInitFree(adapterInit);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    device = GenetGetDeviceContext(wdfDevice);
    adapter = GenetGetAdapterContext(netAdapter);
    device->adapter = adapter;
    adapter->wdfDevice = wdfDevice;
    adapter->netAdapter = netAdapter;
    adapter->dmaEnabler = dmaEnabler;

    status = NetAdapterOpenConfiguration(netAdapter, WDF_NO_OBJECT_ATTRIBUTES,
                                         &adapter->netConfiguration);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&lockAttributes);
    lockAttributes.ParentObject = wdfDevice;
    status = WdfSpinLockCreate(&lockAttributes, &adapter->lock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&timerAttributes, GenetTimer);
    timerAttributes.ParentObject = wdfDevice;
    WDF_TIMER_CONFIG_INIT_PERIODIC(&timerConfig, GenetTimerFunc, 1000);
    status = WdfTimerCreate(&timerConfig, &timerAttributes, &wdfTimer);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    timer = GenetGetTimerContext(wdfTimer);
    timer->adapter = adapter;
    timer->wdfTimer = wdfTimer;
    adapter->timer = timer;

    return status;
}

static void GenetDriverUnload(WDFDRIVER driver) {
    UNREFERENCED_PARAMETER(driver);

    TraceLoggingUnregister(GenetTraceProvider);

    return;
}

NTSTATUS DriverEntry(DRIVER_OBJECT *driverObject,
                     UNICODE_STRING *registryPath) {
    NTSTATUS status;
    WDF_DRIVER_CONFIG driverConfig;

    status = TraceLoggingRegister(GenetTraceProvider);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_DRIVER_CONFIG_INIT(&driverConfig, GenetDeviceAdd);
    driverConfig.EvtDriverUnload = GenetDriverUnload;
    driverConfig.DriverPoolTag = 'gmcB';
    status = WdfDriverCreate(driverObject, registryPath,
                             WDF_NO_OBJECT_ATTRIBUTES, &driverConfig, NULL);
    if (!NT_SUCCESS(status)) {
        TraceLoggingUnregister(GenetTraceProvider);
    }

    return status;
}
