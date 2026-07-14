#include "LiveKitWindowsBridge.h"

#include "Async/Async.h"
#include "LiveKitBridgeModule.h"
#include "LiveKitWindowsSdkV130Access.h"
#include "Misc/ScopeExit.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if WITH_LIVEKIT_WINDOWS
#include <livekit/livekit.h>
#endif

namespace
{
FLiveKitError MakeLiveKitError(
    const FString& Code,
    const FString& Message,
    const FString& Data = FString())
{
    FLiveKitError Error;
    Error.Code = Code;
    Error.Message = Message;
    Error.Data = Data;
    return Error;
}

std::string ToLiveKitString(const FString& Value)
{
    FTCHARToUTF8 Converted(*Value);
    return std::string(Converted.Get(), static_cast<size_t>(Converted.Length()));
}

FString FromLiveKitString(const std::string& Value)
{
    return FString(UTF8_TO_TCHAR(Value.c_str()));
}

#if WITH_LIVEKIT_WINDOWS
FLiveKitError ExceptionError(const FString& Code, const std::exception& Error)
{
    return MakeLiveKitError(Code, FromLiveKitString(Error.what()));
}

FLiveKitParticipantInfo ToParticipantInfo(
    const livekit::Participant& Participant,
    bool bIsSpeaking)
{
    FLiveKitParticipantInfo Result;
    Result.Identity = FromLiveKitString(Participant.identity());
    Result.Sid = FromLiveKitString(Participant.sid());
    Result.Name = FromLiveKitString(Participant.name());
    Result.Metadata = FromLiveKitString(Participant.metadata());
    Result.bIsSpeaking = bIsSpeaking;
    Result.bIsAgent = Participant.kind() == livekit::ParticipantKind::Agent;
    return Result;
}
#endif
}

#if WITH_LIVEKIT_WINDOWS

struct FLiveKitWindowsBridge::FImplementation final : public livekit::RoomDelegate
{
    struct FPendingRpc
    {
        std::mutex Mutex;
        std::condition_variable Ready;
        bool bCompleted = false;
        bool bFailed = false;
        std::string Response;
        std::uint32_t ErrorCode = static_cast<std::uint32_t>(livekit::RpcError::ErrorCode::APPLICATION_ERROR);
        std::string ErrorMessage;
        std::string ErrorData;
    };

    struct FTrackedByteStreamReader
    {
        std::shared_ptr<livekit::ByteStreamReader> Reader;
        std::string Topic;
        uint64 ConnectionGeneration = 0;
        uint64 RegistrationGeneration = 0;
    };

    enum class EShutdownState : uint8
    {
        Running,
        InProgress,
        Complete
    };

    static constexpr int32 MaxConcurrentByteStreamReaders = 4;

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

    // All SDK control operations are serialized. Byte-stream reads use their
    // own bounded dedicated threads so a slow stream cannot stall room work.
    std::mutex ControlMutex;
    std::mutex ControlOrderMutex;
    std::condition_variable ControlTurn;
    uint64 NextControlTicket = 0;
    uint64 ServingControlTicket = 0;
    std::set<uint64> AbandonedControlTickets;

    std::atomic<uint64> ConnectionGeneration{0};
    std::atomic_bool bConnectionRequested{false};

    std::mutex RoomMutex;
    std::shared_ptr<livekit::Room> Room;
    uint64 RoomGeneration = 0;
    bool bRoomReady = false;

    std::mutex AudioMutex;
    std::shared_ptr<livekit::PlatformAudio> PlatformAudio;
    std::shared_ptr<livekit::PlatformAudioSource> MicrophoneSource;
    std::shared_ptr<livekit::LocalAudioTrack> MicrophoneTrack;

    std::mutex RegistrationMutex;
    uint64 NextRegistrationGeneration = 0;
    std::map<std::string, uint64> ByteStreamTopics;
    std::map<std::string, uint64> RpcMethods;

    std::mutex SpeakerMutex;
    std::unordered_set<std::string> ActiveSpeakers;

    std::mutex PendingRpcMutex;
    std::unordered_map<std::string, std::shared_ptr<FPendingRpc>> PendingRpcs;
    bool bRpcAdmissionOpen = false;
    uint64 RpcAdmissionGeneration = 0;

    std::mutex ByteStreamReaderMutex;
    std::unordered_map<const livekit::ByteStreamReader*, FTrackedByteStreamReader>
        TrackedByteStreamReaders;

    bool TryStartTask(bool bByteStreamReader)
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

    void TaskFinished(bool bByteStreamReader)
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

    void StopAcceptingTasks()
    {
        std::lock_guard Lock(TaskMutex);
        bAcceptingTasks = false;
    }

    void WaitForTasks()
    {
        std::unique_lock Lock(TaskMutex);
        TasksFinished.wait(Lock, [this]()
        {
            return ActiveTasks == 0;
        });
    }

    bool BeginShutdown()
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

    void CompleteShutdown()
    {
        {
            std::lock_guard Lock(ShutdownMutex);
            ShutdownState = EShutdownState::Complete;
        }
        ShutdownFinished.notify_all();
    }

    static void CloseByteStreamReaders(
        const std::vector<std::shared_ptr<livekit::ByteStreamReader>>& Readers)
    {
        for (const std::shared_ptr<livekit::ByteStreamReader>& Reader : Readers)
        {
            if (!Reader)
            {
                continue;
            }
            try
            {
                LiveKitWindowsSdkV130Access::CloseByteStreamReader(*Reader);
            }
            catch (...)
            {
                // Waking readers is best-effort. The tracked task remains part
                // of the shutdown barrier if the pinned SDK ever changes.
            }
        }
    }

    bool TryTrackByteStreamReader(
        const std::shared_ptr<livekit::ByteStreamReader>& Reader,
        const livekit::Room* CandidateRoom,
        const std::string& Topic,
        uint64 InConnectionGeneration,
        uint64 InRegistrationGeneration)
    {
        if (!Reader || !CandidateRoom)
        {
            return false;
        }

        // Room -> registration -> reader is the admission lock order. A
        // disconnect changes the room generation before cancelling readers;
        // an unregister removes the registration before cancelling its topic.
        // Therefore every admitted reader is either observed by cancellation
        // or rejected after the state transition.
        std::lock_guard RoomLock(RoomMutex);
        if (Room.get() != CandidateRoom || RoomGeneration != InConnectionGeneration ||
            RoomGeneration != ConnectionGeneration.load(std::memory_order_acquire) ||
            !bConnectionRequested.load(std::memory_order_acquire) ||
            bShuttingDown.load(std::memory_order_acquire))
        {
            return false;
        }

        std::lock_guard RegistrationLock(RegistrationMutex);
        const auto Registration = ByteStreamTopics.find(Topic);
        if (Registration == ByteStreamTopics.end() ||
            Registration->second != InRegistrationGeneration)
        {
            return false;
        }

        std::lock_guard ReaderLock(ByteStreamReaderMutex);
        return TrackedByteStreamReaders.emplace(
            Reader.get(),
            FTrackedByteStreamReader{
                Reader,
                Topic,
                InConnectionGeneration,
                InRegistrationGeneration}).second;
    }

    void UntrackByteStreamReader(const livekit::ByteStreamReader* Reader)
    {
        std::lock_guard Lock(ByteStreamReaderMutex);
        TrackedByteStreamReaders.erase(Reader);
    }

    void CancelTrackedByteStreamReader(const livekit::ByteStreamReader* Reader)
    {
        std::vector<std::shared_ptr<livekit::ByteStreamReader>> ReaderToClose;
        {
            std::lock_guard Lock(ByteStreamReaderMutex);
            const auto Iterator = TrackedByteStreamReaders.find(Reader);
            if (Iterator != TrackedByteStreamReaders.end())
            {
                ReaderToClose.push_back(Iterator->second.Reader);
                TrackedByteStreamReaders.erase(Iterator);
            }
        }
        CloseByteStreamReaders(ReaderToClose);
    }

