#include "LiveKitAppleBridge.h"

#include "Async/Async.h"

#if WITH_LIVEKIT_APPLE
// Carbon still declares a legacy FVector. Rename it only while the generated
// Swift headers pull in Foundation/CoreServices so it cannot collide with UE's FVector.
#define FVector AppleLegacyFVector
#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <Network/Network.h>
#if PLATFORM_MAC
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import "LiveKit-Swift-macOS.h"
#import "LiveKitUnrealFacade-Swift-macOS.h"
#else
#import <ReplayKit/ReplayKit.h>
#import <UIKit/UIKit.h>
#import "LiveKit-Swift-iOS.h"
#import "LiveKitUnrealFacade-Swift-iOS.h"
#endif
#undef FVector

namespace
{
    using FSharedLiveKitBridge = TSharedPtr<FLiveKitAppleBridge, ESPMode::ThreadSafe>;
    using FWeakLiveKitBridge = TWeakPtr<FLiveKitAppleBridge, ESPMode::ThreadSafe>;

    FString FromNSString(NSString* Value)
    {
        return Value ? FString(UTF8_TO_TCHAR(Value.UTF8String)) : FString();
    }

    NSString* ToNSString(const FString& Value)
    {
        return [NSString stringWithUTF8String:TCHAR_TO_UTF8(*Value)];
    }

    FLiveKitError MakeLiveKitError(const FString& Code, NSError* Error)
    {
        FLiveKitError Result;
        Result.Code = Code;
        if (Error)
        {
            Result.Message = FromNSString(Error.localizedDescription);
            Result.Data = FString::Printf(
                TEXT("%s:%ld"),
                *FromNSString(Error.domain),
                static_cast<long>(Error.code));
        }
        return Result;
    }

    FLiveKitError MakeRpcError(LiveKitUnrealRpcError* Error)
    {
        FLiveKitError Result;
        if (!Error)
        {
            return Result;
        }

        const FString Domain = FromNSString(Error.domain);
        Result.Code = Domain == TEXT("LiveKit.RpcError")
            ? FString::FromInt(static_cast<int32>(Error.code))
            : TEXT("rpc_failed");
        Result.Message = FromNSString(Error.message);
        Result.Data = Domain == TEXT("LiveKit.RpcError")
            ? FromNSString(Error.data)
            : FString::Printf(TEXT("%s:%ld"), *Domain, static_cast<long>(Error.code));
        return Result;
    }

    FLiveKitParticipantInfo MakeParticipantInfo(Participant* ParticipantInstance)
    {
        FLiveKitParticipantInfo Result;
        if (!ParticipantInstance)
        {
            return Result;
        }

        Result.Identity = ParticipantInstance.identity
            ? FromNSString(ParticipantInstance.identity.stringValue)
            : FString();
        Result.Sid = ParticipantInstance.sid
            ? FromNSString(ParticipantInstance.sid.stringValue)
            : FString();
        Result.Name = FromNSString(ParticipantInstance.name);
        Result.Metadata = FromNSString(ParticipantInstance.metadata);
        Result.bIsSpeaking = ParticipantInstance.isSpeaking;
        Result.bIsAgent = ParticipantInstance.isAgent;
        return Result;
    }
}

@interface LiveKitUnrealRoomDelegate : NSObject <RoomDelegate>
{
@public
    FWeakLiveKitBridge Owner;
}
@end

@implementation LiveKitUnrealRoomDelegate

- (void)roomDidConnect:(Room*)room
{
    if (FSharedLiveKitBridge Bridge = Owner.Pin())
    {
        Bridge->NotifyState(ELiveKitConnectionState::Connected);
    }
}

- (void)roomIsReconnecting:(Room*)room
{
    if (FSharedLiveKitBridge Bridge = Owner.Pin())
    {
        Bridge->NotifyState(ELiveKitConnectionState::Reconnecting);
    }
}

- (void)room:(Room*)room didStartReconnectWithMode:(enum ReconnectMode)reconnectMode
{
    if (FSharedLiveKitBridge Bridge = Owner.Pin())
    {
        Bridge->NotifyState(ELiveKitConnectionState::Reconnecting);
    }
}

- (void)roomDidReconnect:(Room*)room
{
    if (FSharedLiveKitBridge Bridge = Owner.Pin())
    {
        Bridge->NotifyState(ELiveKitConnectionState::Connected);
    }
}

- (void)room:(Room*)room didCompleteReconnectWithMode:(enum ReconnectMode)reconnectMode
{
    if (FSharedLiveKitBridge Bridge = Owner.Pin())
    {
        Bridge->NotifyState(ELiveKitConnectionState::Connected);
    }
}

