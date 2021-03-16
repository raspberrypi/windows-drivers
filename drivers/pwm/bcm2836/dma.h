/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Abstract:

    This file contains definitions for BCM2836 DMA controller.

--*/

#pragma once

//
// Full DMA buffer size. The buffer holds as much audio packets as possible.
//

#define DMA_BUFFER_PAGE_COUNT           16
#define DMA_BUFFER_SIZE                 (DMA_BUFFER_PAGE_COUNT * PAGE_SIZE)

//
// At the very end of the packet we add a CB for a small data block to generate
// an interrupt and do packet processing.
//

#define AUDIO_PACKET_LAST_CHUNK_SIZE    32

//
// DMA DREQ assingments
//

#define DMA_DREQ_PWM                5

//
// DMA Control Block (needs to be 256bit aligned)
//

typedef struct _DMA_CB
{
    __declspec(align(32)) ULONG TI;
    ULONG SOURCE_AD;
    ULONG DEST_AD;
    ULONG TXFR_LEN;
    ULONG STRIDE;
    ULONG NEXTCONBK;
    ULONG RSVD0;
    ULONG RSVD1;
} DMA_CB,*PDMA_CB;

//
// List for notification events
//

typedef struct _NOTIFICATION_LIST_ENTRY
{
    LIST_ENTRY  ListEntry;
    PKEVENT     NotificationEvent;
} NOTIFICATION_LIST_ENTRY, *PNOTIFICATION_LIST_ENTRY;

//
// DMA Control and status (CS)
//

#define DMA_CS_ACTIVE                           (1<<0)
#define DMA_CS_END                              (1<<1)
#define DMA_CS_INT                              (1<<2)
#define DMA_CS_DREQ                             (1<<3)
#define DMA_CS_PAUSED                           (1<<4)
#define DMA_CS_DREQ_STOPS_DMA                   (1<<5)
#define DMA_CS_WAITING_FOR_OUTSTANDING_WRITES   (1<<6)
#define DMA_CS_ERROR                            (1<<8)
#define DMA_CS_PRIORITY_SHIFT                   16
#define DMA_CS_PRIORITY_MASK                    (0xF<<DMA_CS_PRIORITY_SHIFT)
#define DMA_CS_PANIC_PRIORITY_SHIFT             20
#define DMA_CS_PANIC_PRIORITY_MASK              (0xF<<DMA_CS_PANIC_PRIORITY_SHIFT)
#define DMA_CS_WAIT_FOR_OUTSTANDING_WRITES      (1<<28)
#define DMA_CS_DISDEBUG                         (1<<29)
#define DMA_CS_ABORT                            (1<<30)
#define DMA_CS_RESET                            (1<<31)

#define DMA_CS_PRIORITY_8                       (8 << DMA_CS_PRIORITY_SHIFT)
#define DMA_CS_PANIC_PRIORITY_F                 (0xF << DMA_CS_PANIC_PRIORITY_SHIFT)

//
// DMA Transfer information (TI)
//

#define DMA_TI_INTEN                            (1<<0)
#define DMA_TI_TDMOCE                           (1<<1)
#define DMA_TI_WAIT_RESP                        (1<<3)
#define DMA_TI_DEST_INC                         (1<<4)
#define DMA_TI_DEST_WIDTH_128BIT                (1<<5)
#define DMA_TI_DEST_DREQ                        (1<<6)
#define DMA_TI_DEST_IGNORE                      (1<<7)
#define DMA_TI_SRC_INC                          (1<<8)
#define DMA_TI_SRC_WIDTH_128BIT                 (1<<9)
#define DMA_TI_SRC_DREQ                         (1<<10)
#define DMA_TI_SRC_IGNORE                       (1<<11)
#define DMA_TI_BURST_LENGTH_SHIFT               12
#define DMA_TI_BURST_LENGTH_MASK                (0xF<<DMA_TI_BURST_LENGTH_SHIFT)
#define DMA_TI_BURST_LENGTH_0                   (0<<DMA_TI_BURST_LENGTH_SHIFT)
#define DMA_TI_BURST_LENGTH_1                   (1<<DMA_TI_BURST_LENGTH_SHIFT)
#define DMA_TI_BURST_LENGTH_2                   (2<<DMA_TI_BURST_LENGTH_SHIFT)
#define DMA_TI_BURST_LENGTH_3                   (3<<DMA_TI_BURST_LENGTH_SHIFT)
#define DMA_TI_BURST_LENGTH_4                   (4<<DMA_TI_BURST_LENGTH_SHIFT)
#define DMA_TI_BURST_LENGTH_5                   (5<<DMA_TI_BURST_LENGTH_SHIFT)
#define DMA_TI_BURST_LENGTH_6                   (6<<DMA_TI_BURST_LENGTH_SHIFT)
#define DMA_TI_BURST_LENGTH_7                   (7<<DMA_TI_BURST_LENGTH_SHIFT)
#define DMA_TI_BURST_LENGTH_8                   (8<<DMA_TI_BURST_LENGTH_SHIFT)
#define DMA_TI_BURST_LENGTH_9                   (9<<DMA_TI_BURST_LENGTH_SHIFT)
#define DMA_TI_BURST_LENGTH_10                  (10<<DMA_TI_BURST_LENGTH_SHIFT)
#define DMA_TI_BURST_LENGTH_11                  (11<<DMA_TI_BURST_LENGTH_SHIFT)
#define DMA_TI_BURST_LENGTH_12                  (12<<DMA_TI_BURST_LENGTH_SHIFT)
#define DMA_TI_BURST_LENGTH_13                  (13<<DMA_TI_BURST_LENGTH_SHIFT)
#define DMA_TI_BURST_LENGTH_14                  (14<<DMA_TI_BURST_LENGTH_SHIFT)
#define DMA_TI_BURST_LENGTH_15                  (15<<DMA_TI_BURST_LENGTH_SHIFT)
#define DMA_TI_PERMAP_SHIFT                     16
#define DMA_TI_PERMAP_MASK                      (0xF<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_WAITS_SHIFT                      21
#define DMA_TI_WAITS_MASK                       (0xF<<DMA_TI_WAITS_SHIFT)
#define DMA_TI_NO_WIDE_BURSTS                   (1<<26)

