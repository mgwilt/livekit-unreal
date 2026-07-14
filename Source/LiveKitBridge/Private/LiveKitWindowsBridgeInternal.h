#pragma once

#include "LiveKitWindowsBridge.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if WITH_LIVEKIT_WINDOWS
#include "LiveKitWindowsAdapter.h"

namespace LiveKitWindowsBridgeInternal
{
constexpr uint32 RpcApplicationError = 1500;
constexpr uint32 RpcResponseTimeout = 1502;

FLiveKitError MakeLiveKitError(
    const FString& Code,
    const FString& Message,
    const FString& Data = FString());
std::string ToAdapterString(const FString& Value);
LKUB_StringView ToAdapterView(const std::string& Value);
FString FromUtf8(const char* Data, uint64_t Size);
FString FromAdapterView(const LKUB_StringView& Value);
FLiveKitError AdapterError(const FString& Code, const LKUB_Result& Result);
FLiveKitError UnknownBackendError(const FString& Code);
FLiveKitParticipantInfo ToParticipantInfo(const LKUB_ParticipantInfo& Participant);
}

struct FLiveKitWindowsBridge::FImplementation
{
    struct FPendingRpc
    {
        std::mutex Mutex;
        std::condition_variable Ready;
        bool bCompleted = false;
        bool bFailed = false;
        std::string Response;
        uint32 ErrorCode = LiveKitWindowsBridgeInternal::RpcApplicationError;
        std::string ErrorMessage;
        std::string ErrorData;
    };

    struct FTrackedByteStreamReader
    {
        LKUB_ByteStreamReader* Reader = nullptr;
        std::string Topic;
        uint64 ConnectionGeneration = 0;
        uint64 RegistrationGeneration = 0;
    };

    struct FByteStreamCallbackContext
    {
        FImplementation* Implementation = nullptr;
        LKUB_Room* Room = nullptr;
        std::string Topic;
        uint64 ConnectionGeneration = 0;
        uint64 RegistrationGeneration = 0;
    };

    struct FRpcCallbackContext
    {
        FImplementation* Implementation = nullptr;
        LKUB_Room* Room = nullptr;
        std::string Method;
        uint64 ConnectionGeneration = 0;
        uint64 RegistrationGeneration = 0;
    };

    struct FParticipantVisitContext
    {
        FImplementation* Implementation = nullptr;
        FLiveKitWindowsBridge* Bridge = nullptr;
        LKUB_Room* Room = nullptr;
        uint64 ConnectionGeneration = 0;
    };

    enum class EShutdownState : uint8
    {
        Running,
        InProgress,
        Complete
    };

    static constexpr int32 MaxConcurrentByteStreamReaders = 4;

    // Task and shutdown lifetime.
    bool TryStartTask(bool bByteStreamReader);
    void TaskFinished(bool bByteStreamReader);
    void StopAcceptingTasks();
    void WaitForTasks();
    bool BeginShutdown();
    void CompleteShutdown();
    uint64 ReserveControlTicket();
    void WaitForControlTurn(uint64 Ticket);
    void AdvancePastAbandonedControlTickets();
    void FinishControlTicket(uint64 Ticket);
    void AbandonControlTicket(uint64 Ticket);

    // Room state and lifecycle.
    uint64 BeginConnectionRequest();
    uint64 InvalidateConnectionRequest();
    bool IsConnectionRequestCurrent(uint64 Generation);
    bool IsDisconnectRequestCurrent(uint64 Generation);
    LKUB_Room* GetRoom();
    bool GetCurrentRoom(LKUB_Room*& OutRoom, uint64& OutGeneration);
    bool HasReadyRoom();
    void SetRoom(LKUB_Room* InRoom, uint64 Generation);
    void ClearRoomIf(LKUB_Room* Candidate);
    bool IsCurrentRoom(LKUB_Room* Candidate, uint64 Generation = 0);
    bool MarkRoomReady(LKUB_Room* Candidate, uint64 Generation);
    bool IsCurrentRoomReady(LKUB_Room* Candidate);
    bool DetachTerminalRoom(
        LKUB_Room* Candidate,
        LKUB_Room*& OutRoom,
        bool& bOutShouldNotify);
    bool FailConnectionRequestIfCurrent(uint64 Generation);
    bool DisconnectRoomOrQuarantine(
        LKUB_Room* Candidate,
        FLiveKitError* OutError = nullptr);
    bool IsSpeaking(const std::string& Identity);
    void ClearSpeakers();