    void CancelAllTrackedByteStreamReaders()
    {
        std::vector<std::shared_ptr<livekit::ByteStreamReader>> Readers;
        {
            std::lock_guard Lock(ByteStreamReaderMutex);
            Readers.reserve(TrackedByteStreamReaders.size());
            for (const auto& Pair : TrackedByteStreamReaders)
            {
                Readers.push_back(Pair.second.Reader);
            }
            TrackedByteStreamReaders.clear();
        }
        CloseByteStreamReaders(Readers);
    }

    void CancelTrackedByteStreamReadersForTopic(const std::string& Topic)
    {
        std::vector<std::shared_ptr<livekit::ByteStreamReader>> Readers;
        {
            std::lock_guard Lock(ByteStreamReaderMutex);
            for (auto Iterator = TrackedByteStreamReaders.begin();
                 Iterator != TrackedByteStreamReaders.end();)
            {
                if (Iterator->second.Topic == Topic)
                {
                    Readers.push_back(Iterator->second.Reader);
                    Iterator = TrackedByteStreamReaders.erase(Iterator);
                }
                else
                {
                    ++Iterator;
                }
            }
        }
        CloseByteStreamReaders(Readers);
    }

    uint64 ReserveControlTicket()
    {
        std::lock_guard Lock(ControlOrderMutex);
        return NextControlTicket++;
    }

    void WaitForControlTurn(uint64 Ticket)
    {
        std::unique_lock Lock(ControlOrderMutex);
        ControlTurn.wait(Lock, [this, Ticket]()
        {
            return ServingControlTicket == Ticket;
        });
    }

    void AdvancePastAbandonedControlTickets()
    {
        while (AbandonedControlTickets.erase(ServingControlTicket) > 0)
        {
            ++ServingControlTicket;
        }
    }

    void FinishControlTicket(uint64 Ticket)
    {
        {
            std::lock_guard Lock(ControlOrderMutex);
            check(ServingControlTicket == Ticket);
            ++ServingControlTicket;
            AdvancePastAbandonedControlTickets();
        }
        ControlTurn.notify_all();
    }

    void AbandonControlTicket(uint64 Ticket)
    {
        {
            std::lock_guard Lock(ControlOrderMutex);
            AbandonedControlTickets.insert(Ticket);
            AdvancePastAbandonedControlTickets();
        }
        ControlTurn.notify_all();
    }

    uint64 BeginConnectionRequest()
    {
        uint64 Generation = 0;
        {
            std::lock_guard Lock(RoomMutex);
            bConnectionRequested.store(true, std::memory_order_release);
            Generation = ConnectionGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        }
        CancelPendingRpcs("The LiveKit room connection was replaced.");
        CancelAllTrackedByteStreamReaders();
        return Generation;
    }

    uint64 InvalidateConnectionRequest()
    {
        uint64 Generation = 0;
        {
            std::lock_guard Lock(RoomMutex);
            bConnectionRequested.store(false, std::memory_order_release);
            Generation = ConnectionGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        }
        CancelPendingRpcs("The LiveKit room disconnected.");
        CancelAllTrackedByteStreamReaders();
        return Generation;
    }

    bool IsConnectionRequestCurrent(uint64 Generation)
    {
        std::lock_guard Lock(RoomMutex);
        return !bShuttingDown.load(std::memory_order_acquire) &&
            bConnectionRequested.load(std::memory_order_acquire) &&
            ConnectionGeneration.load(std::memory_order_acquire) == Generation;
    }

    bool IsDisconnectRequestCurrent(uint64 Generation)
    {
        std::lock_guard Lock(RoomMutex);
        return !bShuttingDown.load(std::memory_order_acquire) &&
            !bConnectionRequested.load(std::memory_order_acquire) &&
            ConnectionGeneration.load(std::memory_order_acquire) == Generation;
    }

    std::shared_ptr<livekit::Room> GetRoom()
    {
        std::lock_guard Lock(RoomMutex);
        return Room;
    }

    bool GetCurrentRoom(
        std::shared_ptr<livekit::Room>& OutRoom,
        uint64& OutGeneration)
    {
        std::lock_guard Lock(RoomMutex);
        if (!Room ||
            RoomGeneration != ConnectionGeneration.load(std::memory_order_acquire) ||
            !bConnectionRequested.load(std::memory_order_acquire) ||
            bShuttingDown.load(std::memory_order_acquire))
        {
            return false;
        }
        OutRoom = Room;
        OutGeneration = RoomGeneration;
        return true;
    }

    bool HasCurrentRoom()
    {
        std::lock_guard Lock(RoomMutex);
        return Room &&
            RoomGeneration == ConnectionGeneration.load(std::memory_order_acquire) &&
            bConnectionRequested.load(std::memory_order_acquire) &&
            !bShuttingDown.load(std::memory_order_acquire);
    }

    void SetRoom(std::shared_ptr<livekit::Room> InRoom, uint64 Generation)
    {
        std::lock_guard Lock(RoomMutex);
        Room = MoveTemp(InRoom);
        RoomGeneration = Generation;
        bRoomReady = false;
    }

    void ClearRoomIf(const std::shared_ptr<livekit::Room>& Candidate)
    {
        std::lock_guard Lock(RoomMutex);
        if (Room == Candidate)
        {
            Room.reset();
            RoomGeneration = 0;
            bRoomReady = false;
        }
    }

    bool DisconnectRoomOrQuarantine(const std::shared_ptr<livekit::Room>& Candidate)
    {
        if (!Candidate)
        {
            return true;
        }

        try
        {
            Candidate->setDelegate(nullptr);
            const bool bListenerDrained = Candidate->disconnect();
            if (!bListenerDrained)
            {
                QuarantineLiveKitWindowsRoom(Candidate);
            }
            return bListenerDrained;
        }
        catch (...)
        {
            // A failed disconnect cannot prove that the SDK's raw-this room
            // listener was removed. Keep the Room alive through SDK shutdown.
            QuarantineLiveKitWindowsRoom(Candidate);
            throw;
        }
    }

    bool IsCurrentRoom(const livekit::Room* Candidate, uint64 Generation = 0)
    {
        std::lock_guard Lock(RoomMutex);
        if (!Candidate || Room.get() != Candidate ||
            !bConnectionRequested.load(std::memory_order_acquire) ||
            bShuttingDown.load(std::memory_order_acquire) ||
            RoomGeneration != ConnectionGeneration.load(std::memory_order_acquire))
        {
            return false;
        }
        return Generation == 0 || RoomGeneration == Generation;
    }

    bool MarkRoomReady(const livekit::Room* Candidate, uint64 Generation)
    {
        std::lock_guard Lock(RoomMutex);
        if (!Candidate || Room.get() != Candidate || RoomGeneration != Generation ||
            RoomGeneration != ConnectionGeneration.load(std::memory_order_acquire) ||
            !bConnectionRequested.load(std::memory_order_acquire) ||
            bShuttingDown.load(std::memory_order_acquire))
        {
            return false;
        }
        bRoomReady = true;
        {
            std::lock_guard PendingLock(PendingRpcMutex);
            bRpcAdmissionOpen = true;
            RpcAdmissionGeneration = Generation;
        }
        return true;
    }

    bool IsCurrentRoomReady(const livekit::Room* Candidate)
    {
        std::lock_guard Lock(RoomMutex);
        return Candidate && Room.get() == Candidate && bRoomReady &&
            RoomGeneration == ConnectionGeneration.load(std::memory_order_acquire) &&
            bConnectionRequested.load(std::memory_order_acquire) &&
            !bShuttingDown.load(std::memory_order_acquire);
    }

