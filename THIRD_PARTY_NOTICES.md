# Third-Party Notices and Release Gate

LiveKit for Unreal downloads and redistributes the following pinned dependencies in its Apple release archive. The corresponding source repositories and license texts are listed here.

The machine-readable companion inventory is `THIRD_PARTY_COMPONENTS.json`. It distinguishes runtime dependencies from build-only tools and records whether binary redistribution has enough upstream provenance to pass this project's release policy.

## LiveKit Swift SDK and XCFramework

- Version: 2.15.1
- Source: <https://github.com/livekit/client-sdk-swift>
- Binary distribution: <https://github.com/livekit/client-sdk-swift-xcframework>
- License: Apache License 2.0, reproduced in this repository's `LICENSE`
- Notice: Copyright 2023 LiveKit, Inc., reproduced in `NOTICE`

## RustLiveKitUniFFI XCFramework

- Version: 0.0.6
- Source: <https://github.com/livekit/livekit-uniffi-xcframework>
- License: Apache License 2.0, reproduced in this repository's `LICENSE`

### Binary provenance limitation

The upstream 0.0.6 binary release publishes the XCFramework and wrapper license, but does not publish the source revision used to build the Rust library, its `Cargo.lock`, an SBOM, or a transitive license inventory. The wrapper's Apache-2.0 license does not by itself identify every Rust crate incorporated into the compiled binary.

Accordingly, source publication of this Unreal integration is approved, but redistribution of the RustLiveKitUniFFI binary is blocked until upstream provenance is available or an independently reproducible source build and complete transitive license report are produced and reviewed.

## SwiftProtobuf

SwiftProtobuf is linked into the LiveKit framework.

- Source: <https://github.com/apple/swift-protobuf>
- License: Apache License 2.0, reproduced in this repository's `LICENSE`
- Additional exception:

> Runtime Library Exception to the Apache 2.0 License: As an exception, if you use this Software to compile your source code and portions of this Software are embedded into the binary product as a result, you may redistribute such product without providing attribution as would otherwise be required by Sections 4(a), 4(b) and 4(d) of the License.

## LiveKitWebRTC distribution wrapper

- Version: 144.7559.10
- Source: <https://github.com/livekit/webrtc-xcframework>
- License: MIT

```text
MIT License

Copyright (c) 2021 WebRTC SDKs

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## WebRTC binary

The `LiveKitWebRTC.xcframework.zip` distribution includes WebRTC under the following BSD 3-Clause license. This is distinct from the MIT license on the XCFramework wrapper repository.

```text
Copyright (c) 2011, The WebRTC project authors. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the
    distribution.

  * Neither the name of Google nor the names of its contributors may
    be used to endorse or promote products derived from this software
    without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Release policy

- Public source: approved. Download scripts fetch the pinned upstream binaries directly and verify their checksums.
- Prebuilt binary plugin: blocked. The binary release gate remains closed because RustLiveKitUniFFI 0.0.6 lacks a reproducible transitive component and license inventory.
- Reopening the binary gate requires updating `THIRD_PARTY_COMPONENTS.json`, preserving every applicable license and notice in the archive, running `Scripts/verify-release-compliance.sh binary`, and recording human review of the generated inventory.

This is an engineering attribution record, not legal advice.
