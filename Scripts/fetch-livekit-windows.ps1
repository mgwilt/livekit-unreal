[CmdletBinding()]
param(
    [switch]$Force,
    [string]$VisualStudioRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$PluginRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$WindowsRoot = Join-Path $PluginRoot "Source\ThirdParty\Windows"
$LockPath = Join-Path $WindowsRoot "dependencies.lock"
$VerifyScript = Join-Path $PSScriptRoot "verify-livekit-windows.ps1"
$BuildAdapterScript = Join-Path $PSScriptRoot "build-livekit-windows-adapter.ps1"
. (Join-Path $PSScriptRoot "livekit-windows-sdk-paths.ps1")

function Read-DependencyLock {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Dependency lock file is missing: $Path"
    }

    $Values = @{}
    foreach ($Line in Get-Content -LiteralPath $Path) {
        $Trimmed = $Line.Trim()
        if ($Trimmed.Length -eq 0 -or $Trimmed.StartsWith("#")) {
            continue
        }
        if ($Trimmed -notmatch '^([A-Z0-9_]+)=(.+)$') {
            throw "Invalid dependency lock entry: $Line"
        }
        if ($Values.ContainsKey($Matches[1])) {
            throw "Duplicate dependency lock key: $($Matches[1])"
        }
        $Values[$Matches[1]] = $Matches[2]
    }
    return $Values
}

function Require-LockValue {
    param(
        [Parameter(Mandatory = $true)][hashtable]$Values,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if (-not $Values.ContainsKey($Name) -or [string]::IsNullOrWhiteSpace($Values[$Name])) {
        throw "Dependency lock is missing $Name"
    }
    return [string]$Values[$Name]
}

function Assert-SdkLayout {
    param([Parameter(Mandatory = $true)][string]$Root)

    foreach ($Directory in @("include", "lib", "bin")) {
        $Path = Join-Path $Root $Directory
        if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
            throw "LiveKit SDK archive is missing the $Directory directory"
        }
    }

    foreach ($RelativePath in @(
        "lib\livekit.lib",
        "lib\livekit_ffi.dll.lib",
        "bin\livekit.dll",
        "bin\livekit_ffi.dll"
    )) {
        $Path = Join-Path $Root $RelativePath
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -eq 0) {
            throw "LiveKit SDK archive is missing required file: $RelativePath"
        }
    }

    $Headers = @(Get-ChildItem -LiteralPath (Join-Path $Root "include\livekit") -Filter "*.h" -File -ErrorAction SilentlyContinue)
    if ($Headers.Count -eq 0) {
        throw "LiveKit SDK archive does not contain C++ headers under include/livekit"
    }
}

$Lock = Read-DependencyLock -Path $LockPath
$Version = Require-LockValue -Values $Lock -Name "LIVEKIT_CPP_VERSION"
$ArchiveName = Require-LockValue -Values $Lock -Name "LIVEKIT_CPP_ARCHIVE"
$DownloadUrl = Require-LockValue -Values $Lock -Name "LIVEKIT_CPP_URL"
$ExpectedHash = (Require-LockValue -Values $Lock -Name "LIVEKIT_CPP_SHA256").ToLowerInvariant()

if ($ExpectedHash -notmatch '^[a-f0-9]{64}$') {
    throw "LIVEKIT_CPP_SHA256 must be a 64-character hexadecimal SHA-256 digest"
}

$DownloadUri = [Uri]$DownloadUrl
if ($DownloadUri.Scheme -ne "https" -or [IO.Path]::GetFileName($DownloadUri.AbsolutePath) -ne $ArchiveName) {
    throw "LIVEKIT_CPP_URL must be HTTPS and end with LIVEKIT_CPP_ARCHIVE"
}

