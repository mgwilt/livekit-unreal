#include "LiveKitWindowsAdapterInternal.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace lkub::internal
{
void ResetResult(LKUB_Result* Result)
{
    if (Result != nullptr)
    {
        std::memset(Result, 0, sizeof(*Result));
        Result->status = LKUB_STATUS_OK;
    }
}

template <size_t Capacity>
void CopyResultText(
    char (&Destination)[Capacity],
    uint32_t& OutSize,
    std::string_view Source)
{
    const size_t CopySize = std::min(Source.size(), Capacity - 1);
    if (CopySize != 0)
    {
        std::memcpy(Destination, Source.data(), CopySize);
    }
    Destination[CopySize] = '\0';
    OutSize = static_cast<uint32_t>(CopySize);
}

int32_t SetResult(
    LKUB_Result* Result,
    LKUB_Status Status,
    std::string_view Message,
    std::string_view Data,
    uint32_t RpcErrorCode)
{
    ResetResult(Result);
    if (Result != nullptr)
    {
        Result->status = Status;
        Result->rpc_error_code = RpcErrorCode;
        CopyResultText(Result->message, Result->message_size, Message);
        CopyResultText(Result->data, Result->data_size, Data);
    }
    return static_cast<int32_t>(Status);
}

std::string CopyString(LKUB_StringView View)
{
    if (View.size == 0)
    {
        return {};
    }
    if (View.data == nullptr || View.size > static_cast<uint64_t>(SIZE_MAX))
    {
        throw std::invalid_argument("Invalid UTF-8 string view.");
    }
    return std::string(View.data, static_cast<size_t>(View.size));
}

LKUB_StringView BorrowString(const std::string& Value)
{
    return {Value.data(), static_cast<uint64_t>(Value.size())};
}

void Require(bool Condition, const char* Message)
{
    if (!Condition)
    {
        throw std::invalid_argument(Message);
    }
}

livekit::LogLevel ToLogLevel(int32_t Level)
{
    switch (Level)
    {
    case LKUB_LOG_TRACE: return livekit::LogLevel::Trace;
    case LKUB_LOG_DEBUG: return livekit::LogLevel::Debug;
    case LKUB_LOG_INFO: return livekit::LogLevel::Info;
    case LKUB_LOG_WARN: return livekit::LogLevel::Warn;
    case LKUB_LOG_ERROR: return livekit::LogLevel::Error;
    case LKUB_LOG_CRITICAL: return livekit::LogLevel::Critical;
    case LKUB_LOG_OFF: return livekit::LogLevel::Off;
    default: throw std::invalid_argument("Invalid LiveKit log level.");
    }
}
}

std::pair<LKUB_RoomCallbacks, void*> LKUB_Room::SnapshotCallbacks()
{
    std::lock_guard Lock(CallbackMutex);
    return {Callbacks, CallbackContext};
}

bool LKUB_Room::IsSpeaking(const std::string& Identity)
{
    std::lock_guard Lock(SpeakerMutex);
    return ActiveSpeakers.contains(Identity);
}

LKUB_ParticipantInfo LKUB_Room::ParticipantInfo(
    const livekit::Participant& Participant,
    bool Speaking)
{
    LKUB_ParticipantInfo Result{};
    Result.identity = lkub::internal::BorrowString(Participant.identity());
    Result.sid = lkub::internal::BorrowString(Participant.sid());
    Result.name = lkub::internal::BorrowString(Participant.name());
    Result.metadata = lkub::internal::BorrowString(Participant.metadata());
    Result.is_agent = Participant.kind() == livekit::ParticipantKind::Agent ? 1 : 0;
    Result.is_speaking = Speaking ? 1 : 0;
    return Result;
}

void LKUB_Room::ClearAudio()
{
    std::lock_guard Lock(AudioMutex);
    MicrophoneTrack.reset();
    MicrophoneSource.reset();
    PlatformAudio.reset();
}

void LKUB_Room::ClearSpeakers()
{
    std::lock_guard Lock(SpeakerMutex);
    ActiveSpeakers.clear();
}

void LKUB_Room::onParticipantConnected(
    livekit::Room&,
    const livekit::ParticipantConnectedEvent& Event)
{
    if (Event.participant == nullptr)
    {
        return;
    }
    const auto [CallbacksCopy, Context] = SnapshotCallbacks();
    if (CallbacksCopy.participant_connected != nullptr)
    {
        const LKUB_ParticipantInfo Info = ParticipantInfo(
            *Event.participant,
            IsSpeaking(Event.participant->identity()));
        CallbacksCopy.participant_connected(Context, this, &Info);
    }
}