- (void)room:(Room*)room didFailToConnectWithError:(LiveKitError*)error
{
    if (FSharedLiveKitBridge Bridge = Owner.Pin())
    {
        Bridge->NotifyError(MakeLiveKitError(TEXT("connect_failed"), error));
        Bridge->NotifyState(ELiveKitConnectionState::Failed);
    }
}

- (void)room:(Room*)room didDisconnectWithError:(LiveKitError*)error
{
    FSharedLiveKitBridge Bridge = Owner.Pin();
    if (!Bridge)
    {
        return;
    }

    if (error)
    {
        Bridge->NotifyError(MakeLiveKitError(TEXT("room_disconnected"), error));
    }
    if (!Bridge->IsManualDisconnectInProgress())
    {
        Bridge->NotifyState(ELiveKitConnectionState::Disconnected);
    }
}

- (void)room:(Room*)room participantDidConnect:(RemoteParticipant*)participant
{
    if (FSharedLiveKitBridge Bridge = Owner.Pin())
    {
        Bridge->NotifyParticipantConnected(MakeParticipantInfo(participant));
    }
}

- (void)room:(Room*)room participantDidDisconnect:(RemoteParticipant*)participant
{
    if (FSharedLiveKitBridge Bridge = Owner.Pin())
    {
        Bridge->NotifyParticipantDisconnected(MakeParticipantInfo(participant));
    }
}

- (void)room:(Room*)room didUpdateSpeakingParticipants:(NSArray<Participant*>*)participants
{
    FSharedLiveKitBridge Bridge = Owner.Pin();
    if (!Bridge)
    {
        return;
    }

    for (ParticipantIdentity* identity in room.remoteParticipants)
    {
        RemoteParticipant* participant = room.remoteParticipants[identity];
        Bridge->NotifyParticipantSpeaking(
            MakeParticipantInfo(participant),
            participant.isSpeaking);
    }
}

- (void)room:(Room*)room
    participant:(RemoteParticipant*)participant
    didReceiveData:(NSData*)data
    forTopic:(NSString*)topic
    encryptionType:(enum EncryptionType)encryptionType
{
    FSharedLiveKitBridge Bridge = Owner.Pin();
    if (!Bridge)
    {
        return;
    }

    FLiveKitDataMessage Message;
    Message.SenderIdentity = participant && participant.identity
        ? FromNSString(participant.identity.stringValue)
        : FString();
    Message.Topic = FromNSString(topic);
    if (data.length > 0)
    {
        Message.Data.Append(static_cast<const uint8*>(data.bytes), static_cast<int32>(data.length));
    }
    Bridge->NotifyData(Message);
}

@end
#endif

struct FLiveKitAppleBridge::FImplementation
{
#if WITH_LIVEKIT_APPLE
    Room* RoomInstance = nil;
    LiveKitUnrealRoomDelegate* Delegate = nil;
    LiveKitUnrealSwiftFacade* SwiftFacade = nil;
    TSet<FString> RegisteredRpcMethods;
    TAtomic<bool> bManualDisconnectInProgress{false};
    TAtomic<bool> bShuttingDown{false};
#endif
};

FLiveKitAppleBridge::FLiveKitAppleBridge(
    FStateHandler InStateHandler,
    FErrorHandler InErrorHandler,
    FParticipantHandler InParticipantConnectedHandler,
    FParticipantHandler InParticipantDisconnectedHandler,
    FSpeakingHandler InSpeakingHandler,
    FDataHandler InDataHandler,
    FRpcRegistrationHandler InRpcRegistrationHandler,
    FRpcInvocationHandler InRpcInvocationHandler,
    FRpcResultHandler InRpcResultHandler,
    FPublishResultHandler InPublishResultHandler)
    : Implementation(MakeUnique<FImplementation>())
    , StateHandler(MoveTemp(InStateHandler))
    , ErrorHandler(MoveTemp(InErrorHandler))
    , ParticipantConnectedHandler(MoveTemp(InParticipantConnectedHandler))
    , ParticipantDisconnectedHandler(MoveTemp(InParticipantDisconnectedHandler))
    , SpeakingHandler(MoveTemp(InSpeakingHandler))
    , DataHandler(MoveTemp(InDataHandler))
    , RpcRegistrationHandler(MoveTemp(InRpcRegistrationHandler))
    , RpcInvocationHandler(MoveTemp(InRpcInvocationHandler))
    , RpcResultHandler(MoveTemp(InRpcResultHandler))
    , PublishResultHandler(MoveTemp(InPublishResultHandler))
{
#if WITH_LIVEKIT_APPLE
    Implementation->Delegate = [[LiveKitUnrealRoomDelegate alloc] init];
    CreateRoom();
#endif
}

