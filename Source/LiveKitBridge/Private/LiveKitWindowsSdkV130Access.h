#pragma once

#if WITH_LIVEKIT_WINDOWS
namespace livekit
{
class ByteStreamReader;
}

namespace LiveKitWindowsSdkV130Access
{
/**
 * Wake a blocking ByteStreamReader::readNext call by marking the reader closed.
 *
 * LiveKit C++ 1.3.0 does not expose reader cancellation and Room::disconnect
 * drops its reader registry without closing externally-owned readers. The
 * implementation uses a narrowly-scoped C++ private-member access shim for
 * that exact pinned SDK. Remove this shim when the public SDK exposes cancel.
 */
void CloseByteStreamReader(livekit::ByteStreamReader& Reader);
}
#endif
