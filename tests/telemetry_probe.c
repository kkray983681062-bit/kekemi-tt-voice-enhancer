#include <stdio.h>
#include "../src/telemetry.h"

int main(int argc, char **argv) {
    (void)argv;
    TTMetric value = tt_read_telemetry_for(argc > 1 ? TT_EDITION_ENTERTAINMENT : TT_EDITION_GAME);
    printf("metric_code=%d audio_kbps=%.2f\n", (int)value.code, value.kbps);
    return value.code == TT_METRIC_ACTIVE ? 0 : 2;
}
