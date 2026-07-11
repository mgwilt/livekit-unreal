using System.IO;
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

        void AddMacFramework(string name)
        {
            string frameworkPath = Path.Combine(appleRoot, "Mac", name + ".framework");
            PublicAdditionalFrameworks.Add(new Framework(
                frameworkPath,
                frameworkPath,
                Framework.FrameworkMode.LinkAndCopy));
        }
    }
}
