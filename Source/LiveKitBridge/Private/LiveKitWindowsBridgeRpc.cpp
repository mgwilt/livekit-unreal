#include "LiveKitWindowsBridgeInternal.h"

#include "Misc/ScopeExit.h"

using namespace LiveKitWindowsBridgeInternal;

#if WITH_LIVEKIT_WINDOWS

bool FLiveKitWindowsBridge::FImplementation::TryAdmitPendingRpc(
    const std::string& RequestId,
    const std::shared_ptr<FPendingRpc>& Pending,
    uint64 InConnectionGeneration)
{
    std::lock_guard Lock(PendingRpcMutex);
    if (!bRpcAdmissionOpen || RpcAdmissionGeneration != InConnectionGeneration ||
        bShuttingDown.load(std::memory_order_acquire))
    {
        return false;
    }
    return PendingRpcs.emplace(RequestId, Pending).second;
}

void FLiveKitWindowsBridge::FImplementation::HandleIncomingRpc(
    const FRpcCallbackContext& Registration,
    const LKUB_RpcInvocation& Data,
    std::string& OutResponse,
    bool& bOutFailed,
    uint32& OutErrorCode,
    std::string& OutErrorMessage,
    std::string& OutErrorData)
{
    bOutFailed = true;
    OutErrorCode = RpcApplicationError;
    OutErrorMessage = "The RPC handler is unavailable.";
    OutErrorData.clear();

    const TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe> Pinned = Owner.Pin();
    if (!Pinned ||
        !IsCurrentRoom(Registration.Room, Registration.ConnectionGeneration) ||
        !IsRpcRegistrationCurrent(
            Registration.Method,
            Registration.RegistrationGeneration))
    {
        return;
    }

    const std::string RequestId(
        Data.request_id.data == nullptr ? "" : Data.request_id.data,
        static_cast<size_t>(Data.request_id.size));
    const std::shared_ptr<FPendingRpc> Pending = std::make_shared<FPendingRpc>();
    if (!TryAdmitPendingRpc(RequestId, Pending, Registration.ConnectionGeneration))
    {
        return;
    }
    ON_SCOPE_EXIT
    {
        std::lock_guard Lock(PendingRpcMutex);
        const auto Iterator = PendingRpcs.find(RequestId);
        if (Iterator != PendingRpcs.end() && Iterator->second == Pending)
        {
            PendingRpcs.erase(Iterator);
        }
    };

    FLiveKitRpcInvocation Invocation;
    Invocation.RequestId = FromAdapterView(Data.request_id);
    Invocation.CallerIdentity = FromAdapterView(Data.caller_identity);
    Invocation.Method = FromAdapterView(Data.method);
    Invocation.Payload = FromAdapterView(Data.payload);
    Invocation.ResponseTimeoutSeconds =
        static_cast<float>(Data.response_timeout_seconds);
    Pinned->RpcInvocationHandler(Invocation);

    const double TimeoutSeconds = FMath::Max(0.1, Data.response_timeout_seconds);
    bool bReady = false;
    {
        std::unique_lock Lock(Pending->Mutex);
        bReady = Pending->Ready.wait_for(
            Lock,
            std::chrono::duration<double>(TimeoutSeconds),
            [&Pending]() { return Pending->bCompleted; });
        if (bReady)
        {
            bOutFailed = Pending->bFailed;
            OutResponse = Pending->Response;
            OutErrorCode = Pending->ErrorCode;
            OutErrorMessage = Pending->ErrorMessage;
            OutErrorData = Pending->ErrorData;
        }
    }
    if (!bReady)
    {
        bOutFailed = true;
        OutErrorCode = RpcResponseTimeout;
        OutErrorMessage = "The RPC response timed out.";
    }
}