#define DMA_TI_PERMAP_ALWAYSON  (0<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_DSI0      (1<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_PCMTX     (2<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_PCMRX     (3<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_SMI       (4<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_PWM       (DMA_DREQ_PWM<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_SPITX     (6<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_SPIRX     (7<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_BSC_SPITX (8<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_BSC_SPIRX (9<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_EMMC      (11<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_UARTTX    (12<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_SDHOST    (13<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_UARTRX    (14<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_DSI1      (15<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_MICTX     (16<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_HDMI      (17<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_MICRX     (18<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_DC0       (19<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_DC1       (20<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_DC2       (21<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_DC3       (22<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_DC4       (23<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_SCFIFO0   (24<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_SCFIFO1   (25<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_SCFIFO2   (26<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_DC5       (27<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_DC6       (28<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_DC7       (29<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_DC8       (30<<DMA_TI_PERMAP_SHIFT)
#define DMA_TI_PERMAP_DC9       (31<<DMA_TI_PERMAP_SHIFT)

//
// DMA Transfer length (TXFR_LEN)
//

#define DMA_TXFER_LEN_XLENGTH_SHIFT 0
#define DMA_TXFER_LEN_XLENGTH_MASK (0xFFFF<<DMA_TXFER_LEN_XLENGTH_SHIFT)
#define DMA_TXFER_LEN_YLENGTH_SHIFT 16
#define DMA_TXFER_LEN_YLENGTH_MASK (0x3FFF<<DMA_TXFER_LEN_YLENGTH_SHIFT)

//
// DMA Stride (STRIDE)
//

#define DMA_STRIDE_S_STRIDE_SHIFT 0
#define DMA_STRIDE_S_STRIDE_MASK (0xFFFF<<DMA_STRIDE_S_STRIDE_SHIFT)
#define DMA_STRIDE_D_STRIDE_SHIFT 16
#define DMA_STRIDE_D_STRIDE_MASK (0xFFFF<<DMA_STRIDE_D_STRIDE_SHIFT)

//
// DMA Debug (DEBUG)
//

#define DMA_DEBUG_READ_LAST_NOT_SET_ERROR (1<<0)
#define DMA_DEBUG_FIFO_ERROR (1<<1)
#define DMA_DEBUG_READ_ERROR (1<<2)
#define DMA_DEBUG_OUTSTANDING_WRITES_SHIFT 4
#define DMA_DEBUG_OUTSTANDING_WRITES_MASK (0xF<<DMA_DEBUG_OUTSTANDING_WRITES_SHIFT)
#define DMA_DEBUG_DMA_ID_SHIFT 8
#define DMA_DEBUG_DMA_ID_MASK (0xFF<<DMA_DEBUG_DMA_ID_SHIFT)
#define DMA_DEBUG_DMA_STATE_SHIFT 16
#define DMA_DEBUG_DMA_STATE_MASK (0x1FF<<DMA_DEBUG_DMA_STATE_SHIFT)
#define DMA_DEBUG_DMA_LITE (1<<28)

//
// DMA Channel registers
//

typedef struct _DMA_CHANNEL_REGS
{
    ULONG CS;
    ULONG CONBLK_AD;
    ULONG TI;
    ULONG SOURCE_AD;
    ULONG DEST_AD;
    ULONG TXFR_LEN;
    ULONG STRIDE;
    ULONG NEXTCONBK;
    ULONG DEBUG;
} DMA_CHANNEL_REGS, *PDMA_CHANNEL_REGS;


_Must_inspect_result_
NTSTATUS
AllocateDmaBuffer(
    _Inout_ PDEVICE_CONTEXT DeviceContext
);

VOID
StopDma(
    _In_ PDEVICE_CONTEXT DeviceContext
);

VOID
StartDma(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ PHYSICAL_ADDRESS ControlBlockPa
);

VOID
PauseDma(
    _In_ PDEVICE_CONTEXT DeviceContext
);

VOID
ResumeDma(
    _In_ PDEVICE_CONTEXT DeviceContext
);

NTSTATUS
InitializeAudio(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
);

NTSTATUS
RegisterAudioNotification(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
);

NTSTATUS
UnregisterAudioNotification(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
);

NTSTATUS
StartAudio(
    _In_ WDFDEVICE Device
);

NTSTATUS
StopAudio(
_In_ WDFDEVICE Device
);

NTSTATUS
PauseAudio(
    _In_ WDFDEVICE Device
);

NTSTATUS
ResumeAudio(
    _In_ WDFDEVICE Device
);
