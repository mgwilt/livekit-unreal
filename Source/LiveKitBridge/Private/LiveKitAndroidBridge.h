#pragma once
#include "HAL/Platform.h"
#if PLATFORM_ANDROID
#include "CoreMinimal.h"
#include "LiveKitPlatformBridge.h"
#include <jni.h>

class FLiveKitAndroidBridge final : public ILiveKitPlatformBridge,
    public TSharedFromThis<FLiveKitAndroidBridge>
{
public:
    FLiveKitAndroidBridge(FStateHandler, FErrorHandler, FParticipantHandler, FParticipantHandler,
        FDataHandler, FPublishResultHandler, FRpcResultHandler);
    virtual ~FLiveKitAndroidBridge() override;
    void ActivateLifetimeGate();
    virtual void Shutdown() override;
    virtual bool IsSdkAvailable() const override { return Object != nullptr; }
    virtual FString GetBackendName() const override { return TEXT("Android"); }
    virtual bool ReadConnectionConfiguration(FString&, FString&, FString&) override;
    virtual bool ConfigureVideoReceiver(const FString&, const FString&, FVideoHandler) override;
    void HandleVideo(int64, FLiveKitVideoFrame);
    virtual bool ConfigureAudioReceiver(const FString&, const FString&, FAudioHandler) override;
    virtual void Connect(const FString&, const FString&, bool, bool) override;
    virtual void Disconnect() override;
    virtual void SetMicrophoneEnabled(bool) override;
    virtual bool IsMicrophoneCapturing() const override { return bMicrophoneCapturing; }
    virtual void PublishData(const FString&, const TArray<uint8>&, const FString&,
        ELiveKitDataReliability, const TArray<FString>&) override;
    virtual bool RegisterByteStreamHandler(const FString&) override { return false; }
    virtual void UnregisterByteStreamHandler(const FString&) override {}
    virtual bool RegisterRpcMethod(const FString&) override { return false; }
    virtual void UnregisterRpcMethod(const FString&) override {}
    virtual void PerformRpc(const FString&, const FString&, const FString&, const FString&, float, float) override;
    virtual void CompleteRpcInvocation(const FString&, const FString&) override {}
    virtual void FailRpcInvocation(const FString&, int32, const FString&, const FString&) override {}
    void HandleEvent(int64, const FString&);
    void HandleAudio(int64, FLiveKitAudioFrame);
private:
    bool CheckException(JNIEnv*);
    jmethodID Method(JNIEnv*, const char*, const char*);
    jobject Object = nullptr;
    jclass Class = nullptr;
    int64 Handle = 0;
    int64 Generation = 0;
    uint64 SampleIndex = 0;
    bool bMicrophoneCapturing = false;
    FString Participant;
    FString Track;
    FAudioHandler Audio;
    FVideoHandler Video;
    FStateHandler State;
    FErrorHandler Error;
    FParticipantHandler Joined;
    FParticipantHandler Left;
    FDataHandler Data;
    FPublishResultHandler Published;
    FRpcResultHandler RpcResult;
};
#endif