void FLiveKitWindowsBridge::FImplementation::HandleRpc(
    void* Context,
    LKUB_Room* InRoom,
    const LKUB_RpcInvocation* Invocation,
    LKUB_RpcResponse* Response)
{
    if (Response == nullptr)
    {
        return;
    }
    *Response = {};

    struct FResponseStorage
    {
        std::string Payload;
        std::string ErrorMessage;
        std::string ErrorData;
    };
    // Adapter copies the views immediately after this callback. Thread-local
    // storage keeps them alive after local pending state is released.
    thread_local FResponseStorage Storage;
    Storage = {};

    FRpcCallbackContext* Registration = static_cast<FRpcCallbackContext*>(Context);
    if (Registration == nullptr || Registration->Implementation == nullptr ||
        Invocation == nullptr || InRoom != Registration->Room)
    {
        Storage.ErrorMessage = "The RPC handler is unavailable.";
        Response->kind = LKUB_RPC_RESPONSE_ERROR;
        Response->error_code = RpcApplicationError;
        Response->error_message = ToAdapterView(Storage.ErrorMessage);
        return;
    }

    bool bFailed = true;
    uint32 ErrorCode = RpcApplicationError;
    Registration->Implementation->HandleIncomingRpc(
        *Registration,
        *Invocation,
        Storage.Payload,
        bFailed,
        ErrorCode,
        Storage.ErrorMessage,
        Storage.ErrorData);
    if (bFailed)
    {
        Response->kind = LKUB_RPC_RESPONSE_ERROR;
        Response->error_code = ErrorCode;
        Response->error_message = ToAdapterView(Storage.ErrorMessage);
        Response->error_data = ToAdapterView(Storage.ErrorData);
        return;
    }
    Response->kind = LKUB_RPC_RESPONSE_SUCCESS;
    Response->has_payload = 1;
    Response->payload = ToAdapterView(Storage.Payload);
}

void LKUB_CALL FLiveKitWindowsBridge::FImplementation::OnRpc(
    void* Context,
    LKUB_Room* InRoom,
    const LKUB_RpcInvocation* Invocation,
    LKUB_RpcResponse* Response) noexcept
{
    try
    {
        HandleRpc(Context, InRoom, Invocation, Response);
    }
    catch (...)
    {
        static constexpr char ErrorMessage[] =
            "The Unreal RPC handler raised an internal error.";
        if (Response != nullptr)
        {
            *Response = {};
            Response->kind = LKUB_RPC_RESPONSE_ERROR;
            Response->error_code = RpcApplicationError;
            Response->error_message = LKUB_StringView{
                ErrorMessage,
                sizeof(ErrorMessage) - 1};
        }
    }
}

bool FLiveKitWindowsBridge::FImplementation::ResolvePendingRpc(
    const FString& RequestId,
    const FString* Response,
    int32 ErrorCode,
    const FString& ErrorMessage,
    const FString& ErrorData)
{
    std::shared_ptr<FPendingRpc> Pending;
    {
        std::lock_guard Lock(PendingRpcMutex);
        const auto Iterator = PendingRpcs.find(ToAdapterString(RequestId));
        if (Iterator == PendingRpcs.end())
        {
            return false;
        }
        Pending = Iterator->second;
    }
    {
        std::lock_guard Lock(Pending->Mutex);
        if (Pending->bCompleted)
        {
            return false;
        }
        Pending->bCompleted = true;
        Pending->bFailed = Response == nullptr;
        if (Response != nullptr)
        {
            Pending->Response = ToAdapterString(*Response);
        }
        else
        {
            Pending->ErrorCode = static_cast<uint32>(FMath::Max(ErrorCode, 0));
            Pending->ErrorMessage = ToAdapterString(ErrorMessage);
            Pending->ErrorData = ToAdapterString(ErrorData);
        }
    }
    Pending->Ready.notify_all();
    return true;
}

void FLiveKitWindowsBridge::FImplementation::CancelPendingRpcs(const char* Reason)
{
    std::vector<std::shared_ptr<FPendingRpc>> Pending;
    {
        std::lock_guard Lock(PendingRpcMutex);
        bRpcAdmissionOpen = false;
        RpcAdmissionGeneration = 0;
        for (const auto& Pair : PendingRpcs)
        {
            Pending.push_back(Pair.second);
        }
        PendingRpcs.clear();
    }
    for (const std::shared_ptr<FPendingRpc>& Invocation : Pending)
    {
        {
            std::lock_guard Lock(Invocation->Mutex);
            if (!Invocation->bCompleted)
            {
                Invocation->bCompleted = true;
                Invocation->bFailed = true;
                Invocation->ErrorCode = RpcApplicationError;
                Invocation->ErrorMessage = Reason;
            }
        }
        Invocation->Ready.notify_all();
    }
}

