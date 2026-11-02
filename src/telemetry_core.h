#ifndef TT_TELEMETRY_CORE_H
#define TT_TELEMETRY_CORE_H

#include <stddef.h>
#include <stdint.h>

typedef int (*TTMemoryReader)(void *context, uintptr_t address, void *out, size_t length);
typedef enum { TT_SAMPLE_WAITING, TT_SAMPLE_ACTIVE } TTSampleState;

typedef struct {
    uintptr_t engine_singleton;
    uintptr_t native_vtable;
    uintptr_t publisher_vtable;
    uintptr_t stream_vtable;
} TTLayout;

/* Only call this layout for the exact verified ZEGO build. */
TTSampleState tt_sample_core(TTMemoryReader read_memory, void *context,
                            uintptr_t engine_base, double *kbps);
TTSampleState tt_sample_with_layout(TTMemoryReader read_memory, void *context,
                                    uintptr_t engine_base, const TTLayout *layout, double *kbps);

#endif
