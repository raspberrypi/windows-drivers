/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Abstract:
    Header file for common stuff.

--*/

#pragma once

// Fix warning from stdunk.h by disabling back-compat hack.
#define _NEW_DELETE_OPERATORS_

#include <portcls.h>
#include <stdunk.h>
#include <ksdebug.h>
#include <ntintsafe.h>
#include <wdf.h>
#include <wdfminiport.h>
#include <MsApoFxProxy.h>
#include <Ntstrsafe.h>
#include <bcm2836pwm.h>

//=============================================================================
// Defines
//=============================================================================

// Version number. Revision numbers are specified for each sample.
#define RPIWAV_VERSION               1

// Revision number.
#define RPIWAV_REVISION              0

// Product Id
// {DCA49BAE-8129-4901-91E2-BAF50AF71D96}
#define STATIC_PID_RPIWAV\
    0xdca49bae, 0x8129, 0x4901, 0x91, 0xe2, 0xba, 0xf5, 0x0a, 0xf7, 0x1d, 0x96
DEFINE_GUIDSTRUCT("DCA49BAE-8129-4901-91E2-BAF50AF71D96", PID_RPIWAV);
#define PID_RPIWAV DEFINE_GUIDNAMED(PID_RPIWAV)

// Pool tag used for RPIWAV allocations
#define RPIWAV_POOLTAG               '2IPR'  

// Debug module name
#define STR_MODULENAME              "RPIWAV: "

// Debug utility macros
#define D_FUNC                      5
#define D_BLAB                      DEBUGLVL_BLAB
#define D_VERBOSE                   DEBUGLVL_VERBOSE        
#define D_TERSE                     DEBUGLVL_TERSE          
#define D_ERROR                     DEBUGLVL_ERROR          
#define DPF                         _DbgPrintF
#define DPF_ENTER(x)                DPF(D_FUNC, x)

#define KSPROPERTY_TYPE_ALL         KSPROPERTY_TYPE_BASICSUPPORT | \
                                    KSPROPERTY_TYPE_GET | \
                                    KSPROPERTY_TYPE_SET
 
// Flags to identify stream processing mode
typedef enum {
    SIGNALPROCESSINGMODE_NONE           = 0x00,
    SIGNALPROCESSINGMODE_DEFAULT        = 0x01,
    SIGNALPROCESSINGMODE_RAW            = 0x02,
    SIGNALPROCESSINGMODE_COMMUNICATIONS = 0x04,
    SIGNALPROCESSINGMODE_SPEECH         = 0x08,
    SIGNALPROCESSINGMODE_NOTIFICATION   = 0x10,
    SIGNALPROCESSINGMODE_MEDIA          = 0x20,
    SIGNALPROCESSINGMODE_MOVIE          = 0x40
} SIGNALPROCESSINGMODE;

#define MAP_GUID_TO_MODE(guid, mode)                                                  \
    if (IsEqualGUID(guid, AUDIO_SIGNALPROCESSINGMODE_DEFAULT))                        \
    {                                                                                 \
        mode = SIGNALPROCESSINGMODE_DEFAULT;                                          \
    }                                                                                 \
    else if (IsEqualGUID(guid, AUDIO_SIGNALPROCESSINGMODE_RAW))                       \
    {                                                                                 \
        mode = SIGNALPROCESSINGMODE_RAW;                                              \
    }                                                                                 \
    else if (IsEqualGUID(guid, AUDIO_SIGNALPROCESSINGMODE_COMMUNICATIONS))            \
    {                                                                                 \
        mode = SIGNALPROCESSINGMODE_COMMUNICATIONS;                                   \
    }                                                                                 \
    else if (IsEqualGUID(guid, AUDIO_SIGNALPROCESSINGMODE_SPEECH))                    \
    {                                                                                 \
        mode = SIGNALPROCESSINGMODE_SPEECH;                                           \
    }                                                                                 \
    else if (IsEqualGUID(guid, AUDIO_SIGNALPROCESSINGMODE_NOTIFICATION))              \
    {                                                                                 \
        mode = SIGNALPROCESSINGMODE_NOTIFICATION;                                     \
    }                                                                                 \
    else if (IsEqualGUID(guid, AUDIO_SIGNALPROCESSINGMODE_MEDIA))                     \
    {                                                                                 \
        mode = SIGNALPROCESSINGMODE_MEDIA;                                            \
    }                                                                                 \
    else if (IsEqualGUID(guid, AUDIO_SIGNALPROCESSINGMODE_MOVIE))                     \
    {                                                                                 \
        mode = SIGNALPROCESSINGMODE_MOVIE;                                            \
    }                                                                                 \
    else                                                                              \
    {                                                                                 \
        ASSERT(FALSE && "Unknown Signal Processing Mode");                            \
        mode = SIGNALPROCESSINGMODE_NONE;                                             \
    }

