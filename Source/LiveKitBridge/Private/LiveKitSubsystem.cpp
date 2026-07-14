#include "LiveKitSubsystem.h"

#include "Async/Async.h"
#include "LiveKitAppleBridge.h"
#include "Misc/Guid.h"

void ULiveKitSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    TWeakObjectPtr<ULiveKitSubsystem> WeakThis(this);
    Bridge = MakeShared<FLiveKitAppleBridge>(
        [WeakThis](ELiveKitConnectionState State)
        {
            const auto ApplyState = [WeakThis, State]()
            {
                if (WeakThis.IsValid())
                {
                    WeakThis->SetConnectionState(State);
                }
            };
            if (IsInGameThread())
            {
                ApplyState();
            }
            else
            {
                AsyncTask(ENamedThreads::GameThread, ApplyState);
            }
        },
        [WeakThis](const FLiveKitError& Error)
        {
            AsyncTask(ENamedThreads::GameThread, [WeakThis, Error]()
            {
                if (WeakThis.IsValid())
                {
                    WeakThis->ReportError(Error);
                }
            });
        },
        [WeakThis](const FLiveKitParticipantInfo& Participant)
        {
            AsyncTask(ENamedThreads::GameThread, [WeakThis, Participant]()
            {
                if (WeakThis.IsValid())
                {
                    WeakThis->HandleParticipantConnected(Participant);
                }
            });
        },
        [WeakThis](const FLiveKitParticipantInfo& Participant)
        {
            AsyncTask(ENamedThreads::GameThread, [WeakThis, Participant]()
            {
                if (WeakThis.IsValid())
                {
                    WeakThis->HandleParticipantDisconnected(Participant);
                }
            });
        },
        [WeakThis](const FLiveKitParticipantInfo& Participant, bool bIsSpeaking)
        {
            AsyncTask(ENamedThreads::GameThread, [WeakThis, Participant, bIsSpeaking]()
            {
                if (WeakThis.IsValid())
                {
                    WeakThis->HandleParticipantSpeaking(Participant, bIsSpeaking);
                }
            });
        },
        [WeakThis](const FLiveKitDataMessage& Message)
        {
            AsyncTask(ENamedThreads::GameThread, [WeakThis, Message]()
            {
                if (WeakThis.IsValid())
                {
                    WeakThis->OnDataReceived.Broadcast(Message);
                }
            });
        },
        [WeakThis](const FString& Topic, bool bSuccess, const FLiveKitError& Error)
        {
            AsyncTask(ENamedThreads::GameThread, [WeakThis, Topic, bSuccess, Error]()
            {
                if (!WeakThis.IsValid() || bSuccess)
                {
                    return;
                }

                WeakThis->RegisteredByteStreamTopics.Remove(Topic);
                WeakThis->ReportError(Error);
            });
        },
        [WeakThis](const FLiveKitByteStream& Stream)
        {
            AsyncTask(ENamedThreads::GameThread, [WeakThis, Stream]()
            {
                if (WeakThis.IsValid())
                {
                    WeakThis->OnByteStreamReceived.Broadcast(Stream);
                }
            });
        },
        [WeakThis](const FString& Method, bool bSuccess, const FLiveKitError& Error)
        {
            AsyncTask(ENamedThreads::GameThread, [WeakThis, Method, bSuccess, Error]()
            {
                if (!WeakThis.IsValid() || bSuccess)
                {
                    return;
                }

                WeakThis->RegisteredRpcMethods.Remove(Method);
                WeakThis->ReportError(Error);
            });
        },
        [WeakThis](const FLiveKitRpcInvocation& Invocation)
        {
            AsyncTask(ENamedThreads::GameThread, [WeakThis, Invocation]()
            {
                if (WeakThis.IsValid())
                {
                    WeakThis->OnRpcInvocation.Broadcast(Invocation);
                }
            });
        },
        [WeakThis](
            const FString& RequestId,
            bool bSuccess,
            const FString& ResponsePayload,
            const FLiveKitError& Error)
        {
            AsyncTask(
                ENamedThreads::GameThread,
                [WeakThis, RequestId, bSuccess, ResponsePayload, Error]()
                {
                    if (WeakThis.IsValid())
                    {
                        WeakThis->OnRpcResult.Broadcast(RequestId, bSuccess, ResponsePayload, Error);
                    }
                });
        },
        [WeakThis](const FString& OperationId, bool bSuccess, const FLiveKitError& Error)
        {
            AsyncTask(ENamedThreads::GameThread, [WeakThis, OperationId, bSuccess, Error]()
            {
                if (WeakThis.IsValid())
                {
                    WeakThis->OnPublishResult.Broadcast(OperationId, bSuccess, Error);
                }
            });
        });
    Bridge->ActivateLifetimeGate();
}

void ULiveKitSubsystem::Deinitialize()
{
    if (Bridge)
    {
        Bridge->Shutdown();
    }
    Bridge.Reset();
    RemoteParticipants.Reset();
    RegisteredByteStreamTopics.Reset();
    RegisteredRpcMethods.Reset();
    Super::Deinitialize();
}

