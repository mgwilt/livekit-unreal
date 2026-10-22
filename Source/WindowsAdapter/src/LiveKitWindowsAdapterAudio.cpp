#include "LiveKitWindowsAdapterInternal.h"
#include <cstring>

using namespace lkub::internal;

void LKUB_Room::CloseAudioReceiver()
{
    std::lock_guard Lock(ReceiverLifecycleMutex);
    ReceiverClosed = true;
    StopAudioReceiverLocked();
}
void LKUB_Room::StopAudioReceiver()
{
    std::lock_guard Lock(ReceiverLifecycleMutex);
    StopAudioReceiverLocked();
}
void LKUB_Room::StopAudioReceiverLocked()
{
    std::shared_ptr<livekit::AudioStream> Stream;
    {
        std::lock_guard Lock(ReceiveMutex);
        Stream = std::move(ReceiveStream);
    }
    if (Stream) Stream->close();
    if (ReceiveThread.joinable()) ReceiveThread.join();
    std::lock_guard Lock(ReceiveMutex);
    ReceivedFrames.clear();
    ReceiveFailure.clear();
    AudioTrackSid.clear();
    ReceivedSampleIndex = 0;
    AudioDiscontinuity = true;
}

void LKUB_Room::onTrackSubscribed(livekit::Room&, const livekit::TrackSubscribedEvent& Event)
{
    SubscribeVideo(Event);
    if (!ReceiveAudio || !Event.participant || !Event.track ||
        Event.track->kind() != livekit::TrackKind::KIND_AUDIO ||
        Event.participant->identity() != AudioParticipant ||
        (!AudioTrackName.empty() && Event.track->name() != AudioTrackName)) return;
    // Serialize replacement with disconnect and unsubscribe callbacks.
    std::lock_guard LifecycleLock(ReceiverLifecycleMutex);
    if (ReceiverClosed) return;
    StopAudioReceiverLocked();
    livekit::AudioStream::Options Options;
    Options.capacity = 20;
    auto Stream = livekit::AudioStream::fromTrack(Event.track, Options);
    {
        std::lock_guard Lock(ReceiveMutex);
        ReceiveStream = Stream;
        AudioTrackSid = Event.track->sid();
    }
    ReceiveThread = std::thread([this, Stream]()
    {
        try
        {
            livekit::AudioFrameEvent Event;
            while (Stream->read(Event))
            {
                auto& Frame = Event.frame;
                if (Frame.sampleRate() <= 0 || Frame.numChannels() < 1 ||
                    Frame.numChannels() > 2 || Frame.duration() > 0.1) continue;
                std::lock_guard Lock(ReceiveMutex);
                // At most 200 ms of decoded audio. A stall drops stale speech.
                double Duration = Frame.duration();
                for (const auto& Queued : ReceivedFrames) Duration += Queued.Frame.duration();
                if (Duration > 0.2 || ReceivedFrames.size() >= 20)
                {
                    ReceivedFrames.clear();
                    AudioDiscontinuity = true;
                }
                ReceivedFrames.push_back({std::move(Frame), ReceivedSampleIndex, AudioDiscontinuity});
                ReceivedSampleIndex += ReceivedFrames.back().Frame.samplesPerChannel();
                AudioDiscontinuity = false;
            }
        }
        catch (...)
        {
            std::lock_guard Lock(ReceiveMutex);
            AudioDiscontinuity = true;
            ReceiveFailure = "The remote audio decoder stopped with an error.";
        }
    });
}

void LKUB_Room::onTrackUnsubscribed(livekit::Room&, const livekit::TrackUnsubscribedEvent& Event)
{
    {
        std::lock_guard Lock(VideoLifecycleMutex);
        if (Event.track && Event.track->sid() == VideoTrackSid) StopVideoLocked();
    }
    std::lock_guard LifecycleLock(ReceiverLifecycleMutex);
    bool Matches = false;
    {
        std::lock_guard Lock(ReceiveMutex);
        Matches = Event.track && Event.track->sid() == AudioTrackSid;
    }
    if (Matches) StopAudioReceiverLocked();
}

void LKUB_Room::onTrackMuted(livekit::Room&, const livekit::TrackMutedEvent& Event)
{
    std::lock_guard Lock(ReceiveMutex);
    if (Event.publication && Event.publication->sid() == AudioTrackSid)
    {
        ReceivedFrames.clear();
        AudioDiscontinuity = true;
    }
}

extern "C" int32_t LKUB_CALL lkub_room_configure_audio_receiver(
    LKUB_Room* Room, LKUB_StringView Participant, LKUB_StringView TrackName, LKUB_Result* Result)
{
    return Guard(Result, [&]()
    {
        Require(Room != nullptr, "The room is required.");
        Require(!Room->PlatformAudio, "Configure the receiver before initializing room audio.");
        Room->AudioParticipant = CopyString(Participant);
        Room->AudioTrackName = CopyString(TrackName);
        Require(!Room->AudioParticipant.empty(), "A participant identity is required.");
        Room->ReceiveAudio = true;
    });
}

extern "C" int32_t LKUB_CALL lkub_room_poll_audio(
    LKUB_Room* Room, LKUB_AudioFrame* Frame, LKUB_Result* Result)
{
    bool Available = false;
    const auto Status = Guard(Result, [&]()
    {
        Require(Room && Frame, "Room and frame outputs are required.");
        *Frame = {};
        std::lock_guard Lock(Room->ReceiveMutex);
        Require(Room->ReceiveFailure.empty(), "The remote audio decoder stopped with an error.");
        if (Room->ReceivedFrames.empty()) return;
        const auto& Item = Room->ReceivedFrames.front();
        const auto Bytes = Item.Frame.data().size() * sizeof(int16_t);
        Frame->pcm.data = new uint8_t[Bytes];
        Frame->pcm.size = Bytes;
        std::memcpy(Frame->pcm.data, Item.Frame.data().data(), Bytes);
        Frame->sample_rate = Item.Frame.sampleRate();
        Frame->channels = Item.Frame.numChannels();
        Frame->sample_index = Item.SampleIndex;
        Frame->discontinuity = Item.Discontinuity;
        Room->ReceivedFrames.pop_front();
        Available = true;
    });
    return Status == LKUB_STATUS_OK ? (Available ? 1 : 0) : -1;
}
