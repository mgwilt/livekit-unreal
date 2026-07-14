#pragma once

#include "CoreMinimal.h"
#include "LiveKitTypes.h"

/** Internal platform adapter used by ULiveKitSubsystem. */
class ILiveKitPlatformBridge
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
    using FRpcResultHandler =
        TFunction<void(const FString&, bool, const FString&, const FLiveKitError&)>;
    using FPublishResultHandler = TFunction<void(const FString&, bool, const FLiveKitError&)>;

    virtual ~ILiveKitPlatformBridge() = default;

    virtual void Shutdown() = 0;
    virtual bool IsSdkAvailable() const = 0;
    virtual FString GetBackendName() const = 0;
    virtual void Connect(
        const FString& ServerUrl,
        const FString& Token,
        bool bEnableMicrophone,
        bool bEnableVoiceProcessing) = 0;
    virtual void Disconnect() = 0;
    virtual void SetMicrophoneEnabled(bool bEnabled) = 0;
    virtual void PublishData(
        const FString& OperationId,
        const TArray<uint8>& Data,
        const FString& Topic,
        ELiveKitDataReliability Reliability,
        const TArray<FString>& DestinationIdentities) = 0;
    virtual bool RegisterByteStreamHandler(const FString& Topic) = 0;
    virtual void UnregisterByteStreamHandler(const FString& Topic) = 0;
    virtual bool RegisterRpcMethod(const FString& Method) = 0;
    virtual void UnregisterRpcMethod(const FString& Method) = 0;
    virtual void PerformRpc(
        const FString& RequestId,
        const FString& DestinationIdentity,
        const FString& Method,
        const FString& Payload,
        float ResponseTimeoutSeconds,
        float MaxRoundTripLatencySeconds) = 0;
    virtual void CompleteRpcInvocation(const FString& RequestId, const FString& ResponsePayload) = 0;
    virtual void FailRpcInvocation(
        const FString& RequestId,
        int32 ErrorCode,
        const FString& Message,
        const FString& Data) = 0;
};
