#include "LiveKitWindowsBridgeInternal.h"

using namespace LiveKitWindowsBridgeInternal;

#if WITH_LIVEKIT_WINDOWS

bool FLiveKitWindowsBridge::FImplementation::IsByteStreamRegistrationCurrent(const std::string& Topic, uint64 Generation)
{
    std::lock_guard Lock(RegistrationMutex);
    const auto Iterator = ByteStreamTopics.find(Topic);
    return Iterator != ByteStreamTopics.end() && Iterator->second == Generation;
}

bool FLiveKitWindowsBridge::FImplementation::IsRpcRegistrationCurrent(const std::string& Method, uint64 Generation)
{
    std::lock_guard Lock(RegistrationMutex);
    const auto Iterator = RpcMethods.find(Method);
    return Iterator != RpcMethods.end() && Iterator->second == Generation;
}

bool FLiveKitWindowsBridge::FImplementation::RemoveByteStreamRegistrationIfCurrent(const std::string& Topic, uint64 Generation)
{
    {
        std::lock_guard Lock(RegistrationMutex);
        const auto Iterator = ByteStreamTopics.find(Topic);
        if (Iterator == ByteStreamTopics.end() || Iterator->second != Generation)
        {
            return false;
        }
        ByteStreamTopics.erase(Iterator);
    }
    CancelTrackedByteStreamReadersForTopic(Topic);
    return true;
}

bool FLiveKitWindowsBridge::FImplementation::RemoveRpcRegistrationIfCurrent(const std::string& Method, uint64 Generation)
{
    std::lock_guard Lock(RegistrationMutex);
    const auto Iterator = RpcMethods.find(Method);
    if (Iterator == RpcMethods.end() || Iterator->second != Generation)
    {
        return false;
    }
    RpcMethods.erase(Iterator);
    return true;
}

bool FLiveKitWindowsBridge::FImplementation::InstallByteStreamHandler(
    LKUB_Room* InRoom,
    const std::string& Topic,
    uint64 InConnectionGeneration,
    uint64 InRegistrationGeneration,
    LKUB_Result& OutResult)
{
    const std::shared_ptr<FByteStreamCallbackContext> Context =
        std::make_shared<FByteStreamCallbackContext>();
    Context->Implementation = this;
    Context->Room = InRoom;
    Context->Topic = Topic;
    Context->ConnectionGeneration = InConnectionGeneration;
    Context->RegistrationGeneration = InRegistrationGeneration;
    {
        std::lock_guard Lock(RegistrationMutex);
        ByteStreamContexts.push_back(Context);
    }
    return lkub_room_register_byte_stream_handler(
        InRoom,
        ToAdapterView(Topic),
        &OnByteStream,
        Context.get(),
        &OutResult) == LKUB_STATUS_OK;
}

bool FLiveKitWindowsBridge::FImplementation::InstallRpcMethod(
    LKUB_Room* InRoom,
    const std::string& Method,
    uint64 InConnectionGeneration,
    uint64 InRegistrationGeneration,
    LKUB_Result& OutResult)
{
    const std::shared_ptr<FRpcCallbackContext> Context =
        std::make_shared<FRpcCallbackContext>();
    Context->Implementation = this;
    Context->Room = InRoom;
    Context->Method = Method;
    Context->ConnectionGeneration = InConnectionGeneration;
    Context->RegistrationGeneration = InRegistrationGeneration;
    {
        std::lock_guard Lock(RegistrationMutex);
        RpcContexts.push_back(Context);
    }
    return lkub_room_register_rpc_method(
        InRoom,
        ToAdapterView(Method),
        &OnRpc,
        Context.get(),
        &OutResult) == LKUB_STATUS_OK;
}

