#pragma once
#include "LiveKitWindowsAdapter.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct LKUB_VideoFrame {
    LKUB_OwnedBuffer bgra;
    int32_t width;
    int32_t height;
    int64_t timestamp_us;
} LKUB_VideoFrame;
LKUB_API int32_t LKUB_CALL lkub_video_configure(LKUB_Room*, LKUB_StringView participant, LKUB_StringView track, LKUB_Result*);
LKUB_API int32_t LKUB_CALL lkub_video_poll(LKUB_Room*, LKUB_VideoFrame*, LKUB_Result*);
LKUB_API int32_t LKUB_CALL lkub_video_start(LKUB_Room*, int32_t width, int32_t height, int32_t fps, int32_t bitrate, LKUB_Result*);
LKUB_API int32_t LKUB_CALL lkub_video_push(LKUB_Room*, const uint8_t* bgra, uint64_t size, int64_t timestamp_us, LKUB_Result*);
LKUB_API int32_t LKUB_CALL lkub_output_audio_push(LKUB_Room*, const int16_t*, int32_t count, int32_t rate, int32_t channels, LKUB_Result*);
typedef struct LKUB_VideoStats {
    uint32_t frames_encoded;
    uint32_t frames_sent;
    uint64_t bytes_sent;
    double encode_seconds;
    double fps;
    int32_t quality_reason;
    char encoder[128];
} LKUB_VideoStats;
/* Nonblocking. Returns 1 when a new snapshot is available. */
LKUB_API int32_t LKUB_CALL lkub_video_stats(LKUB_Room*, LKUB_VideoStats*, LKUB_Result*);
#ifdef __cplusplus
}
#endif
