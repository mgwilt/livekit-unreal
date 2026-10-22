#include "../LiveKitWindowsBridgeInternal.h"
#if WITH_LIVEKIT_WINDOWS || WITH_LIVEKIT_LINUX
#include "LiveKitVideo.h"
#endif
using namespace LiveKitWindowsBridgeInternal;
bool FLiveKitWindowsBridge::ConfigureVideoReceiver(const FString& Participant, const FString& Track, FVideoHandler Handler)
{
#if WITH_LIVEKIT_WINDOWS || WITH_LIVEKIT_LINUX
    VideoParticipant = Participant; VideoTrackName = Track; VideoHandler = MoveTemp(Handler); return true;
#else
    return false;
#endif
}
bool FLiveKitWindowsBridge::StartVideoPublisher(int32 W, int32 H, int32 Fps, int32 Bitrate)
{
#if WITH_LIVEKIT_WINDOWS || WITH_LIVEKIT_LINUX
    return DispatchControl([W, H, Fps, Bitrate](FLiveKitWindowsBridge& Self) {
        LKUB_Room* Room = nullptr; uint64 Generation = 0; LKUB_Result Result{};
        if (Self.Implementation->GetCurrentRoom(Room, Generation) &&
            lkub_video_start(Room, W, H, Fps, Bitrate, &Result) != LKUB_STATUS_OK)
            Self.ErrorHandler(AdapterError(TEXT("video_publish_failed"), Result));
    });
#else
    return false;
#endif
}
bool FLiveKitWindowsBridge::PublishVideoFrame(const FLiveKitVideoFrame& Frame)
{
#if WITH_LIVEKIT_WINDOWS || WITH_LIVEKIT_LINUX
    if (VideoPending.Exchange(true)) return false;
    if (DispatchControl([Frame](FLiveKitWindowsBridge& Self) {
        LKUB_Room* Room = nullptr; uint64 Generation = 0; LKUB_Result Result{};
        if (Self.Implementation->GetCurrentRoom(Room, Generation) &&
            lkub_video_push(Room, Frame.Pixels.GetData(), Frame.Pixels.Num(), Frame.TimestampMicroseconds, &Result) != LKUB_STATUS_OK)
            Self.ErrorHandler(AdapterError(TEXT("video_frame_failed"), Result));
        LKUB_VideoStats Stats{};
        if (Room && lkub_video_stats(Room, &Stats, &Result) == 1)
            UE_LOG(LogTemp, Display, TEXT("LiveKit encoder: name=%s fps=%.1f frames=%u bytes=%llu encode_ms=%.2f limit=%d"),
                UTF8_TO_TCHAR(Stats.encoder), Stats.fps, Stats.frames_encoded, Stats.bytes_sent,
                Stats.frames_encoded ? Stats.encode_seconds * 1000 / Stats.frames_encoded : 0, Stats.quality_reason);
        Self.VideoPending.Store(false);
    })) return true;
    VideoPending.Store(false);
#endif
    return false;
}
bool FLiveKitWindowsBridge::PublishOutputAudio(const FLiveKitAudioFrame& Frame)
{
#if WITH_LIVEKIT_WINDOWS || WITH_LIVEKIT_LINUX
    if (Frame.Samples.IsEmpty() || Frame.SampleRate < 8000 || Frame.SampleRate > 48000 ||
        Frame.Channels < 1 || Frame.Channels > 2 || Frame.Samples.Num() % Frame.Channels != 0) return false;
    if (AudioPending.fetch_add(1) >= 20) { AudioPending.fetch_sub(1); return false; }
    if (DispatchControl([Frame](FLiveKitWindowsBridge& Self) {
        LKUB_Room* Room = nullptr; uint64 Generation = 0; LKUB_Result Result{};
        if (Self.Implementation->GetCurrentRoom(Room, Generation) &&
            lkub_output_audio_push(Room, Frame.Samples.GetData(), Frame.Samples.Num(), Frame.SampleRate, Frame.Channels, &Result) != LKUB_STATUS_OK)
            Self.ErrorHandler(AdapterError(TEXT("output_audio_failed"), Result));
        Self.AudioPending.fetch_sub(1);
    })) return true;
    AudioPending.fetch_sub(1);
#endif
    return false;
}
void FLiveKitWindowsBridge::TickVideo()
{
#if WITH_LIVEKIT_WINDOWS || WITH_LIVEKIT_LINUX
    if (!Implementation || !VideoHandler) return;
    std::unique_lock Lock(Implementation->ControlMutex, std::try_to_lock);
    LKUB_Room* Room = nullptr; uint64 Generation = 0;
    if (!Lock.owns_lock() || !Implementation->GetCurrentRoom(Room, Generation)) return;
    LKUB_VideoFrame Incoming{}; LKUB_Result Result{};
    const auto Status = lkub_video_poll(Room, &Incoming, &Result);
    if (Status < 0) ErrorHandler(AdapterError(TEXT("video_receive_failed"), Result));
    if (Status <= 0) return;
    FLiveKitVideoFrame Frame;
    Frame.Width = Incoming.width; Frame.Height = Incoming.height; Frame.TimestampMicroseconds = Incoming.timestamp_us;
    if (Incoming.bgra.size <= 3840ull * 3840 * 4) Frame.Pixels.Append(Incoming.bgra.data, static_cast<int32>(Incoming.bgra.size));
    lkub_buffer_release(&Incoming.bgra);
    Lock.unlock();
    if (Frame.IsValid()) VideoHandler(Frame);
#endif
}
