# LiveKit for Unreal (Community)

Blueprint-ready LiveKit rooms, audio, data, and RPC for Unreal Engine 5.8 on macOS and iOS.

This is a community-maintained integration. It is not an official LiveKit SDK and is not affiliated with or endorsed by LiveKit, Inc.

## Status

Version `0.1.0` targets:

- Unreal Engine 5.8
- macOS Editor and game builds
- Physical iOS devices with a minimum deployment target of iOS 15
- LiveKit Swift SDK 2.15.1
- Microphone publishing and subscribed room audio
- Participant, speaking-state, reliable/lossy data, and RPC Blueprint APIs

Video, screen sharing, Android, Windows, tvOS, visionOS, and a production token service are outside the 0.1 release.

## Prebuilt release status

Prebuilt binaries are not published for 0.1.0. The pinned RustLiveKitUniFFI release does not provide enough provenance to generate a complete transitive binary license inventory. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the enforced release gate.

## Install from source

Clone this repository directly into the project's plugin directory, then fetch and verify the pinned Apple dependencies:

```sh
git clone https://github.com/mgwilt/livekit-unreal.git Plugins/LiveKitBridge
./Plugins/LiveKitBridge/Scripts/fetch-livekit-apple.sh
./Plugins/LiveKitBridge/Scripts/verify-livekit-apple.sh
```

The dependency archives, extracted frameworks, generated Objective-C headers, and facade static libraries are intentionally excluded from Git.

## Blueprint quick start

1. Obtain a LiveKit server URL and participant token from your application backend. Never put an API secret in an Unreal client.
2. Use **Connect to LiveKit**, or get the `LiveKitSubsystem` game-instance subsystem and call `Connect` with the URL, token, and `LiveKitConnectOptions`.
3. Bind to `On Connection State Changed`, `On Error`, participant, speaking, data, and RPC events as needed.
4. Use `Set Microphone Enabled` to control local microphone publication. Subscribed room audio is managed by LiveKit's Apple audio session.
5. Use **Disconnect from LiveKit** to wait for a confirmed SDK disconnect before treating the room connection as ended.

Transient network failures enter `Reconnecting` and retain the same LiveKit room. An explicit disconnect enters `Disconnecting` and ends that connection.

### RPC

Register an incoming method with `Register Rpc Method`, handle `On Rpc Invocation`, and finish each request with `Complete Rpc Invocation` or `Fail Rpc Invocation`. Incoming Blueprint requests expire if they are not completed in time.

Use **Perform LiveKit RPC** for an asynchronous outbound call. The default response timeout is 15 seconds and the default maximum round-trip latency is 7 seconds.

LiveKit Swift 2.15.1 cannot construct a custom public `RpcError` for an incoming failure. In 0.1, an incoming Blueprint failure is reported to the caller as LiveKit's application error rather than a custom wire-level error code.

### Data

Use `Publish Text` or `Publish Data` with reliable or lossy delivery and optional destination identities. Listen on `On Data Received` for general data packets, and use `LiveKit Data Message As Text` when the payload contains UTF-8 text.

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

`test-plugin.sh` packages the plugin and loads it in the repository's Blueprint-only smoke project before running the `LiveKitBridge.*` Unreal automation tests.

Binary packaging intentionally fails until the transitive binary attribution gate has been completed and explicitly approved.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the development workflow and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for dependency attribution.

## Security

Generate participant tokens on a trusted server and grant only the permissions each client needs. See [SECURITY.md](SECURITY.md) for vulnerability reporting.

## License

The plugin is licensed under Apache License 2.0. LiveKit and its transitive dependencies retain their own licenses and notices.
