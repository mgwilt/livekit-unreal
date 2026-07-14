#include "LiveKitBridgeModule.h"

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#if WITH_LIVEKIT_WINDOWS
#include "LiveKitWindowsBridge.h"
#include "Misc/ScopeLock.h"

#include <atomic>
#include <exception>
#include <livekit/livekit.h>
#endif

DEFINE_LOG_CATEGORY_STATIC(LogLiveKitBridgeModule, Log, All);

namespace
{
#if WITH_LIVEKIT_WINDOWS
std::atomic_bool GWindowsSdkAvailable{false};
bool GWindowsSdkInitializationOwned = false;
void* GWindowsFfiDllHandle = nullptr;
void* GWindowsLiveKitDllHandle = nullptr;
FCriticalSection GWindowsBridgeRegistryMutex;
TArray<TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe>> GWindowsBridges;
TArray<std::shared_ptr<livekit::Room>> GWindowsRoomQuarantine;
bool GWindowsModuleShuttingDown = false;
#else
constexpr bool GWindowsSdkAvailable = false;
#endif
}

bool IsLiveKitWindowsSdkInitialized()
{
#if WITH_LIVEKIT_WINDOWS
    return GWindowsSdkAvailable.load(std::memory_order_acquire);
#else
    return GWindowsSdkAvailable;
#endif
}

#if WITH_LIVEKIT_WINDOWS
void RegisterLiveKitWindowsBridge(
    const TSharedRef<FLiveKitWindowsBridge, ESPMode::ThreadSafe>& Bridge)
{
    bool bShutdownImmediately = false;
    {
        FScopeLock Lock(&GWindowsBridgeRegistryMutex);
        bShutdownImmediately = GWindowsModuleShuttingDown;
        if (!bShutdownImmediately)
        {
            GWindowsBridges.AddUnique(Bridge);
        }
    }

    if (bShutdownImmediately)
    {
        Bridge->Shutdown();
    }
}

void QuarantineLiveKitWindowsRoom(std::shared_ptr<livekit::Room> Room)
{
    if (!Room)
    {
        return;
    }

    FScopeLock Lock(&GWindowsBridgeRegistryMutex);
    for (const std::shared_ptr<livekit::Room>& Existing : GWindowsRoomQuarantine)
    {
        if (Existing.get() == Room.get())
        {
            return;
        }
    }
    GWindowsRoomQuarantine.Add(MoveTemp(Room));
}
#endif

class FLiveKitBridgeModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
#if WITH_LIVEKIT_WINDOWS
        {
            FScopeLock Lock(&GWindowsBridgeRegistryMutex);
            if (!ensureAlwaysMsgf(
                    GWindowsBridges.IsEmpty() && GWindowsRoomQuarantine.IsEmpty(),
                    TEXT("LiveKitBridge started with stale Windows bridge lifetime state.")))
            {
                GWindowsModuleShuttingDown = true;
                return;
            }
            GWindowsModuleShuttingDown = false;
        }
        GWindowsSdkAvailable.store(false, std::memory_order_release);
        GWindowsSdkInitializationOwned = false;

        TArray<FString> BinaryDirectories;
#if !IS_MONOLITHIC
        const FString ModuleFilename =
            FModuleManager::Get().GetModuleFilename(FName(TEXT("LiveKitBridge")));
        if (!ModuleFilename.IsEmpty())
        {
            BinaryDirectories.Add(FPaths::GetPath(ModuleFilename));
        }