FLiveKitAppleBridge::~FLiveKitAppleBridge()
{
    Shutdown();
}

void FLiveKitAppleBridge::Shutdown()
{
#if WITH_LIVEKIT_APPLE
    if (!Implementation || Implementation->bShuttingDown.Exchange(true))
    {
        return;
    }

    Implementation->Delegate->Owner.Reset();
    [Implementation->SwiftFacade shutdown];
    Implementation->SwiftFacade = nil;

    Room* RoomToDisconnect = Implementation->RoomInstance;
    Implementation->RoomInstance = nil;
    Implementation->Delegate = nil;
    if (RoomToDisconnect && RoomToDisconnect.connectionState != ConnectionStateDisconnected)
    {
        // Teardown must explicitly leave the room, but it must never call back
        // into a UGameInstanceSubsystem that is being destroyed.
        [RoomToDisconnect disconnectWithCompletionHandler:^{}];
    }
#endif
}

void FLiveKitAppleBridge::ActivateLifetimeGate()
{
#if WITH_LIVEKIT_APPLE
    Implementation->Delegate->Owner = AsShared();
#endif
}

void FLiveKitAppleBridge::CreateRoom()
{
#if WITH_LIVEKIT_APPLE
    if (Implementation->bShuttingDown.Load())
    {
        return;
    }

    [Implementation->SwiftFacade shutdown];
    Implementation->SwiftFacade = nil;
    Implementation->RoomInstance = [[Room alloc]
        initWithDelegate:Implementation->Delegate
        connectOptions:nil
        roomOptions:nil];
    Implementation->SwiftFacade = [[LiveKitUnrealSwiftFacade alloc]
        initWithRoom:Implementation->RoomInstance];

    const TArray<FString> Methods = Implementation->RegisteredRpcMethods.Array();
    for (const FString& Method : Methods)
    {
        RegisterRpcMethod(Method);
    }
#endif
}

bool FLiveKitAppleBridge::IsSdkAvailable() const
{
#if WITH_LIVEKIT_APPLE
    return true;
#else
    return false;
#endif
}

void FLiveKitAppleBridge::Connect(const FString& ServerUrl, const FString& Token, bool bEnableMicrophone)
{
#if WITH_LIVEKIT_APPLE
    NotifyState(ELiveKitConnectionState::Connecting);
    FWeakLiveKitBridge WeakBridge = AsShared();
    [Implementation->RoomInstance
        connectWithUrl:ToNSString(ServerUrl)
        token:ToNSString(Token)
        connectOptions:nil
        roomOptions:nil
        completionHandler:^(NSError* error)
        {
            FSharedLiveKitBridge Bridge = WeakBridge.Pin();
            if (!Bridge)
            {
                return;
            }
            if (error)
            {
                Bridge->NotifyError(MakeLiveKitError(TEXT("connect_failed"), error));
                Bridge->NotifyState(ELiveKitConnectionState::Failed);
                return;
            }
            AsyncTask(ENamedThreads::GameThread, [WeakBridge, bEnableMicrophone]()
            {
                if (FSharedLiveKitBridge GameThreadBridge = WeakBridge.Pin())
                {
                    GameThreadBridge->SetMicrophoneEnabled(bEnableMicrophone);
                }
            });
        }];
#else
    FLiveKitError Error;
    Error.Code = TEXT("sdk_unavailable");
    Error.Message = TEXT("Run the plugin's Scripts/fetch-livekit-apple.sh before building an Apple target.");
    NotifyError(Error);
    NotifyState(ELiveKitConnectionState::Failed);
#endif
}

void FLiveKitAppleBridge::Disconnect()
{
#if WITH_LIVEKIT_APPLE
    if (Implementation->bShuttingDown.Load() ||
        Implementation->bManualDisconnectInProgress.Exchange(true))
    {
        return;
    }

    if (!Implementation->RoomInstance || Implementation->RoomInstance.connectionState == ConnectionStateDisconnected)
    {
        Implementation->bManualDisconnectInProgress.Store(false);
        NotifyState(ELiveKitConnectionState::Disconnected);
        return;
    }

    NotifyState(ELiveKitConnectionState::Disconnecting);
    FWeakLiveKitBridge WeakBridge = AsShared();
    [Implementation->RoomInstance disconnectWithCompletionHandler:^
    {
        AsyncTask(ENamedThreads::GameThread, [WeakBridge]()
        {
            if (FSharedLiveKitBridge Bridge = WeakBridge.Pin())
            {
                Bridge->CreateRoom();
                Bridge->Implementation->bManualDisconnectInProgress.Store(false);
                Bridge->NotifyState(ELiveKitConnectionState::Disconnected);
            }
        });
    }];
#else
    NotifyState(ELiveKitConnectionState::Disconnected);
#endif
}