    bool DetachTerminalRoom(
        const livekit::Room* Candidate,
        std::shared_ptr<livekit::Room>& OutRoom,
        bool& bOutShouldNotify)
    {
        {
            std::lock_guard Lock(RoomMutex);
            if (!Candidate || Room.get() != Candidate)
            {
                return false;
            }

            OutRoom = MoveTemp(Room);
            const uint64 CurrentGeneration =
                ConnectionGeneration.load(std::memory_order_acquire);
            const bool bHasNewerConnectionRequest =
                bConnectionRequested.load(std::memory_order_acquire) &&
                RoomGeneration != CurrentGeneration;
            RoomGeneration = 0;
            bRoomReady = false;

            if (bHasNewerConnectionRequest)
            {
                // The terminal callback belongs to the room being replaced.
                // Preserve the newer request and its generation.
                bOutShouldNotify = false;
            }
            else if (bConnectionRequested.exchange(false, std::memory_order_acq_rel))
            {
                ConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
                bOutShouldNotify = true;
            }
            else
            {
                // Explicit disconnect already invalidated the request. Notify
                // here as well because this callback may detach the room before
                // the queued disconnect work observes it. State is de-duplicated
                // by the subsystem. Shutdown suppresses user-facing callbacks.
                bOutShouldNotify =
                    !bShuttingDown.load(std::memory_order_acquire);
            }
        }
        CancelPendingRpcs("The LiveKit room disconnected.");
        CancelAllTrackedByteStreamReaders();
        return true;
    }

    bool FailConnectionRequestIfCurrent(
        uint64 Generation,
        const std::shared_ptr<livekit::Room>&)
    {
        {
            std::lock_guard Lock(RoomMutex);
            if (ConnectionGeneration.load(std::memory_order_acquire) != Generation)
            {
                return false;
            }
            bConnectionRequested.store(false, std::memory_order_release);
            ConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
        }
        CancelPendingRpcs("The LiveKit room connection failed.");
        CancelAllTrackedByteStreamReaders();
        return true;
    }

    bool IsSpeaking(const std::string& Identity)
    {
        std::lock_guard Lock(SpeakerMutex);
        return ActiveSpeakers.contains(Identity);
    }

    void ClearSpeakers()
    {
        std::lock_guard Lock(SpeakerMutex);
        ActiveSpeakers.clear();
    }

    void ClearAudio()
    {
        std::lock_guard Lock(AudioMutex);
        MicrophoneTrack.reset();
        MicrophoneSource.reset();
        PlatformAudio.reset();
    }

    void EnsurePlatformAudio()
    {
        std::lock_guard Lock(AudioMutex);
        if (!PlatformAudio)
        {
            PlatformAudio = std::make_shared<livekit::PlatformAudio>();
        }
    }

    void SetMicrophoneEnabledOnRoom(
        const std::shared_ptr<livekit::Room>& InRoom,
        bool bEnabled)
    {
        std::shared_ptr<livekit::LocalAudioTrack> ExistingTrack;
        {
            std::lock_guard Lock(AudioMutex);
            ExistingTrack = MicrophoneTrack;

            if (bEnabled && !ExistingTrack && !PlatformAudio)
            {
                PlatformAudio = std::make_shared<livekit::PlatformAudio>();
            }
        }

        if (ExistingTrack)
        {
            if (bEnabled)
            {
                ExistingTrack->unmute();
            }
            else
            {
                ExistingTrack->mute();
            }
            return;
        }
        if (!bEnabled)
        {
            return;
        }

        const std::shared_ptr<livekit::LocalParticipant> Participant =
            InRoom ? InRoom->localParticipant().lock() : nullptr;
        if (!Participant)
        {
            throw std::runtime_error("The local LiveKit participant is unavailable.");
        }

        std::shared_ptr<livekit::PlatformAudio> Audio;
        {
            std::lock_guard Lock(AudioMutex);
            Audio = PlatformAudio;
        }

        // Keep newly-created objects temporary until publish succeeds. A failed
        // publish must not leave a cached track that later calls only unmute.
        const std::shared_ptr<livekit::PlatformAudioSource> NewSource =
            Audio->createAudioSource();
        const std::shared_ptr<livekit::LocalAudioTrack> NewTrack =
            livekit::LocalAudioTrack::createLocalAudioTrack(
                "avatar-microphone",
                NewSource);
        livekit::TrackPublishOptions Options;
        Options.source = livekit::TrackSource::SOURCE_MICROPHONE;
        Participant->publishTrack(NewTrack, Options);
        if (IsCurrentRoom(InRoom.get()))
        {
            std::lock_guard Lock(AudioMutex);
            MicrophoneSource = NewSource;
            MicrophoneTrack = NewTrack;
        }
    }

    bool IsByteStreamRegistrationCurrent(
        const std::string& Topic,
        uint64 Generation)
    {
        std::lock_guard Lock(RegistrationMutex);
        const auto Iterator = ByteStreamTopics.find(Topic);
        return Iterator != ByteStreamTopics.end() && Iterator->second == Generation;
    }

    bool IsRpcRegistrationCurrent(
        const std::string& Method,
        uint64 Generation)
    {
        std::lock_guard Lock(RegistrationMutex);
        const auto Iterator = RpcMethods.find(Method);
        return Iterator != RpcMethods.end() && Iterator->second == Generation;
    }

    bool RemoveByteStreamRegistrationIfCurrent(
        const std::string& Topic,
        uint64 Generation)
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

    bool RemoveRpcRegistrationIfCurrent(
        const std::string& Method,
        uint64 Generation)
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

