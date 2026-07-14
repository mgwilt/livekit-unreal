#include "LiveKitWindowsBridgeInternal.h"

#include "Async/Async.h"
#include "LiveKitBridgeModule.h"
#include "Misc/ScopeExit.h"

namespace LiveKitWindowsBridgeInternal
{
FLiveKitError MakeLiveKitError(
    const FString& Code,
    const FString& Message,
    const FString& Data)
{
    FLiveKitError Error;
    Error.Code = Code;
    Error.Message = Message;
    Error.Data = Data;
    return Error;
}

#if WITH_LIVEKIT_WINDOWS
std::string ToAdapterString(const FString& Value)
{
    FTCHARToUTF8 Converted(*Value);
    return std::string(Converted.Get(), static_cast<size_t>(Converted.Length()));
}

LKUB_StringView ToAdapterView(const std::string& Value)
{
    return LKUB_StringView{Value.data(), static_cast<uint64_t>(Value.size())};
}

FString FromUtf8(const char* Data, uint64_t Size)
{
    if (Data == nullptr || Size == 0)
    {
        return FString();
    }
    const int32 SafeSize = static_cast<int32>(FMath::Min<uint64_t>(Size, MAX_int32));
    const FUTF8ToTCHAR Converted(Data, SafeSize);
    return FString(Converted.Length(), Converted.Get());
}

FString FromAdapterView(const LKUB_StringView& Value)
{
    return FromUtf8(Value.data, Value.size);
}

namespace
{
FString ResultMessage(const LKUB_Result& Result)
{
    return FromUtf8(
        Result.message,
        FMath::Min<uint32>(Result.message_size, LKUB_RESULT_MESSAGE_CAPACITY));
}

FString ResultData(const LKUB_Result& Result)
{
    return FromUtf8(
        Result.data,
        FMath::Min<uint32>(Result.data_size, LKUB_RESULT_DATA_CAPACITY));
}
}

FLiveKitError AdapterError(const FString& Code, const LKUB_Result& Result)
{
    FString Message = ResultMessage(Result);
    if (Message.IsEmpty())
    {
        Message = FString::Printf(
            TEXT("The LiveKit Windows adapter failed with status %d."),
            Result.status);
    }
    return MakeLiveKitError(Code, Message, ResultData(Result));
}

FLiveKitError UnknownBackendError(const FString& Code)
{
    return MakeLiveKitError(
        Code,
        TEXT("The LiveKit Windows backend raised an unknown error."));
}

FLiveKitParticipantInfo ToParticipantInfo(const LKUB_ParticipantInfo& Participant)
{
    FLiveKitParticipantInfo Result;
    Result.Identity = FromAdapterView(Participant.identity);
    Result.Sid = FromAdapterView(Participant.sid);
    Result.Name = FromAdapterView(Participant.name);
    Result.Metadata = FromAdapterView(Participant.metadata);
    Result.bIsSpeaking = Participant.is_speaking != 0;
    Result.bIsAgent = Participant.is_agent != 0;
    return Result;
}
#endif
}

using namespace LiveKitWindowsBridgeInternal;

#if WITH_LIVEKIT_WINDOWS

bool FLiveKitWindowsBridge::FImplementation::TryStartTask(bool bByteStreamReader)
{
    std::lock_guard Lock(TaskMutex);
    if (!bAcceptingTasks || bShuttingDown.load(std::memory_order_acquire))
    {
        return false;
    }
    if (bByteStreamReader &&
        ActiveByteStreamReaders >= MaxConcurrentByteStreamReaders)
    {
        return false;
    }
    ++ActiveTasks;
    if (bByteStreamReader)
    {
        ++ActiveByteStreamReaders;
    }
    return true;
}

void FLiveKitWindowsBridge::FImplementation::TaskFinished(bool bByteStreamReader)
{
    {
        std::lock_guard Lock(TaskMutex);
        check(ActiveTasks > 0);
        --ActiveTasks;
        if (bByteStreamReader)
        {
            check(ActiveByteStreamReaders > 0);
            --ActiveByteStreamReaders;
        }
    }
    TasksFinished.notify_all();
}

