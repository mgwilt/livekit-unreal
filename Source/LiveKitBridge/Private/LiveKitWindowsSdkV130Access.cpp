#include "LiveKitWindowsSdkV130Access.h"

#if WITH_LIVEKIT_WINDOWS
#include <livekit/build.h>
#include <livekit/data_stream.h>

#include <map>
#include <string>
#include <string_view>

static_assert(
    std::string_view(LIVEKIT_BUILD_VERSION) == std::string_view("1.3.0"),
    "The byte-stream cancellation shim must be reviewed for each LiveKit C++ SDK version.");

namespace
{
// Explicit template instantiation is permitted to name a private member. This
// friend-injection pattern exposes only ByteStreamReader::onStreamClose in this
// translation unit and does not rewrite the SDK class definition with macros.
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
}

void LiveKitWindowsSdkV130Access::CloseByteStreamReader(
    livekit::ByteStreamReader& Reader)
{
    const std::map<std::string, std::string> EmptyTrailerAttributes;
    (Reader.*GetPrivateMember(FByteStreamCloseMember()))(EmptyTrailerAttributes);
}
#endif
