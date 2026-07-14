#include "LiveKitWindowsBridgeInternal.h"

#include "LiveKitBridgeModule.h"

using namespace LiveKitWindowsBridgeInternal;

#if WITH_LIVEKIT_WINDOWS

void FLiveKitWindowsBridge::FImplementation::HandleParticipantConnected(
    void* Context,
    LKUB_Room* InRoom,
    const LKUB_ParticipantInfo* Participant)
{
    FImplementation* Self = static_cast<FImplementation*>(Context);
    if (Self == nullptr || Participant == nullptr || !Self->IsCurrentRoom(InRoom))
    {
        return;
    }
    if (const TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe> Pinned =
            Self->Owner.Pin())
    {
        Pinned->ParticipantConnectedHandler(ToParticipantInfo(*Participant));
    }
}

void FLiveKitWindowsBridge::FImplementation::HandleParticipantDisconnected(
    void* Context,
    LKUB_Room* InRoom,
    const LKUB_ParticipantInfo* Participant)
{
    FImplementation* Self = static_cast<FImplementation*>(Context);
    if (Self == nullptr || Participant == nullptr || !Self->IsCurrentRoom(InRoom))
    {
        return;
    }
    const std::string Identity(
        Participant->identity.data == nullptr ? "" : Participant->identity.data,
        static_cast<size_t>(Participant->identity.size));
    if (const TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe> Pinned =
            Self->Owner.Pin())
    {
        Pinned->ParticipantDisconnectedHandler(ToParticipantInfo(*Participant));
    }
    std::lock_guard Lock(Self->SpeakerMutex);
    Self->ActiveSpeakers.erase(Identity);
}

void FLiveKitWindowsBridge::FImplementation::HandleSpeakingChanged(
    void* Context,
    LKUB_Room* InRoom,
    const LKUB_ParticipantInfo* Participant,
    uint8 IsSpeakingNow)
{
    FImplementation* Self = static_cast<FImplementation*>(Context);
    if (Self == nullptr || Participant == nullptr || !Self->IsCurrentRoom(InRoom))
    {
        return;
    }
    const std::string Identity(
        Participant->identity.data == nullptr ? "" : Participant->identity.data,
        static_cast<size_t>(Participant->identity.size));
    {
        std::lock_guard Lock(Self->SpeakerMutex);
        if (IsSpeakingNow != 0)
        {
            Self->ActiveSpeakers.insert(Identity);
        }
        else
        {
            Self->ActiveSpeakers.erase(Identity);
        }
    }
    if (const TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe> Pinned =
            Self->Owner.Pin())
    {
        FLiveKitParticipantInfo Info = ToParticipantInfo(*Participant);
        Info.bIsSpeaking = IsSpeakingNow != 0;
        Pinned->SpeakingHandler(Info, IsSpeakingNow != 0);
    }
}

void FLiveKitWindowsBridge::FImplementation::HandleConnectionStateChanged(
    void* Context,
    LKUB_Room* InRoom,
    int32 State)
{
    FImplementation* Self = static_cast<FImplementation*>(Context);
    if (Self == nullptr || !Self->IsCurrentRoomReady(InRoom))
    {
        return;
    }
    if (const TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe> Pinned =
            Self->Owner.Pin())
    {
        switch (State)
        {
        case LKUB_CONNECTION_CONNECTED:
            Pinned->StateHandler(ELiveKitConnectionState::Connected);
            break;
        case LKUB_CONNECTION_RECONNECTING:
            Pinned->StateHandler(ELiveKitConnectionState::Reconnecting);
            break;
        case LKUB_CONNECTION_DISCONNECTED:
        default:
            Pinned->StateHandler(ELiveKitConnectionState::Disconnected);
            break;
        }
    }
}

void FLiveKitWindowsBridge::FImplementation::HandleTerminalRoom(LKUB_Room* InRoom)
{
    LKUB_Room* TerminalRoom = nullptr;
    bool bShouldNotify = false;
    if (!DetachTerminalRoom(InRoom, TerminalRoom, bShouldNotify))
    {
        return;
    }
    lkub_room_detach_callbacks(TerminalRoom);
    QuarantineLiveKitWindowsRoom(TerminalRoom);
    ClearSpeakers();
    if (bShouldNotify)
    {
        if (const TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe> Pinned =
                Owner.Pin())
        {
            Pinned->StateHandler(ELiveKitConnectionState::Disconnected);
        }
    }
}

void FLiveKitWindowsBridge::FImplementation::HandleTerminal(void* Context, LKUB_Room* InRoom, int32)
{
    if (FImplementation* Self = static_cast<FImplementation*>(Context))
    {
        Self->HandleTerminalRoom(InRoom);
    }
}

