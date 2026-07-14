using System.IO;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using UnrealBuildTool;

public class LiveKitBridge : ModuleRules
{
    public LiveKitBridge(ReadOnlyTargetRules Target) : base(Target)
    {
        // ARC is required by the Swift-generated Objective-C surface. Unreal's shared
        // PCH is compiled without ARC, so this small bridge module intentionally has no PCH.
        PCHUsage = PCHUsageMode.NoPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine"
        });

        bool isApple = Target.Platform == UnrealTargetPlatform.Mac || Target.Platform == UnrealTargetPlatform.IOS;
        string appleRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "ThirdParty", "Apple"));
        string liveKit = Path.Combine(appleRoot, "LiveKit.xcframework.zip");
        string webRtc = Path.Combine(appleRoot, "LiveKitWebRTC.xcframework.zip");
        string uniFfi = Path.Combine(appleRoot, "RustLiveKitUniFFI.xcframework.zip");
        string facadeLibrary = Target.Platform == UnrealTargetPlatform.Mac
            ? Path.Combine(appleRoot, "Facade", "Mac", "libLiveKitUnrealFacade.a")
            : Path.Combine(appleRoot, "Facade", "IOS", "libLiveKitUnrealFacade.a");
        string facadeHeader = Target.Platform == UnrealTargetPlatform.Mac
            ? Path.Combine(appleRoot, "Headers", "LiveKitUnrealFacade-Swift-macOS.h")
            : Path.Combine(appleRoot, "Headers", "LiveKitUnrealFacade-Swift-iOS.h");
        bool hasLiveKit = isApple &&
            File.Exists(liveKit) &&
            File.Exists(webRtc) &&
            File.Exists(uniFfi) &&
            File.Exists(facadeLibrary) &&
            File.Exists(facadeHeader);

        PublicDefinitions.Add("WITH_LIVEKIT_APPLE=" + (hasLiveKit ? "1" : "0"));

        bool isWindows = Target.Platform == UnrealTargetPlatform.Win64;
        bool isInternalBuild =
            Target.Configuration == UnrealTargetConfiguration.Debug ||
            Target.Configuration == UnrealTargetConfiguration.DebugGame ||
            Target.Configuration == UnrealTargetConfiguration.Development;
        string windowsRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "ThirdParty", "Windows"));
        string windowsSdkRoot = Path.Combine(windowsRoot, "SDK");
        string windowsInclude = Path.Combine(windowsSdkRoot, "include");
        string windowsLib = Path.Combine(windowsSdkRoot, "lib");
        string windowsBin = Path.Combine(windowsSdkRoot, "bin");
        string windowsLiveKitLib = Path.Combine(windowsLib, "livekit.lib");
        string windowsFfiLib = Path.Combine(windowsLib, "livekit_ffi.dll.lib");
        string windowsLiveKitDll = Path.Combine(windowsBin, "livekit.dll");
        string windowsFfiDll = Path.Combine(windowsBin, "livekit_ffi.dll");
        bool hasWindowsSdk = isWindows &&
            isInternalBuild &&
            HasVerifiedWindowsSdk(
                windowsRoot,
                windowsSdkRoot,
                windowsInclude,
                windowsLiveKitLib,
                windowsFfiLib,
                windowsLiveKitDll,
                windowsFfiDll);

        PublicDefinitions.Add("WITH_LIVEKIT_WINDOWS=" + (hasWindowsSdk ? "1" : "0"));

        if (isApple)
        {
            bEnableObjCAutomaticReferenceCounting = true;
            PrivateDependencyModuleNames.Add("Swift");
            PublicFrameworks.AddRange(new[]
            {
                "AVFoundation",
                "AudioToolbox",
                "CoreMedia",
                "CoreVideo",
                "Foundation",
                "Network",
                "Security"
            });

            if (Target.Platform == UnrealTargetPlatform.IOS)
            {
                PublicFrameworks.AddRange(new[]
                {
                    "ReplayKit",
                    "UIKit"
                });
            }
        }

        if (hasLiveKit)
        {
            PrivateIncludePaths.Add(Path.Combine(appleRoot, "Headers"));
            PublicAdditionalLibraries.Add(facadeLibrary);

            if (Target.Platform == UnrealTargetPlatform.Mac)
            {
                // UE 5.8's Mac toolchain does not add the selected variant's search path for
                // zipped XCFrameworks. The fetch script materializes those official variants.
                AddMacFramework("LiveKit");
                AddMacFramework("LiveKitWebRTC");
                AddMacFramework("RustLiveKitUniFFI");
            }
            else
            {
                PublicAdditionalFrameworks.Add(new Framework("LiveKit", liveKit, Framework.FrameworkMode.LinkAndCopy));
                PublicAdditionalFrameworks.Add(new Framework("LiveKitWebRTC", webRtc, Framework.FrameworkMode.LinkAndCopy));
                PublicAdditionalFrameworks.Add(new Framework("RustLiveKitUniFFI", uniFfi, Framework.FrameworkMode.LinkAndCopy));
            }
        }

        if (hasWindowsSdk)
        {
            bEnableExceptions = true;
            PrivateIncludePaths.Add(windowsInclude);
            PublicAdditionalLibraries.AddRange(new[]
            {
                windowsLiveKitLib,
                windowsFfiLib
            });
            PublicSystemLibraries.AddRange(new[]
            {
                "ntdll.lib",
                "userenv.lib",
                "winmm.lib",
                "iphlpapi.lib",
                "msdmo.lib",
                "dmoguids.lib",
                "wmcodecdspuuid.lib",
                "ws2_32.lib",
                "secur32.lib",
                "bcrypt.lib",
                "crypt32.lib"
            });
            PublicDelayLoadDLLs.AddRange(new[]
            {
                "livekit_ffi.dll",
                "livekit.dll"
            });
            RuntimeDependencies.Add(
                "$(BinaryOutputDir)/livekit_ffi.dll",
                windowsFfiDll,
                StagedFileType.NonUFS);
            RuntimeDependencies.Add(
                "$(BinaryOutputDir)/livekit.dll",
                windowsLiveKitDll,
                StagedFileType.NonUFS);
            RuntimeDependencies.Add(
                "$(BinaryOutputDir)/LiveKitBridge-THIRD_PARTY_NOTICES.md",
                Path.Combine(PluginDirectory, "THIRD_PARTY_NOTICES.md"),
                StagedFileType.NonUFS);
            RuntimeDependencies.Add(
                "$(BinaryOutputDir)/LiveKitBridge-LICENSE.txt",
                Path.Combine(PluginDirectory, "LICENSE"),
                StagedFileType.NonUFS);
            RuntimeDependencies.Add(
                "$(BinaryOutputDir)/LiveKitBridge-NOTICE.txt",
                Path.Combine(PluginDirectory, "NOTICE"),
                StagedFileType.NonUFS);
        }

        void AddMacFramework(string name)
        {
            string frameworkPath = Path.Combine(appleRoot, "Mac", name + ".framework");
            PublicAdditionalFrameworks.Add(new Framework(
                frameworkPath,
                frameworkPath,
                Framework.FrameworkMode.LinkAndCopy));
        }
    }

    private static bool HasVerifiedWindowsSdk(
        string windowsRoot,
        string sdkRoot,
        string includeRoot,
        params string[] requiredFiles)
    {
        const string pinnedVersion = "1.3.0";
        const string pinnedArchiveSha256 =
            "27a8707348d7fb094023b7c8af29e26b8e4085a4dab75d26be3968f29b2269c3";

        string lockPath = Path.Combine(windowsRoot, "dependencies.lock");
        string sourcePath = Path.Combine(sdkRoot, ".source.json");
        string manifestPath = Path.Combine(sdkRoot, ".files.sha256");
        string markerPath = Path.Combine(sdkRoot, ".verified.json");
        string liveKitHeader = Path.Combine(includeRoot, "livekit", "livekit.h");
        if (!File.Exists(lockPath) ||
            !File.Exists(sourcePath) ||
            !File.Exists(manifestPath) ||
            !File.Exists(markerPath) ||
            !File.Exists(liveKitHeader))
        {
            return false;
        }
        foreach (string requiredFile in requiredFiles)
        {
            if (!File.Exists(requiredFile) || new FileInfo(requiredFile).Length == 0)
            {
                return false;
            }
        }

        string lockText = File.ReadAllText(lockPath);
        if (!Regex.IsMatch(lockText, @"(?m)^LIVEKIT_CPP_VERSION=1\.3\.0\s*$") ||
            !Regex.IsMatch(
                lockText,
                @"(?m)^LIVEKIT_CPP_SHA256=" + pinnedArchiveSha256 + @"\s*$"))
        {
            return false;
        }

        string markerText = File.ReadAllText(markerPath);
        return ReadJsonString(markerText, "version") == pinnedVersion &&
            ReadJsonString(markerText, "archiveSha256") == pinnedArchiveSha256 &&
            ReadJsonString(markerText, "lockSha256") == HashFile(lockPath) &&
            ReadJsonString(markerText, "sourceSha256") == HashFile(sourcePath) &&
            ReadJsonString(markerText, "manifestSha256") == HashFile(manifestPath);
    }

    private static string ReadJsonString(string json, string key)
    {
        Match match = Regex.Match(
            json,
            "\"" + Regex.Escape(key) + "\"\\s*:\\s*\"([^\"]+)\"",
            RegexOptions.CultureInvariant);
        return match.Success ? match.Groups[1].Value.ToLowerInvariant() : string.Empty;
    }

    private static string HashFile(string path)
    {
        using (SHA256 sha256 = SHA256.Create())
        using (FileStream stream = File.OpenRead(path))
        {
            return System.BitConverter.ToString(sha256.ComputeHash(stream))
                .Replace("-", string.Empty)
                .ToLowerInvariant();
        }
    }
}