void ULiveKitSubsystem::Connect(
    const FString& ServerUrl,
    const FString& ParticipantToken,
    const FLiveKitConnectOptions& Options)
{
    if (ConnectionState == ELiveKitConnectionState::Connecting ||
        ConnectionState == ELiveKitConnectionState::Connected ||
        ConnectionState == ELiveKitConnectionState::Reconnecting ||
        ConnectionState == ELiveKitConnectionState::Disconnecting)
    {
        return;
    }

    if (ServerUrl.IsEmpty() || ParticipantToken.IsEmpty())
    {
        FLiveKitError Error;
        Error.Code = TEXT("invalid_connection_details");
        Error.Message = TEXT("Server URL and participant token are required.");
        ReportError(Error);
        SetConnectionState(ELiveKitConnectionState::Failed);
        return;
    }

    if (!Bridge)
    {
        FLiveKitError Error;
        Error.Code = TEXT("bridge_unavailable");
        Error.Message = TEXT("The LiveKit bridge was not initialized.");
        ReportError(Error);
        SetConnectionState(ELiveKitConnectionState::Failed);
        return;
    }

    bMicrophoneEnabled = Options.bEnableMicrophone;
    Bridge->Connect(
        ServerUrl,
        ParticipantToken,
        bMicrophoneEnabled,
        Options.bEnableVoiceProcessing);
}

void ULiveKitSubsystem::Disconnect()
{
    if (ConnectionState == ELiveKitConnectionState::Disconnected ||
        ConnectionState == ELiveKitConnectionState::Disconnecting)
    {
        return;
    }

    if (Bridge)
    {
        Bridge->Disconnect();
    }
    else
    {
        SetConnectionState(ELiveKitConnectionState::Disconnected);
    }
}

void ULiveKitSubsystem::SetMicrophoneEnabled(bool bEnabled)
{
    bMicrophoneEnabled = bEnabled;
    if (Bridge && ConnectionState == ELiveKitConnectionState::Connected)
    {
        Bridge->SetMicrophoneEnabled(bEnabled);
    }
}

bool ULiveKitSubsystem::IsSdkAvailable() const
{
    return Bridge && Bridge->IsSdkAvailable();
}

TArray<FLiveKitParticipantInfo> ULiveKitSubsystem::GetRemoteParticipants() const
{
    TArray<FLiveKitParticipantInfo> Result;
    RemoteParticipants.GenerateValueArray(Result);
    return Result;
}

FString ULiveKitSubsystem::PublishData(
    const TArray<uint8>& Data,
    const FString& Topic,
    ELiveKitDataReliability Reliability,
    const TArray<FString>& DestinationIdentities)
{
    const FString OperationId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
    if (!Bridge || ConnectionState != ELiveKitConnectionState::Connected)
    {
        FLiveKitError Error;
        Error.Code = TEXT("not_connected");
        Error.Message = TEXT("Connect to a LiveKit room before publishing data.");
        TWeakObjectPtr<ULiveKitSubsystem> WeakThis(this);
        AsyncTask(ENamedThreads::GameThread, [WeakThis, OperationId, Error]()
        {
            if (WeakThis.IsValid())
            {
                WeakThis->OnPublishResult.Broadcast(OperationId, false, Error);
            }
        });
        return OperationId;
    }

    Bridge->PublishData(OperationId, Data, Topic, Reliability, DestinationIdentities);
    return OperationId;
}

FString ULiveKitSubsystem::PublishText(
    const FString& Text,
    const FString& Topic,
    ELiveKitDataReliability Reliability,
    const TArray<FString>& DestinationIdentities)
{
    FTCHARToUTF8 Converted(*Text);
    TArray<uint8> Data;
    Data.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
    return PublishData(Data, Topic, Reliability, DestinationIdentities);
}

bool ULiveKitSubsystem::RegisterByteStreamHandler(const FString& Topic)
{
    if (Topic.IsEmpty() || RegisteredByteStreamTopics.Contains(Topic) || !Bridge)
    {
        return false;
    }

    // The Swift registration completes asynchronously. Record intent first so
    // a synchronous rejection can remove it through the completion callback.
    RegisteredByteStreamTopics.Add(Topic);
    if (!Bridge->RegisterByteStreamHandler(Topic))
    {
        RegisteredByteStreamTopics.Remove(Topic);
        return false;
    }

    return true;
}

void ULiveKitSubsystem::UnregisterByteStreamHandler(const FString& Topic)
{
    if (Bridge && RegisteredByteStreamTopics.Remove(Topic) > 0)
    {
        Bridge->UnregisterByteStreamHandler(Topic);
    }
}

