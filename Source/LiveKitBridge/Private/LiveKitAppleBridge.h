#pragma once

#include "CoreMinimal.h"
#include "LiveKitPlatformBridge.h"

class FLiveKitAppleBridge final
    : public ILiveKitPlatformBridge
    , public TSharedFromThis<FLiveKitAppleBridge, ESPMode::ThreadSafe>
{
public:
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
    virtual ~FLiveKitAppleBridge() override;

    // Must be called immediately after MakeShared so asynchronous Apple SDK
    // callbacks can pin this bridge instead of retaining a raw pointer.
    void ActivateLifetimeGate();
    virtual void Shutdown() override;

    virtual bool IsSdkAvailable() const override;
    virtual FString GetBackendName() const override { return TEXT("Apple"); }
    virtual void Connect(
        const FString& ServerUrl,
        const FString& Token,
        bool bEnableMicrophone,
        bool bEnableVoiceProcessing) override;
    virtual void Disconnect() override;
    virtual void SetMicrophoneEnabled(bool bEnabled) override;
    virtual bool IsMicrophoneCapturing() const override;
    virtual bool ReadConnectionConfiguration(FString& Url, FString& Token, FString& Participant) override;
    virtual bool ConfigureAudioReceiver(const FString&, const FString&, FAudioHandler) override;
    virtual bool ConfigureVideoReceiver(const FString&, const FString&, FVideoHandler) override;
    void ConfigureRemoteTrack(void* Participant, void* Publication);
    virtual void PublishData(
        const FString& OperationId,
        const TArray<uint8>& Data,
        const FString& Topic,
        ELiveKitDataReliability Reliability,
        const TArray<FString>& DestinationIdentities) override;
    virtual bool RegisterByteStreamHandler(const FString& Topic) override;
    virtual void UnregisterByteStreamHandler(const FString& Topic) override;
    virtual bool RegisterRpcMethod(const FString& Method) override;
    virtual void UnregisterRpcMethod(const FString& Method) override;
    virtual void PerformRpc(
        const FString& RequestId,
        const FString& DestinationIdentity,
        const FString& Method,
        const FString& Payload,
        float ResponseTimeoutSeconds,
        float MaxRoundTripLatencySeconds) override;
    virtual void CompleteRpcInvocation(const FString& RequestId, const FString& ResponsePayload) override;
    virtual void FailRpcInvocation(
        const FString& RequestId,
        int32 ErrorCode,
        const FString& Message,
        const FString& Data) override;

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
    FString AudioParticipant;
    FString AudioTrack;
    FString VideoParticipant;
    FString VideoTrack;
    FVideoHandler VideoHandler;
    TAtomic<bool> bMicrophoneCapturing{false};
    TAtomic<uint64> MicrophoneRequest{0};
    TAtomic<bool> bVideoPending{false};
};