void LKUB_Room::onParticipantDisconnected(
    livekit::Room&,
    const livekit::ParticipantDisconnectedEvent& Event)
{
    if (Event.participant == nullptr)
    {
        return;
    }
    const bool WasSpeaking = IsSpeaking(Event.participant->identity());
    const auto [CallbacksCopy, Context] = SnapshotCallbacks();
    if (CallbacksCopy.participant_disconnected != nullptr)
    {
        const LKUB_ParticipantInfo Info = ParticipantInfo(
            *Event.participant,
            WasSpeaking);
        CallbacksCopy.participant_disconnected(Context, this, &Info);
    }
    std::lock_guard Lock(SpeakerMutex);
    ActiveSpeakers.erase(Event.participant->identity());
}

void LKUB_Room::onActiveSpeakersChanged(
    livekit::Room& Room,
    const livekit::ActiveSpeakersChangedEvent& Event)
{
    std::unordered_set<std::string> NewSpeakers;
    for (const livekit::Participant* Participant : Event.speakers)
    {
        if (Participant != nullptr &&
            Room.remoteParticipant(Participant->identity()).lock())
        {
            NewSpeakers.insert(Participant->identity());
        }
    }

    std::vector<std::pair<std::string, bool>> Changed;
    {
        std::lock_guard Lock(SpeakerMutex);
        for (const std::string& Identity : ActiveSpeakers)
        {
            if (!NewSpeakers.contains(Identity))
            {
                Changed.emplace_back(Identity, false);
            }
        }
        for (const std::string& Identity : NewSpeakers)
        {
            if (!ActiveSpeakers.contains(Identity))
            {
                Changed.emplace_back(Identity, true);
            }
        }
        ActiveSpeakers = NewSpeakers;
    }

    const auto [CallbacksCopy, Context] = SnapshotCallbacks();
    if (CallbacksCopy.speaking_changed == nullptr)
    {
        return;
    }
    for (const auto& [Identity, Speaking] : Changed)
    {
        if (const std::shared_ptr<livekit::RemoteParticipant> Participant =
                Room.remoteParticipant(Identity).lock())
        {
            const LKUB_ParticipantInfo Info = ParticipantInfo(*Participant, Speaking);
            CallbacksCopy.speaking_changed(
                Context,
                this,
                &Info,
                Speaking ? 1 : 0);
        }
    }
}

void LKUB_Room::EmitConnectionState(int32_t State)
{
    const auto [CallbacksCopy, Context] = SnapshotCallbacks();
    if (CallbacksCopy.connection_state_changed != nullptr)
    {
        CallbacksCopy.connection_state_changed(Context, this, State);
    }
}

void LKUB_Room::onConnectionStateChanged(
    livekit::Room&,
    const livekit::ConnectionStateChangedEvent& Event)
{
    switch (Event.state)
    {
    case livekit::ConnectionState::Connected:
        EmitConnectionState(LKUB_CONNECTION_CONNECTED);
        break;
    case livekit::ConnectionState::Reconnecting:
        EmitConnectionState(LKUB_CONNECTION_RECONNECTING);
        break;
    case livekit::ConnectionState::Disconnected:
    default:
        EmitConnectionState(LKUB_CONNECTION_DISCONNECTED);
        break;
    }
}

void LKUB_Room::EmitTerminal(int32_t Reason)
{
    const auto [CallbacksCopy, Context] = SnapshotCallbacks();
    if (CallbacksCopy.terminal != nullptr)
    {
        CallbacksCopy.terminal(Context, this, Reason);
    }
}

void LKUB_Room::onDisconnected(
    livekit::Room&,
    const livekit::DisconnectedEvent& Event)
{
    ClearAudio();
    ClearSpeakers();
    EmitTerminal(static_cast<int32_t>(Event.reason));
}

void LKUB_Room::onRoomEos(livekit::Room&, const livekit::RoomEosEvent&)
{
    ClearAudio();
    ClearSpeakers();
    EmitTerminal(0);
}

void LKUB_Room::onReconnecting(livekit::Room&, const livekit::ReconnectingEvent&)
{
    EmitConnectionState(LKUB_CONNECTION_RECONNECTING);
}

void LKUB_Room::onReconnected(livekit::Room&, const livekit::ReconnectedEvent&)
{
    EmitConnectionState(LKUB_CONNECTION_CONNECTED);
}

void LKUB_Room::onUserPacketReceived(
    livekit::Room&,
    const livekit::UserDataPacketEvent& Event)
{
    const auto [CallbacksCopy, Context] = SnapshotCallbacks();
    if (CallbacksCopy.user_data_received == nullptr)
    {
        return;
    }
    LKUB_UserData Data{};
    if (Event.participant != nullptr)
    {
        Data.sender_identity = lkub::internal::BorrowString(Event.participant->identity());
    }
    Data.topic = lkub::internal::BorrowString(Event.topic);
    Data.payload = {
        Event.data.data(),
        static_cast<uint64_t>(Event.data.size())};
    Data.reliability = Event.kind == livekit::DataPacketKind::Reliable
        ? LKUB_DATA_RELIABLE
        : LKUB_DATA_LOSSY;
    CallbacksCopy.user_data_received(Context, this, &Data);
}
