#include "../LiveKitWindowsAdapterInternal.h"
#include "LiveKitVideo.h"
#include <livekit/audio_source.h>
#include <cstring>
using namespace lkub::internal;

void LKUB_Room::StopVideoLocked()
{
    std::shared_ptr<livekit::VideoStream> Stream;
    { std::lock_guard Lock(VideoMutex); Stream = std::move(VideoStream); }
    if (Stream) Stream->close();
    if (VideoThread.joinable()) VideoThread.join();
    std::lock_guard Lock(VideoMutex);
    LatestVideo.reset(); VideoFailure.clear(); VideoTrackSid.clear();
}
void LKUB_Room::StopVideo() { std::lock_guard Lock(VideoLifecycleMutex); StopVideoLocked(); }
void LKUB_Room::ClearVideo()
{
    { std::lock_guard Lock(VideoLifecycleMutex); VideoClosed = true; StopVideoLocked(); }
    std::lock_guard Lock(OutputMutex);
    VideoTrack.reset(); VideoSource.reset(); OutputTrack.reset(); OutputAudio.reset();
}
void LKUB_Room::SubscribeVideo(const livekit::TrackSubscribedEvent& Event)
{
    if (!Event.track || !Event.participant || VideoParticipant.empty() ||
        Event.track->kind() != livekit::TrackKind::KIND_VIDEO ||
        Event.participant->identity() != VideoParticipant ||
        (!VideoTrackName.empty() && Event.track->name() != VideoTrackName)) return;
    std::lock_guard Lock(VideoLifecycleMutex);
    if (VideoClosed) return;
    StopVideoLocked();
    livekit::VideoStream::Options Options; Options.capacity = 1; Options.format = livekit::VideoBufferType::BGRA;
    auto Stream = livekit::VideoStream::fromTrack(Event.track, Options);
    VideoTrackSid = Event.track->sid();
    { std::lock_guard FrameLock(VideoMutex); VideoStream = Stream; }
    VideoThread = std::thread([this, Stream]() {
        try {
            livekit::VideoFrameEvent Frame;
            while (Stream->read(Frame)) {
                if (Frame.frame.width() <= 0 || Frame.frame.width() > 3840 ||
                    Frame.frame.height() <= 0 || Frame.frame.height() > 3840) continue;
                std::lock_guard FrameLock(VideoMutex); LatestVideo = std::move(Frame);
            }
        } catch (const std::exception& Error) {
            std::lock_guard FrameLock(VideoMutex); VideoFailure = Error.what();
        }
    });
}
extern "C" int32_t LKUB_CALL lkub_video_configure(LKUB_Room* Room, LKUB_StringView Participant, LKUB_StringView Track, LKUB_Result* Result)
{
    return Guard(Result, [&] { Require(Room != nullptr, "Room is required.");
        Room->VideoParticipant = CopyString(Participant); Room->VideoTrackName = CopyString(Track); });
}
extern "C" int32_t LKUB_CALL lkub_video_poll(LKUB_Room* Room, LKUB_VideoFrame* Frame, LKUB_Result* Result)
{
    bool Available = false;
    const auto Status = Guard(Result, [&] {
        Require(Room && Frame, "Room and output are required."); *Frame = {};
        std::lock_guard Lock(Room->VideoMutex);
        if (!Room->VideoFailure.empty()) throw std::runtime_error(Room->VideoFailure);
        if (!Room->LatestVideo) return;
        auto& Item = *Room->LatestVideo;
        const auto Size = Item.frame.dataSize();
        Frame->bgra.data = new uint8_t[Size]; Frame->bgra.size = Size;
        std::memcpy(Frame->bgra.data, Item.frame.data(), Size);
        Frame->width = Item.frame.width(); Frame->height = Item.frame.height();
        Frame->timestamp_us = Item.timestamp_us; Room->LatestVideo.reset(); Available = true;
    });
    return Status == LKUB_STATUS_OK ? (Available ? 1 : 0) : -1;
}
extern "C" int32_t LKUB_CALL lkub_video_start(LKUB_Room* Room, int32_t Width, int32_t Height, int32_t Fps, int32_t Bitrate, LKUB_Result* Result)
{
    return Guard(Result, [&] {
        Require(Room && Room->SdkRoom, "Room is required.");
        Require(Width >= 64 && Height >= 64 && Width <= 3840 && Height <= 3840 &&
            Width % 2 == 0 && Height % 2 == 0 && Fps > 0 && Fps <= 60 && Bitrate >= 100000, "Invalid video settings.");
        std::lock_guard Lock(Room->OutputMutex);
        Require(!Room->VideoSource, "Video is already published.");
        const auto Local = Room->SdkRoom->localParticipant().lock(); Require(Local != nullptr, "Local participant is unavailable.");
        auto Source = std::make_shared<livekit::VideoSource>(Width, Height);
        auto Track = livekit::LocalVideoTrack::createLocalVideoTrack("avatar-video", Source);
        livekit::TrackPublishOptions Options; Options.source = livekit::TrackSource::SOURCE_CAMERA;
        Options.simulcast = false;
#if defined(__linux__)
        Options.video_codec = livekit::VideoCodec::H264;
        Options.video_encoder = livekit::VideoEncoderBackend::Nvenc;
#endif
        livekit::VideoEncodingOptions Encoding; Encoding.max_bitrate = Bitrate; Encoding.max_framerate = Fps;
        Options.video_encoding = Encoding;
        Local->publishTrack(Track, Options);
        Room->OutputWidth = Width; Room->OutputHeight = Height;
        Room->VideoSource = std::move(Source); Room->VideoTrack = std::move(Track);
    });
}
extern "C" int32_t LKUB_CALL lkub_video_push(LKUB_Room* Room, const uint8_t* Bytes, uint64_t Size, int64_t Timestamp, LKUB_Result* Result)
{
    return Guard(Result, [&] {
        Require(Room && Bytes, "Room and pixels are required."); std::lock_guard Lock(Room->OutputMutex);
        Require(Room->VideoSource && Size == uint64_t(Room->OutputWidth) * Room->OutputHeight * 4, "Video frame does not match publication.");
        livekit::VideoFrame Frame(Room->OutputWidth, Room->OutputHeight, livekit::VideoBufferType::BGRA, std::vector<uint8_t>(Bytes, Bytes + Size));
        Room->VideoSource->captureFrame(Frame, Timestamp);
    });
}
extern "C" int32_t LKUB_CALL lkub_output_audio_push(LKUB_Room* Room, const int16_t* Samples, int32_t Count, int32_t Rate, int32_t Channels, LKUB_Result* Result)
{
    return Guard(Result, [&] {
        Require(Room && Samples && Rate >= 8000 && Rate <= 48000 && Channels >= 1 && Channels <= 2 &&
            Count > 0 && Count % Channels == 0 && Count <= Rate * Channels / 10, "Invalid output audio frame.");
        std::lock_guard Lock(Room->OutputMutex);
        if (!Room->OutputAudio) {
            const auto Local = Room->SdkRoom->localParticipant().lock(); Require(Local != nullptr, "Local participant is unavailable.");
            auto Source = std::make_shared<livekit::AudioSource>(Rate, Channels, 0);
            auto Track = livekit::LocalAudioTrack::createLocalAudioTrack("avatar-audio", Source);
            livekit::TrackPublishOptions Options; Options.source = livekit::TrackSource::SOURCE_MICROPHONE;
            Local->publishTrack(Track, Options); Room->OutputRate = Rate; Room->OutputChannels = Channels;
            Room->OutputAudio = std::move(Source); Room->OutputTrack = std::move(Track);
        }
        Require(Room->OutputRate == Rate && Room->OutputChannels == Channels, "Output audio format changed.");
        livekit::AudioFrame Frame(std::vector<int16_t>(Samples, Samples + Count), Rate, Channels, Count / Channels);
        Room->OutputAudio->captureFrame(Frame, 20);
    });
}

