#ifndef TT_TELEMETRY_H
#define TT_TELEMETRY_H
#include "target.h"

typedef enum {
    TT_METRIC_ACTIVE,
    TT_METRIC_WAITING,
    TT_METRIC_NO_TT,
    TT_METRIC_MULTIPLE_TT,
    TT_METRIC_ACCESS,
    TT_METRIC_UNSUPPORTED
} TTMetricCode;

typedef struct {
    TTMetricCode code;
    double kbps;
} TTMetric;

/* Reads the SDK's live audio-quality field; never writes to the TT process. */
TTMetric tt_read_telemetry(void);
TTMetric tt_read_telemetry_for(TTEdition edition);

#endif