    // Room event callbacks.
    static void HandleParticipantConnected(
        void* Context,
        LKUB_Room* InRoom,
        const LKUB_ParticipantInfo* Participant);
    static void HandleParticipantDisconnected(
        void* Context,
        LKUB_Room* InRoom,
        const LKUB_ParticipantInfo* Participant);
    static void HandleSpeakingChanged(
        void* Context,
        LKUB_Room* InRoom,
        const LKUB_ParticipantInfo* Participant,
        uint8 IsSpeakingNow);
    static void HandleConnectionStateChanged(void* Context, LKUB_Room* InRoom, int32 State);
    void HandleTerminalRoom(LKUB_Room* InRoom);
    static void HandleTerminal(void* Context, LKUB_Room* InRoom, int32 Reason);
    static void HandleUserData(void* Context, LKUB_Room* InRoom, const LKUB_UserData* Data);
    static void LKUB_CALL OnParticipantConnected(
        void* Context,
        LKUB_Room* InRoom,
        const LKUB_ParticipantInfo* Participant) noexcept;
    static void LKUB_CALL OnParticipantDisconnected(
        void* Context,
        LKUB_Room* InRoom,
        const LKUB_ParticipantInfo* Participant) noexcept;
    static void LKUB_CALL OnSpeakingChanged(
        void* Context,
        LKUB_Room* InRoom,
        const LKUB_ParticipantInfo* Participant,
        uint8 IsSpeakingNow) noexcept;
    static void LKUB_CALL OnConnectionStateChanged(
        void* Context,
        LKUB_Room* InRoom,
        int32 State) noexcept;
    static void LKUB_CALL OnTerminal(void* Context, LKUB_Room* InRoom, int32 Reason) noexcept;
    static void LKUB_CALL OnUserData(
        void* Context,
        LKUB_Room* InRoom,
        const LKUB_UserData* Data) noexcept;
    static LKUB_RoomCallbacks MakeRoomCallbacks();
    static void HandleVisitParticipant(void* Context, const LKUB_ParticipantInfo* Participant);
    static void LKUB_CALL VisitParticipant(
        void* Context,
        const LKUB_ParticipantInfo* Participant) noexcept;

    // Byte streams.
    bool IsByteStreamRegistrationCurrent(const std::string& Topic, uint64 Generation);
    bool RemoveByteStreamRegistrationIfCurrent(const std::string& Topic, uint64 Generation);
    bool TryTrackByteStreamReader(
        LKUB_ByteStreamReader* Reader,
        LKUB_Room* CandidateRoom,
        const std::string& Topic,
        uint64 InConnectionGeneration,
        uint64 InRegistrationGeneration);
    void UntrackByteStreamReader(LKUB_ByteStreamReader* Reader);
    void CancelAllTrackedByteStreamReaders();
    void CancelTrackedByteStreamReadersForTopic(const std::string& Topic);
    static void HandleVisitAttribute(void* Context, LKUB_StringView Key, LKUB_StringView Value);
    static void LKUB_CALL VisitAttribute(
        void* Context,
        LKUB_StringView Key,
        LKUB_StringView Value) noexcept;
    static void HandleByteStream(
        void* Context,
        LKUB_Room* InRoom,
        LKUB_ByteStreamReader*& Reader);
    static void LKUB_CALL OnByteStream(
        void* Context,
        LKUB_Room* InRoom,
        LKUB_ByteStreamReader* Reader) noexcept;
    bool InstallByteStreamHandler(
        LKUB_Room* InRoom,
        const std::string& Topic,
        uint64 InConnectionGeneration,
        uint64 InRegistrationGeneration,
        LKUB_Result& OutResult);

