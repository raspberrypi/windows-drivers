#pragma once

/*
ETW (TraceLogging) support:
- ProviderId is EtwNameHash("SDHostBRCME88C") = f2782ab9-d1a5-5f95-240a-ac933a6937e2
- Start trace: tracelog -start MyLoggerName -f MyLoggerFile.etl -guid *SDHostBRCME88C -level 4
- Stop trace:  tracelog -stop MyLoggerName
- View with your favorite ETW tool - tracefmt, traceview, tracerpt, etc.

If you add -kd to the tracelog command line and run KD command "!wmitrace.dynamicprint 1",
the events will be sent directly to KD.

For boot tracing, use an autologger. For development only, the AutoLogger.reg file will enable
an autologger that saves to c:\windows\system32\logfiles\wmi\SDHostBRCME88C.etl,
collects at level Info (4), and sends events to KD in real-time.
*/

// Declare a global variable named g_trace that can be used with TraceLoggingWrite.
TRACELOGGING_DECLARE_PROVIDER(g_trace);

// All ETW events should have at least one keyword set.
// Define a keyword that means "Log":
#define DRIVER_KEYWORD_LOG 0x1

// Macro to slightly simplify the use of TraceLoggingWrite.
#define DRIVER_TRACE(eventName, level, keyword, ...) TraceLoggingWrite( \
    g_trace, \
    eventName, \
    TraceLoggingLevel(level), \
    TraceLoggingKeyword(keyword), \
    __VA_ARGS__)

// Macros to simplify basic logging.
#define LogCritical(eventName, ...) /* Critical = level 1 */ \
    DRIVER_TRACE(eventName, TRACE_LEVEL_CRITICAL, DRIVER_KEYWORD_LOG, __VA_ARGS__)
#define LogError(eventName, ...)    /* Error = level 2 */ \
    DRIVER_TRACE(eventName, TRACE_LEVEL_ERROR, DRIVER_KEYWORD_LOG, __VA_ARGS__)
#define LogWarning(eventName, ...)  /* Warning = level 3 */ \
    DRIVER_TRACE(eventName, TRACE_LEVEL_WARNING, DRIVER_KEYWORD_LOG, __VA_ARGS__)
#define LogInfo(eventName, ...)     /* Info = level 4 */  \
    DRIVER_TRACE(eventName, TRACE_LEVEL_INFORMATION, DRIVER_KEYWORD_LOG, __VA_ARGS__)
#define LogVerbose(eventName, ...)  /* Verbose = level 5 */ \
    DRIVER_TRACE(eventName, TRACE_LEVEL_VERBOSE, DRIVER_KEYWORD_LOG, __VA_ARGS__)

// Some events are useful during driver initialization and become overly verbose
// afterwards. Such events can be logged as: LogVerboseInfo(...), which only
// logs the first 100 VerboseInfo events.
#define LogVerboseInfo(eventName, ...) \
    if (!DriverShouldLogVerboseInfo()) \
    {} \
    else \
    LogInfo(eventName, __VA_ARGS__)
