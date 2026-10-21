#include "LiveKitWindowsAdapterInternal.h"

#include <livekit/audio_source.h>
#include <exception>
#include <vector>

using namespace lkub::internal;

extern "C" int32_t LKUB_CALL lkub_room_start_raw_microphone(
    LKUB_Room* Room,
    int32_t SampleRate,
    int32_t Channels,
    LKUB_Result* OutResult)
{
    return Guard(OutResult, [&]()
    {
        Require(Room != nullptr && Room->SdkRoom, "The room is unavailable.");
        Require(Room->ReceiveAudio, "Configure raw audio receive before microphone capture.");
        Require(SampleRate == 48000 && Channels == 1, "Microphone PCM must use 48 kHz mono audio.");
        std::lock_guard Lock(Room->AudioMutex);
        Require(!Room->PlatformAudio && !Room->MicrophoneSource,
            "Platform audio cannot share the raw microphone path.");
        if (Room->RawMicrophoneSource && Room->MicrophoneTrack) return;
        Require(!Room->MicrophoneTrack && !Room->RawMicrophoneSource,
            "The microphone source state is invalid.");
        const auto Participant = Room->SdkRoom->localParticipant().lock();
        Require(Participant != nullptr, "The local LiveKit participant is unavailable.");
        // This native source creates no platform microphone or speaker device.
        auto Source = std::make_shared<livekit::AudioSource>(SampleRate, Channels, 0);
        auto Track = livekit::LocalAudioTrack::createLocalAudioTrack("avatar-microphone", Source);
        livekit::TrackPublishOptions Options;
        Options.source = livekit::TrackSource::SOURCE_MICROPHONE;
        try
        {
            Participant->publishTrack(Track, Options);
        }
        catch (...)
        {
            // A failed publish can still have allocated a server publication.
            if (const auto Publication = Track->publication())
            {
                try { Participant->unpublishTrack(Publication->sid()); }
                catch (...) {}
            }
            throw;
        }
        Room->RawMicrophoneSource = std::move(Source);
        Room->MicrophoneTrack = std::move(Track);
    });
}

extern "C" int32_t LKUB_CALL lkub_room_push_microphone_pcm(
    LKUB_Room* Room,
    const int16_t* Samples,
    int32_t SamplesPerChannel,
    LKUB_Result* OutResult)
{
    return Guard(OutResult, [&]()
    {
        Require(Room != nullptr && Room->SdkRoom, "The room is unavailable.");
        Require(Samples != nullptr, "Microphone PCM samples are required.");
        Require(SamplesPerChannel == 480, "Push exactly 10 ms of microphone PCM.");
        // AudioSource::captureFrame is not thread-safe. Keep stop and reset outside
        // an active capture call. The caller supplies a separate capture worker.
        std::lock_guard Lock(Room->AudioMutex);
        Require(Room->RawMicrophoneSource != nullptr && Room->MicrophoneTrack != nullptr,
            "The raw microphone is not recording.");
        std::vector<int16_t> Data(Samples, Samples + SamplesPerChannel);
        const livekit::AudioFrame Frame(std::move(Data), 48000, 1, SamplesPerChannel);
        Room->RawMicrophoneSource->captureFrame(Frame, 20);
    });
}

extern "C" int32_t LKUB_CALL lkub_room_stop_raw_microphone(
    LKUB_Room* Room,
    LKUB_Result* OutResult)
{
    return Guard(OutResult, [&]()
    {
        Require(Room != nullptr && Room->SdkRoom, "The room is unavailable.");
        std::lock_guard Lock(Room->AudioMutex);
        if (!Room->RawMicrophoneSource) return;
        // Release room ownership before SDK calls, including their failure paths.
        auto Source = std::move(Room->RawMicrophoneSource);
        auto Track = std::move(Room->MicrophoneTrack);
        std::exception_ptr Failure;
        if (Track)
        {
            try { Track->mute(); }
            catch (...) { Failure = std::current_exception(); }
            try
            {
                if (const auto Participant = Room->SdkRoom->localParticipant().lock())
                {
                    // SDK 1.3.0 retains the pre-publication SID on LocalAudioTrack.
                    // The publication owns the server SID required by unpublish.
                    if (const auto Publication = Track->publication())
                        Participant->unpublishTrack(Publication->sid());
                }
            }
            catch (...) { if (!Failure) Failure = std::current_exception(); }
        }
        Track.reset();
        Source.reset();
        if (Failure) std::rethrow_exception(Failure);
    });
}