    // RPC and registration restoration.
    bool IsRpcRegistrationCurrent(const std::string& Method, uint64 Generation);
    bool RemoveRpcRegistrationIfCurrent(const std::string& Method, uint64 Generation);
    bool TryAdmitPendingRpc(
        const std::string& RequestId,
        const std::shared_ptr<FPendingRpc>& Pending,
        uint64 InConnectionGeneration);
    void HandleIncomingRpc(
        const FRpcCallbackContext& Registration,
        const LKUB_RpcInvocation& Data,
        std::string& OutResponse,
        bool& bOutFailed,
        uint32& OutErrorCode,
        std::string& OutErrorMessage,
        std::string& OutErrorData);
    static void HandleRpc(
        void* Context,
        LKUB_Room* InRoom,
        const LKUB_RpcInvocation* Invocation,
        LKUB_RpcResponse* Response);
    static void LKUB_CALL OnRpc(
        void* Context,
        LKUB_Room* InRoom,
        const LKUB_RpcInvocation* Invocation,
        LKUB_RpcResponse* Response) noexcept;
    bool InstallRpcMethod(
        LKUB_Room* InRoom,
        const std::string& Method,
        uint64 InConnectionGeneration,
        uint64 InRegistrationGeneration,
        LKUB_Result& OutResult);
    void RestoreRegistrations(
        LKUB_Room* InRoom,
        uint64 InConnectionGeneration,
        FLiveKitWindowsBridge& Bridge);
    bool ResolvePendingRpc(
        const FString& RequestId,
        const FString* Response,
        int32 ErrorCode,
        const FString& ErrorMessage,
        const FString& ErrorData);
    void CancelPendingRpcs(const char* Reason = "The LiveKit client is shutting down.");

    TWeakPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe> Owner;
    std::atomic_bool bShuttingDown{false};
    std::mutex ShutdownMutex;
    std::condition_variable ShutdownFinished;
    EShutdownState ShutdownState = EShutdownState::Running;
    std::mutex TaskMutex;
    std::condition_variable TasksFinished;
    bool bAcceptingTasks = true;
    int32 ActiveTasks = 0;
    int32 ActiveByteStreamReaders = 0;
    std::mutex ControlMutex;
    std::mutex ControlOrderMutex;
    std::condition_variable ControlTurn;
    uint64 NextControlTicket = 0;
    uint64 ServingControlTicket = 0;
    std::set<uint64> AbandonedControlTickets;
    std::atomic<uint64> ConnectionGeneration{0};
    std::atomic_bool bConnectionRequested{false};
    std::mutex RoomMutex;
    LKUB_Room* Room = nullptr;
    uint64 RoomGeneration = 0;
    bool bRoomReady = false;
    // Makes registration insertion plus its dispatch decision atomic with
    // connect's registration snapshot boundary. Acquire this before
    // RegistrationMutex when both are needed.
    std::mutex RegistrationReadyHandoffMutex;
    // Guarded by RegistrationReadyHandoffMutex; identifies the connection
    // whose restore snapshot has already been taken.
    uint64 RegistrationRestoreGeneration = 0;
    std::mutex RegistrationMutex;
    uint64 NextRegistrationGeneration = 0;
    std::map<std::string, uint64> ByteStreamTopics;
    std::map<std::string, uint64> RpcMethods;
    std::vector<std::shared_ptr<FByteStreamCallbackContext>> ByteStreamContexts;
    std::vector<std::shared_ptr<FRpcCallbackContext>> RpcContexts;
    std::mutex SpeakerMutex;
    std::unordered_set<std::string> ActiveSpeakers;
    std::mutex PendingRpcMutex;
    std::unordered_map<std::string, std::shared_ptr<FPendingRpc>> PendingRpcs;
    bool bRpcAdmissionOpen = false;
    uint64 RpcAdmissionGeneration = 0;
    std::mutex ByteStreamReaderMutex;
    std::unordered_map<LKUB_ByteStreamReader*, FTrackedByteStreamReader>
        TrackedByteStreamReaders;
};

#else

namespace LiveKitWindowsBridgeInternal
{
FLiveKitError MakeLiveKitError(
    const FString& Code,
    const FString& Message,
    const FString& Data = FString());
}

struct FLiveKitWindowsBridge::FImplementation
{
    std::atomic_bool bShuttingDown{false};
};

#endif
