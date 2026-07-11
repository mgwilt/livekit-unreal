#pragma once

#include "CoreMinimal.h"
#include "LiveKitTypes.h"

class FLiveKitAppleBridge : public TSharedFromThis<FLiveKitAppleBridge, ESPMode::ThreadSafe>
{
public:
    using FStateHandler = TFunction<void(ELiveKitConnectionState)>;
    using FErrorHandler = TFunction<void(const FLiveKitError&)>;
    using FParticipantHandler = TFunction<void(const FLiveKitParticipantInfo&)>;
    using FSpeakingHandler = TFunction<void(const FLiveKitParticipantInfo&, bool)>;
    using FDataHandler = TFunction<void(const FLiveKitDataMessage&)>;
    using FByteStreamRegistrationHandler =
        TFunction<void(const FString&, bool, const FLiveKitError&)>;
    using FByteStreamHandler = TFunction<void(const FLiveKitByteStream&)>;
    using FRpcRegistrationHandler =
        TFunction<void(const FString&, bool, const FLiveKitError&)>;
    using FRpcInvocationHandler = TFunction<void(const FLiveKitRpcInvocation&)>;
    using FRpcResultHandler = TFunction<void(const FString&, bool, const FString&, const FLiveKitError&)>;
    using FPublishResultHandler = TFunction<void(const FString&, bool, const FLiveKitError&)>;

    FLiveKitAppleBridge(
        FStateHandler InStateHandler,
        FErrorHandler InErrorHandler,
        FParticipantHandler InParticipantConnectedHandler,
        FParticipantHandler InParticipantDisconnectedHandler,
        FSpeakingHandler InSpeakingHandler,
        FDataHandler InDataHandler,
        FByteStreamRegistrationHandler InByteStreamRegistrationHandler,
        FByteStreamHandler InByteStreamHandler,
        FRpcRegistrationHandler InRpcRegistrationHandler,
        FRpcInvocationHandler InRpcInvocationHandler,
        FRpcResultHandler InRpcResultHandler,
        FPublishResultHandler InPublishResultHandler);
    ~FLiveKitAppleBridge();

    // Must be called immediately after MakeShared so asynchronous Apple SDK
    // callbacks can pin this bridge instead of retaining a raw pointer.
    void ActivateLifetimeGate();
    void Shutdown();

    bool IsSdkAvailable() const;
    void Connect(const FString& ServerUrl, const FString& Token, bool bEnableMicrophone);
    void Disconnect();
    void SetMicrophoneEnabled(bool bEnabled);
    void PublishData(
        const FString& OperationId,
        const TArray<uint8>& Data,
        const FString& Topic,
        ELiveKitDataReliability Reliability,
        const TArray<FString>& DestinationIdentities);
    bool RegisterByteStreamHandler(const FString& Topic);
    void UnregisterByteStreamHandler(const FString& Topic);
    bool RegisterRpcMethod(const FString& Method);
    void UnregisterRpcMethod(const FString& Method);
    void PerformRpc(
        const FString& RequestId,
        const FString& DestinationIdentity,
        const FString& Method,
        const FString& Payload,
        float ResponseTimeoutSeconds,
        float MaxRoundTripLatencySeconds);
    void CompleteRpcInvocation(const FString& RequestId, const FString& ResponsePayload);
    void FailRpcInvocation(
        const FString& RequestId,
        int32 ErrorCode,
        const FString& Message,
        const FString& Data);

    void NotifyState(ELiveKitConnectionState State);
    void NotifyError(const FLiveKitError& Error);
    void NotifyParticipantConnected(const FLiveKitParticipantInfo& Participant);
    void NotifyParticipantDisconnected(const FLiveKitParticipantInfo& Participant);
    void NotifyParticipantSpeaking(const FLiveKitParticipantInfo& Participant, bool bIsSpeaking);
    void NotifyData(const FLiveKitDataMessage& Message);
    void NotifyByteStreamRegistrationResult(
        const FString& Topic,
        bool bSuccess,
        const FLiveKitError& Error);
    void NotifyByteStream(const FLiveKitByteStream& Stream);
    void NotifyRpcRegistrationResult(
        const FString& Method,
        bool bSuccess,
        const FLiveKitError& Error);
    void NotifyRpcInvocation(const FLiveKitRpcInvocation& Invocation);
    void NotifyRpcResult(
        const FString& RequestId,
        bool bSuccess,
        const FString& ResponsePayload,
        const FLiveKitError& Error);
    void NotifyPublishResult(const FString& OperationId, bool bSuccess, const FLiveKitError& Error);
    bool IsManualDisconnectInProgress() const;

private:
    void CreateRoom();

    struct FImplementation;
    TUniquePtr<FImplementation> Implementation;
    FStateHandler StateHandler;
    FErrorHandler ErrorHandler;
    FParticipantHandler ParticipantConnectedHandler;
    FParticipantHandler ParticipantDisconnectedHandler;
    FSpeakingHandler SpeakingHandler;
    FDataHandler DataHandler;
    FByteStreamRegistrationHandler ByteStreamRegistrationHandler;
    FByteStreamHandler ByteStreamHandler;
    FRpcRegistrationHandler RpcRegistrationHandler;
    FRpcInvocationHandler RpcInvocationHandler;
    FRpcResultHandler RpcResultHandler;
    FPublishResultHandler PublishResultHandler;
};
