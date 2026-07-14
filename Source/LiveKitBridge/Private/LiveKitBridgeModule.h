#pragma once

#include "Templates/SharedPointer.h"

/** True only when the Win64 adapter and SDK DLLs loaded and initialized. */
bool IsLiveKitWindowsSdkInitialized();

#if WITH_LIVEKIT_WINDOWS
class FLiveKitWindowsBridge;
struct LKUB_Room;

/**
 * Keep strong bridge ownership through module shutdown so no bridge destructor
 * can race process-global SDK shutdown or DLL release.
 */
void RegisterLiveKitWindowsBridge(
    const TSharedRef<FLiveKitWindowsBridge, ESPMode::ThreadSafe>& Bridge);

/**
 * Keep a terminal adapter Room alive until SDK shutdown has drained the
 * raw-this listener callbacks. The quarantine is destroyed after shutdown
 * while the Win64 runtime DLL chain remains pinned for process lifetime.
 */
void QuarantineLiveKitWindowsRoom(LKUB_Room* Room);
#endif
