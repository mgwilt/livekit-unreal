#include "LiveKitWindowsBridgeInternal.h"

#include "LiveKitBridgeModule.h"

using namespace LiveKitWindowsBridgeInternal;

#if WITH_LIVEKIT_WINDOWS

uint64 FLiveKitWindowsBridge::FImplementation::BeginConnectionRequest()
{
    uint64 Generation = 0;
    {
        std::lock_guard Lock(RoomMutex);
        bConnectionRequested.store(true, std::memory_order_release);
        Generation = ConnectionGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    CancelPendingRpcs("The LiveKit room connection was replaced.");
    CancelAllTrackedByteStreamReaders();
    return Generation;
}

uint64 FLiveKitWindowsBridge::FImplementation::InvalidateConnectionRequest()
{
    uint64 Generation = 0;
    {
        std::lock_guard Lock(RoomMutex);
        bConnectionRequested.store(false, std::memory_order_release);
        Generation = ConnectionGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    CancelPendingRpcs("The LiveKit room disconnected.");
    CancelAllTrackedByteStreamReaders();
    return Generation;
}

bool FLiveKitWindowsBridge::FImplementation::IsConnectionRequestCurrent(uint64 Generation)
{
    std::lock_guard Lock(RoomMutex);
    return !bShuttingDown.load(std::memory_order_acquire) &&
        bConnectionRequested.load(std::memory_order_acquire) &&
        ConnectionGeneration.load(std::memory_order_acquire) == Generation;
}

bool FLiveKitWindowsBridge::FImplementation::IsDisconnectRequestCurrent(uint64 Generation)
{
    std::lock_guard Lock(RoomMutex);
    return !bShuttingDown.load(std::memory_order_acquire) &&
        !bConnectionRequested.load(std::memory_order_acquire) &&
        ConnectionGeneration.load(std::memory_order_acquire) == Generation;
}

LKUB_Room* FLiveKitWindowsBridge::FImplementation::GetRoom()
{
    std::lock_guard Lock(RoomMutex);
    return Room;
}

bool FLiveKitWindowsBridge::FImplementation::GetCurrentRoom(LKUB_Room*& OutRoom, uint64& OutGeneration)
{
    std::lock_guard Lock(RoomMutex);
    if (Room == nullptr ||
        RoomGeneration != ConnectionGeneration.load(std::memory_order_acquire) ||
        !bConnectionRequested.load(std::memory_order_acquire) ||
        bShuttingDown.load(std::memory_order_acquire))
    {
        return false;
    }
    OutRoom = Room;
    OutGeneration = RoomGeneration;
    return true;
}

bool FLiveKitWindowsBridge::FImplementation::HasReadyRoom()
{
    std::lock_guard Lock(RoomMutex);
    return Room != nullptr &&
        RoomGeneration == ConnectionGeneration.load(std::memory_order_acquire) &&
        bRoomReady &&
        bConnectionRequested.load(std::memory_order_acquire) &&
        !bShuttingDown.load(std::memory_order_acquire);
}

void FLiveKitWindowsBridge::FImplementation::SetRoom(LKUB_Room* InRoom, uint64 Generation)
{
    std::lock_guard Lock(RoomMutex);
    Room = InRoom;
    RoomGeneration = Generation;
    bRoomReady = false;
}

void FLiveKitWindowsBridge::FImplementation::ClearRoomIf(LKUB_Room* Candidate)
{
    std::lock_guard Lock(RoomMutex);
    if (Room == Candidate)
    {
        Room = nullptr;
        RoomGeneration = 0;
        bRoomReady = false;
    }
}

bool FLiveKitWindowsBridge::FImplementation::IsCurrentRoom(LKUB_Room* Candidate, uint64 Generation)
{
    std::lock_guard Lock(RoomMutex);
    if (Candidate == nullptr || Room != Candidate ||
        !bConnectionRequested.load(std::memory_order_acquire) ||
        bShuttingDown.load(std::memory_order_acquire) ||
        RoomGeneration != ConnectionGeneration.load(std::memory_order_acquire))
    {
        return false;
    }
    return Generation == 0 || RoomGeneration == Generation;
}

bool FLiveKitWindowsBridge::FImplementation::MarkRoomReady(LKUB_Room* Candidate, uint64 Generation)
{
    std::lock_guard Lock(RoomMutex);
    if (Candidate == nullptr || Room != Candidate || RoomGeneration != Generation ||
        RoomGeneration != ConnectionGeneration.load(std::memory_order_acquire) ||
        !bConnectionRequested.load(std::memory_order_acquire) ||
        bShuttingDown.load(std::memory_order_acquire))
    {
        return false;
    }
    bRoomReady = true;
    {
        std::lock_guard PendingLock(PendingRpcMutex);
        bRpcAdmissionOpen = true;
        RpcAdmissionGeneration = Generation;
    }
    return true;
}

bool FLiveKitWindowsBridge::FImplementation::IsCurrentRoomReady(LKUB_Room* Candidate)
{
    std::lock_guard Lock(RoomMutex);
    return Candidate != nullptr && Room == Candidate && bRoomReady &&
        RoomGeneration == ConnectionGeneration.load(std::memory_order_acquire) &&
        bConnectionRequested.load(std::memory_order_acquire) &&
        !bShuttingDown.load(std::memory_order_acquire);
}

bool FLiveKitWindowsBridge::FImplementation::DetachTerminalRoom(
    LKUB_Room* Candidate,
    LKUB_Room*& OutRoom,
    bool& bOutShouldNotify)
{
    {
        std::lock_guard Lock(RoomMutex);
        if (Candidate == nullptr || Room != Candidate)
        {
            return false;
        }

        OutRoom = Room;
        Room = nullptr;
        const uint64 CurrentGeneration =
            ConnectionGeneration.load(std::memory_order_acquire);
        const bool bHasNewerConnectionRequest =
            bConnectionRequested.load(std::memory_order_acquire) &&
            RoomGeneration != CurrentGeneration;
        RoomGeneration = 0;
        bRoomReady = false;

        if (bHasNewerConnectionRequest)
        {
            bOutShouldNotify = false;
        }
        else if (bConnectionRequested.exchange(false, std::memory_order_acq_rel))
        {
            ConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
            bOutShouldNotify = true;
        }
        else
        {
            bOutShouldNotify = !bShuttingDown.load(std::memory_order_acquire);
        }
    }
    CancelPendingRpcs("The LiveKit room disconnected.");
    CancelAllTrackedByteStreamReaders();
    return true;
}

bool FLiveKitWindowsBridge::FImplementation::FailConnectionRequestIfCurrent(uint64 Generation)
{
    {
        std::lock_guard Lock(RoomMutex);
        if (ConnectionGeneration.load(std::memory_order_acquire) != Generation)
        {
            return false;
        }
        bConnectionRequested.store(false, std::memory_order_release);
        ConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
    }
    CancelPendingRpcs("The LiveKit room connection failed.");
    CancelAllTrackedByteStreamReaders();
    return true;
}

bool FLiveKitWindowsBridge::FImplementation::DisconnectRoomOrQuarantine(
    LKUB_Room* Candidate,
    FLiveKitError* OutError)
{
    if (Candidate == nullptr)
    {
        return true;
    }

    // Detaching first prevents terminal callbacks from racing ownership of
    // the same opaque room while disconnect is in progress.
    lkub_room_detach_callbacks(Candidate);
    uint8 ListenerDrained = 0;
    LKUB_Result Result{};
    const int32 Status = lkub_room_disconnect(Candidate, &ListenerDrained, &Result);
    if (Status == LKUB_STATUS_OK && ListenerDrained != 0)
    {
        lkub_room_destroy(Candidate);
        return true;
    }

    QuarantineLiveKitWindowsRoom(Candidate);
    if (Status != LKUB_STATUS_OK && OutError != nullptr)
    {
        *OutError = AdapterError(TEXT("disconnect_failed"), Result);
    }
    return false;
}

bool FLiveKitWindowsBridge::FImplementation::IsSpeaking(const std::string& Identity)
{
    std::lock_guard Lock(SpeakerMutex);
    return ActiveSpeakers.contains(Identity);
}

void FLiveKitWindowsBridge::FImplementation::ClearSpeakers()
{
    std::lock_guard Lock(SpeakerMutex);
    ActiveSpeakers.clear();
}

#endif

void FLiveKitWindowsBridge::Connect(
    const FString& ServerUrl,
    const FString& Token,
    bool bEnableMicrophone,
    bool /* bEnableVoiceProcessing */)
{
    if (!IsSdkAvailable())
    {
        ReportUnavailable(TEXT("connect"));
        StateHandler(ELiveKitConnectionState::Failed);
        return;
    }
#if WITH_LIVEKIT_WINDOWS
    const uint64 ConnectionGeneration = Implementation->BeginConnectionRequest();
    StateHandler(ELiveKitConnectionState::Connecting);
    const bool bDispatched = DispatchControl([
        ServerUrl,
        Token,
        bEnableMicrophone,
        ConnectionGeneration](FLiveKitWindowsBridge& Bridge)
    {
        if (!Bridge.Implementation->IsConnectionRequestCurrent(ConnectionGeneration))
        {
            return;
        }
        LKUB_Room* Room = nullptr;
        bool bConnectAttempted = false;
        auto CleanupRoom = [&Bridge, &Room, &bConnectAttempted]()
        {
            if (Room == nullptr)
            {
                return;
            }
            if (bConnectAttempted)
            {
                Bridge.Implementation->DisconnectRoomOrQuarantine(Room);
            }
            else
            {
                lkub_room_detach_callbacks(Room);
                lkub_room_destroy(Room);
            }
            Bridge.Implementation->ClearRoomIf(Room);
            Room = nullptr;
            Bridge.Implementation->ClearSpeakers();
        };

        try
        {
            if (LKUB_Room* PreviousRoom = Bridge.Implementation->GetRoom())
            {
                Bridge.Implementation->DisconnectRoomOrQuarantine(PreviousRoom);
                Bridge.Implementation->ClearRoomIf(PreviousRoom);
            }
            Bridge.Implementation->ClearSpeakers();
            if (!Bridge.Implementation->IsConnectionRequestCurrent(ConnectionGeneration))
            {
                return;
            }

            const LKUB_RoomCallbacks Callbacks = FImplementation::MakeRoomCallbacks();
            LKUB_Result Result{};
            if (lkub_room_create(
                    &Callbacks,
                    Bridge.Implementation.Get(),
                    &Room,
                    &Result) != LKUB_STATUS_OK || Room == nullptr)
            {
                if (Room != nullptr)
                {
                    lkub_room_detach_callbacks(Room);
                    lkub_room_destroy(Room);
                    Room = nullptr;
                }
                if (Bridge.Implementation->FailConnectionRequestIfCurrent(ConnectionGeneration))
                {
                    Bridge.ErrorHandler(AdapterError(TEXT("connect_failed"), Result));
                    Bridge.StateHandler(ELiveKitConnectionState::Failed);
                }
                return;
            }
            Bridge.Implementation->SetRoom(Room, ConnectionGeneration);

            Result = {};
            if (lkub_room_prepare_audio(Room, &Result) != LKUB_STATUS_OK)
            {
                Bridge.ErrorHandler(AdapterError(TEXT("audio_initialization_failed"), Result));
            }
            const std::string UrlUtf8 = ToAdapterString(ServerUrl);
            const std::string TokenUtf8 = ToAdapterString(Token);
            Result = {};
            bConnectAttempted = true;
            if (lkub_room_connect(
                    Room,
                    ToAdapterView(UrlUtf8),
                    ToAdapterView(TokenUtf8),
                    15000,
                    &Result) != LKUB_STATUS_OK)
            {
                CleanupRoom();
                if (Bridge.Implementation->FailConnectionRequestIfCurrent(ConnectionGeneration))
                {
                    Bridge.ErrorHandler(AdapterError(TEXT("connect_failed"), Result));
                    Bridge.StateHandler(ELiveKitConnectionState::Failed);
                }
                return;
            }
            if (!Bridge.Implementation->IsCurrentRoom(Room, ConnectionGeneration))
            {
                CleanupRoom();
                return;
            }

            Result = {};
            if (lkub_room_set_microphone_enabled(
                    Room,
                    bEnableMicrophone ? 1 : 0,
                    &Result) != LKUB_STATUS_OK)
            {
                Bridge.ErrorHandler(AdapterError(TEXT("microphone_publish_failed"), Result));
            }
            if (!Bridge.Implementation->IsCurrentRoom(Room, ConnectionGeneration))
            {
                CleanupRoom();
                return;
            }
            Bridge.Implementation->RestoreRegistrations(
                Room,
                ConnectionGeneration,
                Bridge);
            if (!Bridge.Implementation->MarkRoomReady(Room, ConnectionGeneration))
            {
                CleanupRoom();
                return;
            }

            FImplementation::FParticipantVisitContext VisitContext{
                Bridge.Implementation.Get(),
                &Bridge,
                Room,
                ConnectionGeneration};
            Result = {};
            if (lkub_room_visit_remote_participants(
                    Room,
                    &FImplementation::VisitParticipant,
                    &VisitContext,
                    &Result) != LKUB_STATUS_OK)
            {
                Bridge.ErrorHandler(AdapterError(TEXT("participant_snapshot_failed"), Result));
            }
            if (Bridge.Implementation->IsCurrentRoom(Room, ConnectionGeneration))
            {
                Bridge.StateHandler(ELiveKitConnectionState::Connected);
            }
        }
        catch (...)
        {
            CleanupRoom();
            if (Bridge.Implementation->FailConnectionRequestIfCurrent(ConnectionGeneration))
            {
                Bridge.ErrorHandler(UnknownBackendError(TEXT("connect_failed")));
                Bridge.StateHandler(ELiveKitConnectionState::Failed);
            }
        }
    });
    if (!bDispatched &&
        Implementation->FailConnectionRequestIfCurrent(ConnectionGeneration))
    {
        StateHandler(ELiveKitConnectionState::Failed);
    }
#endif
}

void FLiveKitWindowsBridge::Disconnect()
{
    if (!IsSdkAvailable())
    {
        StateHandler(ELiveKitConnectionState::Disconnected);
        return;
    }
#if WITH_LIVEKIT_WINDOWS
    const uint64 DisconnectGeneration = Implementation->InvalidateConnectionRequest();
    StateHandler(ELiveKitConnectionState::Disconnecting);
    const bool bDispatched = DispatchControl([DisconnectGeneration](
        FLiveKitWindowsBridge& Bridge)
    {
        if (LKUB_Room* Room = Bridge.Implementation->GetRoom())
        {
            FLiveKitError Error;
            Bridge.Implementation->DisconnectRoomOrQuarantine(Room, &Error);
            if (!Error.Code.IsEmpty())
            {
                Bridge.ErrorHandler(Error);
            }
            Bridge.Implementation->ClearRoomIf(Room);
        }
        Bridge.Implementation->ClearSpeakers();
        if (Bridge.Implementation->IsDisconnectRequestCurrent(DisconnectGeneration))
        {
            Bridge.StateHandler(ELiveKitConnectionState::Disconnected);
        }
    });
    if (!bDispatched && Implementation->IsDisconnectRequestCurrent(DisconnectGeneration))
    {
        StateHandler(ELiveKitConnectionState::Disconnected);
    }
#endif
}

void FLiveKitWindowsBridge::SetMicrophoneEnabled(bool bEnabled)
{
    if (!IsSdkAvailable())
    {
        ReportUnavailable(TEXT("microphone"));
        return;
    }
    DispatchControl([bEnabled](FLiveKitWindowsBridge& Bridge)
    {
#if WITH_LIVEKIT_WINDOWS
        LKUB_Room* Room = Bridge.Implementation->GetRoom();
        if (Room == nullptr)
        {
            return;
        }
        LKUB_Result Result{};
        if (lkub_room_set_microphone_enabled(
                Room,
                bEnabled ? 1 : 0,
                &Result) != LKUB_STATUS_OK)
        {
            Bridge.ErrorHandler(AdapterError(TEXT("microphone_update_failed"), Result));
        }
#endif
    });
}

void FLiveKitWindowsBridge::PublishData(
    const FString& OperationId,
    const TArray<uint8>& Data,
    const FString& Topic,
    ELiveKitDataReliability Reliability,
    const TArray<FString>& DestinationIdentities)
{
    if (!IsSdkAvailable())
    {
        PublishResultHandler(
            OperationId,
            false,
            MakeLiveKitError(
                TEXT("sdk_unavailable"),
                TEXT("The LiveKit Windows adapter is unavailable for data publishing.")));
        return;
    }
    DispatchControl([OperationId, Data, Topic, Reliability, DestinationIdentities](
        FLiveKitWindowsBridge& Bridge)
    {
#if WITH_LIVEKIT_WINDOWS
        LKUB_Room* Room = Bridge.Implementation->GetRoom();
        if (Room == nullptr)
        {
            Bridge.PublishResultHandler(
                OperationId,
                false,
                MakeLiveKitError(TEXT("publish_failed"), TEXT("The LiveKit room is unavailable.")));
            return;
        }
        const std::string TopicUtf8 = ToAdapterString(Topic);
        std::vector<std::string> DestinationStorage;
        DestinationStorage.reserve(DestinationIdentities.Num());
        for (const FString& Identity : DestinationIdentities)
        {
            DestinationStorage.push_back(ToAdapterString(Identity));
        }
        std::vector<LKUB_StringView> Destinations;
        Destinations.reserve(DestinationStorage.size());
        for (const std::string& Identity : DestinationStorage)
        {
            Destinations.push_back(ToAdapterView(Identity));
        }
        const LKUB_BytesView Payload{
            Data.IsEmpty() ? nullptr : Data.GetData(),
            static_cast<uint64_t>(Data.Num())};
        LKUB_Result Result{};
        if (lkub_room_publish_data(
                Room,
                Payload,
                Reliability == ELiveKitDataReliability::Reliable
                    ? LKUB_DATA_RELIABLE
                    : LKUB_DATA_LOSSY,
                ToAdapterView(TopicUtf8),
                Destinations.empty() ? nullptr : Destinations.data(),
                static_cast<uint32>(Destinations.size()),
                &Result) != LKUB_STATUS_OK)
        {
            Bridge.PublishResultHandler(
                OperationId,
                false,
                AdapterError(TEXT("publish_failed"), Result));
            return;
        }
        Bridge.PublishResultHandler(OperationId, true, FLiveKitError());
#endif
    });
}