#endif
        BinaryDirectories.AddUnique(FPlatformProcess::BaseDir());

        FString LoadedDirectory;
        for (const FString& BinaryDirectory : BinaryDirectories)
        {
            if (TryLoadWindowsLibraries(BinaryDirectory))
            {
                LoadedDirectory = BinaryDirectory;
                break;
            }
        }
        if (LoadedDirectory.IsEmpty())
        {
            UE_LOG(
                LogLiveKitBridgeModule,
                Error,
                TEXT("Unable to load livekit_ffi.dll and livekit.dll beside the LiveKitBridge module or packaged executable."));
            return;
        }

        try
        {
            GWindowsSdkInitializationOwned = livekit::initialize(livekit::LogLevel::Info);
            if (!GWindowsSdkInitializationOwned)
            {
                UE_LOG(
                    LogLiveKitBridgeModule,
                    Error,
                    TEXT("LiveKit C++ SDK was already initialized by another owner; the Win64 backend requires exclusive lifecycle ownership."));
                ReleaseWindowsLibraries();
                return;
            }
            GWindowsSdkAvailable.store(true, std::memory_order_release);
            UE_LOG(
                LogLiveKitBridgeModule,
                Log,
                TEXT("LiveKit C++ SDK available for Win64 from %s (%s)."),
                *LoadedDirectory,
                TEXT("initialized by LiveKitBridge"));
        }
        catch (const std::exception& Error)
        {
            UE_LOG(
                LogLiveKitBridgeModule,
                Error,
                TEXT("LiveKit C++ SDK initialization failed: %s"),
                UTF8_TO_TCHAR(Error.what()));
            ReleaseWindowsLibraries();
        }
#endif
    }

    virtual void ShutdownModule() override
    {
#if WITH_LIVEKIT_WINDOWS
        TArray<TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe>> ActiveBridges;
        {
            FScopeLock Lock(&GWindowsBridgeRegistryMutex);
            GWindowsModuleShuttingDown = true;
            GWindowsSdkAvailable.store(false, std::memory_order_release);
            ActiveBridges.Reserve(GWindowsBridges.Num());
            for (const TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe>& Bridge :
                 GWindowsBridges)
            {
                if (Bridge)
                {
                    ActiveBridges.Add(Bridge);
                }
            }
            GWindowsBridges.Reset();
        }

        // Stop every bridge and transfer any listener-ambiguous terminal Room
        // to quarantine before the process-global SDK drains callbacks.
        for (const TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe>& Bridge :
             ActiveBridges)
        {
            Bridge->Shutdown();
        }

        if (GWindowsSdkInitializationOwned)
        {
            livekit::shutdown();
        }
        GWindowsSdkInitializationOwned = false;

        // SDK shutdown is the only public barrier that guarantees every
        // ListenerSlot callback has returned. Release terminal Rooms after it,
        // while the C++ DLL is still loaded.
        TArray<std::shared_ptr<livekit::Room>> QuarantinedRooms;
        {
            FScopeLock Lock(&GWindowsBridgeRegistryMutex);
            QuarantinedRooms = MoveTemp(GWindowsRoomQuarantine);
            GWindowsRoomQuarantine.Reset();
        }
        QuarantinedRooms.Reset();
        ActiveBridges.Reset();
        ReleaseWindowsLibraries();
#endif
    }

private:
    static bool TryLoadWindowsLibraries(const FString& BinaryDirectory)
    {
        const FString FfiPath = FPaths::Combine(BinaryDirectory, TEXT("livekit_ffi.dll"));
        const FString LiveKitPath = FPaths::Combine(BinaryDirectory, TEXT("livekit.dll"));

        GWindowsFfiDllHandle = FPlatformProcess::GetDllHandle(*FfiPath);
        if (GWindowsFfiDllHandle == nullptr)
        {
            return false;
        }

        GWindowsLiveKitDllHandle = FPlatformProcess::GetDllHandle(*LiveKitPath);
        if (GWindowsLiveKitDllHandle == nullptr)
        {
            FPlatformProcess::FreeDllHandle(GWindowsFfiDllHandle);
            GWindowsFfiDllHandle = nullptr;
            return false;
        }
        return true;
    }

    static void ReleaseWindowsLibraries()
    {
        if (GWindowsLiveKitDllHandle != nullptr)
        {
            FPlatformProcess::FreeDllHandle(GWindowsLiveKitDllHandle);
            GWindowsLiveKitDllHandle = nullptr;
        }
        if (GWindowsFfiDllHandle != nullptr)
        {
            FPlatformProcess::FreeDllHandle(GWindowsFfiDllHandle);
            GWindowsFfiDllHandle = nullptr;
        }
    }
};

IMPLEMENT_MODULE(FLiveKitBridgeModule, LiveKitBridge)