$OperationLock = Enter-LiveKitWindowsSdkOperationLock -WindowsRoot $WindowsRoot
$TransactionRoot = $null
try {
$PreviousSdkRoot = Resolve-LiveKitWindowsSdkRootBeforeReplacement `
    -WindowsRoot $WindowsRoot `
    -AllowInvalidActivePointer:$Force
if (-not [string]::IsNullOrWhiteSpace($PreviousSdkRoot) -and
    -not (Test-Path -LiteralPath $PreviousSdkRoot -PathType Container)) {
    $PreviousSdkRoot = $null
}

if (-not $Force -and $null -ne $PreviousSdkRoot) {
    try {
        & $VerifyScript -SdkRoot $PreviousSdkRoot -Quiet
        Write-Host "LiveKit C++ SDK $Version is already installed and verified."
        return
    }
    catch {
        Write-Warning "The existing SDK did not verify and will be replaced: $($_.Exception.Message)"
    }
}

New-Item -ItemType Directory -Path $WindowsRoot -Force | Out-Null
$TransactionId = [Guid]::NewGuid().ToString("N")
$TransactionRoot = Join-Path (Join-Path $WindowsRoot ".extract") $TransactionId
$DownloadPath = Join-Path $TransactionRoot $ArchiveName
$ExtractRoot = Join-Path $TransactionRoot "archive"
$StagedSdk = Join-Path $TransactionRoot "SDK-ready"
$InstallsRoot = Get-LiveKitWindowsSdkInstallsRoot -WindowsRoot $WindowsRoot
$InstallName = "livekit-{0}-{1}-{2}" -f $Version, $ExpectedHash.Substring(0, 16), $TransactionId
if ($InstallName -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') {
    throw "LIVEKIT_CPP_VERSION cannot be represented as a safe immutable SDK install name"
}
$InstalledSdk = Join-Path $InstallsRoot $InstallName
$ActivationSucceeded = $false

New-Item -ItemType Directory -Path $TransactionRoot -Force | Out-Null

    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $DownloadError = $null
    foreach ($Attempt in 1..3) {
        try {
            Write-Host "Downloading LiveKit C++ SDK $Version (attempt $Attempt of 3)..."
            Invoke-WebRequest -Uri $DownloadUri -OutFile $DownloadPath -UseBasicParsing
            $DownloadError = $null
            break
        }
        catch {
            $DownloadError = $_
            if ($Attempt -lt 3) {
                Start-Sleep -Seconds (2 * $Attempt)
            }
        }
    }
    if ($null -ne $DownloadError) {
        throw $DownloadError
    }

    $ActualHash = (Get-FileHash -LiteralPath $DownloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($ActualHash -ne $ExpectedHash) {
        throw "LiveKit SDK checksum mismatch. Expected $ExpectedHash but downloaded $ActualHash"
    }

    New-Item -ItemType Directory -Path $ExtractRoot -Force | Out-Null
    Expand-Archive -LiteralPath $DownloadPath -DestinationPath $ExtractRoot -Force

    $ArchiveRoot = $ExtractRoot
    if (-not (Test-Path -LiteralPath (Join-Path $ArchiveRoot "include") -PathType Container)) {
        $TopLevelDirectories = @(Get-ChildItem -LiteralPath $ExtractRoot -Directory)
        if ($TopLevelDirectories.Count -eq 1 -and
            (Test-Path -LiteralPath (Join-Path $TopLevelDirectories[0].FullName "include") -PathType Container)) {
            $ArchiveRoot = $TopLevelDirectories[0].FullName
        }
    }

    Assert-SdkLayout -Root $ArchiveRoot
    Move-Item -LiteralPath $ArchiveRoot -Destination $StagedSdk

    $ManifestLines = @(
        Get-ChildItem -LiteralPath $StagedSdk -Recurse -File |
            Sort-Object FullName |
            ForEach-Object {
                $RelativePath = $_.FullName.Substring($StagedSdk.Length + 1).Replace('\', '/')
                $Hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
                "{0}`t{1}" -f $Hash, $RelativePath
            }
    )
    if ($ManifestLines.Count -eq 0) {
        throw "LiveKit SDK archive is empty"
    }
    $ManifestLines | Set-Content -LiteralPath (Join-Path $StagedSdk ".files.sha256") -Encoding ASCII

    [ordered]@{
        version = $Version
        archive = $ArchiveName
        url = $DownloadUrl
        sha256 = $ExpectedHash
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $StagedSdk ".source.json") -Encoding UTF8

    $AdapterBuildArguments = @{ SdkRoot = $StagedSdk }
    if (-not [string]::IsNullOrWhiteSpace($VisualStudioRoot)) {
        $AdapterBuildArguments.VisualStudioRoot = $VisualStudioRoot
    }
    $AdapterBuildArguments.InternalWorker = $true
    & $BuildAdapterScript @AdapterBuildArguments
    & $VerifyScript -SdkRoot $StagedSdk -Quiet

    New-Item -ItemType Directory -Path $InstallsRoot -Force | Out-Null
    Assert-LiveKitWindowsSdkPathIsNotReparsePoint -Path $InstallsRoot
    try {
        # The install directory is immutable once activated. It is first moved
        # into a unique, inactive path and verified there; only the tiny pointer
        # file is replaced during activation.
        Move-Item -LiteralPath $StagedSdk -Destination $InstalledSdk
        New-LiveKitWindowsManagedSdkInstallMarker `
            -WindowsRoot $WindowsRoot `
            -SdkRoot $InstalledSdk `
            -InstallKind "fetch"
        & $VerifyScript -SdkRoot $InstalledSdk -Quiet
        $null = Set-LiveKitWindowsActiveSdk -WindowsRoot $WindowsRoot -SdkRoot $InstalledSdk
        $ActivationSucceeded = $true
    }
    catch {
        # Until pointer replacement succeeds, the previous pointer and SDK are
        # untouched. Only the never-active candidate is safe to remove here.
        if (-not $ActivationSucceeded -and (Test-Path -LiteralPath $InstalledSdk)) {
            Remove-Item -LiteralPath $InstalledSdk -Recurse -Force -ErrorAction SilentlyContinue
        }
        throw
    }

    # Cleanup happens strictly after successful activation. Individual cleanup
    # failures are warnings because the new pointer already names a verified SDK.
    try {
        Remove-LiveKitWindowsObsoleteSdkInstalls `
            -WindowsRoot $WindowsRoot `
            -ActiveSdkRoot $InstalledSdk `
            -PreviousActiveSdkRoot $PreviousSdkRoot
    }
    catch {
        Write-Warning "The new SDK is active, but obsolete install cleanup failed: $($_.Exception.Message)"
    }

    Write-Host "LiveKit C++ SDK $Version installed and verified at $InstalledSdk"
}
finally {
    if ($null -ne $TransactionRoot -and (Test-Path -LiteralPath $TransactionRoot)) {
        Remove-Item -LiteralPath $TransactionRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
    Exit-LiveKitWindowsSdkOperationLock -Lock $OperationLock
}
