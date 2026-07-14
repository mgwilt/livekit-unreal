#pragma once

#include "LiveKitPlatformBridge.h"

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
    ILiveKitPlatformBridge::FPublishResultHandler PublishResultHandler);
