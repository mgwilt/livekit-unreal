# Changelog

All notable changes to LiveKit for Unreal are documented in this file.

The project follows [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- Pinned, checksum-verified LiveKit C++ SDK 1.3.0 dependency bootstrap for Windows x64, with immutable versioned installs, atomic active-pointer replacement, and installed-file verification.
- Visual Studio 2022 build and source-bound verification for the generated Windows ABI adapter, with automatic stale-binary fallback and runtime staging.
- Process-lifetime Win64 runtime pinning and dynamic-reload rejection so late FFI worker cleanup cannot execute from unloaded DLLs during application exit.
- Windows SDK provenance and transitive-license review status in the third-party component inventory and notices.

## [0.2.0] - 2026-07-13

### Added

- Blueprint-ready incoming byte streams with topic registration, sender and stream metadata, attributes, bounded complete-payload delivery, and room-recreation persistence.
- Blueprint-configurable Apple voice processing, applied before room connection so hosts can opt out without disabling microphone publication.

### Fixed

- Retained publish-operation and RPC request identifiers by value across asynchronous Apple SDK callbacks, preventing callbacks from reading expired caller state.
- Reported Apple audio-configuration failures through the normal LiveKit error and connection-state APIs before attempting room connection.

## [0.1.0] - 2026-07-10

### Added

- Blueprint-first `ULiveKitSubsystem` for direct URL/token connections.
- Confirmed connection, reconnection, and disconnect lifecycle states.
- Microphone publication and subscribed room audio on macOS and iOS.
- Participant, speaking-state, reliable/lossy data, and RPC events.
- Blueprint async actions for connect, disconnect, and outbound RPC.
- Verified LiveKit 2.15.1 Apple dependency bootstrap and generated Swift RPC facade.
- UE 5.8 source builds and precompiled Mac/iOS release packaging.

[Unreleased]: https://github.com/mgwilt/livekit-unreal/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/mgwilt/livekit-unreal/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/mgwilt/livekit-unreal/releases/tag/v0.1.0