void FLiveKitWindowsBridge::FImplementation::RestoreRegistrations(
    LKUB_Room* InRoom,
    uint64 InConnectionGeneration,
    FLiveKitWindowsBridge& Bridge)
{
    std::map<std::string, uint64> Topics;
    std::map<std::string, uint64> Methods;
    {
        std::lock_guard HandoffLock(RegistrationReadyHandoffMutex);
        std::lock_guard RegistrationLock(RegistrationMutex);
        Topics = ByteStreamTopics;
        Methods = RpcMethods;
        RegistrationRestoreGeneration = InConnectionGeneration;
    }

    for (const auto& Pair : Topics)
    {
        const std::string& Topic = Pair.first;
        const uint64 RegistrationGeneration = Pair.second;
        if (!IsCurrentRoom(InRoom, InConnectionGeneration) ||
            !IsByteStreamRegistrationCurrent(Topic, RegistrationGeneration))
        {
            continue;
        }
        LKUB_Result Result{};
        if (!InstallByteStreamHandler(
                InRoom,
                Topic,
                InConnectionGeneration,
                RegistrationGeneration,
                Result))
        {
            if (IsCurrentRoom(InRoom, InConnectionGeneration) &&
                RemoveByteStreamRegistrationIfCurrent(Topic, RegistrationGeneration))
            {
                Bridge.ByteStreamRegistrationHandler(
                    FromUtf8(Topic.data(), Topic.size()),
                    false,
                    AdapterError(TEXT("byte_stream_registration_failed"), Result));
            }
            continue;
        }
        if (!IsCurrentRoom(InRoom, InConnectionGeneration) ||
            !IsByteStreamRegistrationCurrent(Topic, RegistrationGeneration))
        {
            Result = {};
            lkub_room_unregister_byte_stream_handler(
                InRoom,
                ToAdapterView(Topic),
                &Result);
            continue;
        }
        Bridge.ByteStreamRegistrationHandler(
            FromUtf8(Topic.data(), Topic.size()),
            true,
            FLiveKitError());
    }

    for (const auto& Pair : Methods)
    {
        const std::string& Method = Pair.first;
        const uint64 RegistrationGeneration = Pair.second;
        if (!IsCurrentRoom(InRoom, InConnectionGeneration) ||
            !IsRpcRegistrationCurrent(Method, RegistrationGeneration))
        {
            continue;
        }
        LKUB_Result Result{};
        if (!InstallRpcMethod(
                InRoom,
                Method,
                InConnectionGeneration,
                RegistrationGeneration,
                Result))
        {
            if (IsCurrentRoom(InRoom, InConnectionGeneration) &&
                RemoveRpcRegistrationIfCurrent(Method, RegistrationGeneration))
            {
                Bridge.RpcRegistrationHandler(
                    FromUtf8(Method.data(), Method.size()),
                    false,
                    AdapterError(TEXT("rpc_registration_failed"), Result));
            }
            continue;
        }
        if (!IsCurrentRoom(InRoom, InConnectionGeneration) ||
            !IsRpcRegistrationCurrent(Method, RegistrationGeneration))
        {
            Result = {};
            lkub_room_unregister_rpc_method(
                InRoom,
                ToAdapterView(Method),
                &Result);
            continue;
        }
        Bridge.RpcRegistrationHandler(
            FromUtf8(Method.data(), Method.size()),
            true,
            FLiveKitError());
    }
}

#endif

