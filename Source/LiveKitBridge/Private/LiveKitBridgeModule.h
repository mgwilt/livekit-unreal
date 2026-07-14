#pragma once

#include "Templates/SharedPointer.h"

#include <memory>

/** True only when the Win64 SDK DLLs loaded and livekit::initialize succeeded. */
bool IsLiveKitWindowsSdkInitialized();

#if WITH_LIVEKIT_WINDOWS
class FLiveKitWindowsBridge;
namespace livekit
{
class Room;
}

/**
 * Keep strong bridge ownership through module shutdown so no bridge destructor
 * can race process-global SDK shutdown or DLL release.
 */
void RegisterLiveKitWindowsBridge(
    const TSharedRef<FLiveKitWindowsBridge, ESPMode::ThreadSafe>& Bridge);

/**
 * Keep a terminal Room alive until livekit::shutdown has drained the SDK's
 * raw-this listener callbacks. The quarantine is released before DLL unload.
 */
void QuarantineLiveKitWindowsRoom(std::shared_ptr<livekit::Room> Room);
#endif
