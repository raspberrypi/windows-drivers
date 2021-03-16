//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//
// Module Name:
//
//     vchiq_core.h
//
// Abstract:
//
//     This header contains sepcific definition from Raspberry Pi
//     public userland repo. This is based on the vchiq_core.h header.
//     Only minimal definitions are ported over.
//

#pragma once

EXTERN_C_START

#define VCHIQ_SLOT_SIZE            4096

#define VCHIQ_SLOT_MASK          (VCHIQ_SLOT_SIZE - 1)
#define VCHIQ_SLOT_QUEUE_MASK  (VCHIQ_MAX_SLOTS_PER_SIDE - 1)
#define VCHIQ_SLOT_ZERO_SLOTS  ((sizeof(VCHIQ_SLOT_ZERO) + \
    VCHIQ_SLOT_SIZE - 1) / VCHIQ_SLOT_SIZE)


#define VCHIQ_MSG_PADDING           0  /* -                                            */
#define VCHIQ_MSG_CONNECT           1  /* -                                            */
#define VCHIQ_MSG_OPEN              2  /* + (srcport, -), fourcc, client_id */
#define VCHIQ_MSG_OPENACK           3  /* + (srcport, dstport)                  */
#define VCHIQ_MSG_CLOSE             4  /* + (srcport, dstport)                  */
#define VCHIQ_MSG_DATA              5  /* + (srcport, dstport)                  */
#define VCHIQ_MSG_BULK_RX           6  /* + (srcport, dstport), data, size  */
#define VCHIQ_MSG_BULK_TX           7  /* + (srcport, dstport), data, size  */
#define VCHIQ_MSG_BULK_RX_DONE      8  /* + (srcport, dstport), actual        */
#define VCHIQ_MSG_BULK_TX_DONE      9  /* + (srcport, dstport), actual        */
#define VCHIQ_MSG_PAUSE             10  /* -                                            */
#define VCHIQ_MSG_RESUME            11  /* -                                            */
#define VCHIQ_MSG_REMOTE_USE        12  /* -                                            */
#define VCHIQ_MSG_REMOTE_RELEASE    13  /* -                                            */
#define VCHIQ_MSG_REMOTE_USE_ACTIVE 14  /* -                                            */

#define VCHIQ_PORT_MAX                      (VCHIQ_MAX_SERVICES - 1)
#define VCHIQ_PORT_FREE                     0x1000
#define VCHIQ_PORT_IS_VALID(port)        (port < VCHIQ_PORT_FREE)
#define VCHIQ_MAKE_MSG(type, srcport, dstport) \
    ((type<<24) | (srcport<<12) | (dstport<<0))
#define VCHIQ_MSG_TYPE(msgid)             ((unsigned int)msgid >> 24)
#define VCHIQ_MSG_SRCPORT(msgid) \
    (unsigned short)(((unsigned int)msgid >> 12) & 0xfff)
#define VCHIQ_MSG_DSTPORT(msgid) \
    ((unsigned short)msgid & 0xfff)

#define VCHIQ_FOURCC_AS_4CHARS(fourcc)    \
    ((fourcc) >> 24) & 0xff, \
    ((fourcc) >> 16) & 0xff, \
    ((fourcc) >>  8) & 0xff, \
    (fourcc) & 0xff

#define VCHIQ_MSGID_PADDING                VCHIQ_MAKE_MSG(VCHIQ_MSG_PADDING, 0, 0)
#define VCHIQ_MSGID_CLAIMED                0x40000000

#define VCHIQ_FOURCC_INVALID              0x00000000
#define VCHIQ_FOURCC_IS_LEGAL(fourcc)  (fourcc != VCHIQ_FOURCC_INVALID)

#define VCHIQ_BULK_ACTUAL_ABORTED -1

enum
{
    DEBUG_ENTRIES,
#if VCHIQ_ENABLE_DEBUG
    DEBUG_SLOT_HANDLER_COUNT,
    DEBUG_SLOT_HANDLER_LINE,
    DEBUG_PARSE_LINE,
    DEBUG_PARSE_HEADER,
    DEBUG_PARSE_MSGID,
    DEBUG_AWAIT_COMPLETION_LINE,
    DEBUG_DEQUEUE_MESSAGE_LINE,
    DEBUG_SERVICE_CALLBACK_LINE,
    DEBUG_MSG_QUEUE_FULL_COUNT,
    DEBUG_COMPLETION_QUEUE_FULL_COUNT,
#endif
    DEBUG_MAX
};

#include <pshpack1.h>

typedef struct _VCHIQ_REMOTE_EVENT {
    ULONG Armed;
    ULONG Fired;

    // This is unused for now
    ULONG Semaphore;
} VCHIQ_REMOTE_EVENT;

typedef struct _VCHIQ_SLOT {
    char Data[VCHIQ_SLOT_SIZE];
} VCHIQ_SLOT, *PVCHIQ_SLOT;

typedef struct VCHIQ_SLOT_INFO_T {
    SHORT UseCount;
    SHORT ReleaseCount;
} VCHIQ_SLOT_INFO_T;

typedef struct _VCHIQ_SHARED_STATE {
    /* A non-zero value here indicates that the content is valid. */
    ULONG Initialised;

    /* The first and last (inclusive) slots allocated to the owner. */
    ULONG SlotFirst;
    ULONG SlotLast;

    /* The slot allocated to synchronous messages from the owner. */
    ULONG SlotSync;

    /* Signalling this event indicates that owner's slot handler thread
    ** should run. */
    VCHIQ_REMOTE_EVENT Trigger;

    /* Indicates the byte position within the stream where the next message
    ** will be written. The least significant bits are an index into the
    ** slot. The next bits are the index of the slot in slot_queue. */
    ULONG TxPos;

    /* This event should be signalled when a slot is recycled. */
    VCHIQ_REMOTE_EVENT Recycle;

    /* The slot_queue index where the next recycled slot will be written. */
    ULONG SlotQueueRecycle;

    /* This event should be signalled when a synchronous message is sent. */
    VCHIQ_REMOTE_EVENT SyncTrigger;

    /* This event should be signalled when a synchronous message has been
    ** released. */
    VCHIQ_REMOTE_EVENT SyncRelease;

    /* A circular buffer of slot indexes. */
    ULONG SlotQueue[VCHIQ_MAX_SLOTS_PER_SIDE];

    /* Debugging state */
    ULONG Debug[DEBUG_MAX];
} VCHIQ_SHARED_STATE;

typedef struct _VCHIQ_SLOT_ZERO {
    ULONG Magic;
    SHORT Version;
    SHORT VersionMin;
    ULONG SlotZeroSize;
    ULONG SlotSize;
    ULONG MaxSlots;
    ULONG MaxSlotsPerSide;
    ULONG PlatformData[2];
    VCHIQ_SHARED_STATE Master;
    VCHIQ_SHARED_STATE Slave;
    VCHIQ_SLOT_INFO_T Slots[VCHIQ_MAX_SLOTS];
} VCHIQ_SLOT_ZERO;

#include <poppack.h>

EXTERN_C_END