void FLiveKitAppleBridge::SetMicrophoneEnabled(bool bEnabled)
{
#if WITH_LIVEKIT_APPLE
    FWeakLiveKitBridge WeakBridge = AsShared();
    [Implementation->RoomInstance.localParticipant
        setMicrophoneWithEnabled:bEnabled
        captureOptions:nil
        publishOptions:nil
        completionHandler:^(LocalTrackPublication* publication, NSError* error)
        {
            if (FSharedLiveKitBridge Bridge = WeakBridge.Pin(); Bridge && error)
            {
                Bridge->NotifyError(MakeLiveKitError(TEXT("microphone_failed"), error));
            }
        }];
#endif
}

void FLiveKitAppleBridge::PublishData(
    const FString& OperationId,
    const TArray<uint8>& Data,
    const FString& Topic,
    ELiveKitDataReliability Reliability,
    const TArray<FString>& DestinationIdentities)
{
#if WITH_LIVEKIT_APPLE
    NSMutableArray<ParticipantIdentity*>* Destinations = [NSMutableArray array];
    for (const FString& Identity : DestinationIdentities)
    {
        [Destinations addObject:[[ParticipantIdentity alloc] initFrom:ToNSString(Identity)]];
    }

    DataPublishOptions* Options = [[DataPublishOptions alloc]
        initWithName:nil
        destinationIdentities:Destinations
        topic:ToNSString(Topic)
        reliable:Reliability == ELiveKitDataReliability::Reliable];
    NSData* Payload = [NSData dataWithBytes:Data.GetData() length:Data.Num()];
    FWeakLiveKitBridge WeakBridge = AsShared();
    [Implementation->RoomInstance.localParticipant
        publishWithData:Payload
        options:Options
        completionHandler:^(NSError* error)
        {
            if (FSharedLiveKitBridge Bridge = WeakBridge.Pin())
            {
                Bridge->NotifyPublishResult(
                    OperationId,
                    error == nil,
                    error ? MakeLiveKitError(TEXT("publish_failed"), error) : FLiveKitError());
            }
        }];
#else
    FLiveKitError Error;
    Error.Code = TEXT("sdk_unavailable");
    Error.Message = TEXT("The LiveKit Apple SDK is unavailable.");
    NotifyPublishResult(OperationId, false, Error);
#endif
}

bool FLiveKitAppleBridge::RegisterRpcMethod(const FString& Method)
{
#if WITH_LIVEKIT_APPLE
    if (Method.IsEmpty() || !Implementation->SwiftFacade)
    {
        return false;
    }

    Implementation->RegisteredRpcMethods.Add(Method);
    FWeakLiveKitBridge WeakBridge = AsShared();
    const FString RegisteredMethod = Method;
    [Implementation->SwiftFacade
        registerRpcMethod:ToNSString(Method)
        invocationHandler:^(NSString* requestId,
                            NSString* callerIdentity,
                            NSString* method,
                            NSString* payload,
                            NSTimeInterval responseTimeout)
        {
            FLiveKitRpcInvocation Invocation;
            Invocation.RequestId = FromNSString(requestId);
            Invocation.CallerIdentity = FromNSString(callerIdentity);
            Invocation.Method = FromNSString(method);
            Invocation.Payload = FromNSString(payload);
            Invocation.ResponseTimeoutSeconds = static_cast<float>(responseTimeout);
            if (FSharedLiveKitBridge Bridge = WeakBridge.Pin())
            {
                Bridge->NotifyRpcInvocation(Invocation);
            }
        }
        completion:^(NSError* error)
        {
            if (FSharedLiveKitBridge Bridge = WeakBridge.Pin())
            {
                Bridge->NotifyRpcRegistrationResult(
                    RegisteredMethod,
                    error == nil,
                    error
                        ? MakeLiveKitError(TEXT("rpc_registration_failed"), error)
                        : FLiveKitError());
            }
        }];
    return true;
#else
    return false;
#endif
}

void FLiveKitAppleBridge::UnregisterRpcMethod(const FString& Method)
{
#if WITH_LIVEKIT_APPLE
    Implementation->RegisteredRpcMethods.Remove(Method);
    [Implementation->SwiftFacade unregisterRpcMethod:ToNSString(Method) completion:^{}];
#endif
}

