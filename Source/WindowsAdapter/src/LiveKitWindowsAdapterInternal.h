#pragma once

#include "LiveKitWindowsAdapter.h"

#include <livekit/build.h>
#include <livekit/livekit.h>

#include <atomic>
#include <deque>
#include <thread>
#include <livekit/audio_stream.h>
#include <livekit/video_stream.h>
#include <livekit/local_video_track.h>
#include <optional>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#if defined(__linux__)
constexpr std::string_view ExpectedLiveKitVersion = "1.12.3";
#else
constexpr std::string_view ExpectedLiveKitVersion = "1.3.0";
#endif
static_assert(
    std::string_view(LIVEKIT_BUILD_VERSION) == ExpectedLiveKitVersion,
    "The adapter must be reviewed for every LiveKit C++ SDK version.");

namespace lkub::internal
{
void ResetResult(LKUB_Result* Result);

int32_t SetResult(
    LKUB_Result* Result,
    LKUB_Status Status,
    std::string_view Message = {},
    std::string_view Data = {},
    uint32_t RpcErrorCode = 0);

std::string CopyString(LKUB_StringView View);
LKUB_StringView BorrowString(const std::string& Value);
void Require(bool Condition, const char* Message);
livekit::LogLevel ToLogLevel(int32_t Level);

template <typename WorkType>
int32_t Guard(LKUB_Result* Result, WorkType&& Work)
{
    ResetResult(Result);
    try
    {
        Work();
        return LKUB_STATUS_OK;
    }
    catch (const livekit::RpcError& Error)
    {
        return SetResult(
            Result,
            LKUB_STATUS_RPC_ERROR,
            Error.message(),
            Error.data(),
            Error.code());
    }
    catch (const std::bad_alloc&)
    {
        return SetResult(Result, LKUB_STATUS_OUT_OF_MEMORY, "Out of memory.");
    }
    catch (const std::invalid_argument& Error)
    {
        return SetResult(Result, LKUB_STATUS_INVALID_ARGUMENT, Error.what());
    }
    catch (const std::exception& Error)
    {
        return SetResult(Result, LKUB_STATUS_SDK_ERROR, Error.what());
    }
    catch (...)
    {
        return SetResult(
            Result,
            LKUB_STATUS_INTERNAL_ERROR,
            "The LiveKit adapter raised an unknown exception.");
    }
}
}

struct LKUB_ByteStreamReader
{
    std::shared_ptr<livekit::ByteStreamReader> Reader;
    std::string SenderIdentity;
    std::atomic_bool Cancelled{false};
};

struct LKUB_Room final : livekit::RoomDelegate
{
    std::shared_ptr<livekit::Room> SdkRoom;

    std::mutex CallbackMutex;
    LKUB_RoomCallbacks Callbacks{};
    void* CallbackContext = nullptr;

    std::mutex AudioMutex;
    std::shared_ptr<livekit::PlatformAudio> PlatformAudio;
    std::shared_ptr<livekit::PlatformAudioSource> MicrophoneSource;
    std::shared_ptr<livekit::AudioSource> RawMicrophoneSource;
    std::shared_ptr<livekit::LocalAudioTrack> MicrophoneTrack;

    struct ReceivedAudio
    {
        livekit::AudioFrame Frame;
        uint64_t SampleIndex = 0;
        bool Discontinuity = false;
    };
    std::mutex ReceiverLifecycleMutex;
    bool ReceiverClosed = false;
    std::string ReceiveFailure;
    std::mutex ReceiveMutex;
    std::string AudioParticipant;
    std::string AudioTrackName;
    std::string AudioTrackSid;
    bool ReceiveAudio = false;
    std::shared_ptr<livekit::AudioStream> ReceiveStream;
    std::thread ReceiveThread;
    std::deque<ReceivedAudio> ReceivedFrames;
    uint64_t ReceivedSampleIndex = 0;
    bool AudioDiscontinuity = true;
    void StopAudioReceiver();
    void StopAudioReceiverLocked();
    void CloseAudioReceiver();
    void onTrackSubscribed(livekit::Room&, const livekit::TrackSubscribedEvent&) override;
    void onTrackUnsubscribed(livekit::Room&, const livekit::TrackUnsubscribedEvent&) override;
    void onTrackMuted(livekit::Room&, const livekit::TrackMutedEvent&) override;

    std::mutex VideoLifecycleMutex;
    std::mutex VideoMutex;
    bool VideoClosed = false;
    std::string VideoParticipant, VideoTrackName, VideoTrackSid;
    std::string VideoFailure;
    std::shared_ptr<livekit::VideoStream> VideoStream;
    std::thread VideoThread;
    std::optional<livekit::VideoFrameEvent> LatestVideo;
    std::mutex OutputMutex;
    std::shared_ptr<livekit::VideoSource> VideoSource;
    std::shared_ptr<livekit::LocalVideoTrack> VideoTrack;
    std::shared_ptr<livekit::AudioSource> OutputAudio;
    std::shared_ptr<livekit::LocalAudioTrack> OutputTrack;
    int OutputWidth = 0, OutputHeight = 0, OutputRate = 0, OutputChannels = 0;
    std::optional<std::future<std::vector<livekit::RtcStats>>> VideoStatsRequest;
    std::chrono::steady_clock::time_point NextVideoStats{};
    void SubscribeVideo(const livekit::TrackSubscribedEvent&);
    void StopVideo();
    void StopVideoLocked();
    void ClearVideo();

    std::mutex SpeakerMutex;
    std::unordered_set<std::string> ActiveSpeakers;

    std::pair<LKUB_RoomCallbacks, void*> SnapshotCallbacks();
    bool IsSpeaking(const std::string& Identity);
    LKUB_ParticipantInfo ParticipantInfo(
        const livekit::Participant& Participant,
        bool Speaking);
    void ClearAudio();
    void ClearSpeakers();
    void EmitConnectionState(int32_t State);
    void EmitTerminal(int32_t Reason);

    void onParticipantConnected(
        livekit::Room& Room,
        const livekit::ParticipantConnectedEvent& Event) override;
    void onParticipantDisconnected(
        livekit::Room& Room,
        const livekit::ParticipantDisconnectedEvent& Event) override;
    void onActiveSpeakersChanged(
        livekit::Room& Room,
        const livekit::ActiveSpeakersChangedEvent& Event) override;
    void onConnectionStateChanged(
        livekit::Room& Room,
        const livekit::ConnectionStateChangedEvent& Event) override;
    void onDisconnected(
        livekit::Room& Room,
        const livekit::DisconnectedEvent& Event) override;
    void onRoomEos(
        livekit::Room& Room,
        const livekit::RoomEosEvent& Event) override;
    void onReconnecting(
        livekit::Room& Room,
        const livekit::ReconnectingEvent& Event) override;
    void onReconnected(
        livekit::Room& Room,
        const livekit::ReconnectedEvent& Event) override;
    void onUserPacketReceived(
        livekit::Room& Room,
        const livekit::UserDataPacketEvent& Event) override;
};
