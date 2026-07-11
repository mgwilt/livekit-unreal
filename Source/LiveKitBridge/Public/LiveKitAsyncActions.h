#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "LiveKitTypes.h"
#include "LiveKitAsyncActions.generated.h"

class ULiveKitSubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnLiveKitAsyncSuccess);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnLiveKitAsyncFailure, const FLiveKitError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnLiveKitRpcSuccess, const FString&, ResponsePayload);

UCLASS()
class LIVEKITBRIDGE_API UConnectLiveKitAsyncAction : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    UFUNCTION(
        BlueprintCallable,
        Category="LiveKit",
        meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Connect to LiveKit"))
    static UConnectLiveKitAsyncAction* ConnectToLiveKit(
        UObject* WorldContextObject,
        const FString& ServerUrl,
        const FString& ParticipantToken,
        const FLiveKitConnectOptions& Options);

    virtual void Activate() override;

    UPROPERTY(BlueprintAssignable)
    FOnLiveKitAsyncSuccess Connected;

    UPROPERTY(BlueprintAssignable)
    FOnLiveKitAsyncFailure Failed;

private:
    UFUNCTION()
    void HandleStateChanged(ELiveKitConnectionState State);

    UFUNCTION()
    void HandleError(const FLiveKitError& Error);

    void Finish();

    UPROPERTY()
    TObjectPtr<UObject> WorldContext;

    UPROPERTY()
    TObjectPtr<ULiveKitSubsystem> Subsystem;

    FString Url;
    FString Token;
    FLiveKitConnectOptions ConnectOptions;
    FLiveKitError LastError;
};

UCLASS()
class LIVEKITBRIDGE_API UDisconnectLiveKitAsyncAction : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    UFUNCTION(
        BlueprintCallable,
        Category="LiveKit",
        meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Disconnect from LiveKit"))
    static UDisconnectLiveKitAsyncAction* DisconnectFromLiveKit(UObject* WorldContextObject);

    virtual void Activate() override;

    UPROPERTY(BlueprintAssignable)
    FOnLiveKitAsyncSuccess Disconnected;

    UPROPERTY(BlueprintAssignable)
    FOnLiveKitAsyncFailure Failed;

private:
    UFUNCTION()
    void HandleStateChanged(ELiveKitConnectionState State);

    UFUNCTION()
    void HandleError(const FLiveKitError& Error);

    void Finish();

    UPROPERTY()
    TObjectPtr<UObject> WorldContext;

    UPROPERTY()
    TObjectPtr<ULiveKitSubsystem> Subsystem;

    FLiveKitError LastError;
};

UCLASS()
class LIVEKITBRIDGE_API UPerformLiveKitRpcAsyncAction : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    UFUNCTION(
        BlueprintCallable,
        Category="LiveKit|RPC",
        meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Perform LiveKit RPC"))
    static UPerformLiveKitRpcAsyncAction* PerformLiveKitRpc(
        UObject* WorldContextObject,
        const FString& DestinationIdentity,
        const FString& Method,
        const FString& Payload,
        float ResponseTimeoutSeconds = 15.0f,
        float MaxRoundTripLatencySeconds = 7.0f);

    virtual void Activate() override;

    UPROPERTY(BlueprintAssignable)
    FOnLiveKitRpcSuccess Succeeded;

    UPROPERTY(BlueprintAssignable)
    FOnLiveKitAsyncFailure Failed;

private:
    UFUNCTION()
    void HandleRpcResult(
        const FString& ResultRequestId,
        bool bSuccess,
        const FString& ResponsePayload,
        const FLiveKitError& Error);

    void Finish();

    UPROPERTY()
    TObjectPtr<UObject> WorldContext;

    UPROPERTY()
    TObjectPtr<ULiveKitSubsystem> Subsystem;

    FString Destination;
    FString RpcMethod;
    FString RpcPayload;
    FString RequestId;
    float ResponseTimeout = 15.0f;
    float MaxRoundTripLatency = 7.0f;
};