#endif

void FLiveKitWindowsBridge::PerformRpc(
    const FString& RequestId,
    const FString& DestinationIdentity,
    const FString& Method,
    const FString& Payload,
    float ResponseTimeoutSeconds,
    float MaxRoundTripLatencySeconds)
{
    if (!IsSdkAvailable())
    {
        RpcResultHandler(
            RequestId,
            false,
            FString(),
            MakeLiveKitError(
                TEXT("sdk_unavailable"),
                TEXT("The LiveKit Windows adapter is unavailable for RPC.")));
        return;
    }
    DispatchControl([
        RequestId,
        DestinationIdentity,
        Method,
        Payload,
        ResponseTimeoutSeconds,
        MaxRoundTripLatencySeconds](FLiveKitWindowsBridge& Bridge)
    {
#if WITH_LIVEKIT_WINDOWS
        LKUB_Room* Room = Bridge.Implementation->GetRoom();
        if (Room == nullptr)
        {
            Bridge.RpcResultHandler(
                RequestId,
                false,
                FString(),
                MakeLiveKitError(TEXT("rpc_failed"), TEXT("The LiveKit room is unavailable.")));
            return;
        }
        double EffectiveTimeout = 15.0;
        if (ResponseTimeoutSeconds > 0.0f)
        {
            EffectiveTimeout = ResponseTimeoutSeconds;
        }
        if (MaxRoundTripLatencySeconds > 0.0f)
        {
            EffectiveTimeout = FMath::Min(
                EffectiveTimeout,
                static_cast<double>(MaxRoundTripLatencySeconds));
        }
        EffectiveTimeout = FMath::Max(0.1, EffectiveTimeout);

        const std::string DestinationUtf8 = ToAdapterString(DestinationIdentity);
        const std::string MethodUtf8 = ToAdapterString(Method);
        const std::string PayloadUtf8 = ToAdapterString(Payload);
        LKUB_OwnedBuffer Response{};
        ON_SCOPE_EXIT
        {
            lkub_buffer_release(&Response);
        };
        LKUB_Result Result{};
        if (lkub_room_perform_rpc(
                Room,
                ToAdapterView(DestinationUtf8),
                ToAdapterView(MethodUtf8),
                ToAdapterView(PayloadUtf8),
                EffectiveTimeout,
                &Response,
                &Result) != LKUB_STATUS_OK)
        {
            const FString ErrorCode = Result.status == LKUB_STATUS_RPC_ERROR
                ? FString::Printf(TEXT("rpc_%u"), Result.rpc_error_code)
                : TEXT("rpc_failed");
            Bridge.RpcResultHandler(
                RequestId,
                false,
                FString(),
                AdapterError(ErrorCode, Result));
            return;
        }
        Bridge.RpcResultHandler(
            RequestId,
            true,
            FromUtf8(reinterpret_cast<const char*>(Response.data), Response.size),
            FLiveKitError());
#endif
    });
}

void FLiveKitWindowsBridge::CompleteRpcInvocation(
    const FString& RequestId,
    const FString& ResponsePayload)
{
#if WITH_LIVEKIT_WINDOWS
    Implementation->ResolvePendingRpc(
        RequestId,
        &ResponsePayload,
        0,
        FString(),
        FString());
#endif
}

void FLiveKitWindowsBridge::FailRpcInvocation(
    const FString& RequestId,
    int32 ErrorCode,
    const FString& Message,
    const FString& Data)
{
#if WITH_LIVEKIT_WINDOWS
    Implementation->ResolvePendingRpc(
        RequestId,
        nullptr,
        ErrorCode,
        Message,
        Data);
#endif
}