    void InstallByteStreamHandler(
        const std::shared_ptr<livekit::Room>& InRoom,
        const std::string& Topic,
        uint64 InConnectionGeneration,
        uint64 InRegistrationGeneration)
    {
        const TWeakPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe> WeakOwner = Owner;
        const std::weak_ptr<livekit::Room> WeakRoom = InRoom;
        InRoom->registerByteStreamHandler(
            Topic,
            [
                WeakOwner,
                WeakRoom,
                Topic,
                InConnectionGeneration,
                InRegistrationGeneration](
                std::shared_ptr<livekit::ByteStreamReader> Reader,
                const std::string& ParticipantIdentity)
            {
                if (const TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe> Pinned = WeakOwner.Pin())
                {
                    const std::shared_ptr<livekit::Room> HandlerRoom = WeakRoom.lock();
                    if (!HandlerRoom ||
                        !Pinned->Implementation->TryTrackByteStreamReader(
                            Reader,
                            HandlerRoom.get(),
                            Topic,
                            InConnectionGeneration,
                            InRegistrationGeneration))
                    {
                        return;
                    }

                    const livekit::ByteStreamReader* ReaderIdentity = Reader.get();
                    const bool bDispatched = Pinned->DispatchByteStream(
                        [
                            Reader,
                            ParticipantIdentity,
                            HandlerRoom,
                            Topic,
                            InConnectionGeneration,
                            InRegistrationGeneration](FLiveKitWindowsBridge& Bridge)
                        {
                            ON_SCOPE_EXIT
                            {
                                Bridge.Implementation->UntrackByteStreamReader(Reader.get());
                            };

                            bool bTooLarge = false;
                            TArray<uint8> Bytes;
                            const livekit::ByteStreamInfo& InitialInfo = Reader->info();
                            if (InitialInfo.size.has_value() &&
                                InitialInfo.size.value() >
                                    static_cast<size_t>(LiveKitLimits::MaxIncomingByteStreamBytes))
                            {
                                bTooLarge = true;
                                LiveKitWindowsSdkV130Access::CloseByteStreamReader(*Reader);
                            }

                            try
                            {
                                std::vector<std::uint8_t> Chunk;
                                while (!bTooLarge && Reader->readNext(Chunk))
                                {
                                    if (!Bridge.Implementation->IsCurrentRoom(
                                            HandlerRoom.get(),
                                            InConnectionGeneration) ||
                                        !Bridge.Implementation->IsByteStreamRegistrationCurrent(
                                            Topic,
                                            InRegistrationGeneration))
                                    {
                                        return;
                                    }
                                    if (!bTooLarge &&
                                        Chunk.size() <= static_cast<size_t>(MAX_int32) &&
                                        Bytes.Num() <=
                                            LiveKitLimits::MaxIncomingByteStreamBytes -
                                                static_cast<int32>(Chunk.size()))
                                    {
                                        if (!Chunk.empty())
                                        {
                                            Bytes.Append(
                                                Chunk.data(),
                                                static_cast<int32>(Chunk.size()));
                                        }
                                    }
                                    else
                                    {
                                        bTooLarge = true;
                                        LiveKitWindowsSdkV130Access::CloseByteStreamReader(*Reader);
                                        break;
                                    }
                                    Chunk.clear();
                                }
                            }
                            catch (const std::exception& Error)
                            {
                                if (Bridge.Implementation->IsCurrentRoom(
                                        HandlerRoom.get(),
                                        InConnectionGeneration))
                                {
                                    Bridge.ErrorHandler(ExceptionError(
                                        TEXT("byte_stream_read_failed"),
                                        Error));
                                }
                                return;
                            }

                            if (!Bridge.Implementation->IsCurrentRoom(
                                    HandlerRoom.get(),
                                    InConnectionGeneration) ||
                                !Bridge.Implementation->IsByteStreamRegistrationCurrent(
                                    Topic,
                                    InRegistrationGeneration))
                            {
                                return;
                            }

                            if (bTooLarge)
                            {
                                Bridge.ErrorHandler(MakeLiveKitError(
                                    TEXT("byte_stream_too_large"),
                                    FString::Printf(
                                        TEXT("Incoming byte streams may not exceed %d bytes."),
                                        LiveKitLimits::MaxIncomingByteStreamBytes)));
                                return;
                            }

                            const livekit::ByteStreamInfo& Info = Reader->info();
                            FLiveKitByteStream Stream;
                            Stream.SenderIdentity = FromLiveKitString(ParticipantIdentity);
                            Stream.StreamId = FromLiveKitString(Info.stream_id);
                            Stream.Topic = FromLiveKitString(Info.topic);
                            Stream.Name = FromLiveKitString(Info.name);
                            Stream.MimeType = FromLiveKitString(Info.mime_type);
                            for (const auto& Pair : Info.attributes)
                            {
                                Stream.Attributes.Add(
                                    FromLiveKitString(Pair.first),
                                    FromLiveKitString(Pair.second));
                            }
                            Stream.Data = MoveTemp(Bytes);
                            Bridge.ByteStreamHandler(Stream);
                        });

                    if (!bDispatched)
                    {
                        Pinned->Implementation->CancelTrackedByteStreamReader(ReaderIdentity);
                    }
                    if (!bDispatched &&
                        Pinned->Implementation->IsCurrentRoom(
                            HandlerRoom.get(),
                            InConnectionGeneration))
                    {
                        Pinned->ErrorHandler(MakeLiveKitError(
                            TEXT("byte_stream_reader_limit"),
                            FString::Printf(
                                TEXT("At most %d LiveKit byte streams may be read concurrently."),
                                FImplementation::MaxConcurrentByteStreamReaders)));
                    }
                }
            });
    }

    bool TryAdmitPendingRpc(
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

    std::optional<std::string> HandleIncomingRpc(
        const std::string& Method,
        const livekit::RpcInvocationData& Data,
        const std::shared_ptr<livekit::Room>& HandlerRoom,
        uint64 InConnectionGeneration,
        uint64 InRegistrationGeneration)
    {
        const TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe> Pinned = Owner.Pin();
        if (!Pinned ||
            !IsCurrentRoom(HandlerRoom.get(), InConnectionGeneration) ||
            !IsRpcRegistrationCurrent(Method, InRegistrationGeneration))
        {
            throw livekit::RpcError::builtIn(livekit::RpcError::ErrorCode::APPLICATION_ERROR);
        }

        const std::shared_ptr<FPendingRpc> Pending = std::make_shared<FPendingRpc>();
        if (!TryAdmitPendingRpc(Data.request_id, Pending, InConnectionGeneration))
        {
            throw livekit::RpcError::builtIn(
                livekit::RpcError::ErrorCode::APPLICATION_ERROR);
        }
        ON_SCOPE_EXIT
        {
            std::lock_guard Lock(PendingRpcMutex);
            const auto Iterator = PendingRpcs.find(Data.request_id);
            if (Iterator != PendingRpcs.end() && Iterator->second == Pending)
            {
                PendingRpcs.erase(Iterator);
            }
        };

        FLiveKitRpcInvocation Invocation;
        Invocation.RequestId = FromLiveKitString(Data.request_id);
        Invocation.CallerIdentity = FromLiveKitString(Data.caller_identity);
        Invocation.Method = FromLiveKitString(Method);
        Invocation.Payload = FromLiveKitString(Data.payload);
        Invocation.ResponseTimeoutSeconds = static_cast<float>(Data.response_timeout_sec);
        Pinned->RpcInvocationHandler(Invocation);

        const double TimeoutSeconds = FMath::Max(0.1, Data.response_timeout_sec);
        bool bReady = false;
        {
            std::unique_lock Lock(Pending->Mutex);
            bReady = Pending->Ready.wait_for(
                Lock,
                std::chrono::duration<double>(TimeoutSeconds),
                [&Pending]() { return Pending->bCompleted; });
        }

        if (!bReady)
        {
            throw livekit::RpcError::builtIn(livekit::RpcError::ErrorCode::RESPONSE_TIMEOUT);
        }
        if (Pending->bFailed)
        {
            throw livekit::RpcError(
                Pending->ErrorCode,
                Pending->ErrorMessage,
                Pending->ErrorData);
        }
        return Pending->Response;
    }

    void InstallRpcMethod(
        const std::shared_ptr<livekit::Room>& InRoom,
        const std::string& Method,
        uint64 InConnectionGeneration,
        uint64 InRegistrationGeneration)
    {
        const std::shared_ptr<livekit::LocalParticipant> Participant =
            InRoom ? InRoom->localParticipant().lock() : nullptr;
        if (!Participant)
        {
            throw std::runtime_error("The local LiveKit participant is unavailable.");
        }

        const TWeakPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe> WeakOwner = Owner;
        const std::weak_ptr<livekit::Room> WeakRoom = InRoom;
        Participant->registerRpcMethod(
            Method,
            [
                WeakOwner,
                WeakRoom,
                Method,
                InConnectionGeneration,
                InRegistrationGeneration](const livekit::RpcInvocationData& Data)
                -> std::optional<std::string>
            {
                const TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe> Pinned = WeakOwner.Pin();
                const std::shared_ptr<livekit::Room> HandlerRoom = WeakRoom.lock();
                if (!Pinned || !Pinned->Implementation || !HandlerRoom ||
                    !Pinned->Implementation->IsCurrentRoom(
                        HandlerRoom.get(),
                        InConnectionGeneration) ||
                    !Pinned->Implementation->IsRpcRegistrationCurrent(
                        Method,
                        InRegistrationGeneration))
                {
                    throw livekit::RpcError::builtIn(
                        livekit::RpcError::ErrorCode::APPLICATION_ERROR);
                }
                return Pinned->Implementation->HandleIncomingRpc(
                    Method,
                    Data,
                    HandlerRoom,
                    InConnectionGeneration,
                    InRegistrationGeneration);
            });
    }

    void RestoreRegistrations(
        const std::shared_ptr<livekit::Room>& InRoom,
        uint64 InConnectionGeneration,
        FLiveKitWindowsBridge& Bridge)
    {
        std::map<std::string, uint64> Topics;
        std::map<std::string, uint64> Methods;
        {
            std::lock_guard Lock(RegistrationMutex);
            Topics = ByteStreamTopics;
            Methods = RpcMethods;
        }

        for (const auto& Pair : Topics)
        {
            const std::string& Topic = Pair.first;
            const uint64 RegistrationGeneration = Pair.second;
            try
            {
                if (!IsCurrentRoom(InRoom.get(), InConnectionGeneration) ||
                    !IsByteStreamRegistrationCurrent(Topic, RegistrationGeneration))
                {
                    continue;
                }
                InstallByteStreamHandler(
                    InRoom,
                    Topic,
                    InConnectionGeneration,
                    RegistrationGeneration);
                if (!IsCurrentRoom(InRoom.get(), InConnectionGeneration) ||
                    !IsByteStreamRegistrationCurrent(Topic, RegistrationGeneration))
                {
                    InRoom->unregisterByteStreamHandler(Topic);
                    continue;
                }
                Bridge.ByteStreamRegistrationHandler(
                    FromLiveKitString(Topic),
                    true,
                    FLiveKitError());
            }
            catch (const std::exception& Error)
            {
                if (IsCurrentRoom(InRoom.get(), InConnectionGeneration) &&
                    RemoveByteStreamRegistrationIfCurrent(Topic, RegistrationGeneration))
                {
                    Bridge.ByteStreamRegistrationHandler(
                        FromLiveKitString(Topic),
                        false,
                        ExceptionError(TEXT("byte_stream_registration_failed"), Error));
                }
            }
        }

        for (const auto& Pair : Methods)
        {
            const std::string& Method = Pair.first;
            const uint64 RegistrationGeneration = Pair.second;
            try
            {
                if (!IsCurrentRoom(InRoom.get(), InConnectionGeneration) ||
                    !IsRpcRegistrationCurrent(Method, RegistrationGeneration))
                {
                    continue;
                }
                InstallRpcMethod(
                    InRoom,
                    Method,
                    InConnectionGeneration,
                    RegistrationGeneration);
                if (!IsCurrentRoom(InRoom.get(), InConnectionGeneration) ||
                    !IsRpcRegistrationCurrent(Method, RegistrationGeneration))
                {
                    if (const std::shared_ptr<livekit::LocalParticipant> Participant =
                            InRoom->localParticipant().lock())
                    {
                        Participant->unregisterRpcMethod(Method);
                    }
                    continue;
                }
                Bridge.RpcRegistrationHandler(
                    FromLiveKitString(Method),
                    true,
                    FLiveKitError());
            }
            catch (const std::exception& Error)
            {
                if (IsCurrentRoom(InRoom.get(), InConnectionGeneration) &&
                    RemoveRpcRegistrationIfCurrent(Method, RegistrationGeneration))
                {
                    Bridge.RpcRegistrationHandler(
                        FromLiveKitString(Method),
                        false,
                        ExceptionError(TEXT("rpc_registration_failed"), Error));
                }
            }
        }
    }

    bool ResolvePendingRpc(
        const FString& RequestId,
        const FString* Response,
        int32 ErrorCode,
        const FString& ErrorMessage,
        const FString& ErrorData)
    {
        std::shared_ptr<FPendingRpc> Pending;
        {
            std::lock_guard Lock(PendingRpcMutex);
            const auto Iterator = PendingRpcs.find(ToLiveKitString(RequestId));
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
                Pending->Response = ToLiveKitString(*Response);
            }
            else
            {
                Pending->ErrorCode = static_cast<std::uint32_t>(FMath::Max(ErrorCode, 0));
                Pending->ErrorMessage = ToLiveKitString(ErrorMessage);
                Pending->ErrorData = ToLiveKitString(ErrorData);
            }
        }
        Pending->Ready.notify_all();
        return true;
    }

