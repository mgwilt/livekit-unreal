[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$PluginRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$HelperScript = Join-Path $PSScriptRoot "livekit-windows-sdk-paths.ps1"
. $HelperScript

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Initialize-ManagedInstall {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$RootWindows,
        [Parameter(Mandatory = $true)][DateTimeOffset]$CreatedUtc,
        [ValidateSet("fetch", "adapter-rebuild")][string]$Kind = "fetch"
    )

    New-Item -ItemType Directory -Path $Root -Force | Out-Null
    foreach ($MarkerName in @(
        ".source.json",
        ".files.sha256",
        ".adapter-build.json",
        ".verified.json"
    )) {
        [IO.File]::WriteAllText((Join-Path $Root $MarkerName), "test")
    }
    New-LiveKitWindowsManagedSdkInstallMarker `
        -WindowsRoot $RootWindows `
        -SdkRoot $Root `
        -InstallKind $Kind `
        -CreatedUtc $CreatedUtc
}

$TestRoot = Join-Path $PluginRoot ("Intermediate\SdkPathTests\{0}" -f [Guid]::NewGuid().ToString("N"))
$WindowsRoot = Join-Path $TestRoot "Windows"
$LegacySdk = Join-Path $WindowsRoot "SDK"
$InstallsRoot = Join-Path $WindowsRoot "SDKs"
$InstallA = Join-Path $InstallsRoot "livekit-1.3.0-27a8707348d7fb09-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
$InstallB = Join-Path $InstallsRoot "livekit-1.3.0-27a8707348d7fb09-adapter-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
$InstallC = Join-Path $InstallsRoot "livekit-1.3.0-27a8707348d7fb09-cccccccccccccccccccccccccccccccc"
$UnknownInstall = Join-Path $InstallsRoot "livekit-1.3.0-27a8707348d7fb09-dddddddddddddddddddddddddddddddd"
$PointerPath = Join-Path $WindowsRoot "active-sdk.txt"
$HeldLock = $null
$WaitingJob = $null

