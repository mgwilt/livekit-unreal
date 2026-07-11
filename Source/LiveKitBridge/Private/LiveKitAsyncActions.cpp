#include "LiveKitAsyncActions.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "LiveKitSubsystem.h"

namespace
{
    ULiveKitSubsystem* ResolveSubsystem(UObject* WorldContext)
    {
        const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
        UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
        return GameInstance ? GameInstance->GetSubsystem<ULiveKitSubsystem>() : nullptr;
    }

    FLiveKitError MakeContextError()
    {
        FLiveKitError Error;
        Error.Code = TEXT("invalid_world_context");
        Error.Message = TEXT("A valid game world and game instance are required.");
        return Error;
    }
}

UConnectLiveKitAsyncAction* UConnectLiveKitAsyncAction::ConnectToLiveKit(
    UObject* WorldContextObject,
    const FString& ServerUrl,
    const FString& ParticipantToken,
    const FLiveKitConnectOptions& Options)
{
    UConnectLiveKitAsyncAction* Action = NewObject<UConnectLiveKitAsyncAction>();
    Action->WorldContext = WorldContextObject;
    Action->Url = ServerUrl;
    Action->Token = ParticipantToken;
    Action->ConnectOptions = Options;
    Action->RegisterWithGameInstance(WorldContextObject);
    return Action;
}

void UConnectLiveKitAsyncAction::Activate()
{
    Subsystem = ResolveSubsystem(WorldContext);
    if (!Subsystem)
    {
        Failed.Broadcast(MakeContextError());
        Finish();
        return;
    }

    const ELiveKitConnectionState State = Subsystem->GetConnectionState();
    if (State == ELiveKitConnectionState::Connected)
    {
        Connected.Broadcast();
        Finish();
        return;
    }

    if (State == ELiveKitConnectionState::Connecting ||
        State == ELiveKitConnectionState::Reconnecting ||
        State == ELiveKitConnectionState::Disconnecting)
    {
        FLiveKitError Error;
        Error.Code = TEXT("connection_busy");
        Error.Message = TEXT("Another LiveKit connection operation is already in progress.");
        Failed.Broadcast(Error);
        Finish();
        return;
    }

    Subsystem->OnConnectionStateChanged.AddDynamic(this, &UConnectLiveKitAsyncAction::HandleStateChanged);
    Subsystem->OnError.AddDynamic(this, &UConnectLiveKitAsyncAction::HandleError);
    Subsystem->Connect(Url, Token, ConnectOptions);
}

void UConnectLiveKitAsyncAction::HandleStateChanged(ELiveKitConnectionState State)
{
    if (State == ELiveKitConnectionState::Connected)
    {
        Connected.Broadcast();
        Finish();
    }
    else if (State == ELiveKitConnectionState::Failed)
    {
        if (!LastError.IsSet())
        {
            LastError.Code = TEXT("connect_failed");
            LastError.Message = TEXT("The LiveKit connection failed.");
        }
        Failed.Broadcast(LastError);
        Finish();
    }
}

void UConnectLiveKitAsyncAction::HandleError(const FLiveKitError& Error)
{
    LastError = Error;
}

void UConnectLiveKitAsyncAction::Finish()
{
    if (Subsystem)
    {
        Subsystem->OnConnectionStateChanged.RemoveDynamic(this, &UConnectLiveKitAsyncAction::HandleStateChanged);
        Subsystem->OnError.RemoveDynamic(this, &UConnectLiveKitAsyncAction::HandleError);
    }
    SetReadyToDestroy();
}

UDisconnectLiveKitAsyncAction* UDisconnectLiveKitAsyncAction::DisconnectFromLiveKit(UObject* WorldContextObject)
{
    UDisconnectLiveKitAsyncAction* Action = NewObject<UDisconnectLiveKitAsyncAction>();
    Action->WorldContext = WorldContextObject;
    Action->RegisterWithGameInstance(WorldContextObject);
    return Action;
}

