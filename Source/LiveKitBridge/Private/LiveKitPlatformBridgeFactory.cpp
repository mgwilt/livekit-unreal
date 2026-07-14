#include "LiveKitPlatformBridgeFactory.h"

#if PLATFORM_MAC || PLATFORM_IOS
#include "LiveKitAppleBridge.h"
#elif PLATFORM_WINDOWS
#include "LiveKitWindowsBridge.h"
#endif

TSharedPtr<ILiveKitPlatformBridge> CreateLiveKitPlatformBridge(
    ILiveKitPlatformBridge::FStateHandler StateHandler,
    ILiveKitPlatformBridge::FErrorHandler ErrorHandler,
    ILiveKitPlatformBridge::FParticipantHandler ParticipantConnectedHandler,
    ILiveKitPlatformBridge::FParticipantHandler ParticipantDisconnectedHandler,
    ILiveKitPlatformBridge::FSpeakingHandler SpeakingHandler,
    ILiveKitPlatformBridge::FDataHandler DataHandler,
    ILiveKitPlatformBridge::FByteStreamRegistrationHandler ByteStreamRegistrationHandler,
    ILiveKitPlatformBridge::FByteStreamHandler ByteStreamHandler,
    ILiveKitPlatformBridge::FRpcRegistrationHandler RpcRegistrationHandler,
    ILiveKitPlatformBridge::FRpcInvocationHandler RpcInvocationHandler,
    ILiveKitPlatformBridge::FRpcResultHandler RpcResultHandler,
    ILiveKitPlatformBridge::FPublishResultHandler PublishResultHandler)
{
#if PLATFORM_MAC || PLATFORM_IOS
    TSharedPtr<FLiveKitAppleBridge> Bridge = MakeShared<FLiveKitAppleBridge>(
        MoveTemp(StateHandler),
        MoveTemp(ErrorHandler),
        MoveTemp(ParticipantConnectedHandler),
        MoveTemp(ParticipantDisconnectedHandler),
        MoveTemp(SpeakingHandler),
        MoveTemp(DataHandler),
        MoveTemp(ByteStreamRegistrationHandler),
        MoveTemp(ByteStreamHandler),
        MoveTemp(RpcRegistrationHandler),
        MoveTemp(RpcInvocationHandler),
        MoveTemp(RpcResultHandler),
        MoveTemp(PublishResultHandler));
    Bridge->ActivateLifetimeGate();
    return Bridge;
#elif PLATFORM_WINDOWS
    TSharedPtr<FLiveKitWindowsBridge> Bridge = MakeShared<FLiveKitWindowsBridge>(
        MoveTemp(StateHandler),
        MoveTemp(ErrorHandler),
        MoveTemp(ParticipantConnectedHandler),
        MoveTemp(ParticipantDisconnectedHandler),
        MoveTemp(SpeakingHandler),
        MoveTemp(DataHandler),
        MoveTemp(ByteStreamRegistrationHandler),
        MoveTemp(ByteStreamHandler),
        MoveTemp(RpcRegistrationHandler),
        MoveTemp(RpcInvocationHandler),
        MoveTemp(RpcResultHandler),
        MoveTemp(PublishResultHandler));
    Bridge->ActivateLifetimeGate();
    return Bridge;
#else
    static_assert(PLATFORM_MAC || PLATFORM_IOS || PLATFORM_WINDOWS, "Unsupported LiveKitBridge platform");
    return nullptr;
#endif
}
