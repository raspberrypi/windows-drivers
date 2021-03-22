#ifndef BCMGENET_TRACE_H
#define BCMGENET_TRACE_H

#define BG_TRACE_INFO       1
                            
TRACELOGGING_DECLARE_PROVIDER(GenetTraceProvider);

#define TraceLoggingFunctionName() TraceLoggingWideString(__FUNCTIONW__, "Function")

#define TraceInfo(event, ...) \
    TraceLoggingWrite( \
        GenetTraceProvider, \
        event, \
        TraceLoggingKeyword(BG_TRACE_INFO), \
        TraceLoggingLevel(TRACE_LEVEL_INFORMATION), \
        TraceLoggingFunctionName(), \
        __VA_ARGS__)

#define TraceError(event, ...) \
    TraceLoggingWrite( \
        GenetTraceProvider, \
        event, \
        TraceLoggingKeyword(BG_TRACE_INFO), \
        TraceLoggingLevel(TRACE_LEVEL_ERROR), \
        TraceLoggingFunctionName(), \
        __VA_ARGS__)

#define TraceB TraceLoggingBinary
#define TraceUCX TraceLoggingHexUInt8
#define TraceUSX TraceLoggingHexUInt16
#define TraceULX TraceLoggingHexUInt32
#define TraceUQX TraceLoggingHexUInt64

#endif /* BCMGENET_TRACE_H */