bool FLiveKitWindowsBridge::RegisterByteStreamHandler(const FString& Topic)
{
    if (!IsSdkAvailable() || Topic.IsEmpty())
    {
        return false;
    }
#if WITH_LIVEKIT_WINDOWS
    const std::string AdapterTopic = ToAdapterString(Topic);
    uint64 RegistrationGeneration = 0;
    bool bShouldDispatch = false;
    {
        std::lock_guard HandoffLock(
            Implementation->RegistrationReadyHandoffMutex);
        {
            std::lock_guard RegistrationLock(Implementation->RegistrationMutex);
            if (Implementation->ByteStreamTopics.contains(AdapterTopic))
            {
                return false;
            }
            RegistrationGeneration = ++Implementation->NextRegistrationGeneration;
            Implementation->ByteStreamTopics.emplace(
                AdapterTopic,
                RegistrationGeneration);
        }
        bShouldDispatch = Implementation->HasReadyRoom();
        if (!bShouldDispatch)
        {
            LKUB_Room* CurrentRoom = nullptr;
            uint64 CurrentGeneration = 0;
            bShouldDispatch = Implementation->GetCurrentRoom(
                    CurrentRoom,
                    CurrentGeneration) &&
                CurrentGeneration == Implementation->RegistrationRestoreGeneration;
        }
    }
    if (bShouldDispatch)
    {
        const bool bDispatched = DispatchControl([
            Topic,
            AdapterTopic,
            RegistrationGeneration](FLiveKitWindowsBridge& Bridge)
        {
            LKUB_Room* Room = nullptr;
            uint64 ConnectionGeneration = 0;
            if (!Bridge.Implementation->GetCurrentRoom(Room, ConnectionGeneration) ||
                !Bridge.Implementation->IsByteStreamRegistrationCurrent(
                    AdapterTopic,
                    RegistrationGeneration))
            {
                return;
            }
            LKUB_Result Result{};
            if (!Bridge.Implementation->InstallByteStreamHandler(
                    Room,
                    AdapterTopic,
                    ConnectionGeneration,
                    RegistrationGeneration,
                    Result))
            {
                if (Bridge.Implementation->IsCurrentRoom(Room, ConnectionGeneration) &&
                    Bridge.Implementation->RemoveByteStreamRegistrationIfCurrent(
                        AdapterTopic,
                        RegistrationGeneration))
                {
                    Bridge.ByteStreamRegistrationHandler(
                        Topic,
                        false,
                        AdapterError(TEXT("byte_stream_registration_failed"), Result));
                }
                return;
            }
            if (!Bridge.Implementation->IsCurrentRoom(Room, ConnectionGeneration) ||
                !Bridge.Implementation->IsByteStreamRegistrationCurrent(
                    AdapterTopic,
                    RegistrationGeneration))
            {
                Result = {};
                lkub_room_unregister_byte_stream_handler(
                    Room,
                    ToAdapterView(AdapterTopic),
                    &Result);
                return;
            }
            Bridge.ByteStreamRegistrationHandler(Topic, true, FLiveKitError());
        });
        if (!bDispatched)
        {
            Implementation->RemoveByteStreamRegistrationIfCurrent(
                AdapterTopic,
                RegistrationGeneration);
            return false;
        }
    }
#endif
    return true;
}

void FLiveKitWindowsBridge::UnregisterByteStreamHandler(const FString& Topic)
{
#if WITH_LIVEKIT_WINDOWS
    const std::string AdapterTopic = ToAdapterString(Topic);
    {
        std::lock_guard Lock(Implementation->RegistrationMutex);
        Implementation->ByteStreamTopics.erase(AdapterTopic);
    }
    Implementation->CancelTrackedByteStreamReadersForTopic(AdapterTopic);
    DispatchControl([AdapterTopic](FLiveKitWindowsBridge& Bridge)
    {
        if (LKUB_Room* Room = Bridge.Implementation->GetRoom())
        {
            LKUB_Result Result{};
            if (lkub_room_unregister_byte_stream_handler(
                    Room,
                    ToAdapterView(AdapterTopic),
                    &Result) != LKUB_STATUS_OK &&
                Bridge.Implementation->IsCurrentRoom(Room))
            {
                Bridge.ErrorHandler(AdapterError(
                    TEXT("byte_stream_unregistration_failed"),
                    Result));
            }
        }
    });
#endif
}

