#pragma once

#include "CoreMinimal.h"
#include "LiveKitTypes.generated.h"

UENUM(BlueprintType)
enum class ELiveKitConnectionState : uint8
{
    Disconnected,
    Connecting,
    Connected,
    Reconnecting,
    Disconnecting,
    Failed
};

UENUM(BlueprintType)
enum class ELiveKitDataReliability : uint8
{
    Lossy,
    Reliable
};

USTRUCT(BlueprintType)
struct LIVEKITBRIDGE_API FLiveKitConnectOptions
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LiveKit")
    bool bEnableMicrophone = true;
};

USTRUCT(BlueprintType)
struct LIVEKITBRIDGE_API FLiveKitParticipantInfo
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="LiveKit")
    FString Identity;

    UPROPERTY(BlueprintReadOnly, Category="LiveKit")
    FString Sid;

    UPROPERTY(BlueprintReadOnly, Category="LiveKit")
    FString Name;

    UPROPERTY(BlueprintReadOnly, Category="LiveKit")
    FString Metadata;

    UPROPERTY(BlueprintReadOnly, Category="LiveKit")
    bool bIsSpeaking = false;

    UPROPERTY(BlueprintReadOnly, Category="LiveKit")
    bool bIsAgent = false;
};

USTRUCT(BlueprintType)
struct LIVEKITBRIDGE_API FLiveKitDataMessage
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="LiveKit")
    FString SenderIdentity;

    UPROPERTY(BlueprintReadOnly, Category="LiveKit")
    FString Topic;

    UPROPERTY(BlueprintReadOnly, Category="LiveKit")
    TArray<uint8> Data;

    FString AsString() const;
};

USTRUCT(BlueprintType)
struct LIVEKITBRIDGE_API FLiveKitRpcInvocation
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="LiveKit")
    FString RequestId;

    UPROPERTY(BlueprintReadOnly, Category="LiveKit")
    FString CallerIdentity;

    UPROPERTY(BlueprintReadOnly, Category="LiveKit")
    FString Method;

    UPROPERTY(BlueprintReadOnly, Category="LiveKit")
    FString Payload;

    UPROPERTY(BlueprintReadOnly, Category="LiveKit")
    float ResponseTimeoutSeconds = 10.0f;
};

USTRUCT(BlueprintType)
struct LIVEKITBRIDGE_API FLiveKitError
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="LiveKit")
    FString Code;

    UPROPERTY(BlueprintReadOnly, Category="LiveKit")
    FString Message;

    UPROPERTY(BlueprintReadOnly, Category="LiveKit")
    FString Data;

    bool IsSet() const { return !Code.IsEmpty() || !Message.IsEmpty(); }
};
