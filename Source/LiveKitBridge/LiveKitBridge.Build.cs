using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;
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
        string windowsAdapterRoot = Path.Combine(PluginDirectory, "Source", "WindowsAdapter");
        string windowsAdapterInclude = Path.Combine(windowsAdapterRoot, "include");
        string windowsAdapterLib = Path.Combine(windowsLib, "LiveKitUnrealWindowsAdapter.lib");
        string windowsAdapterDll = Path.Combine(windowsBin, "LiveKitUnrealWindowsAdapter.dll");
        string windowsAdapterPdb = Path.Combine(windowsBin, "LiveKitUnrealWindowsAdapter.pdb");
        bool hasWindowsSdk = isWindows &&
            isInternalBuild &&
            HasVerifiedWindowsSdk(
                windowsRoot,
                windowsSdkRoot,
                windowsInclude,
                windowsAdapterRoot,
                windowsAdapterDll,
                windowsAdapterLib,
                windowsAdapterPdb,
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
            PrivateIncludePaths.Add(windowsAdapterInclude);
            PublicAdditionalLibraries.Add(windowsAdapterLib);
            PublicDelayLoadDLLs.AddRange(new[]
            {
                "livekit_ffi.dll",
                "livekit.dll",
                "LiveKitUnrealWindowsAdapter.dll"
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
                "$(BinaryOutputDir)/LiveKitUnrealWindowsAdapter.dll",
                windowsAdapterDll,
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
        string adapterRoot,
        string adapterDll,
        string adapterLib,
        string adapterPdb,
        params string[] requiredFiles)
    {
        const string pinnedVersion = "1.3.0";
        const string pinnedArchiveSha256 =
            "27a8707348d7fb094023b7c8af29e26b8e4085a4dab75d26be3968f29b2269c3";

        string lockPath = Path.Combine(windowsRoot, "dependencies.lock");
        string sourcePath = Path.Combine(sdkRoot, ".source.json");
        string manifestPath = Path.Combine(sdkRoot, ".files.sha256");
        string adapterBuildPath = Path.Combine(sdkRoot, ".adapter-build.json");
        string markerPath = Path.Combine(sdkRoot, ".verified.json");
        string liveKitHeader = Path.Combine(includeRoot, "livekit", "livekit.h");
        if (!File.Exists(lockPath) ||
            !File.Exists(sourcePath) ||
            !File.Exists(manifestPath) ||
            !File.Exists(adapterBuildPath) ||
            !File.Exists(markerPath) ||
            !File.Exists(liveKitHeader) ||
            !Directory.Exists(Path.Combine(adapterRoot, "include")) ||
            !Directory.Exists(Path.Combine(adapterRoot, "src")) ||
            !File.Exists(adapterDll) ||
            !File.Exists(adapterLib) ||
            !File.Exists(adapterPdb))
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

        string adapterSourceSetSha256 = HashAdapterSourceSet(adapterRoot);
        if (string.IsNullOrEmpty(adapterSourceSetSha256))
        {
            return false;
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
            ReadJsonString(markerText, "manifestSha256") == HashFile(manifestPath) &&
            ReadJsonString(markerText, "adapterBuildSha256") == HashFile(adapterBuildPath) &&
            ReadJsonString(markerText, "adapterSourceSetSha256") == adapterSourceSetSha256 &&
            ReadJsonString(markerText, "adapterDllSha256") == HashFile(adapterDll) &&
            ReadJsonString(markerText, "adapterLibSha256") == HashFile(adapterLib) &&
            ReadJsonString(markerText, "adapterPdbSha256") == HashFile(adapterPdb) &&
            VerifyManifestFiles(sdkRoot, manifestPath);
    }

    private static string HashAdapterSourceSet(string adapterRoot)
    {
        try
        {
            string normalizedRoot = Path.GetFullPath(adapterRoot).TrimEnd(
                Path.DirectorySeparatorChar,
                Path.AltDirectorySeparatorChar);
            string rootPrefix = normalizedRoot + Path.DirectorySeparatorChar;
            List<string> relativePaths = new List<string>();

            foreach (string directoryName in new[] { "include", "src" })
            {
                string directory = Path.Combine(normalizedRoot, directoryName);
                if (!Directory.Exists(directory))
                {
                    return string.Empty;
                }

                foreach (string sourcePath in Directory.GetFiles(
                    directory,
                    "*",
                    SearchOption.AllDirectories))
                {
                    string extension = Path.GetExtension(sourcePath);
                    if (!IsAdapterSourceExtension(extension))
                    {
                        continue;
                    }

                    string fullPath = Path.GetFullPath(sourcePath);
                    if (!fullPath.StartsWith(rootPrefix, StringComparison.OrdinalIgnoreCase))
                    {
                        return string.Empty;
                    }
                    relativePaths.Add(fullPath.Substring(rootPrefix.Length).Replace('\\', '/'));
                }
            }

            if (relativePaths.Count == 0)
            {
                return string.Empty;
            }
            relativePaths.Sort(StringComparer.Ordinal);

            StringBuilder canonical = new StringBuilder();
            foreach (string relativePath in relativePaths)
            {
                string fullPath = Path.Combine(
                    normalizedRoot,
                    relativePath.Replace('/', Path.DirectorySeparatorChar));
                canonical.Append(relativePath)
                    .Append('\t')
                    .Append(HashFile(fullPath))
                    .Append('\n');
            }
            return HashBytes(Encoding.UTF8.GetBytes(canonical.ToString()));
        }
        catch
        {
            return string.Empty;
        }
    }

    private static bool IsAdapterSourceExtension(string extension)
    {
        return extension.Equals(".c", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".cc", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".cpp", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".cxx", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".h", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".hh", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".hpp", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".hxx", StringComparison.OrdinalIgnoreCase);
    }

    private static bool VerifyManifestFiles(string sdkRoot, string manifestPath)
    {
        try
        {
            string rootPrefix = sdkRoot.TrimEnd(
                Path.DirectorySeparatorChar,
                Path.AltDirectorySeparatorChar) + Path.DirectorySeparatorChar;
            int verifiedFileCount = 0;

            foreach (string line in File.ReadAllLines(manifestPath))
            {
                if (string.IsNullOrWhiteSpace(line))
                {
                    continue;
                }

                int separatorIndex = line.IndexOf('\t');
                if (separatorIndex != 64)
                {
                    return false;
                }

                string expectedHash = line.Substring(0, separatorIndex).ToLowerInvariant();
                string relativePath = line.Substring(separatorIndex + 1);
                if (!Regex.IsMatch(expectedHash, "^[a-f0-9]{64}$") ||
                    string.IsNullOrWhiteSpace(relativePath) ||
                    Path.IsPathRooted(relativePath))
                {
                    return false;
                }

                string normalizedRelativePath = relativePath.Replace(
                    Path.AltDirectorySeparatorChar,
                    Path.DirectorySeparatorChar);
                foreach (string segment in normalizedRelativePath.Split(Path.DirectorySeparatorChar))
                {
                    if (segment == "..")
                    {
                        return false;
                    }
                }

                string filePath = Path.GetFullPath(Path.Combine(sdkRoot, normalizedRelativePath));
                if (!filePath.StartsWith(rootPrefix, StringComparison.OrdinalIgnoreCase) ||
                    !File.Exists(filePath) ||
                    HashFile(filePath) != expectedHash)
                {
                    return false;
                }

                verifiedFileCount++;
            }

            return verifiedFileCount > 0;
        }
        catch
        {
            return false;
        }
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

    private static string HashBytes(byte[] bytes)
    {
        using (SHA256 sha256 = SHA256.Create())
        {
            return System.BitConverter.ToString(sha256.ComputeHash(bytes))
                .Replace("-", string.Empty)
                .ToLowerInvariant();
        }
    }
}
