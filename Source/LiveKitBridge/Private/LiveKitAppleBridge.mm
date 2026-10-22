#include "HAL/Platform.h"

#if PLATFORM_MAC || PLATFORM_IOS

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

- (void)room:(Room*)room participant:(RemoteParticipant*)participant didSubscribeTrack:(RemoteTrackPublication*)publication
{
    if (FSharedLiveKitBridge Bridge = Owner.Pin())
    {
        Bridge->ConfigureRemoteTrack((__bridge void*)participant, (__bridge void*)publication);
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
    LiveKitUnrealVideoReceiver* VideoReceiver = nil;
    RemoteVideoTrack* VideoTrackInstance = nil;
    TAtomic<uint64> RoomGeneration{0};
    TSet<FString> RegisteredByteStreamTopics;
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
    FByteStreamRegistrationHandler InByteStreamRegistrationHandler,
    FByteStreamHandler InByteStreamHandler,
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
    , ByteStreamRegistrationHandler(MoveTemp(InByteStreamRegistrationHandler))
    , ByteStreamHandler(MoveTemp(InByteStreamHandler))
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
    bMicrophoneCapturing.Store(false);
    ++MicrophoneRequest;
#if WITH_LIVEKIT_APPLE
    if (!Implementation || Implementation->bShuttingDown.Exchange(true))
    {
        return;
    }

    Implementation->Delegate->Owner.Reset();
    [Implementation->VideoTrackInstance removeVideoRenderer:Implementation->VideoReceiver];
    Implementation->VideoTrackInstance = nil;
    Implementation->VideoReceiver = nil;
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
    [Implementation->VideoTrackInstance removeVideoRenderer:Implementation->VideoReceiver];
    Implementation->VideoTrackInstance = nil;
    Implementation->SwiftFacade = nil;
    ++Implementation->RoomGeneration;
    Implementation->RoomInstance = [[Room alloc]
        initWithDelegate:Implementation->Delegate
        connectOptions:nil
        roomOptions:nil];
    Implementation->SwiftFacade = [[LiveKitUnrealSwiftFacade alloc]
        initWithRoom:Implementation->RoomInstance];

    const TArray<FString> ByteStreamTopics = Implementation->RegisteredByteStreamTopics.Array();
    for (const FString& Topic : ByteStreamTopics)
    {
        RegisterByteStreamHandler(Topic);
    }

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

void FLiveKitAppleBridge::Connect(
    const FString& ServerUrl,
    const FString& Token,
    bool bEnableMicrophone,
    bool bEnableVoiceProcessing)
{
#if WITH_LIVEKIT_APPLE
    NotifyState(ELiveKitConnectionState::Connecting);
    NSError* VoiceProcessingError = nil;
    if (![Implementation->SwiftFacade
            setVoiceProcessingEnabled:bEnableVoiceProcessing
            error:&VoiceProcessingError])
    {
        NotifyError(MakeLiveKitError(TEXT("audio_configuration_failed"), VoiceProcessingError));
        NotifyState(ELiveKitConnectionState::Failed);
        return;
    }
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
                    if (bEnableMicrophone) GameThreadBridge->SetMicrophoneEnabled(true);
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
    bMicrophoneCapturing.Store(false);
    ++MicrophoneRequest;
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
    if (bEnabled && (Implementation->bManualDisconnectInProgress.Load() ||
        Implementation->bShuttingDown.Load() || !Implementation->RoomInstance ||
        Implementation->RoomInstance.connectionState != ConnectionStateConnected)) return;
    const uint64 Request = ++MicrophoneRequest;
    const uint64 Generation = Implementation->RoomGeneration.Load();
    FWeakLiveKitBridge WeakBridge = AsShared();
    [Implementation->RoomInstance.localParticipant
        setMicrophoneWithEnabled:bEnabled
        captureOptions:nil
        publishOptions:nil
        completionHandler:^(LocalTrackPublication* publication, NSError* error)
        {
            FSharedLiveKitBridge Bridge = WeakBridge.Pin();
            if (!Bridge || Bridge->MicrophoneRequest.Load() != Request ||
                Bridge->Implementation->RoomGeneration.Load() != Generation ||
                Bridge->Implementation->bManualDisconnectInProgress.Load() ||
                Bridge->Implementation->bShuttingDown.Load()) return;
            if (error)
            {
                Bridge->NotifyError(MakeLiveKitError(TEXT("microphone_failed"), error));
            }
            else
            {
                Bridge->bMicrophoneCapturing.Store(bEnabled);
                UE_LOG(LogTemp, Display, TEXT("LiveKit Apple: microphone %s"), bEnabled ? TEXT("on") : TEXT("off"));
            }
        }];
#endif
}

bool FLiveKitAppleBridge::IsMicrophoneCapturing() const
{
    return bMicrophoneCapturing.Load();
}

bool FLiveKitAppleBridge::ReadConnectionConfiguration(FString& Url, FString& Token, FString& Participant)
{
#if WITH_LIVEKIT_APPLE
    NSString* Documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSData* Data = [NSData dataWithContentsOfFile:[Documents stringByAppendingPathComponent:@"embi-livekit.json"]];
    if (!Data) return false;
    id Value = [NSJSONSerialization JSONObjectWithData:Data options:0 error:nil];
    if (![Value isKindOfClass:[NSDictionary class]]) return false;
    NSDictionary* Configuration = Value;
    for (NSString* Key in @[@"url", @"token", @"participant"])
        if (![Configuration[Key] isKindOfClass:[NSString class]] || [Configuration[Key] length] == 0) return false;
    Url = FromNSString(Configuration[@"url"]);
    Token = FromNSString(Configuration[@"token"]);
    Participant = FromNSString(Configuration[@"participant"]);
    return true;
#else
    return false;
#endif
}

bool FLiveKitAppleBridge::ConfigureAudioReceiver(const FString& Participant, const FString& Track, FAudioHandler)
{
    // Apple plays the selected track through its native audio session.
    AudioParticipant = Participant;
    AudioTrack = Track;
    return IsSdkAvailable();
}

bool FLiveKitAppleBridge::ConfigureVideoReceiver(const FString& Participant, const FString& Track, FVideoHandler Handler)
{
    VideoParticipant = Participant;
    VideoTrack = Track;
    VideoHandler = MoveTemp(Handler);
#if WITH_LIVEKIT_APPLE
    FWeakLiveKitBridge WeakBridge = AsShared();
    Implementation->VideoReceiver = [[LiveKitUnrealVideoReceiver alloc] initWithHandler:
        ^(NSData* Bytes, NSInteger Width, NSInteger Height, int64_t Timestamp)
        {
            FSharedLiveKitBridge Bridge = WeakBridge.Pin();
            if (!Bridge || Bridge->bVideoPending.Exchange(true)) return;
            FLiveKitVideoFrame Frame;
            Frame.Width = Width; Frame.Height = Height; Frame.TimestampMicroseconds = Timestamp;
            Frame.Pixels.Append(static_cast<const uint8*>(Bytes.bytes), static_cast<int32>(Bytes.length));
            const uint64 Generation = Bridge->Implementation->RoomGeneration.Load();
            AsyncTask(ENamedThreads::GameThread, [WeakBridge, Generation, Frame = MoveTemp(Frame)]()
            {
                if (FSharedLiveKitBridge Current = WeakBridge.Pin())
                {
                    Current->bVideoPending.Store(false);
                    if (Current->Implementation->RoomGeneration.Load() == Generation && Current->VideoHandler)
                        Current->VideoHandler(Frame);
                }
            });
        }];
#endif
    return IsSdkAvailable();
}

void FLiveKitAppleBridge::ConfigureRemoteTrack(void* ParticipantValue, void* PublicationValue)
{
#if WITH_LIVEKIT_APPLE
    RemoteParticipant* Participant = (__bridge RemoteParticipant*)ParticipantValue;
    RemoteTrackPublication* Publication = (__bridge RemoteTrackPublication*)PublicationValue;
    const FString Identity = FromNSString(Participant.identity.stringValue);
    const FString Name = FromNSString(Publication.name);
    if ([Publication.track isKindOfClass:[RemoteAudioTrack class]])
    {
        RemoteAudioTrack* Track = (RemoteAudioTrack*)Publication.track;
        Track.volume = Identity == AudioParticipant && Name == AudioTrack ? 1.0 : 0.0;
        if (Track.volume > 0)
            UE_LOG(LogTemp, Display, TEXT("LiveKit Apple: selected audio %s/%s"), *Identity, *Name);
    }
    if ([Publication.track isKindOfClass:[RemoteVideoTrack class]] &&
        Identity == VideoParticipant && Name == VideoTrack && Implementation->VideoReceiver)
    {
        [Implementation->VideoTrackInstance removeVideoRenderer:Implementation->VideoReceiver];
        Implementation->VideoTrackInstance = (RemoteVideoTrack*)Publication.track;
        [Implementation->VideoTrackInstance addVideoRenderer:Implementation->VideoReceiver];
        UE_LOG(LogTemp, Display, TEXT("LiveKit Apple: selected video %s/%s"), *Identity, *Name);
    }
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
    // Objective-C completion blocks can outlive this call. A block capture of a
    // C++ reference parameter preserves the reference rather than the FString,
    // so keep an owned value for the eventual result callback.
    const FString OwnedOperationId = OperationId;
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
                    OwnedOperationId,
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

bool FLiveKitAppleBridge::RegisterByteStreamHandler(const FString& Topic)
{
#if WITH_LIVEKIT_APPLE
    if (Topic.IsEmpty() || !Implementation->SwiftFacade)
    {
        return false;
    }

    Implementation->RegisteredByteStreamTopics.Add(Topic);
    FWeakLiveKitBridge WeakBridge = AsShared();
    const FString RegisteredTopic = Topic;
    const uint64 RegistrationGeneration = Implementation->RoomGeneration.Load();
    [Implementation->SwiftFacade
        registerByteStreamHandler:ToNSString(Topic)
        maximumBytes:LiveKitLimits::MaxIncomingByteStreamBytes
        streamHandler:^(NSString* senderIdentity,
                        NSString* streamId,
                        NSString* streamTopic,
                        NSString* name,
                        NSString* mimeType,
                        NSDictionary<NSString*, NSString*>* attributes,
                        NSData* data,
                        NSError* error)
        {
            FSharedLiveKitBridge Bridge = WeakBridge.Pin();
            if (!Bridge)
            {
                return;
            }

            if (error)
            {
                const bool bTooLarge =
                    [error.domain isEqualToString:@"LiveKitForUnreal.ByteStream"] &&
                    error.code == 7;
                Bridge->NotifyError(MakeLiveKitError(
                    bTooLarge ? TEXT("byte_stream_too_large") : TEXT("byte_stream_read_failed"),
                    error));
                return;
            }

            FLiveKitByteStream Stream;
            Stream.SenderIdentity = FromNSString(senderIdentity);
            Stream.StreamId = FromNSString(streamId);
            Stream.Topic = FromNSString(streamTopic);
            Stream.Name = FromNSString(name);
            Stream.MimeType = FromNSString(mimeType);
            for (NSString* Key in attributes)
            {
                Stream.Attributes.Add(FromNSString(Key), FromNSString(attributes[Key]));
            }
            if (data.length > 0)
            {
                Stream.Data.Append(
                    static_cast<const uint8*>(data.bytes),
                    static_cast<int32>(data.length));
            }
            Bridge->NotifyByteStream(Stream);
        }
        completion:^(NSError* error)
        {
            const bool bSuccess = error == nil;
            const FLiveKitError Result = error
                ? MakeLiveKitError(TEXT("byte_stream_registration_failed"), error)
                : FLiveKitError();
            AsyncTask(
                ENamedThreads::GameThread,
                [WeakBridge, RegisteredTopic, RegistrationGeneration, bSuccess, Result]()
            {
                if (FSharedLiveKitBridge Bridge = WeakBridge.Pin())
                {
                    if (Bridge->Implementation->RoomGeneration.Load() != RegistrationGeneration)
                    {
                        return;
                    }
                    if (!bSuccess)
                    {
                        Bridge->Implementation->RegisteredByteStreamTopics.Remove(RegisteredTopic);
                    }
                    Bridge->NotifyByteStreamRegistrationResult(
                        RegisteredTopic,
                        bSuccess,
                        Result);
                }
            });
        }];
    return true;
#else
    return false;
#endif
}

void FLiveKitAppleBridge::UnregisterByteStreamHandler(const FString& Topic)
{
#if WITH_LIVEKIT_APPLE
    Implementation->RegisteredByteStreamTopics.Remove(Topic);
    [Implementation->SwiftFacade unregisterByteStreamHandler:ToNSString(Topic) completion:^{}];
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
    // The SDK completes asynchronously. Capturing the const reference parameter
    // directly leaves the block pointing at the caller's expired stack frame.
    const FString OwnedRequestId = RequestId;
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
                    OwnedRequestId,
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

void FLiveKitAppleBridge::NotifyByteStreamRegistrationResult(
    const FString& Topic,
    bool bSuccess,
    const FLiveKitError& Error)
{
    if (ByteStreamRegistrationHandler)
    {
        ByteStreamRegistrationHandler(Topic, bSuccess, Error);
    }
}

void FLiveKitAppleBridge::NotifyByteStream(const FLiveKitByteStream& Stream)
{
    if (ByteStreamHandler)
    {
        ByteStreamHandler(Stream);
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

#endif // PLATFORM_MAC || PLATFORM_IOS
