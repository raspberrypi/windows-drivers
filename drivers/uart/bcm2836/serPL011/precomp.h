//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//
// Module Name: 
//
//    PL011driver.cpp
//
// Abstract:
//
//    This is the standard header for all of the source modules within the 
//    ARM PL011 UART device driver.
//
// Environment:
//
//    kernel-mode only
//

// Various compilation options
#pragma warning(disable:4127)

#include <initguid.h>
#include <wdmguid.h>
#include <ntddk.h>
#include <wdf.h>
#include <ntddser.h>
#include <ntintsafe.h>
#include <ntstrsafe.h>
#include <devpkey.h>
#include <acpiioct.h>
#include <acpiutil.hpp>

// Class Extension includes
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>
#include <SerCx.h>

// For initial debugging with a serial debugger
#if DBG
    #define IS_DONT_CHANGE_HW   0
#else   // DBG
    #define IS_DONT_CHANGE_HW   0
#endif  // !DBG