bool ULiveKitSubsystem::RegisterRpcMethod(const FString& Method)
{
    if (Method.IsEmpty() || RegisteredRpcMethods.Contains(Method) || !Bridge)
    {
        return false;
    }

    // Record intent before calling the Swift facade because a rejected
    // registration may complete synchronously. The completion callback removes
    // the entry on failure so the caller can retry.
    RegisteredRpcMethods.Add(Method);
    if (!Bridge->RegisterRpcMethod(Method))
    {
        RegisteredRpcMethods.Remove(Method);
        return false;
    }

    return true;
}

void ULiveKitSubsystem::UnregisterRpcMethod(const FString& Method)
{
    if (Bridge && RegisteredRpcMethods.Remove(Method) > 0)
    {
        Bridge->UnregisterRpcMethod(Method);
    }
}

FString ULiveKitSubsystem::PerformRpc(
    const FString& DestinationIdentity,
    const FString& Method,
    const FString& Payload,
    float ResponseTimeoutSeconds,
    float MaxRoundTripLatencySeconds)
{
    const FString RequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
    if (!Bridge || ConnectionState != ELiveKitConnectionState::Connected)
    {
        FLiveKitError Error;
        Error.Code = TEXT("not_connected");
        Error.Message = TEXT("Connect to a LiveKit room before performing RPC.");
        TWeakObjectPtr<ULiveKitSubsystem> WeakThis(this);
        AsyncTask(ENamedThreads::GameThread, [WeakThis, RequestId, Error]()
        {
            if (WeakThis.IsValid())
            {
                WeakThis->OnRpcResult.Broadcast(RequestId, false, FString(), Error);
            }
        });
        return RequestId;
    }

    if (DestinationIdentity.IsEmpty() || Method.IsEmpty())
    {
        FLiveKitError Error;
        Error.Code = TEXT("invalid_rpc_request");
        Error.Message = TEXT("Destination identity and method are required.");
        TWeakObjectPtr<ULiveKitSubsystem> WeakThis(this);
        AsyncTask(ENamedThreads::GameThread, [WeakThis, RequestId, Error]()
        {
            if (WeakThis.IsValid())
            {
                WeakThis->OnRpcResult.Broadcast(RequestId, false, FString(), Error);
            }
        });
        return RequestId;
    }

    Bridge->PerformRpc(
        RequestId,
        DestinationIdentity,
        Method,
        Payload,
        ResponseTimeoutSeconds,
        MaxRoundTripLatencySeconds);
    return RequestId;
}

void ULiveKitSubsystem::CompleteRpcInvocation(const FString& RequestId, const FString& ResponsePayload)
{
    if (Bridge)
    {
        Bridge->CompleteRpcInvocation(RequestId, ResponsePayload);
    }
}

void ULiveKitSubsystem::FailRpcInvocation(
    const FString& RequestId,
    int32 ErrorCode,
    const FString& Message,
    const FString& Data)
{
    if (Bridge)
    {
        Bridge->FailRpcInvocation(RequestId, ErrorCode, Message, Data);
    }
}

void ULiveKitSubsystem::SetConnectionState(ELiveKitConnectionState NewState)
{
    if (ConnectionState == NewState)
    {
        return;
    }

    ConnectionState = NewState;
    if (NewState == ELiveKitConnectionState::Disconnected)
    {
        RemoteParticipants.Reset();
    }

    const UEnum* StateEnum = StaticEnum<ELiveKitConnectionState>();
    UE_LOG(
        LogTemp,
        Log,
        TEXT("LiveKit connection state: %s"),
        StateEnum ? *StateEnum->GetNameStringByValue(static_cast<int64>(NewState)) : TEXT("Unknown"));
    OnConnectionStateChanged.Broadcast(NewState);
}

void ULiveKitSubsystem::ReportError(const FLiveKitError& Error)
{
    UE_LOG(LogTemp, Error, TEXT("LiveKit [%s]: %s"), *Error.Code, *Error.Message);
    OnError.Broadcast(Error);
}

void ULiveKitSubsystem::HandleParticipantConnected(const FLiveKitParticipantInfo& Participant)
{
    RemoteParticipants.Add(Participant.Identity, Participant);
    OnParticipantConnected.Broadcast(Participant);
}

void ULiveKitSubsystem::HandleParticipantDisconnected(const FLiveKitParticipantInfo& Participant)
{
    RemoteParticipants.Remove(Participant.Identity);
    OnParticipantDisconnected.Broadcast(Participant);
}

void ULiveKitSubsystem::HandleParticipantSpeaking(
    const FLiveKitParticipantInfo& Participant,
    bool bIsSpeaking)
{
    FLiveKitParticipantInfo Updated = Participant;
    Updated.bIsSpeaking = bIsSpeaking;
    RemoteParticipants.Add(Updated.Identity, Updated);
    OnParticipantSpeakingChanged.Broadcast(Updated, bIsSpeaking);
}

#if WITH_DEV_AUTOMATION_TESTS
void ULiveKitSubsystem::ReceiveRpcInvocationForTesting(const FLiveKitRpcInvocation& Invocation)
{
    OnRpcInvocation.Broadcast(Invocation);
}
#endif
