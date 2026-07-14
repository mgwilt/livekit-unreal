#include "LiveKitWindowsBridgeInternal.h"

#include "Async/Async.h"
#include "Misc/ScopeExit.h"

using namespace LiveKitWindowsBridgeInternal;

#if WITH_LIVEKIT_WINDOWS

bool FLiveKitWindowsBridge::FImplementation::TryTrackByteStreamReader(
    LKUB_ByteStreamReader* Reader,
    LKUB_Room* CandidateRoom,
    const std::string& Topic,
    uint64 InConnectionGeneration,
    uint64 InRegistrationGeneration)
{
    if (Reader == nullptr || CandidateRoom == nullptr)
    {
        return false;
    }

    std::lock_guard RoomLock(RoomMutex);
    if (Room != CandidateRoom || RoomGeneration != InConnectionGeneration ||
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
        Reader,
        FTrackedByteStreamReader{
            Reader,
            Topic,
            InConnectionGeneration,
            InRegistrationGeneration}).second;
}

void FLiveKitWindowsBridge::FImplementation::UntrackByteStreamReader(LKUB_ByteStreamReader* Reader)
{
    std::lock_guard Lock(ByteStreamReaderMutex);
    TrackedByteStreamReaders.erase(Reader);
}

void FLiveKitWindowsBridge::FImplementation::CancelAllTrackedByteStreamReaders()
{
    std::lock_guard Lock(ByteStreamReaderMutex);
    // Reader-task scope exit takes this same mutex before destroying its
    // opaque handle. Keep it held through cancel and removal so the handle
    // cannot be destroyed between lookup and the adapter call. Adapter cancel
    // does not call back into Unreal, so this cannot invert the callback locks.
    for (const auto& Pair : TrackedByteStreamReaders)
    {
        lkub_byte_stream_cancel(Pair.first);
    }
    TrackedByteStreamReaders.clear();
}

void FLiveKitWindowsBridge::FImplementation::CancelTrackedByteStreamReadersForTopic(
    const std::string& Topic)
{
    std::lock_guard Lock(ByteStreamReaderMutex);
    // Cancellation and erasure are one critical section for the same reason
    // as CancelAllTrackedByteStreamReaders: task cleanup destroys only after
    // it has acquired this mutex and observed that tracking has ended.
    for (auto Iterator = TrackedByteStreamReaders.begin();
         Iterator != TrackedByteStreamReaders.end();)
    {
        if (Iterator->second.Topic == Topic)
        {
            lkub_byte_stream_cancel(Iterator->first);
            Iterator = TrackedByteStreamReaders.erase(Iterator);
        }
        else
        {
            ++Iterator;
        }
    }
}

void FLiveKitWindowsBridge::FImplementation::HandleVisitAttribute(
    void* Context,
    LKUB_StringView Key,
    LKUB_StringView Value)
{
    if (TMap<FString, FString>* Attributes =
            static_cast<TMap<FString, FString>*>(Context))
    {
        Attributes->Add(FromAdapterView(Key), FromAdapterView(Value));
    }
}

void LKUB_CALL FLiveKitWindowsBridge::FImplementation::VisitAttribute(
    void* Context,
    LKUB_StringView Key,
    LKUB_StringView Value) noexcept
{
    try
    {
        HandleVisitAttribute(Context, Key, Value);
    }
    catch (...)
    {
    }
}