#define VERIFY_MODE_RESOURCES_AVAILABLE(modes, guid, status)                          \
    {                                                                                 \
        SIGNALPROCESSINGMODE mode = SIGNALPROCESSINGMODE_NONE;                        \
        MAP_GUID_TO_MODE(guid, mode);                                                 \
        if (SIGNALPROCESSINGMODE_NONE != mode)                                        \
        {                                                                             \
            status = (modes & mode) ? STATUS_INSUFFICIENT_RESOURCES : STATUS_SUCCESS; \
        }                                                                             \
        else                                                                          \
        {                                                                             \
            status = STATUS_INVALID_PARAMETER;                                        \
        }                                                                             \
    }

#define ALLOCATE_MODE_RESOURCES(modes, guid)                                          \
    {                                                                                 \
        SIGNALPROCESSINGMODE mode = SIGNALPROCESSINGMODE_NONE;                        \
        MAP_GUID_TO_MODE(guid, mode);                                                 \
        modes |= mode;                                                                \
    }

#define FREE_MODE_RESOURCES(modes, guid)                                              \
    {                                                                                 \
        SIGNALPROCESSINGMODE mode = SIGNALPROCESSINGMODE_NONE;                        \
        MAP_GUID_TO_MODE(guid, mode);                                                 \
        modes &= (~mode);                                                             \
    }

//=============================================================================
// Typedefs
//=============================================================================

// Flags to identify stream processing mode
typedef enum {
    CONNECTIONTYPE_TOPOLOGY_OUTPUT = 0,
    CONNECTIONTYPE_WAVE_OUTPUT     = 1
} CONNECTIONTYPE;

// Connection table for registering topology/wave bridge connection
typedef struct _PHYSICALCONNECTIONTABLE
{
    ULONG            ulTopology;
    ULONG            ulWave;
    CONNECTIONTYPE   eType;
} PHYSICALCONNECTIONTABLE, *PPHYSICALCONNECTIONTABLE;

//
// This is the structure of the portclass FDO device extension Nt has created
// for us.  We keep the adapter common object here.
//
struct IAdapterCommon;
typedef struct _PortClassDeviceContext              // 32       64      Byte offsets for 32 and 64 bit architectures
{
    ULONG_PTR m_pulReserved1[2];                    // 0-7      0-15    First two pointers are reserved.
    PDEVICE_OBJECT m_DoNotUsePhysicalDeviceObject;  // 8-11     16-23   Reserved pointer to our Physical Device Object (PDO).
    PVOID m_pvReserved2;                            // 12-15    24-31   Reserved pointer to our Start Device function.
    PVOID m_pvReserved3;                            // 16-19    32-39   "Out Memory" according to DDK.  
    IAdapterCommon* m_pCommon;                      // 20-23    40-47   Pointer to our adapter common object.
    PVOID m_pvUnused1;                              // 24-27    48-55   Unused space.
    PVOID m_pvUnused2;                              // 28-31    56-63   Unused space.

    // Anything after above line should not be used.
    // This actually goes on for (64*sizeof(ULONG_PTR)) but it is all opaque.
} PortClassDeviceContext;

//=============================================================================
// Function prototypes
//=============================================================================

// Generic topology handler
NTSTATUS PropertyHandler_Topology
( 
    _In_  PPCPROPERTY_REQUEST PropertyRequest 
);

// Default WaveFilter automation table.
// Handles the GeneralComponentId request.
NTSTATUS PropertyHandler_WaveFilter
(
    _In_ PPCPROPERTY_REQUEST PropertyRequest
);

NTSTATUS PropertyHandler_OffloadPin
(
    _In_ PPCPROPERTY_REQUEST PropertyRequest
);

// common.h uses some of the above definitions.
#include "common.h"
#include "kshelper.h"