void FLiveKitAppleBridge::PerformRpc(
    const FString& RequestId,
    const FString& DestinationIdentity,
    const FString& Method,
    const FString& Payload,
    float ResponseTimeoutSeconds,
    float MaxRoundTripLatencySeconds)
{
#if WITH_LIVEKIT_APPLE
    FWeakLiveKitBridge WeakBridge = AsShared();
    [Implementation->SwiftFacade
        performRpcWithDestinationIdentity:ToNSString(DestinationIdentity)
        method:ToNSString(Method)
        payload:ToNSString(Payload)
        responseTimeout:FMath::Max(1.0f, ResponseTimeoutSeconds)
        maxRoundTripLatency:FMath::Max(0.1f, MaxRoundTripLatencySeconds)
        completion:^(NSString* response, LiveKitUnrealRpcError* error)
        {
            if (FSharedLiveKitBridge Bridge = WeakBridge.Pin())
            {
                Bridge->NotifyRpcResult(
                    RequestId,
                    error == nil,
                    FromNSString(response),
                    error ? MakeRpcError(error) : FLiveKitError());
            }
        }];
#else
    FLiveKitError Error;
    Error.Code = TEXT("sdk_unavailable");
    Error.Message = TEXT("The LiveKit Apple SDK is unavailable.");
    NotifyRpcResult(RequestId, false, FString(), Error);
#endif
}

void FLiveKitAppleBridge::CompleteRpcInvocation(const FString& RequestId, const FString& ResponsePayload)
{
#if WITH_LIVEKIT_APPLE
    [Implementation->SwiftFacade
        completeRpcInvocation:ToNSString(RequestId)
        payload:ToNSString(ResponsePayload)];
#endif
}

void FLiveKitAppleBridge::FailRpcInvocation(
    const FString& RequestId,
    int32 ErrorCode,
    const FString& Message,
    const FString& Data)
{
#if WITH_LIVEKIT_APPLE
    [Implementation->SwiftFacade
        failRpcInvocation:ToNSString(RequestId)
        code:ErrorCode
        message:ToNSString(Message)
        data:ToNSString(Data)];
#endif
}

void FLiveKitAppleBridge::NotifyState(ELiveKitConnectionState State)
{
    if (StateHandler)
    {
        StateHandler(State);
    }
}

void FLiveKitAppleBridge::NotifyError(const FLiveKitError& Error)
{
    if (ErrorHandler)
    {
        ErrorHandler(Error);
    }
}

void FLiveKitAppleBridge::NotifyParticipantConnected(const FLiveKitParticipantInfo& Participant)
{
    if (ParticipantConnectedHandler)
    {
        ParticipantConnectedHandler(Participant);
    }
}

void FLiveKitAppleBridge::NotifyParticipantDisconnected(const FLiveKitParticipantInfo& Participant)
{
    if (ParticipantDisconnectedHandler)
    {
        ParticipantDisconnectedHandler(Participant);
    }
}

void FLiveKitAppleBridge::NotifyParticipantSpeaking(
    const FLiveKitParticipantInfo& Participant,
    bool bIsSpeaking)
{
    if (SpeakingHandler)
    {
        SpeakingHandler(Participant, bIsSpeaking);
    }
}

void FLiveKitAppleBridge::NotifyData(const FLiveKitDataMessage& Message)
{
    if (DataHandler)
    {
        DataHandler(Message);
    }
}

void FLiveKitAppleBridge::NotifyRpcRegistrationResult(
    const FString& Method,
    bool bSuccess,
    const FLiveKitError& Error)
{
    if (RpcRegistrationHandler)
    {
        RpcRegistrationHandler(Method, bSuccess, Error);
    }
}

void FLiveKitAppleBridge::NotifyRpcInvocation(const FLiveKitRpcInvocation& Invocation)
{
    if (RpcInvocationHandler)
    {
        RpcInvocationHandler(Invocation);
    }
}

void FLiveKitAppleBridge::NotifyRpcResult(
    const FString& RequestId,
    bool bSuccess,
    const FString& ResponsePayload,
    const FLiveKitError& Error)
{
    if (RpcResultHandler)
    {
        RpcResultHandler(RequestId, bSuccess, ResponsePayload, Error);
    }
}

void FLiveKitAppleBridge::NotifyPublishResult(
    const FString& OperationId,
    bool bSuccess,
    const FLiveKitError& Error)
{
    if (PublishResultHandler)
    {
        PublishResultHandler(OperationId, bSuccess, Error);
    }
}

bool FLiveKitAppleBridge::IsManualDisconnectInProgress() const
{
#if WITH_LIVEKIT_APPLE
    return Implementation->bManualDisconnectInProgress.Load();
#else
    return false;
#endif
}