extern "C" int32_t LKUB_CALL lkub_video_stats(LKUB_Room* Room, LKUB_VideoStats* Output, LKUB_Result* Result)
{
    bool Available = false;
    const auto Status = Guard(Result, [&] {
        Require(Room && Output, "Room and output are required.");
        std::lock_guard Lock(Room->OutputMutex);
        if (!Room->VideoTrack) return;
        const auto Now = std::chrono::steady_clock::now();
        if (!Room->VideoStatsRequest && Now >= Room->NextVideoStats) {
            Room->VideoStatsRequest = Room->VideoTrack->getStats();
            Room->NextVideoStats = Now + std::chrono::seconds(5);
        }
        if (!Room->VideoStatsRequest || Room->VideoStatsRequest->wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        const auto Records = Room->VideoStatsRequest->get(); Room->VideoStatsRequest.reset();
        for (const auto& Record : Records) {
            const auto* Sent = std::get_if<livekit::RtcOutboundRtpStats>(&Record.stats);
            if (!Sent || Sent->stream.kind != "video") continue;
            *Output = {};
            Output->frames_encoded = Sent->outbound.frames_encoded;
            Output->frames_sent = Sent->outbound.frames_sent;
            Output->bytes_sent = Sent->sent.bytes_sent;
            Output->encode_seconds = Sent->outbound.total_encode_time;
            Output->fps = Sent->outbound.frames_per_second;
            Output->quality_reason = static_cast<int32_t>(Sent->outbound.quality_limitation_reason);
            const auto& Encoder = Sent->outbound.encoder_implementation;
            std::memcpy(Output->encoder, Encoder.data(), std::min(Encoder.size(), sizeof(Output->encoder) - 1));
            Available = true;
        }
    });
    return Status == LKUB_STATUS_OK ? (Available ? 1 : 0) : -1;
}
