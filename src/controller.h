#ifndef TT_BITRATE_CONTROLLER_H
#define TT_BITRATE_CONTROLLER_H

#include <windows.h>
#include "target.h"

enum { TT_MIN_KBPS = 6, TT_MAX_KBPS = 192 };

typedef struct {
    int bitrate;
    int channel;
    int codec;
} AudioConfig;

typedef enum {
    TT_OK = 0,
    TT_OUT_OF_RANGE,
    TT_NO_TARGET,
    TT_MULTIPLE_TARGETS,
    TT_NO_ENGINE,
    TT_ACCESS_DENIED,
    TT_ENGINE_UNSUPPORTED,
    TT_ENGINE_NOT_READY,
    TT_SDK_ERROR,
    TT_REMOTE_FAILED,
    TT_REMOTE_TIMEOUT,
    TT_READBACK_MISMATCH,
    TT_SYSTEM_ERROR,
    TT_CHANNEL_NOT_EFFECTIVE,
    TT_RESTORE_FAILED
} TTCode;

typedef struct {
    TTCode code;
    DWORD system_error;
    DWORD pid;
    AudioConfig config;
    int capture_channels;
    int encoder_channels;
} TTResult;

TTResult tt_read_config(void);
TTResult tt_apply_bitrate(int bitrate);
TTResult tt_read_config_for(TTEdition edition);
TTResult tt_apply_bitrate_for(TTEdition edition, int bitrate);
/* Verified game 52035 / entertainment 52036 SDKs. Preserves bitrate/codec. */
TTResult tt_apply_channels_for(TTEdition edition, int channels);
const char *tt_code_name(TTCode code);

#endif
