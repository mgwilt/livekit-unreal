#include "LiveKitSubsystem.h"
#include "../LiveKitPlatformBridge.h"
bool ULiveKitSubsystem::ConfigureVideoReceiver(const FString& Participant, const FString& TrackName)
{
    if (!Bridge || ConnectionState != ELiveKitConnectionState::Disconnected) return false;
    TWeakObjectPtr<ULiveKitSubsystem> WeakThis(this);
    return Bridge->ConfigureVideoReceiver(Participant, TrackName, [WeakThis](const FLiveKitVideoFrame& Frame) {
        check(IsInGameThread());
        if (WeakThis.IsValid() && Frame.IsValid()) WeakThis->OnVideoReceived.Broadcast(Frame);
    });
}
bool ULiveKitSubsystem::StartVideoPublisher(int32 W, int32 H, int32 Fps, int32 Bitrate)
{
    return Bridge && ConnectionState == ELiveKitConnectionState::Connected && Bridge->StartVideoPublisher(W, H, Fps, Bitrate);
}
bool ULiveKitSubsystem::PublishVideoFrame(const FLiveKitVideoFrame& Frame)
{
    return Bridge && ConnectionState == ELiveKitConnectionState::Connected && Frame.IsValid() && Bridge->PublishVideoFrame(Frame);
}
bool ULiveKitSubsystem::PublishOutputAudio(const FLiveKitAudioFrame& Frame)
{
    return Bridge && ConnectionState == ELiveKitConnectionState::Connected && Bridge->PublishOutputAudio(Frame);
}
