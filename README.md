# LiveKit for Unreal (Community)

Blueprint-ready LiveKit rooms, audio, data, and RPC for Unreal Engine 5.8 on macOS, iOS, and Windows x64.

This is a community-maintained integration. It is not an official LiveKit SDK and is not affiliated with or endorsed by LiveKit, Inc.

## Status

The current source targets:

- Unreal Engine 5.8
- macOS Editor and game builds
- Physical iOS devices with a minimum deployment target of iOS 15
- Windows x64 Editor and game builds
- LiveKit Swift SDK 2.15.1 on Apple platforms
- LiveKit C++ SDK 1.3.0 on Windows
- Microphone publishing and subscribed room audio
- Participant, speaking-state, reliable/lossy data, incoming byte-stream, and RPC Blueprint APIs

Video, screen sharing, Android, tvOS, visionOS, and a production token service are outside the current source scope.

## Prebuilt release status

Prebuilt binaries are not published. The pinned RustLiveKitUniFFI Apple release and LiveKit C++ Windows release have not supplied a complete, reviewed transitive binary license inventory for this project. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the enforced release gate.

## Install from source

Clone this repository directly into the project's plugin directory, then fetch and verify the dependencies for the host platform.

Apple:

```sh
git clone https://github.com/mgwilt/livekit-unreal.git Plugins/LiveKitBridge
./Plugins/LiveKitBridge/Scripts/fetch-livekit-apple.sh
./Plugins/LiveKitBridge/Scripts/verify-livekit-apple.sh
```

Windows PowerShell:

```powershell
.\Plugins\LiveKitBridge\Scripts\fetch-livekit-windows.ps1
.\Plugins\LiveKitBridge\Scripts\verify-livekit-windows.ps1
```

The dependency archives, extracted frameworks, generated headers, libraries, DLLs, and facade static libraries are intentionally excluded from Git. The Windows fetch script verifies the pinned archive checksum before atomically replacing `Source/ThirdParty/Windows/SDK`; the separate verifier checks every installed file and writes the lock-bound marker required to enable the Win64 backend at build time.

## Blueprint quick start

1. Obtain a LiveKit server URL and participant token from your application backend. Never put an API secret in an Unreal client.
2. Use **Connect to LiveKit**, or get the `LiveKitSubsystem` game-instance subsystem and call `Connect` with the URL, token, and `LiveKitConnectOptions`.
3. Bind to `On Connection State Changed`, `On Error`, participant, speaking, data, and RPC events as needed.
4. Use `Set Microphone Enabled` to control local microphone publication. Subscribed room audio is managed by the selected platform SDK.
5. Use **Disconnect from LiveKit** to wait for a confirmed SDK disconnect before treating the room connection as ended.

Transient network failures enter `Reconnecting` and retain the same LiveKit room. An explicit disconnect enters `Disconnecting` and ends that connection.

`LiveKitConnectOptions` enables Apple's voice-processing I/O by default for echo cancellation and automatic gain control. A host that provides its own processing, or a macOS runtime that cannot safely initialize the system voice processor, can disable `Enable Voice Processing` before connecting without disabling microphone publication. LiveKit's audio manager is process-global, so all concurrent rooms in one process must use the same setting; the most recent connection option is applied before that room connects.

### RPC

Register an incoming method with `Register Rpc Method`, handle `On Rpc Invocation`, and finish each request with `Complete Rpc Invocation` or `Fail Rpc Invocation`. Incoming Blueprint requests expire if they are not completed in time.

Use **Perform LiveKit RPC** for an asynchronous outbound call. The default response timeout is 15 seconds and the default maximum round-trip latency is 7 seconds.

On Apple platforms, LiveKit Swift 2.15.1 cannot construct a custom public `RpcError` for an incoming failure. An incoming Blueprint failure is therefore reported to the caller as LiveKit's application error rather than a custom wire-level error code.

### Data

Use `Publish Text` or `Publish Data` with reliable or lossy delivery and optional destination identities. Listen on `On Data Received` for general data packets, and use `LiveKit Data Message As Text` when the payload contains UTF-8 text.

### Incoming byte streams

Call `Register Byte Stream Handler` with an exact, application-defined topic, then bind `On Byte Stream Received`. Each `LiveKit Byte Stream` contains the sender identity, stream ID, topic, optional name, MIME type, attributes, and complete byte payload. Call `Unregister Byte Stream Handler` when that topic is no longer needed.

Registrations survive transient reconnects and are restored when an explicit disconnect creates the next room connection. The plugin accumulates chunks up to a hard 8 MiB limit before broadcasting the completed payload on Unreal's game thread. A larger declared length or accumulated payload is rejected through `On Error` with code `byte_stream_too_large`. Topics beginning with LiveKit's reserved `lk.rpc` prefix are rejected by the pinned SDK.

## iOS microphone permission

Every host project that enables microphone capture must provide a user-facing `NSMicrophoneUsageDescription`. For example, in `Config/DefaultEngine.ini`:

```ini
[/Script/IOSRuntimeSettings.IOSRuntimeSettings]
MinimumiOSVersion=IOS_15
AdditionalPlistData=<key>NSMicrophoneUsageDescription</key><string>This app uses the microphone for realtime conversation.</string>
```

The plugin does not choose a bundle identifier, signing team, orientation, token endpoint, or privacy wording for the host application.

## Development

Set `UE_ROOT` when Unreal is not installed at `/Users/Shared/Epic Games/UE_5.8`.

```sh
export UE_ROOT="/Users/Shared/Epic Games/UE_5.8"
./Scripts/fetch-livekit-apple.sh
./Scripts/test-plugin.sh
./Scripts/verify-release-compliance.sh source
```

On Windows, run the dependency verification from PowerShell before building:

```powershell
$env:UE_ROOT = "D:\unreal\engines\UE_5.8"
.\Scripts\fetch-livekit-windows.ps1
.\Scripts\verify-livekit-windows.ps1
```

`test-plugin.sh` packages the plugin and loads it in the repository's Blueprint-only smoke project before running the `LiveKitBridge.*` Unreal automation tests.

Binary packaging intentionally fails until the transitive binary attribution gate has been completed and explicitly approved.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the development workflow and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for dependency attribution.

## Security

Generate participant tokens on a trusted server and grant only the permissions each client needs. See [SECURITY.md](SECURITY.md) for vulnerability reporting.

## License

The plugin is licensed under Apache License 2.0. LiveKit and its transitive dependencies retain their own licenses and notices.
