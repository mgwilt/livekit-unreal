#include "LiveKitBridgeModule.h"

#include "Containers/StringConv.h"
#include "HAL/PlatformProcess.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#if WITH_LIVEKIT_WINDOWS
#include "LiveKitWindowsBridge.h"
#include "LiveKitWindowsAdapter.h"
#include "Misc/ScopeLock.h"
#include "Windows/AllowWindowsPlatformTypes.h"

#include <atomic>
#include <delayimp.h>

#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogLiveKitBridgeModule, Log, All);

namespace
{
#if WITH_LIVEKIT_WINDOWS
std::atomic_bool GWindowsSdkAvailable{false};
bool GWindowsSdkInitializationOwned = false;
void* GWindowsFfiDllHandle = nullptr;
void* GWindowsLiveKitDllHandle = nullptr;
void* GWindowsAdapterDllHandle = nullptr;
FCriticalSection GWindowsBridgeRegistryMutex;
TArray<TSharedPtr<FLiveKitWindowsBridge, ESPMode::ThreadSafe>> GWindowsBridges;
TArray<LKUB_Room*> GWindowsRoomQuarantine;
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

void QuarantineLiveKitWindowsRoom(LKUB_Room* Room)
{
    if (Room == nullptr)
    {
        return;
    }

    FScopeLock Lock(&GWindowsBridgeRegistryMutex);
    for (LKUB_Room* Existing : GWindowsRoomQuarantine)
    {
        if (Existing == Room)
        {
            return;
        }
    }
    GWindowsRoomQuarantine.Add(Room);
}
#endif

class FLiveKitBridgeModule final : public IModuleInterface
{
public:
    virtual bool SupportsDynamicReloading() override
    {
#if WITH_LIVEKIT_WINDOWS
        // The pinned LiveKit FFI can still execute worker cleanup after its
        // synchronous shutdown call returns. A live Win64 backend therefore
        // cannot safely unload or hot-reload this module in-process.
        return false;
#else
        return true;
#endif
    }

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
                TEXT("Unable to load livekit_ffi.dll, livekit.dll, and LiveKitUnrealWindowsAdapter.dll beside the LiveKitBridge module or packaged executable."));
            return;
        }

        const uint32 AdapterAbiVersion = lkub_get_abi_version();
        if (AdapterAbiVersion != LKUB_ABI_VERSION)
        {
            UE_LOG(
                LogLiveKitBridgeModule,
                Error,
                TEXT("LiveKit Windows adapter ABI mismatch: expected %u but loaded %u."),
                static_cast<uint32>(LKUB_ABI_VERSION),
                AdapterAbiVersion);
            ReleaseWindowsLibrariesBeforeInitialization();
            return;
        }

        LKUB_Result InitializeResult{};
        lkub_result_reset(&InitializeResult);
        uint8 bInitializationOwned = 0;
        const int32 InitializeStatus = lkub_initialize(
            LKUB_LOG_INFO,
            &bInitializationOwned,
            &InitializeResult);
        GWindowsSdkInitializationOwned =
            InitializeStatus == LKUB_STATUS_OK && bInitializationOwned != 0;
        if (!GWindowsSdkInitializationOwned)
        {
            const int32 MessageByteCount = static_cast<int32>(FMath::Min<uint32>(
                InitializeResult.message_size,
                LKUB_RESULT_MESSAGE_CAPACITY));
            const FUTF8ToTCHAR ConvertedMessage(
                InitializeResult.message,
                MessageByteCount);
            const FString Message(ConvertedMessage.Length(), ConvertedMessage.Get());
            UE_LOG(
                LogLiveKitBridgeModule,
                Error,
                TEXT("LiveKit Windows adapter initialization failed (status %d): %s"),
                InitializeStatus,
                *Message);
            if (bInitializationOwned != 0)
            {
                lkub_shutdown();
            }
            // Once lkub_initialize has been entered, a failure or an
            // already-initialized result cannot prove that no FFI worker is
            // alive. Keep the runtime chain pinned just as on the success
            // path; only pre-initialize failures may release these handles.
            return;
        }

        GWindowsSdkAvailable.store(true, std::memory_order_release);
        UE_LOG(
            LogLiveKitBridgeModule,
            Log,
            TEXT("LiveKit Windows adapter ABI %u available from %s (SDK lifecycle owned by LiveKitBridge)."),
            AdapterAbiVersion,
            *LoadedDirectory);
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
            lkub_shutdown();
        }
        GWindowsSdkInitializationOwned = false;

        // SDK shutdown is the only public barrier that guarantees every
        // ListenerSlot callback has returned. Release terminal Rooms after it,
        // while the C++ DLL is still loaded.
        TArray<LKUB_Room*> QuarantinedRooms;
        {
            FScopeLock Lock(&GWindowsBridgeRegistryMutex);
            QuarantinedRooms = MoveTemp(GWindowsRoomQuarantine);
            GWindowsRoomQuarantine.Reset();
        }
        for (LKUB_Room* Room : QuarantinedRooms)
        {
            lkub_room_destroy(Room);
        }
        QuarantinedRooms.Reset();
        ActiveBridges.Reset();

        // Do not release the adapter, LiveKit C++, or FFI DLL handles after a
        // successful initialization. The pinned SDK's synchronous shutdown is
        // not a proven thread-join barrier: Windows has observed late FFI
        // worker cleanup execute after ShutdownModule returned. Keeping all
        // three libraries mapped until process termination prevents those
        // workers from returning into unloaded code. Windows reclaims these
        // process-lifetime references after all process threads have stopped.
#endif
    }

private:
#if WITH_LIVEKIT_WINDOWS
    static bool TryLoadWindowsLibraries(const FString& BinaryDirectory)
    {
        const FString FfiPath = FPaths::Combine(BinaryDirectory, TEXT("livekit_ffi.dll"));
        const FString LiveKitPath = FPaths::Combine(BinaryDirectory, TEXT("livekit.dll"));
        const FString AdapterPath = FPaths::Combine(
            BinaryDirectory,
            TEXT("LiveKitUnrealWindowsAdapter.dll"));

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

        GWindowsAdapterDllHandle = FPlatformProcess::GetDllHandle(*AdapterPath);
        if (GWindowsAdapterDllHandle == nullptr)
        {
            FPlatformProcess::FreeDllHandle(GWindowsLiveKitDllHandle);
            GWindowsLiveKitDllHandle = nullptr;
            FPlatformProcess::FreeDllHandle(GWindowsFfiDllHandle);
            GWindowsFfiDllHandle = nullptr;
            return false;
        }
        return true;
    }

    static void ReleaseWindowsLibrariesBeforeInitialization()
    {
        // This path is valid only before this module has acquired successful
        // SDK lifecycle ownership. No SDK/FFI worker can still be running.
        if (GWindowsAdapterDllHandle != nullptr)
        {
            // Unreal's Win64 linker emits /DELAY:UNLOAD. Reset this module's
            // delay-import table before releasing our explicit reference so a
            // hot reload cannot retain stale adapter entry points.
            if (!__FUnloadDelayLoadedDLL2("LiveKitUnrealWindowsAdapter.dll"))
            {
                UE_LOG(
                    LogLiveKitBridgeModule,
                    Warning,
                    TEXT("Unable to reset the LiveKit Windows adapter delay-import table during unload."));
            }
            FPlatformProcess::FreeDllHandle(GWindowsAdapterDllHandle);
            GWindowsAdapterDllHandle = nullptr;
        }
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
#endif
};

IMPLEMENT_MODULE(FLiveKitBridgeModule, LiveKitBridge)
