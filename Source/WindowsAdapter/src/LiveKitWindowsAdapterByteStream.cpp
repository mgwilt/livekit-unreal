#include "LiveKitWindowsAdapterInternal.h"

#include <cstring>
#include <map>
#include <vector>

using namespace lkub::internal;

namespace
{
template <typename Tag, typename Tag::FMember Member>
struct TPrivateMemberAccess
{
    friend typename Tag::FMember GetPrivateMember(Tag)
    {
        return Member;
    }
};

struct FByteStreamCloseMember
{
    using FMember = void (livekit::ByteStreamReader::*)(
        const std::map<std::string, std::string>&);
    friend FMember GetPrivateMember(FByteStreamCloseMember);
};

template struct TPrivateMemberAccess<
    FByteStreamCloseMember,
    &livekit::ByteStreamReader::onStreamClose>;

void CloseReader(livekit::ByteStreamReader& Reader)
{
    const std::map<std::string, std::string> EmptyAttributes;
    (Reader.*GetPrivateMember(FByteStreamCloseMember()))(EmptyAttributes);
}
}

extern "C"
{
int32_t LKUB_CALL lkub_room_register_byte_stream_handler(
    LKUB_Room* Room,
    LKUB_StringView Topic,
    LKUB_ByteStreamCallback Callback,
    void* CallbackContext,
    LKUB_Result* OutResult)
{
    return Guard(OutResult, [&]()
    {
        Require(Room != nullptr && Room->SdkRoom, "The room is unavailable.");
        Require(Callback != nullptr, "The byte-stream callback is required.");
        const std::string TopicCopy = CopyString(Topic);
        Room->SdkRoom->registerByteStreamHandler(
            TopicCopy,
            [Room, Callback, CallbackContext](
                std::shared_ptr<livekit::ByteStreamReader> Reader,
                const std::string& ParticipantIdentity)
            {
                if (!Reader)
                {
                    return;
                }
                std::unique_ptr<LKUB_ByteStreamReader> AdapterReader(
                    new (std::nothrow) LKUB_ByteStreamReader());
                if (!AdapterReader)
                {
                    try
                    {
                        CloseReader(*Reader);
                    }
                    catch (...)
                    {
                    }
                    return;
                }
                AdapterReader->Reader = std::move(Reader);
                AdapterReader->SenderIdentity = ParticipantIdentity;
                LKUB_ByteStreamReader* TransferredReader = AdapterReader.release();
                try
                {
                    Callback(CallbackContext, Room, TransferredReader);
                }
                catch (...)
                {
                    try
                    {
                        CloseReader(*TransferredReader->Reader);
                    }
                    catch (...)
                    {
                    }
                    delete TransferredReader;
                }
            });
    });
}

int32_t LKUB_CALL lkub_room_unregister_byte_stream_handler(
    LKUB_Room* Room,
    LKUB_StringView Topic,
    LKUB_Result* OutResult)
{
    return Guard(OutResult, [&]()
    {
        Require(Room != nullptr && Room->SdkRoom, "The room is unavailable.");
        Room->SdkRoom->unregisterByteStreamHandler(CopyString(Topic));
    });
}

int32_t LKUB_CALL lkub_byte_stream_get_info(
    LKUB_ByteStreamReader* Reader,
    LKUB_ByteStreamInfo* OutInfo,
    LKUB_Result* OutResult)
{
    if (OutInfo != nullptr)
    {
        *OutInfo = {};
    }
    return Guard(OutResult, [&]()
    {
        Require(Reader != nullptr && Reader->Reader, "The byte-stream reader is unavailable.");
        Require(OutInfo != nullptr, "The byte-stream info output is required.");
        const livekit::ByteStreamInfo& Info = Reader->Reader->info();
        OutInfo->sender_identity = BorrowString(Reader->SenderIdentity);
        OutInfo->stream_id = BorrowString(Info.stream_id);
        OutInfo->topic = BorrowString(Info.topic);
        OutInfo->name = BorrowString(Info.name);
        OutInfo->mime_type = BorrowString(Info.mime_type);
        if (Info.size.has_value())
        {
            OutInfo->has_total_size = 1;
            OutInfo->total_size = static_cast<uint64_t>(Info.size.value());
        }
    });
}

int32_t LKUB_CALL lkub_byte_stream_visit_attributes(
    LKUB_ByteStreamReader* Reader,
    LKUB_AttributeVisitor Visitor,
    void* VisitorContext,
    LKUB_Result* OutResult)
{
    return Guard(OutResult, [&]()
    {
        Require(Reader != nullptr && Reader->Reader, "The byte-stream reader is unavailable.");
        Require(Visitor != nullptr, "The attribute visitor is required.");
        for (const auto& [Key, Value] : Reader->Reader->info().attributes)
        {
            Visitor(VisitorContext, BorrowString(Key), BorrowString(Value));
        }
    });
}

int32_t LKUB_CALL lkub_byte_stream_read_next(
    LKUB_ByteStreamReader* Reader,
    LKUB_OwnedBuffer* OutChunk,
    uint8_t* OutEndOfStream,
    LKUB_Result* OutResult)
{
    if (OutChunk != nullptr)
    {
        OutChunk->data = nullptr;
        OutChunk->size = 0;
    }
    if (OutEndOfStream != nullptr)
    {
        *OutEndOfStream = 0;
    }
    return Guard(OutResult, [&]()
    {
        Require(Reader != nullptr && Reader->Reader, "The byte-stream reader is unavailable.");
        Require(OutChunk != nullptr, "The byte-stream chunk output is required.");
        Require(OutEndOfStream != nullptr, "The byte-stream EOS output is required.");

        std::vector<uint8_t> Chunk;
        if (!Reader->Reader->readNext(Chunk))
        {
            *OutEndOfStream = 1;
            return;
        }
        if (Chunk.empty())
        {
            return;
        }
        std::unique_ptr<uint8_t[]> Copy(new uint8_t[Chunk.size()]);
        std::memcpy(Copy.get(), Chunk.data(), Chunk.size());
        OutChunk->size = static_cast<uint64_t>(Chunk.size());
        OutChunk->data = Copy.release();
    });
}

void LKUB_CALL lkub_byte_stream_cancel(LKUB_ByteStreamReader* Reader)
{
    if (Reader == nullptr || !Reader->Reader ||
        Reader->Cancelled.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    try
    {
        CloseReader(*Reader->Reader);
    }
    catch (...)
    {
    }
}

void LKUB_CALL lkub_byte_stream_destroy(LKUB_ByteStreamReader* Reader)
{
    delete Reader;
}
}