try {
    New-Item -ItemType Directory -Path $LegacySdk -Force | Out-Null
    Initialize-ManagedInstall `
        -Root $InstallA `
        -RootWindows $WindowsRoot `
        -CreatedUtc ([DateTimeOffset]::UtcNow.AddDays(-2))
    Initialize-ManagedInstall `
        -Root $InstallB `
        -RootWindows $WindowsRoot `
        -CreatedUtc ([DateTimeOffset]::UtcNow.AddDays(-1)) `
        -Kind "adapter-rebuild"
    Initialize-ManagedInstall `
        -Root $InstallC `
        -RootWindows $WindowsRoot `
        -CreatedUtc ([DateTimeOffset]::UtcNow.AddDays(-4))
    New-Item -ItemType Directory -Path $UnknownInstall -Force | Out-Null

    $Resolved = Resolve-LiveKitWindowsSdkRoot -WindowsRoot $WindowsRoot
    Assert-True `
        -Condition (Test-LiveKitWindowsPathEqual -Left $Resolved -Right $LegacySdk) `
        -Message "The legacy literal SDK fallback did not resolve."

    $null = Set-LiveKitWindowsActiveSdk -WindowsRoot $WindowsRoot -SdkRoot $InstallA
    Assert-True `
        -Condition ([IO.File]::ReadAllText($PointerPath) -ceq "SDKs/$([IO.Path]::GetFileName($InstallA))") `
        -Message "The first active pointer was not written in canonical form."
    $Resolved = Resolve-LiveKitWindowsSdkRoot -WindowsRoot $WindowsRoot
    Assert-True `
        -Condition (Test-LiveKitWindowsPathEqual -Left $Resolved -Right $InstallA) `
        -Message "The first immutable SDK install did not resolve."

    # A process interrupted before File.Replace leaves only an unreferenced
    # temporary file; the prior pointer remains fully usable.
    [IO.File]::WriteAllText("$PointerPath.interrupted.tmp", "SDKs/$([IO.Path]::GetFileName($InstallB))")
    $Resolved = Resolve-LiveKitWindowsSdkRoot -WindowsRoot $WindowsRoot
    Assert-True `
        -Condition (Test-LiveKitWindowsPathEqual -Left $Resolved -Right $InstallA) `
        -Message "An interrupted pointer write changed the active SDK."

    $null = Set-LiveKitWindowsActiveSdk -WindowsRoot $WindowsRoot -SdkRoot $InstallB
    $Resolved = Resolve-LiveKitWindowsSdkRoot -WindowsRoot $WindowsRoot
    Assert-True `
        -Condition (Test-LiveKitWindowsPathEqual -Left $Resolved -Right $InstallB) `
        -Message "Atomic replacement did not activate the second SDK install."

    foreach ($UnsafePointer in @(
        "C:\outside",
        "\server\share",
        "../SDK",
        "SDKs/../SDK",
        "SDKs/livekit/a",
        " SDKs/$([IO.Path]::GetFileName($InstallA))",
        "SDKs/$([IO.Path]::GetFileName($InstallA)).",
        "SDKs/$([IO.Path]::GetFileName($InstallA))`nSDKs/$([IO.Path]::GetFileName($InstallB))"
    )) {
        [IO.File]::WriteAllText($PointerPath, $UnsafePointer)
        $Rejected = $false
        try {
            $null = Resolve-LiveKitWindowsSdkRootBeforeReplacement -WindowsRoot $WindowsRoot
        }
        catch {
            $Rejected = $true
        }
        Assert-True -Condition $Rejected -Message "Non-Force pointer resolution accepted: $UnsafePointer"
        $RecoveryRoot = Resolve-LiveKitWindowsSdkRootBeforeReplacement `
            -WindowsRoot $WindowsRoot `
            -AllowInvalidActivePointer
        Assert-True `
            -Condition ($null -eq $RecoveryRoot) `
            -Message "Force recovery did not tolerate invalid pointer state: $UnsafePointer"
    }
    [IO.File]::WriteAllText($PointerPath, "SDKs/missing-managed-install")
    $RecoveryRoot = Resolve-LiveKitWindowsSdkRootBeforeReplacement `
        -WindowsRoot $WindowsRoot `
        -AllowInvalidActivePointer
    Assert-True `
        -Condition ($null -eq $RecoveryRoot) `
        -Message "Force recovery did not tolerate a missing pointer target."
    $FetchScriptText = Get-Content -LiteralPath (Join-Path $PSScriptRoot "fetch-livekit-windows.ps1") -Raw
    Assert-True `
        -Condition ($FetchScriptText -match 'Resolve-LiveKitWindowsSdkRootBeforeReplacement[\s\S]+-AllowInvalidActivePointer:\$Force') `
        -Message "The fetch workflow no longer binds Force to recovery-safe pointer resolution."

    # The exclusive file handle serializes top-level fetch/rebuild work. A
    # second process must remain blocked until the first operation releases it.
    $HeldLock = Enter-LiveKitWindowsSdkOperationLock -WindowsRoot $WindowsRoot
    $WaitingJob = Start-Job -ScriptBlock {
        param($HelperPath, $Root)
        . $HelperPath
        $Stopwatch = [Diagnostics.Stopwatch]::StartNew()
        $Lock = Enter-LiveKitWindowsSdkOperationLock -WindowsRoot $Root -TimeoutSeconds 10
        try {
            $Stopwatch.Stop()
            return $Stopwatch.ElapsedMilliseconds
        }
        finally {
            Exit-LiveKitWindowsSdkOperationLock -Lock $Lock
        }
    } -ArgumentList $HelperScript, $WindowsRoot
    Start-Sleep -Milliseconds 600
    Assert-True `
        -Condition ($WaitingJob.State -eq "Running") `
        -Message "A concurrent SDK operation bypassed the WindowsRoot-scoped lock."
    Exit-LiveKitWindowsSdkOperationLock -Lock $HeldLock
    $HeldLock = $null
    $null = Wait-Job -Job $WaitingJob -Timeout 10
    $WaitMilliseconds = [int64](Receive-Job -Job $WaitingJob)
    Assert-True `
        -Condition ($WaitMilliseconds -ge 400) `
        -Message "A concurrent SDK operation did not wait for the active lock."
    Remove-Job -Job $WaitingJob -Force
    $WaitingJob = $null

    $null = Set-LiveKitWindowsActiveSdk -WindowsRoot $WindowsRoot -SdkRoot $InstallB
    $CleanupRejected = $false
    try {
        Remove-LiveKitWindowsObsoleteSdkInstalls `
            -WindowsRoot $WindowsRoot `
            -ActiveSdkRoot $InstallA
    }
    catch {
        $CleanupRejected = $true
    }
    Assert-True `
        -Condition ($CleanupRejected -and (Test-Path -LiteralPath $InstallA -PathType Container)) `
        -Message "Cleanup ran without proving which immutable install was active."

    Remove-LiveKitWindowsObsoleteSdkInstalls `
        -WindowsRoot $WindowsRoot `
        -ActiveSdkRoot $InstallB `
        -PreviousActiveSdkRoot $InstallA `
        -MinimumAgeHours 0
    Assert-True `
        -Condition ((Test-Path -LiteralPath $InstallB -PathType Container) -and
            (Test-Path -LiteralPath $InstallA -PathType Container) -and
            -not (Test-Path -LiteralPath $InstallC) -and
            (Test-Path -LiteralPath $UnknownInstall -PathType Container) -and
            (Test-Path -LiteralPath $LegacySdk -PathType Container)) `
        -Message "Conservative cleanup did not retain active/previous/unknown/legacy SDK paths."

    # When an SDK is installed, the public explicit worker path must reject the
    # active immutable root before touching any marker or generated binary.
    $ActualWindowsRoot = Join-Path $PluginRoot "Source\ThirdParty\Windows"
    try {
        $ActualActiveSdkRoot = Resolve-LiveKitWindowsSdkRoot -WindowsRoot $ActualWindowsRoot
    }
    catch {
        $ActualActiveSdkRoot = $null
    }
    if ($null -ne $ActualActiveSdkRoot -and
        (Test-Path -LiteralPath $ActualActiveSdkRoot -PathType Container)) {
        $ActualVerificationMarker = Join-Path $ActualActiveSdkRoot ".verified.json"
        $BeforeHash = if (Test-Path -LiteralPath $ActualVerificationMarker -PathType Leaf) {
            (Get-FileHash -LiteralPath $ActualVerificationMarker -Algorithm SHA256).Hash
        }
        else {
            "missing"
        }
        $ActiveBuildRejected = $false
        try {
            & (Join-Path $PSScriptRoot "build-livekit-windows-adapter.ps1") `
                -SdkRoot $ActualActiveSdkRoot
        }
        catch {
            $ActiveBuildRejected = $_.Exception.Message -like "Refusing to rebuild the active immutable SDK*"
        }
        $AfterHash = if (Test-Path -LiteralPath $ActualVerificationMarker -PathType Leaf) {
            (Get-FileHash -LiteralPath $ActualVerificationMarker -Algorithm SHA256).Hash
        }
        else {
            "missing"
        }
        Assert-True `
            -Condition ($ActiveBuildRejected -and $BeforeHash -eq $AfterHash) `
            -Message "The explicit adapter worker did not reject the active SDK without mutation."
    }

    Write-Host "LiveKit Windows SDK atomic activation tests passed."
}
finally {
    Exit-LiveKitWindowsSdkOperationLock -Lock $HeldLock
    if ($null -ne $WaitingJob) {
        Stop-Job -Job $WaitingJob -ErrorAction SilentlyContinue
        Remove-Job -Job $WaitingJob -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $TestRoot) {
        Remove-Item -LiteralPath $TestRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