void FLiveKitWindowsBridge::FImplementation::HandleByteStream(
    void* Context,
    LKUB_Room* InRoom,
    LKUB_ByteStreamReader*& Reader)
{
    auto ReleaseReader = [&Reader]()
    {
        if (Reader != nullptr)
        {
            lkub_byte_stream_cancel(Reader);
            lkub_byte_stream_destroy(Reader);
            Reader = nullptr;
        }
    };
    FByteStreamCallbackContext* Registration =
        static_cast<FByteStreamCallbackContext*>(Context);
    if (Registration == nullptr || Registration->Implementation == nullptr ||
        Reader == nullptr || InRoom != Registration->Room)
    {
        ReleaseReader();
        return;
    }
    FImplementation* Self = Registration->Implementation;
    if (!Self->TryTrackByteStreamReader(
            Reader,
            InRoom,
            Registration->Topic,
            Registration->ConnectionGeneration,
            Registration->RegistrationGeneration))
    {
        ReleaseReader();
        return;
    }
    const TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe> Pinned = Self->Owner.Pin();
    if (!Pinned)
    {
        Self->UntrackByteStreamReader(Reader);
        ReleaseReader();
        return;
    }
    const std::string Topic = Registration->Topic;
    const uint64 ConnectionGeneration = Registration->ConnectionGeneration;
    const uint64 RegistrationGeneration = Registration->RegistrationGeneration;
    const bool bDispatched = Pinned->DispatchByteStream(
        [Reader, InRoom, Topic, ConnectionGeneration, RegistrationGeneration](
            FLiveKitWindowsBridge& Bridge)
        {
            ON_SCOPE_EXIT
            {
                Bridge.Implementation->UntrackByteStreamReader(Reader);
                lkub_byte_stream_destroy(Reader);
            };
            LKUB_Result Result{};
            LKUB_ByteStreamInfo AdapterInfo{};
            if (lkub_byte_stream_get_info(Reader, &AdapterInfo, &Result) != LKUB_STATUS_OK)
            {
                if (Bridge.Implementation->IsCurrentRoom(InRoom, ConnectionGeneration))
                {
                    Bridge.ErrorHandler(AdapterError(TEXT("byte_stream_read_failed"), Result));
                }
                return;
            }
            FLiveKitByteStream Stream;
            Stream.SenderIdentity = FromAdapterView(AdapterInfo.sender_identity);
            Stream.StreamId = FromAdapterView(AdapterInfo.stream_id);
            Stream.Topic = FromAdapterView(AdapterInfo.topic);
            Stream.Name = FromAdapterView(AdapterInfo.name);
            Stream.MimeType = FromAdapterView(AdapterInfo.mime_type);
            bool bTooLarge = AdapterInfo.has_total_size != 0 &&
                AdapterInfo.total_size >
                    static_cast<uint64>(LiveKitLimits::MaxIncomingByteStreamBytes);
            bool bReachedEndOfStream = false;
            if (bTooLarge)
            {
                lkub_byte_stream_cancel(Reader);
            }
            while (!bTooLarge)
            {
                if (!Bridge.Implementation->IsCurrentRoom(InRoom, ConnectionGeneration) ||
                    !Bridge.Implementation->IsByteStreamRegistrationCurrent(
                        Topic,
                        RegistrationGeneration))
                {
                    return;
                }
                LKUB_OwnedBuffer Chunk{};
                uint8 EndOfStream = 0;
                Result = {};
                const int32 Status =
                    lkub_byte_stream_read_next(Reader, &Chunk, &EndOfStream, &Result);
                ON_SCOPE_EXIT
                {
                    lkub_buffer_release(&Chunk);
                };
                if (Status != LKUB_STATUS_OK)
                {
                    if (Bridge.Implementation->IsCurrentRoom(InRoom, ConnectionGeneration) &&
                        Bridge.Implementation->IsByteStreamRegistrationCurrent(
                            Topic,
                            RegistrationGeneration))
                    {
                        Bridge.ErrorHandler(AdapterError(
                            TEXT("byte_stream_read_failed"),
                            Result));
                    }
                    return;
                }
                if (Chunk.size > MAX_int32 ||
                    Stream.Data.Num() > LiveKitLimits::MaxIncomingByteStreamBytes -
                        static_cast<int32>(Chunk.size))
                {
                    bTooLarge = true;
                    lkub_byte_stream_cancel(Reader);
                    break;
                }
                if (Chunk.data != nullptr && Chunk.size > 0)
                {
                    Stream.Data.Append(Chunk.data, static_cast<int32>(Chunk.size));
                }
                if (EndOfStream != 0)
                {
                    bReachedEndOfStream = true;
                    break;
                }
            }
            if (!Bridge.Implementation->IsCurrentRoom(InRoom, ConnectionGeneration) ||
                !Bridge.Implementation->IsByteStreamRegistrationCurrent(
                    Topic,
                    RegistrationGeneration))
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
            if (!bReachedEndOfStream)
            {
                return;
            }
            // LiveKit merges trailer attributes when the stream closes. Visit
            // them only after EOS so the map is complete and no longer being
            // mutated by the reader while the adapter traverses it.
            Result = {};
            if (lkub_byte_stream_visit_attributes(
                    Reader,
                    &VisitAttribute,
                    &Stream.Attributes,
                    &Result) != LKUB_STATUS_OK)
            {
                if (Bridge.Implementation->IsCurrentRoom(InRoom, ConnectionGeneration) &&
                    Bridge.Implementation->IsByteStreamRegistrationCurrent(
                        Topic,
                        RegistrationGeneration))
                {
                    Bridge.ErrorHandler(AdapterError(TEXT("byte_stream_read_failed"), Result));
                }
                return;
            }
            Bridge.ByteStreamHandler(Stream);
        });
    if (!bDispatched)
    {
        Self->UntrackByteStreamReader(Reader);
        ReleaseReader();
        if (Self->IsCurrentRoom(InRoom, ConnectionGeneration))
        {
            Pinned->ErrorHandler(MakeLiveKitError(
                TEXT("byte_stream_reader_limit"),
                FString::Printf(
                    TEXT("At most %d LiveKit byte streams may be read concurrently."),
                    MaxConcurrentByteStreamReaders)));
        }
    }
    else
    {
        // The asynchronous reader task owns the transferred adapter handle.
        Reader = nullptr;
    }
}

void LKUB_CALL FLiveKitWindowsBridge::FImplementation::OnByteStream(
    void* Context,
    LKUB_Room* InRoom,
    LKUB_ByteStreamReader* Reader) noexcept
{
    LKUB_ByteStreamReader* OwnedReader = Reader;
    try
    {
        HandleByteStream(Context, InRoom, OwnedReader);
    }
    catch (...)
    {
        if (OwnedReader != nullptr)
        {
            try
            {
                FByteStreamCallbackContext* Registration =
                    static_cast<FByteStreamCallbackContext*>(Context);
                if (Registration != nullptr &&
                    Registration->Implementation != nullptr)
                {
                    Registration->Implementation->UntrackByteStreamReader(OwnedReader);
                }
            }
            catch (...)
            {
            }
            lkub_byte_stream_cancel(OwnedReader);
            lkub_byte_stream_destroy(OwnedReader);
        }
    }
}

#endif

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
            // Every admitted byte task must execute its work wrapper because
            // that wrapper owns and destroys the transferred reader handle.
            // Shutdown invalidates the room and cancels the reader first, so
            // the work exits quickly without delivering stale data.
            try
            {
                Work(Self.Get());
            }
            catch (...)
            {
                Self->ErrorHandler(UnknownBackendError(TEXT("byte_stream_read_failed")));
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