void FLiveKitWindowsBridge::FImplementation::StopAcceptingTasks()
{
    std::lock_guard Lock(TaskMutex);
    bAcceptingTasks = false;
}

void FLiveKitWindowsBridge::FImplementation::WaitForTasks()
{
    std::unique_lock Lock(TaskMutex);
    TasksFinished.wait(Lock, [this]() { return ActiveTasks == 0; });
}

bool FLiveKitWindowsBridge::FImplementation::BeginShutdown()
{
    std::unique_lock Lock(ShutdownMutex);
    if (ShutdownState == EShutdownState::Complete)
    {
        return false;
    }
    if (ShutdownState == EShutdownState::InProgress)
    {
        ShutdownFinished.wait(Lock, [this]()
        {
            return ShutdownState == EShutdownState::Complete;
        });
        return false;
    }
    ShutdownState = EShutdownState::InProgress;
    bShuttingDown.store(true, std::memory_order_release);
    return true;
}

void FLiveKitWindowsBridge::FImplementation::CompleteShutdown()
{
    {
        std::lock_guard Lock(ShutdownMutex);
        ShutdownState = EShutdownState::Complete;
    }
    ShutdownFinished.notify_all();
}

uint64 FLiveKitWindowsBridge::FImplementation::ReserveControlTicket()
{
    std::lock_guard Lock(ControlOrderMutex);
    return NextControlTicket++;
}

void FLiveKitWindowsBridge::FImplementation::WaitForControlTurn(uint64 Ticket)
{
    std::unique_lock Lock(ControlOrderMutex);
    ControlTurn.wait(Lock, [this, Ticket]() { return ServingControlTicket == Ticket; });
}

void FLiveKitWindowsBridge::FImplementation::AdvancePastAbandonedControlTickets()
{
    while (AbandonedControlTickets.erase(ServingControlTicket) > 0)
    {
        ++ServingControlTicket;
    }
}

void FLiveKitWindowsBridge::FImplementation::FinishControlTicket(uint64 Ticket)
{
    {
        std::lock_guard Lock(ControlOrderMutex);
        check(ServingControlTicket == Ticket);
        ++ServingControlTicket;
        AdvancePastAbandonedControlTickets();
    }
    ControlTurn.notify_all();
}

void FLiveKitWindowsBridge::FImplementation::AbandonControlTicket(uint64 Ticket)
{
    {
        std::lock_guard Lock(ControlOrderMutex);
        AbandonedControlTickets.insert(Ticket);
        AdvancePastAbandonedControlTickets();
    }
    ControlTurn.notify_all();
}

#endif

FLiveKitWindowsBridge::FLiveKitWindowsBridge(
    FStateHandler InStateHandler,
    FErrorHandler InErrorHandler,
    FParticipantHandler InParticipantConnectedHandler,
    FParticipantHandler InParticipantDisconnectedHandler,
    FSpeakingHandler InSpeakingHandler,
    FDataHandler InDataHandler,
    FByteStreamRegistrationHandler InByteStreamRegistrationHandler,
    FByteStreamHandler InByteStreamHandler,
    FRpcRegistrationHandler InRpcRegistrationHandler,
    FRpcInvocationHandler InRpcInvocationHandler,
    FRpcResultHandler InRpcResultHandler,
    FPublishResultHandler InPublishResultHandler)
    : Implementation(MakeUnique<FImplementation>())
    , StateHandler(MoveTemp(InStateHandler))
    , ErrorHandler(MoveTemp(InErrorHandler))
    , ParticipantConnectedHandler(MoveTemp(InParticipantConnectedHandler))
    , ParticipantDisconnectedHandler(MoveTemp(InParticipantDisconnectedHandler))
    , SpeakingHandler(MoveTemp(InSpeakingHandler))
    , DataHandler(MoveTemp(InDataHandler))
    , ByteStreamRegistrationHandler(MoveTemp(InByteStreamRegistrationHandler))
    , ByteStreamHandler(MoveTemp(InByteStreamHandler))
    , RpcRegistrationHandler(MoveTemp(InRpcRegistrationHandler))
    , RpcInvocationHandler(MoveTemp(InRpcInvocationHandler))
    , RpcResultHandler(MoveTemp(InRpcResultHandler))
    , PublishResultHandler(MoveTemp(InPublishResultHandler))
{
}

