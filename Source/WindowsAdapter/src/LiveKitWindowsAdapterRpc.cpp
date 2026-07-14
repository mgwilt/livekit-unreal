#include "LiveKitWindowsAdapterInternal.h"

#include <cstring>
#include <optional>

using namespace lkub::internal;

extern "C"
{
int32_t LKUB_CALL lkub_room_register_rpc_method(
    LKUB_Room* Room,
    LKUB_StringView Method,
    LKUB_RpcHandler Handler,
    void* HandlerContext,
    LKUB_Result* OutResult)
{
    return Guard(OutResult, [&]()
    {
        Require(Room != nullptr && Room->SdkRoom, "The room is unavailable.");
        Require(Handler != nullptr, "The RPC handler is required.");
        const std::shared_ptr<livekit::LocalParticipant> Participant =
            Room->SdkRoom->localParticipant().lock();
        if (!Participant)
        {
            throw std::runtime_error("The local LiveKit participant is unavailable.");
        }
        const std::string MethodCopy = CopyString(Method);
        Participant->registerRpcMethod(
            MethodCopy,
            [Room, Handler, HandlerContext, MethodCopy](
                const livekit::RpcInvocationData& Data)
                -> std::optional<std::string>
            {
                LKUB_RpcInvocation Invocation{};
                Invocation.request_id = BorrowString(Data.request_id);
                Invocation.caller_identity = BorrowString(Data.caller_identity);
                Invocation.method = BorrowString(MethodCopy);
                Invocation.payload = BorrowString(Data.payload);
                Invocation.response_timeout_seconds = Data.response_timeout_sec;

                LKUB_RpcResponse Response{};
                try
                {
                    Handler(HandlerContext, Room, &Invocation, &Response);
                }
                catch (...)
                {
                    throw livekit::RpcError::builtIn(
                        livekit::RpcError::ErrorCode::APPLICATION_ERROR);
                }

                if (Response.kind == LKUB_RPC_RESPONSE_ERROR)
                {
                    const uint32_t ErrorCode = Response.error_code == 0
                        ? static_cast<uint32_t>(
                            livekit::RpcError::ErrorCode::APPLICATION_ERROR)
                        : Response.error_code;
                    throw livekit::RpcError(
                        ErrorCode,
                        CopyString(Response.error_message),
                        CopyString(Response.error_data));
                }
                if (Response.kind != LKUB_RPC_RESPONSE_SUCCESS)
                {
                    throw livekit::RpcError::builtIn(
                        livekit::RpcError::ErrorCode::APPLICATION_ERROR);
                }
                if (Response.has_payload == 0)
                {
                    return std::nullopt;
                }
                return CopyString(Response.payload);
            });
    });
}

int32_t LKUB_CALL lkub_room_unregister_rpc_method(
    LKUB_Room* Room,
    LKUB_StringView Method,
    LKUB_Result* OutResult)
{
    return Guard(OutResult, [&]()
    {
        Require(Room != nullptr && Room->SdkRoom, "The room is unavailable.");
        const std::shared_ptr<livekit::LocalParticipant> Participant =
            Room->SdkRoom->localParticipant().lock();
        if (Participant)
        {
            Participant->unregisterRpcMethod(CopyString(Method));
        }
    });
}

int32_t LKUB_CALL lkub_room_perform_rpc(
    LKUB_Room* Room,
    LKUB_StringView DestinationIdentity,
    LKUB_StringView Method,
    LKUB_StringView Payload,
    double ResponseTimeoutSeconds,
    LKUB_OwnedBuffer* OutResponse,
    LKUB_Result* OutResult)
{
    if (OutResponse != nullptr)
    {
        OutResponse->data = nullptr;
        OutResponse->size = 0;
    }
    return Guard(OutResult, [&]()
    {
        Require(Room != nullptr && Room->SdkRoom, "The room is unavailable.");
        Require(OutResponse != nullptr, "The RPC response output is required.");
        Require(ResponseTimeoutSeconds > 0.0, "The RPC timeout must be positive.");
        const std::shared_ptr<livekit::LocalParticipant> Participant =
            Room->SdkRoom->localParticipant().lock();
        if (!Participant)
        {
            throw std::runtime_error("The local LiveKit participant is unavailable.");
        }

        const std::string Response = Participant->performRpc(
            CopyString(DestinationIdentity),
            CopyString(Method),
            CopyString(Payload),
            ResponseTimeoutSeconds);
        if (!Response.empty())
        {
            std::unique_ptr<uint8_t[]> Copy(new uint8_t[Response.size()]);
            std::memcpy(Copy.get(), Response.data(), Response.size());
            OutResponse->size = static_cast<uint64_t>(Response.size());
            OutResponse->data = Copy.release();
        }
    });
}
}
