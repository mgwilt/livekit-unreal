#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include <atomic>
#include "LiveKitPlatformBridge.h"

class FLiveKitWindowsBridge final
    : public ILiveKitPlatformBridge
    , public TSharedFromThis<FLiveKitWindowsBridge, ESPMode::ThreadSafe>
{
public:
    FLiveKitWindowsBridge(
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
    virtual bool ConfigureAudioReceiver(const FString& Participant, const FString& TrackName, FAudioHandler Handler) override;
    virtual bool ConfigureVideoReceiver(const FString&, const FString&, FVideoHandler) override;
    virtual bool StartVideoPublisher(int32, int32, int32, int32) override;
    virtual bool PublishVideoFrame(const FLiveKitVideoFrame&) override;
    virtual bool PublishOutputAudio(const FLiveKitAudioFrame&) override;
    void TickVideo();
    FString VideoParticipant, VideoTrackName;
    FVideoHandler VideoHandler;
    TAtomic<bool> VideoPending{false};
    std::atomic<int32> AudioPending{0};
    bool TickAudio(float DeltaTime);
    FTSTicker::FDelegateHandle AudioTicker;
    FString AudioParticipant;
    FString AudioTrackName;
    FAudioHandler AudioHandler;

    virtual ~FLiveKitWindowsBridge() override;

    /** Called by the factory after MakeShared so tasks can pin this object. */
    void ActivateLifetimeGate();

    virtual void Shutdown() override;
    virtual bool IsSdkAvailable() const override;
    virtual FString GetBackendName() const override { return PLATFORM_LINUX ? TEXT("Linux") : TEXT("Windows"); }
    virtual void Connect(
        const FString& ServerUrl,
        const FString& Token,
        bool bEnableMicrophone,
        bool bEnableVoiceProcessing) override;
    virtual void Disconnect() override;
    virtual void SetMicrophoneEnabled(bool bEnabled) override;
    virtual bool IsMicrophoneCapturing() const override { return bMicrophoneCapturing.Load(); }
    TAtomic<bool> bMicrophoneCapturing{false};
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

private:
    bool DispatchControl(TFunction<void(FLiveKitWindowsBridge&)> Work);
    bool DispatchByteStream(TFunction<void(FLiveKitWindowsBridge&)> Work);
    void ReportUnavailable(const FString& Operation = FString()) const;

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
