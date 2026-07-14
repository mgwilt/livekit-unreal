#pragma once

#include "CoreMinimal.h"
#include "LiveKitTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "LiveKitSubsystem.generated.h"

class ILiveKitPlatformBridge;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLiveKitConnectionStateChanged, ELiveKitConnectionState, State);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLiveKitErrorEvent, const FLiveKitError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLiveKitParticipantEvent, const FLiveKitParticipantInfo&, Participant);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
    FLiveKitParticipantSpeakingEvent,
    const FLiveKitParticipantInfo&,
    Participant,
    bool,
    bIsSpeaking);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLiveKitDataReceivedEvent, const FLiveKitDataMessage&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FLiveKitByteStreamReceivedEvent,
    const FLiveKitByteStream&,
    Stream);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLiveKitRpcInvocationEvent, const FLiveKitRpcInvocation&, Invocation);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(
    FLiveKitRpcResultEvent,
    const FString&,
    RequestId,
    bool,
    bSuccess,
    const FString&,
    ResponsePayload,
    const FLiveKitError&,
    Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
    FLiveKitPublishResultEvent,
    const FString&,
    OperationId,
    bool,
    bSuccess,
    const FLiveKitError&,
    Error);

/**
 * Blueprint-first LiveKit room client for Unreal Engine.
 *
 * The subsystem accepts a URL and participant token supplied by the host game.
 * It never creates tokens or stores LiveKit API secrets.
 */
UCLASS()
class LIVEKITBRIDGE_API ULiveKitSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    UFUNCTION(BlueprintCallable, Category="LiveKit")
    void Connect(
        const FString& ServerUrl,
        const FString& ParticipantToken,
        const FLiveKitConnectOptions& Options);

    UFUNCTION(BlueprintCallable, Category="LiveKit")
    void Disconnect();

    UFUNCTION(BlueprintCallable, Category="LiveKit|Audio")
    void SetMicrophoneEnabled(bool bEnabled);

    UFUNCTION(BlueprintPure, Category="LiveKit|Audio")
    bool IsMicrophoneEnabled() const { return bMicrophoneEnabled; }

    UFUNCTION(BlueprintPure, Category="LiveKit")
    ELiveKitConnectionState GetConnectionState() const { return ConnectionState; }

    UFUNCTION(BlueprintPure, Category="LiveKit")
    bool IsSdkAvailable() const;

    UFUNCTION(BlueprintPure, Category="LiveKit|Participants")
    TArray<FLiveKitParticipantInfo> GetRemoteParticipants() const;

    UFUNCTION(BlueprintCallable, Category="LiveKit|Data")
    FString PublishData(
        const TArray<uint8>& Data,
        const FString& Topic,
        ELiveKitDataReliability Reliability,
        const TArray<FString>& DestinationIdentities);

    UFUNCTION(BlueprintCallable, Category="LiveKit|Data")
    FString PublishText(
        const FString& Text,
        const FString& Topic,
        ELiveKitDataReliability Reliability,
        const TArray<FString>& DestinationIdentities);

    /** Register one completed-byte-stream receiver for an exact LiveKit topic. */
    UFUNCTION(BlueprintCallable, Category="LiveKit|Byte Streams")
    bool RegisterByteStreamHandler(const FString& Topic);

    UFUNCTION(BlueprintCallable, Category="LiveKit|Byte Streams")
    void UnregisterByteStreamHandler(const FString& Topic);

    UFUNCTION(BlueprintCallable, Category="LiveKit|RPC")
    bool RegisterRpcMethod(const FString& Method);

    UFUNCTION(BlueprintCallable, Category="LiveKit|RPC")
    void UnregisterRpcMethod(const FString& Method);

    UFUNCTION(BlueprintCallable, Category="LiveKit|RPC")
    FString PerformRpc(
        const FString& DestinationIdentity,
        const FString& Method,
        const FString& Payload,
        float ResponseTimeoutSeconds = 15.0f,
        float MaxRoundTripLatencySeconds = 7.0f);

    UFUNCTION(BlueprintCallable, Category="LiveKit|RPC")
    void CompleteRpcInvocation(const FString& RequestId, const FString& ResponsePayload);

    /**
     * Reject an incoming RPC invocation.
     *
     * The Windows C++ backend transmits ErrorCode, Message, and Data. The Apple
     * Swift backend maps the failure to built-in error 1500 (Application Error)
     * because its pinned SDK does not expose a public RpcError initializer.
     */
    UFUNCTION(BlueprintCallable, Category="LiveKit|RPC")
    void FailRpcInvocation(
        const FString& RequestId,
        int32 ErrorCode,
        const FString& Message,
        const FString& Data = FString());

    UPROPERTY(BlueprintAssignable, Category="LiveKit")
    FLiveKitConnectionStateChanged OnConnectionStateChanged;

    UPROPERTY(BlueprintAssignable, Category="LiveKit")
    FLiveKitErrorEvent OnError;

    UPROPERTY(BlueprintAssignable, Category="LiveKit|Participants")
    FLiveKitParticipantEvent OnParticipantConnected;

    UPROPERTY(BlueprintAssignable, Category="LiveKit|Participants")
    FLiveKitParticipantEvent OnParticipantDisconnected;

    UPROPERTY(BlueprintAssignable, Category="LiveKit|Participants")
    FLiveKitParticipantSpeakingEvent OnParticipantSpeakingChanged;

    UPROPERTY(BlueprintAssignable, Category="LiveKit|Data")
    FLiveKitDataReceivedEvent OnDataReceived;

    /** Fired after an incoming byte stream has been read completely. */
    UPROPERTY(BlueprintAssignable, Category="LiveKit|Byte Streams")
    FLiveKitByteStreamReceivedEvent OnByteStreamReceived;

    UPROPERTY(BlueprintAssignable, Category="LiveKit|RPC")
    FLiveKitRpcInvocationEvent OnRpcInvocation;

    UPROPERTY(BlueprintAssignable, Category="LiveKit|RPC")
    FLiveKitRpcResultEvent OnRpcResult;

    UPROPERTY(BlueprintAssignable, Category="LiveKit|Data")
    FLiveKitPublishResultEvent OnPublishResult;

#if WITH_DEV_AUTOMATION_TESTS
    void SetConnectionStateForTesting(ELiveKitConnectionState NewState) { SetConnectionState(NewState); }
    void ReceiveRpcInvocationForTesting(const FLiveKitRpcInvocation& Invocation);
#endif

private:
#if WITH_DEV_AUTOMATION_TESTS
    friend class FLiveKitSubsystemTestAccess;
#endif

    void SetConnectionState(ELiveKitConnectionState NewState);
    void ReportError(const FLiveKitError& Error);
    void HandleParticipantConnected(const FLiveKitParticipantInfo& Participant);
    void HandleParticipantDisconnected(const FLiveKitParticipantInfo& Participant);
    void HandleParticipantSpeaking(const FLiveKitParticipantInfo& Participant, bool bIsSpeaking);

    ELiveKitConnectionState ConnectionState = ELiveKitConnectionState::Disconnected;
    bool bMicrophoneEnabled = true;
    TMap<FString, FLiveKitParticipantInfo> RemoteParticipants;
    TSet<FString> RegisteredByteStreamTopics;
    TSet<FString> RegisteredRpcMethods;
    TSharedPtr<ILiveKitPlatformBridge> Bridge;
};