    void CancelPendingRpcs(const char* Reason = "The LiveKit client is shutting down.")
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
                    Invocation->ErrorMessage = Reason;
                }
            }
            Invocation->Ready.notify_all();
        }
    }

    virtual void onParticipantConnected(
        livekit::Room& InRoom,
        const livekit::ParticipantConnectedEvent& Event) override
    {
        if (Event.participant != nullptr && IsCurrentRoom(&InRoom))
        {
            if (const TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe> Pinned = Owner.Pin())
            {
                Pinned->ParticipantConnectedHandler(ToParticipantInfo(
                    *Event.participant,
                    IsSpeaking(Event.participant->identity())));
            }
        }
    }

    virtual void onParticipantDisconnected(
        livekit::Room& InRoom,
        const livekit::ParticipantDisconnectedEvent& Event) override
    {
        if (Event.participant != nullptr && IsCurrentRoom(&InRoom))
        {
            if (const TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe> Pinned = Owner.Pin())
            {
                Pinned->ParticipantDisconnectedHandler(ToParticipantInfo(
                    *Event.participant,
                    IsSpeaking(Event.participant->identity())));
            }
            std::lock_guard Lock(SpeakerMutex);
            ActiveSpeakers.erase(Event.participant->identity());
        }
    }

    virtual void onActiveSpeakersChanged(
        livekit::Room& InRoom,
        const livekit::ActiveSpeakersChangedEvent& Event) override
    {
        if (!IsCurrentRoom(&InRoom))
        {
            return;
        }

        std::unordered_set<std::string> NewSpeakers;
        for (const livekit::Participant* Participant : Event.speakers)
        {
            if (Participant != nullptr && InRoom.remoteParticipant(Participant->identity()).lock())
            {
                NewSpeakers.insert(Participant->identity());
            }
        }

        std::unordered_set<std::string> Changed;
        {
            std::lock_guard Lock(SpeakerMutex);
            for (const std::string& Identity : ActiveSpeakers)
            {
                if (!NewSpeakers.contains(Identity))
                {
                    Changed.insert(Identity);
                }
            }
            for (const std::string& Identity : NewSpeakers)
            {
                if (!ActiveSpeakers.contains(Identity))
                {
                    Changed.insert(Identity);
                }
            }
            ActiveSpeakers = NewSpeakers;
        }

        if (const TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe> Pinned = Owner.Pin())
        {
            for (const std::string& Identity : Changed)
            {
                if (const std::shared_ptr<livekit::RemoteParticipant> Participant =
                        InRoom.remoteParticipant(Identity).lock())
                {
                    const bool bSpeaking = NewSpeakers.contains(Identity);
                    Pinned->SpeakingHandler(ToParticipantInfo(*Participant, bSpeaking), bSpeaking);
                }
            }
        }
    }

    virtual void onConnectionStateChanged(
        livekit::Room& InRoom,
        const livekit::ConnectionStateChangedEvent& Event) override
    {
        if (!IsCurrentRoomReady(&InRoom))
        {
            return;
        }

        if (const TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe> Pinned = Owner.Pin())
        {
            switch (Event.state)
            {
            case livekit::ConnectionState::Connected:
                Pinned->StateHandler(ELiveKitConnectionState::Connected);
                break;
            case livekit::ConnectionState::Reconnecting:
                Pinned->StateHandler(ELiveKitConnectionState::Reconnecting);
                break;
            case livekit::ConnectionState::Disconnected:
            default:
                Pinned->StateHandler(ELiveKitConnectionState::Disconnected);
                break;
            }
        }
    }

    void HandleTerminalRoom(livekit::Room& InRoom)
    {
        std::shared_ptr<livekit::Room> TerminalRoom;
        bool bShouldNotify = false;
        if (!DetachTerminalRoom(&InRoom, TerminalRoom, bShouldNotify))
        {
            return;
        }

        // Room 1.3.0 keeps a raw-this FFI listener after the Disconnected
        // event and removes it only at EOS. Never destroy a terminal Room from
        // either delegate callback; global SDK shutdown is the drain barrier.
        QuarantineLiveKitWindowsRoom(TerminalRoom);
        TerminalRoom->setDelegate(nullptr);
        ClearAudio();
        ClearSpeakers();

        if (bShouldNotify)
        {
            if (const TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe> Pinned = Owner.Pin())
            {
                Pinned->StateHandler(ELiveKitConnectionState::Disconnected);
            }
        }
    }

    virtual void onDisconnected(
        livekit::Room& InRoom,
        const livekit::DisconnectedEvent&) override
    {
        HandleTerminalRoom(InRoom);
    }

    virtual void onRoomEos(
        livekit::Room& InRoom,
        const livekit::RoomEosEvent&) override
    {
        HandleTerminalRoom(InRoom);
    }

    virtual void onReconnecting(
        livekit::Room& InRoom,
        const livekit::ReconnectingEvent&) override
    {
        if (!IsCurrentRoomReady(&InRoom))
        {
            return;
        }
        if (const TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe> Pinned = Owner.Pin())
        {
            Pinned->StateHandler(ELiveKitConnectionState::Reconnecting);
        }
    }

    virtual void onReconnected(
        livekit::Room& InRoom,
        const livekit::ReconnectedEvent&) override
    {
        if (!IsCurrentRoomReady(&InRoom))
        {
            return;
        }
        if (const TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe> Pinned = Owner.Pin())
        {
            Pinned->StateHandler(ELiveKitConnectionState::Connected);
        }
    }

    virtual void onUserPacketReceived(
        livekit::Room& InRoom,
        const livekit::UserDataPacketEvent& Event) override
    {
        if (!IsCurrentRoom(&InRoom))
        {
            return;
        }
        if (const TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe> Pinned = Owner.Pin())
        {
            FLiveKitDataMessage Message;
            if (Event.participant != nullptr)
            {
                Message.SenderIdentity = FromLiveKitString(Event.participant->identity());
            }
            Message.Topic = FromLiveKitString(Event.topic);
            if (Event.data.size() <= static_cast<size_t>(MAX_int32))
            {
                Message.Data.Append(Event.data.data(), static_cast<int32>(Event.data.size()));
            }
            Pinned->DataHandler(Message);
        }
    }
};