void FLiveKitWindowsBridge::ActivateLifetimeGate()
{
#if WITH_LIVEKIT_WINDOWS
    Implementation->Owner = AsShared();
    RegisterLiveKitWindowsBridge(AsShared());
#endif
}

bool FLiveKitWindowsBridge::IsSdkAvailable() const
{
#if WITH_LIVEKIT_WINDOWS
    return IsLiveKitWindowsSdkInitialized();
#else
    return false;
#endif
}

void FLiveKitWindowsBridge::ReportUnavailable(const FString& Operation) const
{
    ErrorHandler(MakeLiveKitError(
        TEXT("sdk_unavailable"),
        TEXT("The verified LiveKit Windows adapter is not installed or initialized for this Win64 build."),
        Operation));
}

bool FLiveKitWindowsBridge::DispatchControl(TFunction<void(FLiveKitWindowsBridge&)> Work)
{
#if WITH_LIVEKIT_WINDOWS
    if (!Implementation || !Implementation->TryStartTask(false))
    {
        return false;
    }
    const uint64 ControlTicket = Implementation->ReserveControlTicket();
    const TSharedRef<FLiveKitWindowsBridge, ESPMode::ThreadSafe> Self = AsShared();
    try
    {
        Async(EAsyncExecution::Thread, [Self, ControlTicket, Work = MoveTemp(Work)]() mutable
        {
            bool bOwnsControlTurn = false;
            ON_SCOPE_EXIT
            {
                if (bOwnsControlTurn)
                {
                    Self->Implementation->FinishControlTicket(ControlTicket);
                }
                else
                {
                    Self->Implementation->AbandonControlTicket(ControlTicket);
                }
                Self->Implementation->TaskFinished(false);
            };
            Self->Implementation->WaitForControlTurn(ControlTicket);
            bOwnsControlTurn = true;
            std::lock_guard ControlLock(Self->Implementation->ControlMutex);
            if (!Self->Implementation->bShuttingDown.load(std::memory_order_acquire))
            {
                try
                {
                    Work(Self.Get());
                }
                catch (...)
                {
                    Self->ErrorHandler(UnknownBackendError(TEXT("windows_backend_error")));
                }
            }
        });
    }
    catch (...)
    {
        Implementation->AbandonControlTicket(ControlTicket);
        Implementation->TaskFinished(false);
        return false;
    }
    return true;
#else
    return false;
#endif
}

void FLiveKitWindowsBridge::Shutdown()
{
    if (!Implementation)
    {
        return;
    }
#if WITH_LIVEKIT_WINDOWS
    if (!Implementation->BeginShutdown())
    {
        return;
    }
    ON_SCOPE_EXIT
    {
        Implementation->CompleteShutdown();
    };
    Implementation->StopAcceptingTasks();
    Implementation->InvalidateConnectionRequest();
    {
        std::lock_guard ControlLock(Implementation->ControlMutex);
        if (LKUB_Room* Room = Implementation->GetRoom())
        {
            Implementation->DisconnectRoomOrQuarantine(Room);
            Implementation->ClearRoomIf(Room);
        }
        Implementation->ClearSpeakers();
    }
    Implementation->WaitForTasks();
    Implementation->Owner.Reset();
#else
    Implementation->bShuttingDown.store(true, std::memory_order_release);
#endif
}

FLiveKitWindowsBridge::~FLiveKitWindowsBridge()
{
    Shutdown();
}
