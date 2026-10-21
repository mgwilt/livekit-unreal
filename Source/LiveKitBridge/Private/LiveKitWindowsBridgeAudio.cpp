#include "LiveKitWindowsBridgeInternal.h"
using namespace LiveKitWindowsBridgeInternal;

bool FLiveKitWindowsBridge::ConfigureAudioReceiver(
    const FString& Participant, const FString& TrackName, FAudioHandler Handler)
{
#if WITH_LIVEKIT_LINUX
    AudioParticipant = Participant;
    AudioTrackName = TrackName;
    AudioHandler = MoveTemp(Handler);
    return true;
#else
    return false;
#endif
}

bool FLiveKitWindowsBridge::TickAudio(float)
{
#if WITH_LIVEKIT_LINUX
    if (!AudioHandler || !Implementation) return true;
    std::unique_lock Lock(Implementation->ControlMutex, std::try_to_lock);
    if (!Lock.owns_lock()) return true;
    LKUB_Room* Room = nullptr;
    uint64 Generation = 0;
    if (!Implementation->GetCurrentRoom(Room, Generation)) return true;
    for (int32 Index = 0; Index < 20; ++Index)
    {
        LKUB_AudioFrame Frame{};
        LKUB_Result Result{};
        const int32 Status = lkub_room_poll_audio(Room, &Frame, &Result);
        if (Status < 0) ErrorHandler(AdapterError(TEXT("audio_receive_failed"), Result));
        if (Status <= 0) break;
        FLiveKitAudioFrame Copy;
        const uint64 Count = Frame.pcm.size / sizeof(int16);
        if (Count <= 9600 && Frame.sample_rate >= 8000 && Frame.sample_rate <= 48000 &&
            Frame.channels > 0 && Frame.channels <= 2 && Count % Frame.channels == 0)
        {
            Copy.Samples.Append(reinterpret_cast<const int16*>(Frame.pcm.data), static_cast<int32>(Count));
            Copy.SampleRate = Frame.sample_rate;
            Copy.Channels = Frame.channels;
            Copy.SampleIndex = Frame.sample_index;
            Copy.bDiscontinuity = Frame.discontinuity != 0;
        }
        lkub_buffer_release(&Frame.pcm);
        if (!Copy.Samples.IsEmpty()) AudioHandler(Copy);
    }
#endif
    return true;
}