bool FLiveKitWindowsBridge::RegisterRpcMethod(const FString& Method)
{
    if (!IsSdkAvailable() || Method.IsEmpty())
    {
        return false;
    }
#if WITH_LIVEKIT_WINDOWS
    const std::string AdapterMethod = ToAdapterString(Method);
    uint64 RegistrationGeneration = 0;
    bool bShouldDispatch = false;
    {
        std::lock_guard HandoffLock(
            Implementation->RegistrationReadyHandoffMutex);
        {
            std::lock_guard RegistrationLock(Implementation->RegistrationMutex);
            if (Implementation->RpcMethods.contains(AdapterMethod))
            {
                return false;
            }
            RegistrationGeneration = ++Implementation->NextRegistrationGeneration;
            Implementation->RpcMethods.emplace(
                AdapterMethod,
                RegistrationGeneration);
        }
        bShouldDispatch = Implementation->HasReadyRoom();
        if (!bShouldDispatch)
        {
            LKUB_Room* CurrentRoom = nullptr;
            uint64 CurrentGeneration = 0;
            bShouldDispatch = Implementation->GetCurrentRoom(
                    CurrentRoom,
                    CurrentGeneration) &&
                CurrentGeneration == Implementation->RegistrationRestoreGeneration;
        }
    }
    if (bShouldDispatch)
    {
        const bool bDispatched = DispatchControl([
            Method,
            AdapterMethod,
            RegistrationGeneration](FLiveKitWindowsBridge& Bridge)
        {
            LKUB_Room* Room = nullptr;
            uint64 ConnectionGeneration = 0;
            if (!Bridge.Implementation->GetCurrentRoom(Room, ConnectionGeneration) ||
                !Bridge.Implementation->IsRpcRegistrationCurrent(
                    AdapterMethod,
                    RegistrationGeneration))
            {
                return;
            }
            LKUB_Result Result{};
            if (!Bridge.Implementation->InstallRpcMethod(
                    Room,
                    AdapterMethod,
                    ConnectionGeneration,
                    RegistrationGeneration,
                    Result))
            {
                if (Bridge.Implementation->IsCurrentRoom(Room, ConnectionGeneration) &&
                    Bridge.Implementation->RemoveRpcRegistrationIfCurrent(
                        AdapterMethod,
                        RegistrationGeneration))
                {
                    Bridge.RpcRegistrationHandler(
                        Method,
                        false,
                        AdapterError(TEXT("rpc_registration_failed"), Result));
                }
                return;
            }
            if (!Bridge.Implementation->IsCurrentRoom(Room, ConnectionGeneration) ||
                !Bridge.Implementation->IsRpcRegistrationCurrent(
                    AdapterMethod,
                    RegistrationGeneration))
            {
                Result = {};
                lkub_room_unregister_rpc_method(
                    Room,
                    ToAdapterView(AdapterMethod),
                    &Result);
                return;
            }
            Bridge.RpcRegistrationHandler(Method, true, FLiveKitError());
        });
        if (!bDispatched)
        {
            Implementation->RemoveRpcRegistrationIfCurrent(
                AdapterMethod,
                RegistrationGeneration);
            return false;
        }
    }
#endif
    return true;
}

void FLiveKitWindowsBridge::UnregisterRpcMethod(const FString& Method)
{
#if WITH_LIVEKIT_WINDOWS
    const std::string AdapterMethod = ToAdapterString(Method);
    {
        std::lock_guard Lock(Implementation->RegistrationMutex);
        Implementation->RpcMethods.erase(AdapterMethod);
    }
    DispatchControl([AdapterMethod](FLiveKitWindowsBridge& Bridge)
    {
        if (LKUB_Room* Room = Bridge.Implementation->GetRoom())
        {
            LKUB_Result Result{};
            if (lkub_room_unregister_rpc_method(
                    Room,
                    ToAdapterView(AdapterMethod),
                    &Result) != LKUB_STATUS_OK &&
                Bridge.Implementation->IsCurrentRoom(Room))
            {
                Bridge.ErrorHandler(AdapterError(TEXT("rpc_unregistration_failed"), Result));
            }
        }
    });
#endif
}
