# Contributing

Thanks for helping improve LiveKit for Unreal. This repository is a community project and follows the behavior of the pinned LiveKit Apple SDK rather than defining a separate wire protocol.

## Development requirements

- macOS with Xcode and the iOS SDK
- Windows x64 with Visual Studio 2022 and the Desktop development with C++ workload
- Unreal Engine 5.8
- A clone with no generated Unreal, Apple dependency, Windows SDK, or Windows adapter binary output committed

Set the engine location when needed, then bootstrap the exact dependencies:

```sh
export UE_ROOT="<path-to-UE-5.8>"
./Scripts/fetch-livekit-apple.sh
./Scripts/verify-livekit-apple.sh
```

For Windows development, use PowerShell:

```powershell
$env:UE_ROOT = "<path-to-UE-5.8>"
.\Scripts\fetch-livekit-windows.ps1
.\Scripts\verify-livekit-windows.ps1
```

When any C/C++ file under `Source/WindowsAdapter/include` or `Source/WindowsAdapter/src` changes, run `Scripts/build-livekit-windows-adapter.ps1` followed by `Scripts/verify-livekit-windows.ps1`. The build provenance records the exact ordinal-sorted path and SHA-256 of every source plus a canonical source-set hash, so Unreal will not enable the Win64 backend with stale generated binaries.

## Before opening a pull request

Run:

```sh
./Scripts/test-plugin.sh
git diff --check
```

Changes to public Blueprint functions, reflected structs, enums, events, defaults, or error semantics must include matching automation coverage and README or changelog updates.

Do not commit:

- XCFramework archives or extracted frameworks
- generated Swift facade archives or headers
- downloaded LiveKit Windows SDK files or generated Windows adapter DLL, import library, and PDB
- Unreal `Binaries`, `Intermediate`, `Saved`, or derived data
- room tokens, API keys, API secrets, sandbox identifiers, signing credentials, or local configuration

## Pull request scope

Keep changes focused. Explain the user-visible behavior, supported platforms, tests run, and any compatibility impact. A change that alters the LiveKit SDK version must also update `Source/ThirdParty/Apple/dependencies.lock`, its verified checksums, third-party notices when necessary, and release validation.

External pull requests are never executed automatically on the project's self-hosted Unreal release runner. Maintainers run native validation only after reviewing the code.

## Releases

The integer `Version` in `LiveKitBridge.uplugin` must increase for every release, while `VersionName` follows semantic versioning. A release tag must be exactly `v<VersionName>`.

Release archives are built from a trusted tag with `Scripts/package-release.sh`. Do not change repository visibility or publish a draft release until dependency attribution, clean Blueprint installation, full-history secret scanning, and Mac/iOS validation have passed.