void FLiveKitWindowsBridge::FImplementation::HandleUserData(
    void* Context,
    LKUB_Room* InRoom,
    const LKUB_UserData* Data)
{
    FImplementation* Self = static_cast<FImplementation*>(Context);
    if (Self == nullptr || Data == nullptr || !Self->IsCurrentRoom(InRoom))
    {
        return;
    }
    if (const TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe> Pinned =
            Self->Owner.Pin())
    {
        FLiveKitDataMessage Message;
        Message.SenderIdentity = FromAdapterView(Data->sender_identity);
        Message.Topic = FromAdapterView(Data->topic);
        if (Data->payload.data != nullptr && Data->payload.size <= MAX_int32)
        {
            Message.Data.Append(Data->payload.data, static_cast<int32>(Data->payload.size));
        }
        Pinned->DataHandler(Message);
    }
}

void LKUB_CALL FLiveKitWindowsBridge::FImplementation::OnParticipantConnected(
    void* Context,
    LKUB_Room* InRoom,
    const LKUB_ParticipantInfo* Participant) noexcept
{
    try
    {
        HandleParticipantConnected(Context, InRoom, Participant);
    }
    catch (...)
    {
    }
}

void LKUB_CALL FLiveKitWindowsBridge::FImplementation::OnParticipantDisconnected(
    void* Context,
    LKUB_Room* InRoom,
    const LKUB_ParticipantInfo* Participant) noexcept
{
    try
    {
        HandleParticipantDisconnected(Context, InRoom, Participant);
    }
    catch (...)
    {
    }
}

void LKUB_CALL FLiveKitWindowsBridge::FImplementation::OnSpeakingChanged(
    void* Context,
    LKUB_Room* InRoom,
    const LKUB_ParticipantInfo* Participant,
    uint8 IsSpeakingNow) noexcept
{
    try
    {
        HandleSpeakingChanged(Context, InRoom, Participant, IsSpeakingNow);
    }
    catch (...)
    {
    }
}

void LKUB_CALL FLiveKitWindowsBridge::FImplementation::OnConnectionStateChanged(
    void* Context,
    LKUB_Room* InRoom,
    int32 State) noexcept
{
    try
    {
        HandleConnectionStateChanged(Context, InRoom, State);
    }
    catch (...)
    {
    }
}

void LKUB_CALL FLiveKitWindowsBridge::FImplementation::OnTerminal(
    void* Context,
    LKUB_Room* InRoom,
    int32 Reason) noexcept
{
    try
    {
        HandleTerminal(Context, InRoom, Reason);
    }
    catch (...)
    {
    }
}

void LKUB_CALL FLiveKitWindowsBridge::FImplementation::OnUserData(
    void* Context,
    LKUB_Room* InRoom,
    const LKUB_UserData* Data) noexcept
{
    try
    {
        HandleUserData(Context, InRoom, Data);
    }
    catch (...)
    {
    }
}

LKUB_RoomCallbacks FLiveKitWindowsBridge::FImplementation::MakeRoomCallbacks()
{
    LKUB_RoomCallbacks Callbacks{};
    Callbacks.struct_size = sizeof(LKUB_RoomCallbacks);
    Callbacks.abi_version = LKUB_ABI_VERSION;
    Callbacks.participant_connected = &OnParticipantConnected;
    Callbacks.participant_disconnected = &OnParticipantDisconnected;
    Callbacks.speaking_changed = &OnSpeakingChanged;
    Callbacks.connection_state_changed = &OnConnectionStateChanged;
    Callbacks.terminal = &OnTerminal;
    Callbacks.user_data_received = &OnUserData;
    return Callbacks;
}

void FLiveKitWindowsBridge::FImplementation::HandleVisitParticipant(
    void* Context,
    const LKUB_ParticipantInfo* Participant)
{
    FParticipantVisitContext* Visit = static_cast<FParticipantVisitContext*>(Context);
    if (Visit == nullptr || Participant == nullptr ||
        Visit->Implementation == nullptr || Visit->Bridge == nullptr ||
        !Visit->Implementation->IsCurrentRoom(
            Visit->Room,
            Visit->ConnectionGeneration))
    {
        return;
    }
    Visit->Bridge->ParticipantConnectedHandler(ToParticipantInfo(*Participant));
}

void LKUB_CALL FLiveKitWindowsBridge::FImplementation::VisitParticipant(
    void* Context,
    const LKUB_ParticipantInfo* Participant) noexcept
{
    try
    {
        HandleVisitParticipant(Context, Participant);
    }
    catch (...)
    {
    }
}

#endif
