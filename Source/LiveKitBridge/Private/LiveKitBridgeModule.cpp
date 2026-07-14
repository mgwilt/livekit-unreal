#include "LiveKitBridgeModule.h"

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#if WITH_LIVEKIT_WINDOWS
#include <exception>
#include <livekit/livekit.h>
#endif

DEFINE_LOG_CATEGORY_STATIC(LogLiveKitBridgeModule, Log, All);

namespace
{
bool GWindowsSdkAvailable = false;
bool GWindowsSdkInitializationOwned = false;
void* GWindowsFfiDllHandle = nullptr;
void* GWindowsLiveKitDllHandle = nullptr;
}

bool IsLiveKitWindowsSdkInitialized()
{
    return GWindowsSdkAvailable;
}

class FLiveKitBridgeModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
#if WITH_LIVEKIT_WINDOWS
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
            GWindowsSdkAvailable = true;
            UE_LOG(
                LogLiveKitBridgeModule,
                Log,
                TEXT("LiveKit C++ SDK available for Win64 from %s (%s)."),
                *LoadedDirectory,
                GWindowsSdkInitializationOwned
                    ? TEXT("initialized by LiveKitBridge")
                    : TEXT("already initialized by this process"));
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
        if (GWindowsSdkAvailable && GWindowsSdkInitializationOwned)
        {
            livekit::shutdown();
        }
        GWindowsSdkAvailable = false;
        GWindowsSdkInitializationOwned = false;
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