#else

struct FLiveKitWindowsBridge::FImplementation
{
    std::atomic_bool bShuttingDown{false};
};

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

FLiveKitWindowsBridge::~FLiveKitWindowsBridge()
{
    Shutdown();
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
        TEXT("The verified LiveKit C++ SDK is not installed or initialized for this Win64 build."),
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
            {
                std::lock_guard ControlLock(Self->Implementation->ControlMutex);
                if (!Self->Implementation->bShuttingDown.load(std::memory_order_acquire))
                {
                    try
                    {
                        Work(Self.Get());
                    }
                    catch (const std::exception& Error)
                    {
                        Self->ErrorHandler(ExceptionError(TEXT("windows_backend_error"), Error));
                    }
                    catch (...)
                    {
                        Self->ErrorHandler(MakeLiveKitError(
                            TEXT("windows_backend_error"),
                            TEXT("The LiveKit Windows backend raised an unknown error.")));
                    }
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

bool FLiveKitWindowsBridge::DispatchByteStream(
    TFunction<void(FLiveKitWindowsBridge&)> Work)
{
#if WITH_LIVEKIT_WINDOWS
    if (!Implementation || !Implementation->TryStartTask(true))
    {
        return false;
    }

    const TSharedRef<FLiveKitWindowsBridge, ESPMode::ThreadSafe> Self = AsShared();
    try
    {
        Async(EAsyncExecution::Thread, [Self, Work = MoveTemp(Work)]() mutable
        {
            ON_SCOPE_EXIT
            {
                Self->Implementation->TaskFinished(true);
            };

            if (!Self->Implementation->bShuttingDown.load(std::memory_order_acquire))
            {
                try
                {
                    Work(Self.Get());
                }
                catch (const std::exception& Error)
                {
                    Self->ErrorHandler(ExceptionError(TEXT("byte_stream_read_failed"), Error));
                }
                catch (...)
                {
                    Self->ErrorHandler(MakeLiveKitError(
                        TEXT("byte_stream_read_failed"),
                        TEXT("A LiveKit byte-stream reader raised an unknown error.")));
                }
            }
        });
    }
    catch (...)
    {
        Implementation->TaskFinished(true);
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
        if (const std::shared_ptr<livekit::Room> Room = Implementation->GetRoom())
        {
            try
            {
                Implementation->DisconnectRoomOrQuarantine(Room);
            }
            catch (...)
            {
                // The helper quarantines on failure; shutdown remains best-effort.
            }
            Implementation->ClearRoomIf(Room);
        }
        Implementation->ClearAudio();
        Implementation->ClearSpeakers();
    }

    Implementation->WaitForTasks();
    Implementation->Owner.Reset();
#else
    Implementation->bShuttingDown.store(true, std::memory_order_release);
#endif
}

void FLiveKitWindowsBridge::Connect(
    const FString& ServerUrl,
    const FString& Token,
    bool bEnableMicrophone,
    bool /* bEnableVoiceProcessing */)
{
    if (!IsSdkAvailable())
    {
        ReportUnavailable(TEXT("connect"));
        StateHandler(ELiveKitConnectionState::Failed);
        return;
    }

#if WITH_LIVEKIT_WINDOWS
    const uint64 ConnectionGeneration = Implementation->BeginConnectionRequest();
    StateHandler(ELiveKitConnectionState::Connecting);
    const bool bDispatched = DispatchControl([
        ServerUrl,
        Token,
        bEnableMicrophone,
        ConnectionGeneration](FLiveKitWindowsBridge& Bridge)
    {
#if WITH_LIVEKIT_WINDOWS
        if (!Bridge.Implementation->IsConnectionRequestCurrent(ConnectionGeneration))
        {
            return;
        }

        std::shared_ptr<livekit::Room> Room;
        bool bRoomListenerKnownDrained = false;
        auto CleanupRoom = [&Bridge, &Room, &bRoomListenerKnownDrained]()
        {
            if (Room)
            {
                if (!bRoomListenerKnownDrained)
                {
                    try
                    {
                        Bridge.Implementation->DisconnectRoomOrQuarantine(Room);
                    }
                    catch (...)
                    {
                    }
                }
                else
                {
                    // Room::connect(false) already removes and drains the SDK
                    // listener, so this never-connected instance is safe to drop.
                    Room->setDelegate(nullptr);
                }
                Bridge.Implementation->ClearRoomIf(Room);
            }
            Bridge.Implementation->ClearAudio();
            Bridge.Implementation->ClearSpeakers();
        };

        try
        {
            if (const std::shared_ptr<livekit::Room> PreviousRoom =
                    Bridge.Implementation->GetRoom())
            {
                try
                {
                    Bridge.Implementation->DisconnectRoomOrQuarantine(PreviousRoom);
                }
                catch (...)
                {
                }
                Bridge.Implementation->ClearRoomIf(PreviousRoom);
            }
            Bridge.Implementation->ClearAudio();
            Bridge.Implementation->ClearSpeakers();

            if (!Bridge.Implementation->IsConnectionRequestCurrent(ConnectionGeneration))
            {
                return;
            }

            Room = std::make_shared<livekit::Room>();
            Room->setDelegate(Bridge.Implementation.Get());
            Bridge.Implementation->SetRoom(Room, ConnectionGeneration);

            try
            {
                Bridge.Implementation->EnsurePlatformAudio();
            }
            catch (const std::exception& Error)
            {
                Bridge.ErrorHandler(ExceptionError(TEXT("audio_initialization_failed"), Error));
            }

            livekit::RoomOptions Options;
            Options.auto_subscribe = true;
            Options.connect_timeout = std::chrono::seconds(15);
            const bool bConnected = Room->connect(
                ToLiveKitString(ServerUrl),
                ToLiveKitString(Token),
                Options);
            if (!bConnected)
            {
                bRoomListenerKnownDrained = true;
                CleanupRoom();
                if (Bridge.Implementation->FailConnectionRequestIfCurrent(
                        ConnectionGeneration,
                        Room))
                {
                    Bridge.ErrorHandler(MakeLiveKitError(
                        TEXT("connect_failed"),
                        TEXT("The LiveKit C++ SDK did not connect to the room.")));
                    Bridge.StateHandler(ELiveKitConnectionState::Failed);
                }
                return;
            }

            if (!Bridge.Implementation->IsCurrentRoom(Room.get(), ConnectionGeneration))
            {
                CleanupRoom();
                return;
            }

            try
            {
                Bridge.Implementation->SetMicrophoneEnabledOnRoom(Room, bEnableMicrophone);
            }
            catch (const std::exception& Error)
            {
                Bridge.ErrorHandler(ExceptionError(TEXT("microphone_publish_failed"), Error));
            }

            if (!Bridge.Implementation->IsCurrentRoom(Room.get(), ConnectionGeneration))
            {
                CleanupRoom();
                return;
            }

            Bridge.Implementation->RestoreRegistrations(
                Room,
                ConnectionGeneration,
                Bridge);
            if (Bridge.Implementation->MarkRoomReady(Room.get(), ConnectionGeneration))
            {
                // The SDK can finish its initial participant synchronization before
                // the room delegate becomes observable to this bridge. Seed the
                // current snapshot so callers always receive every participant that
                // was already in the room when connect completed. The subsystem
                // de-duplicates any delegate event that raced with this snapshot.
                for (const std::weak_ptr<livekit::RemoteParticipant>& WeakParticipant :
                     Room->remoteParticipants())
                {
                    if (const std::shared_ptr<livekit::RemoteParticipant> Participant =
                            WeakParticipant.lock())
                    {
                        Bridge.ParticipantConnectedHandler(ToParticipantInfo(
                            *Participant,
                            Bridge.Implementation->IsSpeaking(Participant->identity())));
                    }
                }
                Bridge.StateHandler(ELiveKitConnectionState::Connected);
            }
            else
            {
                CleanupRoom();
            }
        }
        catch (const std::exception& Error)
        {
            CleanupRoom();
            if (Bridge.Implementation->FailConnectionRequestIfCurrent(
                    ConnectionGeneration,
                    Room))
            {
                Bridge.ErrorHandler(ExceptionError(TEXT("connect_failed"), Error));
                Bridge.StateHandler(ELiveKitConnectionState::Failed);
            }
        }
        catch (...)
        {
            CleanupRoom();
            if (Bridge.Implementation->FailConnectionRequestIfCurrent(
                    ConnectionGeneration,
                    Room))
            {
                Bridge.ErrorHandler(MakeLiveKitError(
                    TEXT("connect_failed"),
                    TEXT("The LiveKit Windows backend raised an unknown error while connecting.")));
                Bridge.StateHandler(ELiveKitConnectionState::Failed);
            }
        }
#endif
    });
    if (!bDispatched &&
        Implementation->FailConnectionRequestIfCurrent(ConnectionGeneration, nullptr))
    {
        StateHandler(ELiveKitConnectionState::Failed);
    }
#endif
}

void FLiveKitWindowsBridge::Disconnect()
{
    if (!IsSdkAvailable())
    {
        StateHandler(ELiveKitConnectionState::Disconnected);
        return;
    }

#if WITH_LIVEKIT_WINDOWS
    const uint64 DisconnectGeneration = Implementation->InvalidateConnectionRequest();
    StateHandler(ELiveKitConnectionState::Disconnecting);
    const bool bDispatched = DispatchControl([DisconnectGeneration](FLiveKitWindowsBridge& Bridge)
    {
#if WITH_LIVEKIT_WINDOWS
        if (const std::shared_ptr<livekit::Room> Room = Bridge.Implementation->GetRoom())
        {
            try
            {
                Bridge.Implementation->DisconnectRoomOrQuarantine(Room);
            }
            catch (const std::exception& Error)
            {
                Bridge.ErrorHandler(ExceptionError(TEXT("disconnect_failed"), Error));
            }
            catch (...)
            {
                Bridge.ErrorHandler(MakeLiveKitError(
                    TEXT("disconnect_failed"),
                    TEXT("The LiveKit room raised an unknown error while disconnecting.")));
            }
            Bridge.Implementation->ClearRoomIf(Room);
        }
        Bridge.Implementation->ClearAudio();
        Bridge.Implementation->ClearSpeakers();
        if (Bridge.Implementation->IsDisconnectRequestCurrent(DisconnectGeneration))
        {
            Bridge.StateHandler(ELiveKitConnectionState::Disconnected);
        }
#endif
    });
    if (!bDispatched && Implementation->IsDisconnectRequestCurrent(DisconnectGeneration))
    {
        StateHandler(ELiveKitConnectionState::Disconnected);
    }
#endif
}

void FLiveKitWindowsBridge::SetMicrophoneEnabled(bool bEnabled)
{
    if (!IsSdkAvailable())
    {
        ReportUnavailable(TEXT("microphone"));
        return;
    }

    DispatchControl([bEnabled](FLiveKitWindowsBridge& Bridge)
    {
#if WITH_LIVEKIT_WINDOWS
        const std::shared_ptr<livekit::Room> Room = Bridge.Implementation->GetRoom();
        if (!Room)
        {
            return;
        }
        try
        {
            Bridge.Implementation->SetMicrophoneEnabledOnRoom(Room, bEnabled);
        }
        catch (const std::exception& Error)
        {
            Bridge.ErrorHandler(ExceptionError(TEXT("microphone_update_failed"), Error));
        }
#endif
    });
}

void FLiveKitWindowsBridge::PublishData(
    const FString& OperationId,
    const TArray<uint8>& Data,
    const FString& Topic,
    ELiveKitDataReliability Reliability,
    const TArray<FString>& DestinationIdentities)
{
    if (!IsSdkAvailable())
    {
        const FLiveKitError Error = MakeLiveKitError(
            TEXT("sdk_unavailable"),
            TEXT("The LiveKit C++ SDK is unavailable for data publishing."));
        PublishResultHandler(OperationId, false, Error);
        return;
    }

    DispatchControl([OperationId, Data, Topic, Reliability, DestinationIdentities](FLiveKitWindowsBridge& Bridge)
    {
#if WITH_LIVEKIT_WINDOWS
        try
        {
            const std::shared_ptr<livekit::Room> Room = Bridge.Implementation->GetRoom();
            const std::shared_ptr<livekit::LocalParticipant> Participant =
                Room ? Room->localParticipant().lock() : nullptr;
            if (!Participant)
            {
                throw std::runtime_error("The local LiveKit participant is unavailable.");
            }

            std::vector<std::uint8_t> Payload;
            if (!Data.IsEmpty())
            {
                Payload.assign(Data.GetData(), Data.GetData() + Data.Num());
            }
            std::vector<std::string> Destinations;
            Destinations.reserve(DestinationIdentities.Num());
            for (const FString& Identity : DestinationIdentities)
            {
                Destinations.push_back(ToLiveKitString(Identity));
            }
            Participant->publishData(
                Payload,
                Reliability == ELiveKitDataReliability::Reliable,
                Destinations,
                ToLiveKitString(Topic));
            Bridge.PublishResultHandler(OperationId, true, FLiveKitError());
        }
        catch (const std::exception& Error)
        {
            Bridge.PublishResultHandler(
                OperationId,
                false,
                ExceptionError(TEXT("publish_failed"), Error));
        }
#endif
    });
}

bool FLiveKitWindowsBridge::RegisterByteStreamHandler(const FString& Topic)
{
    if (!IsSdkAvailable() || Topic.IsEmpty())
    {
        return false;
    }

#if WITH_LIVEKIT_WINDOWS
    const std::string LiveKitTopic = ToLiveKitString(Topic);
    uint64 RegistrationGeneration = 0;
    {
        std::lock_guard Lock(Implementation->RegistrationMutex);
        if (Implementation->ByteStreamTopics.contains(LiveKitTopic))
        {
            return false;
        }
        RegistrationGeneration = ++Implementation->NextRegistrationGeneration;
        Implementation->ByteStreamTopics.emplace(
            LiveKitTopic,
            RegistrationGeneration);
    }

    if (Implementation->HasCurrentRoom())
    {
        const bool bDispatched = DispatchControl([
            Topic,
            LiveKitTopic,
            RegistrationGeneration](FLiveKitWindowsBridge& Bridge)
        {
            std::shared_ptr<livekit::Room> Room;
            uint64 ConnectionGeneration = 0;
            if (!Bridge.Implementation->GetCurrentRoom(Room, ConnectionGeneration) ||
                !Bridge.Implementation->IsByteStreamRegistrationCurrent(
                    LiveKitTopic,
                    RegistrationGeneration))
            {
                return;
            }
            try
            {
                Bridge.Implementation->InstallByteStreamHandler(
                    Room,
                    LiveKitTopic,
                    ConnectionGeneration,
                    RegistrationGeneration);
                if (!Bridge.Implementation->IsCurrentRoom(
                        Room.get(),
                        ConnectionGeneration) ||
                    !Bridge.Implementation->IsByteStreamRegistrationCurrent(
                        LiveKitTopic,
                        RegistrationGeneration))
                {
                    Room->unregisterByteStreamHandler(LiveKitTopic);
                    return;
                }
                Bridge.ByteStreamRegistrationHandler(Topic, true, FLiveKitError());
            }
            catch (const std::exception& Error)
            {
                if (Bridge.Implementation->IsCurrentRoom(
                        Room.get(),
                        ConnectionGeneration) &&
                    Bridge.Implementation->RemoveByteStreamRegistrationIfCurrent(
                        LiveKitTopic,
                        RegistrationGeneration))
                {
                    Bridge.ByteStreamRegistrationHandler(
                        Topic,
                        false,
                        ExceptionError(TEXT("byte_stream_registration_failed"), Error));
                }
            }
        });
        if (!bDispatched)
        {
            Implementation->RemoveByteStreamRegistrationIfCurrent(
                LiveKitTopic,
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
    const std::string LiveKitTopic = ToLiveKitString(Topic);
    {
        std::lock_guard Lock(Implementation->RegistrationMutex);
        Implementation->ByteStreamTopics.erase(LiveKitTopic);
    }
    Implementation->CancelTrackedByteStreamReadersForTopic(LiveKitTopic);
    DispatchControl([LiveKitTopic](FLiveKitWindowsBridge& Bridge)
    {
        if (const std::shared_ptr<livekit::Room> Room = Bridge.Implementation->GetRoom())
        {
            Room->unregisterByteStreamHandler(LiveKitTopic);
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
    const std::string LiveKitMethod = ToLiveKitString(Method);
    uint64 RegistrationGeneration = 0;
    {
        std::lock_guard Lock(Implementation->RegistrationMutex);
        if (Implementation->RpcMethods.contains(LiveKitMethod))
        {
            return false;
        }
        RegistrationGeneration = ++Implementation->NextRegistrationGeneration;
        Implementation->RpcMethods.emplace(
            LiveKitMethod,
            RegistrationGeneration);
    }

    if (Implementation->HasCurrentRoom())
    {
        const bool bDispatched = DispatchControl([
            Method,
            LiveKitMethod,
            RegistrationGeneration](FLiveKitWindowsBridge& Bridge)
        {
            std::shared_ptr<livekit::Room> Room;
            uint64 ConnectionGeneration = 0;
            if (!Bridge.Implementation->GetCurrentRoom(Room, ConnectionGeneration) ||
                !Bridge.Implementation->IsRpcRegistrationCurrent(
                    LiveKitMethod,
                    RegistrationGeneration))
            {
                return;
            }
            try
            {
                Bridge.Implementation->InstallRpcMethod(
                    Room,
                    LiveKitMethod,
                    ConnectionGeneration,
                    RegistrationGeneration);
                if (!Bridge.Implementation->IsCurrentRoom(
                        Room.get(),
                        ConnectionGeneration) ||
                    !Bridge.Implementation->IsRpcRegistrationCurrent(
                        LiveKitMethod,
                        RegistrationGeneration))
                {
                    if (const std::shared_ptr<livekit::LocalParticipant> Participant =
                            Room->localParticipant().lock())
                    {
                        Participant->unregisterRpcMethod(LiveKitMethod);
                    }
                    return;
                }
                Bridge.RpcRegistrationHandler(Method, true, FLiveKitError());
            }
            catch (const std::exception& Error)
            {
                if (Bridge.Implementation->IsCurrentRoom(
                        Room.get(),
                        ConnectionGeneration) &&
                    Bridge.Implementation->RemoveRpcRegistrationIfCurrent(
                        LiveKitMethod,
                        RegistrationGeneration))
                {
                    Bridge.RpcRegistrationHandler(
                        Method,
                        false,
                        ExceptionError(TEXT("rpc_registration_failed"), Error));
                }
            }
        });
        if (!bDispatched)
        {
            Implementation->RemoveRpcRegistrationIfCurrent(
                LiveKitMethod,
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
    const std::string LiveKitMethod = ToLiveKitString(Method);
    {
        std::lock_guard Lock(Implementation->RegistrationMutex);
        Implementation->RpcMethods.erase(LiveKitMethod);
    }
    DispatchControl([LiveKitMethod](FLiveKitWindowsBridge& Bridge)
    {
        const std::shared_ptr<livekit::Room> Room = Bridge.Implementation->GetRoom();
        const std::shared_ptr<livekit::LocalParticipant> Participant =
            Room ? Room->localParticipant().lock() : nullptr;
        if (Participant)
        {
            Participant->unregisterRpcMethod(LiveKitMethod);
        }
    });
#endif
}

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
                TEXT("The LiveKit C++ SDK is unavailable for RPC.")));
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
        try
        {
            const std::shared_ptr<livekit::Room> Room = Bridge.Implementation->GetRoom();
            const std::shared_ptr<livekit::LocalParticipant> Participant =
                Room ? Room->localParticipant().lock() : nullptr;
            if (!Participant)
            {
                throw std::runtime_error("The local LiveKit participant is unavailable.");
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

            const std::string Response = Participant->performRpc(
                ToLiveKitString(DestinationIdentity),
                ToLiveKitString(Method),
                ToLiveKitString(Payload),
                EffectiveTimeout);
            Bridge.RpcResultHandler(
                RequestId,
                true,
                FromLiveKitString(Response),
                FLiveKitError());
        }
        catch (const livekit::RpcError& Error)
        {
            Bridge.RpcResultHandler(
                RequestId,
                false,
                FString(),
                MakeLiveKitError(
                    FString::Printf(TEXT("rpc_%u"), Error.code()),
                    FromLiveKitString(Error.message()),
                    FromLiveKitString(Error.data())));
        }
        catch (const std::exception& Error)
        {
            Bridge.RpcResultHandler(
                RequestId,
                false,
                FString(),
                ExceptionError(TEXT("rpc_failed"), Error));
        }
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
