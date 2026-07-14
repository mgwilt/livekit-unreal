#include "LiveKitWindowsAdapterInternal.h"

#include <algorithm>
#include <chrono>
#include <vector>

using namespace lkub::internal;

extern "C"
{
uint32_t LKUB_CALL lkub_get_abi_version(void)
{
    return LKUB_ABI_VERSION;
}

int32_t LKUB_CALL lkub_initialize(
    int32_t LogLevel,
    uint8_t* OutInitializationOwned,
    LKUB_Result* OutResult)
{
    if (OutInitializationOwned != nullptr)
    {
        *OutInitializationOwned = 0;
    }
    return Guard(OutResult, [&]()
    {
        Require(
            OutInitializationOwned != nullptr,
            "The initialization ownership output is required.");
        *OutInitializationOwned = livekit::initialize(ToLogLevel(LogLevel)) ? 1 : 0;
    });
}

void LKUB_CALL lkub_shutdown(void)
{
    try
    {
        livekit::shutdown();
    }
    catch (...)
    {
    }
}

void LKUB_CALL lkub_result_reset(LKUB_Result* Result)
{
    ResetResult(Result);
}

void LKUB_CALL lkub_buffer_release(LKUB_OwnedBuffer* Buffer)
{
    if (Buffer != nullptr)
    {
        // C ABI buffers are allocated and released inside this /MD adapter DLL.
        // Unreal only borrows the pointer and never crosses the CRT heap boundary.
        delete[] Buffer->data;
        Buffer->data = nullptr;
        Buffer->size = 0;
    }
}

int32_t LKUB_CALL lkub_room_create(
    const LKUB_RoomCallbacks* Callbacks,
    void* CallbackContext,
    LKUB_Room** OutRoom,
    LKUB_Result* OutResult)
{
    if (OutRoom != nullptr)
    {
        *OutRoom = nullptr;
    }
    return Guard(OutResult, [&]()
    {
        Require(Callbacks != nullptr, "Room callbacks are required.");
        Require(OutRoom != nullptr, "The room output is required.");
        Require(
            Callbacks->struct_size == sizeof(LKUB_RoomCallbacks),
            "The room callback structure size does not match the adapter ABI.");
        Require(
            Callbacks->abi_version == LKUB_ABI_VERSION,
            "The room callback ABI version does not match the adapter.");

        std::unique_ptr<LKUB_Room> Room = std::make_unique<LKUB_Room>();
        Room->Callbacks = *Callbacks;
        Room->CallbackContext = CallbackContext;
        Room->SdkRoom = std::make_shared<livekit::Room>();
        Room->SdkRoom->setDelegate(Room.get());
        *OutRoom = Room.release();
    });
}

void LKUB_CALL lkub_room_detach_callbacks(LKUB_Room* Room)
{
    if (Room == nullptr)
    {
        return;
    }
    try
    {
        {
            std::lock_guard Lock(Room->CallbackMutex);
            Room->Callbacks = {};
            Room->CallbackContext = nullptr;
        }
        if (Room->SdkRoom)
        {
            Room->SdkRoom->setDelegate(nullptr);
        }
    }
    catch (...)
    {
    }
}

void LKUB_CALL lkub_room_destroy(LKUB_Room* Room)
{
    if (Room == nullptr)
    {
        return;
    }
    lkub_room_detach_callbacks(Room);
    try
    {
        Room->ClearAudio();
        Room->ClearSpeakers();
        Room->SdkRoom.reset();
    }
    catch (...)
    {
    }
    delete Room;
}

int32_t LKUB_CALL lkub_room_prepare_audio(
    LKUB_Room* Room,
    LKUB_Result* OutResult)
{
    return Guard(OutResult, [&]()
    {
        Require(Room != nullptr, "The room is required.");
        std::lock_guard Lock(Room->AudioMutex);
        if (!Room->PlatformAudio)
        {
            Room->PlatformAudio = std::make_shared<livekit::PlatformAudio>();
        }
    });
}

int32_t LKUB_CALL lkub_room_connect(
    LKUB_Room* Room,
    LKUB_StringView ServerUrl,
    LKUB_StringView Token,
    uint32_t ConnectTimeoutMilliseconds,
    LKUB_Result* OutResult)
{
    return Guard(OutResult, [&]()
    {
        Require(Room != nullptr && Room->SdkRoom, "The room is unavailable.");
        livekit::RoomOptions Options;
        Options.auto_subscribe = true;
        Options.connect_timeout = std::chrono::milliseconds(
            std::max<uint32_t>(ConnectTimeoutMilliseconds, 1));
        if (!Room->SdkRoom->connect(
                CopyString(ServerUrl),
                CopyString(Token),
                Options))
        {
            throw std::runtime_error("The LiveKit SDK did not connect to the room.");
        }
    });
}

int32_t LKUB_CALL lkub_room_disconnect(
    LKUB_Room* Room,
    uint8_t* OutListenerDrained,
    LKUB_Result* OutResult)
{
    if (OutListenerDrained != nullptr)
    {
        *OutListenerDrained = 0;
    }
    return Guard(OutResult, [&]()
    {
        Require(Room != nullptr && Room->SdkRoom, "The room is unavailable.");
        Require(OutListenerDrained != nullptr, "The listener drain output is required.");
        lkub_room_detach_callbacks(Room);
        try
        {
            (void)Room->SdkRoom->disconnect();
        }
        catch (...)
        {
            // The Room remains quarantined, but media is session-owned and must
            // not accumulate across reconnect attempts, even on SDK failure.
            Room->ClearAudio();
            Room->ClearSpeakers();
            throw;
        }
        Room->ClearAudio();
        Room->ClearSpeakers();
        // Room::disconnect reports graceful success, not that the raw FFI
        // listener work is fully drained. Keep the Room itself alive until
        // process-global SDK shutdown; its adapter-owned media is released above.
        *OutListenerDrained = 0;
    });
}

int32_t LKUB_CALL lkub_room_set_microphone_enabled(
    LKUB_Room* Room,
    uint8_t Enabled,
    LKUB_Result* OutResult)
{
    return Guard(OutResult, [&]()
    {
        Require(Room != nullptr && Room->SdkRoom, "The room is unavailable.");
        std::shared_ptr<livekit::LocalAudioTrack> ExistingTrack;
        std::shared_ptr<livekit::PlatformAudio> Audio;
        {
            std::lock_guard Lock(Room->AudioMutex);
            ExistingTrack = Room->MicrophoneTrack;
            if (Enabled != 0 && !Room->PlatformAudio)
            {
                Room->PlatformAudio = std::make_shared<livekit::PlatformAudio>();
            }
            Audio = Room->PlatformAudio;
        }

        if (ExistingTrack)
        {
            if (Enabled != 0)
            {
                ExistingTrack->unmute();
            }
            else
            {
                ExistingTrack->mute();
            }
            return;
        }
        if (Enabled == 0)
        {
            return;
        }

        const std::shared_ptr<livekit::LocalParticipant> Participant =
            Room->SdkRoom->localParticipant().lock();
        if (!Participant)
        {
            throw std::runtime_error("The local LiveKit participant is unavailable.");
        }
        const std::shared_ptr<livekit::PlatformAudioSource> NewSource =
            Audio->createAudioSource();
        const std::shared_ptr<livekit::LocalAudioTrack> NewTrack =
            livekit::LocalAudioTrack::createLocalAudioTrack(
                "avatar-microphone",
                NewSource);
        livekit::TrackPublishOptions Options;
        Options.source = livekit::TrackSource::SOURCE_MICROPHONE;
        Participant->publishTrack(NewTrack, Options);
        {
            std::lock_guard Lock(Room->AudioMutex);
            Room->MicrophoneSource = NewSource;
            Room->MicrophoneTrack = NewTrack;
        }
    });
}

int32_t LKUB_CALL lkub_room_publish_data(
    LKUB_Room* Room,
    LKUB_BytesView Payload,
    int32_t Reliability,
    LKUB_StringView Topic,
    const LKUB_StringView* DestinationIdentities,
    uint32_t DestinationCount,
    LKUB_Result* OutResult)
{
    return Guard(OutResult, [&]()
    {
        Require(Room != nullptr && Room->SdkRoom, "The room is unavailable.");
        Require(
            Payload.size == 0 || Payload.data != nullptr,
            "The data payload is invalid.");
        Require(
            DestinationCount == 0 || DestinationIdentities != nullptr,
            "The destination identity array is invalid.");
        Require(
            Reliability == LKUB_DATA_RELIABLE || Reliability == LKUB_DATA_LOSSY,
            "The data reliability value is invalid.");

        const std::shared_ptr<livekit::LocalParticipant> Participant =
            Room->SdkRoom->localParticipant().lock();
        if (!Participant)
        {
            throw std::runtime_error("The local LiveKit participant is unavailable.");
        }

        std::vector<uint8_t> Bytes;
        if (Payload.size != 0)
        {
            if (Payload.size > static_cast<uint64_t>(SIZE_MAX))
            {
                throw std::invalid_argument("The data payload is too large.");
            }
            Bytes.assign(Payload.data, Payload.data + static_cast<size_t>(Payload.size));
        }
        std::vector<std::string> Destinations;
        Destinations.reserve(DestinationCount);
        for (uint32_t Index = 0; Index < DestinationCount; ++Index)
        {
            Destinations.push_back(CopyString(DestinationIdentities[Index]));
        }
        Participant->publishData(
            Bytes,
            Reliability == LKUB_DATA_RELIABLE,
            Destinations,
            CopyString(Topic));
    });
}

int32_t LKUB_CALL lkub_room_visit_remote_participants(
    LKUB_Room* Room,
    LKUB_ParticipantVisitor Visitor,
    void* VisitorContext,
    LKUB_Result* OutResult)
{
    return Guard(OutResult, [&]()
    {
        Require(Room != nullptr && Room->SdkRoom, "The room is unavailable.");
        Require(Visitor != nullptr, "The participant visitor is required.");
        for (const std::weak_ptr<livekit::RemoteParticipant>& WeakParticipant :
             Room->SdkRoom->remoteParticipants())
        {
            if (const std::shared_ptr<livekit::RemoteParticipant> Participant =
                    WeakParticipant.lock())
            {
                const LKUB_ParticipantInfo Info = Room->ParticipantInfo(
                    *Participant,
                    Room->IsSpeaking(Participant->identity()));
                Visitor(VisitorContext, &Info);
            }
        }
    });
}
}
