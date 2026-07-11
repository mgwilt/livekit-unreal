# Security Policy

## Supported versions

Security fixes are provided for the latest 0.x release line. Pre-release builds and older commits may not receive backports.

| Version | Supported |
| --- | --- |
| 0.1.x | Yes |
| Earlier | No |

## Reporting a vulnerability

Please report vulnerabilities privately through the **Security** tab's private vulnerability reporting flow at `mgwilt/livekit-unreal`. Do not open a public issue for an unpatched vulnerability and do not include room tokens, API credentials, signing material, or personal data in a report.

Include the affected plugin and Unreal versions, target platform, impact, reproduction steps, and any suggested mitigation. A maintainer will acknowledge the report, assess scope, and coordinate disclosure after a fix is available.

## Credential model

This plugin accepts a LiveKit server URL and participant token supplied by its host application. It must never receive or embed a LiveKit API secret. Production applications should issue short-lived, least-privilege participant tokens from a trusted backend over an authenticated connection.