void UDisconnectLiveKitAsyncAction::Activate()
{
    Subsystem = ResolveSubsystem(WorldContext);
    if (!Subsystem)
    {
        Failed.Broadcast(MakeContextError());
        Finish();
        return;
    }

    if (Subsystem->GetConnectionState() == ELiveKitConnectionState::Disconnected)
    {
        Disconnected.Broadcast();
        Finish();
        return;
    }

    Subsystem->OnConnectionStateChanged.AddDynamic(this, &UDisconnectLiveKitAsyncAction::HandleStateChanged);
    Subsystem->OnError.AddDynamic(this, &UDisconnectLiveKitAsyncAction::HandleError);
    Subsystem->Disconnect();
}

void UDisconnectLiveKitAsyncAction::HandleStateChanged(ELiveKitConnectionState State)
{
    if (State == ELiveKitConnectionState::Disconnected)
    {
        Disconnected.Broadcast();
        Finish();
    }
    else if (State == ELiveKitConnectionState::Failed)
    {
        if (!LastError.IsSet())
        {
            LastError.Code = TEXT("disconnect_failed");
            LastError.Message = TEXT("The LiveKit disconnect failed.");
        }
        Failed.Broadcast(LastError);
        Finish();
    }
}

void UDisconnectLiveKitAsyncAction::HandleError(const FLiveKitError& Error)
{
    LastError = Error;
}

void UDisconnectLiveKitAsyncAction::Finish()
{
    if (Subsystem)
    {
        Subsystem->OnConnectionStateChanged.RemoveDynamic(this, &UDisconnectLiveKitAsyncAction::HandleStateChanged);
        Subsystem->OnError.RemoveDynamic(this, &UDisconnectLiveKitAsyncAction::HandleError);
    }
    SetReadyToDestroy();
}

UPerformLiveKitRpcAsyncAction* UPerformLiveKitRpcAsyncAction::PerformLiveKitRpc(
    UObject* WorldContextObject,
    const FString& DestinationIdentity,
    const FString& Method,
    const FString& Payload,
    float ResponseTimeoutSeconds,
    float MaxRoundTripLatencySeconds)
{
    UPerformLiveKitRpcAsyncAction* Action = NewObject<UPerformLiveKitRpcAsyncAction>();
    Action->WorldContext = WorldContextObject;
    Action->Destination = DestinationIdentity;
    Action->RpcMethod = Method;
    Action->RpcPayload = Payload;
    Action->ResponseTimeout = ResponseTimeoutSeconds;
    Action->MaxRoundTripLatency = MaxRoundTripLatencySeconds;
    Action->RegisterWithGameInstance(WorldContextObject);
    return Action;
}

void UPerformLiveKitRpcAsyncAction::Activate()
{
    Subsystem = ResolveSubsystem(WorldContext);
    if (!Subsystem)
    {
        Failed.Broadcast(MakeContextError());
        Finish();
        return;
    }

    Subsystem->OnRpcResult.AddDynamic(this, &UPerformLiveKitRpcAsyncAction::HandleRpcResult);
    RequestId = Subsystem->PerformRpc(
        Destination,
        RpcMethod,
        RpcPayload,
        ResponseTimeout,
        MaxRoundTripLatency);
}

void UPerformLiveKitRpcAsyncAction::HandleRpcResult(
    const FString& ResultRequestId,
    bool bSuccess,
    const FString& ResponsePayload,
    const FLiveKitError& Error)
{
    if (ResultRequestId != RequestId)
    {
        return;
    }

    if (bSuccess)
    {
        Succeeded.Broadcast(ResponsePayload);
    }
    else
    {
        Failed.Broadcast(Error);
    }
    Finish();
}

void UPerformLiveKitRpcAsyncAction::Finish()
{
    if (Subsystem)
    {
        Subsystem->OnRpcResult.RemoveDynamic(this, &UPerformLiveKitRpcAsyncAction::HandleRpcResult);
    }
    SetReadyToDestroy();
}
