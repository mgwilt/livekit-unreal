[CmdletBinding()]
param(
    [string]$SdkRoot,
    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$PluginRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$WindowsRoot = Join-Path $PluginRoot "Source\ThirdParty\Windows"
$LockPath = Join-Path $WindowsRoot "dependencies.lock"
if ([string]::IsNullOrWhiteSpace($SdkRoot)) {
    $SdkRoot = Join-Path $WindowsRoot "SDK"
}

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

if (-not (Test-Path -LiteralPath $SdkRoot -PathType Container)) {
    throw "LiveKit Windows SDK is not installed at $SdkRoot. Run Scripts/fetch-livekit-windows.ps1 first."
}
$SdkRoot = (Resolve-Path -LiteralPath $SdkRoot).Path
$VerificationMarkerPath = Join-Path $SdkRoot ".verified.json"
# A failed or interrupted verification must never leave a marker that can enable
# the Win64 backend on a later direct Unreal build.
Remove-Item -LiteralPath $VerificationMarkerPath -Force -ErrorAction SilentlyContinue

$Lock = Read-DependencyLock -Path $LockPath
$ExpectedVersion = Require-LockValue -Values $Lock -Name "LIVEKIT_CPP_VERSION"
$ExpectedArchive = Require-LockValue -Values $Lock -Name "LIVEKIT_CPP_ARCHIVE"
$ExpectedUrl = Require-LockValue -Values $Lock -Name "LIVEKIT_CPP_URL"
$ExpectedHash = (Require-LockValue -Values $Lock -Name "LIVEKIT_CPP_SHA256").ToLowerInvariant()
if ($ExpectedHash -notmatch '^[a-f0-9]{64}$') {
    throw "LIVEKIT_CPP_SHA256 must be a 64-character hexadecimal SHA-256 digest"
}

foreach ($Directory in @("include", "lib", "bin")) {
    if (-not (Test-Path -LiteralPath (Join-Path $SdkRoot $Directory) -PathType Container)) {
        throw "LiveKit Windows SDK is missing directory: $Directory"
    }
}
foreach ($RelativePath in @(
    "lib\livekit.lib",
    "lib\livekit_ffi.dll.lib",
    "bin\livekit.dll",
    "bin\livekit_ffi.dll"
)) {
    $Path = Join-Path $SdkRoot $RelativePath
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -eq 0) {
        throw "LiveKit Windows SDK is missing required file: $RelativePath"
    }
}
$Headers = @(Get-ChildItem -LiteralPath (Join-Path $SdkRoot "include\livekit") -Filter "*.h" -File -ErrorAction SilentlyContinue)
if ($Headers.Count -eq 0) {
    throw "LiveKit Windows SDK does not contain C++ headers under include/livekit"
}

$SourcePath = Join-Path $SdkRoot ".source.json"
if (-not (Test-Path -LiteralPath $SourcePath -PathType Leaf)) {
    throw "LiveKit Windows SDK provenance marker is missing: .source.json"
}
$Source = Get-Content -LiteralPath $SourcePath -Raw | ConvertFrom-Json
if ([string]$Source.version -ne $ExpectedVersion -or
    [string]$Source.archive -ne $ExpectedArchive -or
    [string]$Source.url -ne $ExpectedUrl -or
    ([string]$Source.sha256).ToLowerInvariant() -ne $ExpectedHash) {
    throw "LiveKit Windows SDK provenance does not match dependencies.lock"
}

$ManifestPath = Join-Path $SdkRoot ".files.sha256"
if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
    throw "LiveKit Windows SDK file manifest is missing: .files.sha256"
}

$ExpectedFiles = @{}
$RootPrefix = $SdkRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
foreach ($Line in Get-Content -LiteralPath $ManifestPath) {
    if ([string]::IsNullOrWhiteSpace($Line)) {
        continue
    }
    $Parts = $Line.Split([char]9, 2)
    if ($Parts.Count -ne 2 -or $Parts[0] -notmatch '^[a-fA-F0-9]{64}$') {
        throw "Invalid SDK file manifest entry: $Line"
    }

    $RelativePath = $Parts[1].Replace('\', '/')
    if ([IO.Path]::IsPathRooted($RelativePath) -or $RelativePath.Split('/') -contains "..") {
        throw "Unsafe SDK file manifest path: $RelativePath"
    }
    if ($ExpectedFiles.ContainsKey($RelativePath)) {
        throw "Duplicate SDK file manifest path: $RelativePath"
    }

    $FilePath = [IO.Path]::GetFullPath((Join-Path $SdkRoot $RelativePath.Replace('/', '\')))
    if (-not $FilePath.StartsWith($RootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "SDK file manifest path escapes the SDK root: $RelativePath"
    }
    if (-not (Test-Path -LiteralPath $FilePath -PathType Leaf)) {
        throw "SDK file listed in the manifest is missing: $RelativePath"
    }

    $ActualHash = (Get-FileHash -LiteralPath $FilePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $ManifestHash = $Parts[0].ToLowerInvariant()
    if ($ActualHash -ne $ManifestHash) {
        throw "SDK file checksum mismatch: $RelativePath"
    }
    $ExpectedFiles[$RelativePath] = $ManifestHash
}
if ($ExpectedFiles.Count -eq 0) {
    throw "LiveKit Windows SDK file manifest is empty"
}

$ActualFiles = @(
    Get-ChildItem -LiteralPath $SdkRoot -Recurse -File |
        Where-Object {
            $_.FullName -ne $ManifestPath -and
            $_.FullName -ne $SourcePath -and
            $_.FullName -ne $VerificationMarkerPath
        } |
        ForEach-Object { $_.FullName.Substring($SdkRoot.Length + 1).Replace('\', '/') } |
        Sort-Object
)
$ManifestFiles = @($ExpectedFiles.Keys | Sort-Object)
$Differences = @(Compare-Object -ReferenceObject $ManifestFiles -DifferenceObject $ActualFiles)
if ($Differences.Count -ne 0) {
    $Summary = ($Differences | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" }) -join ", "
    throw "SDK files do not match the verified archive manifest: $Summary"
}

$MarkerTemporaryPath = Join-Path $SdkRoot (".verified.{0}.tmp" -f [Guid]::NewGuid().ToString("N"))
try {
    [ordered]@{
        version = $ExpectedVersion
        archiveSha256 = $ExpectedHash
        lockSha256 = (Get-FileHash -LiteralPath $LockPath -Algorithm SHA256).Hash.ToLowerInvariant()
        sourceSha256 = (Get-FileHash -LiteralPath $SourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
        manifestSha256 = (Get-FileHash -LiteralPath $ManifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
        verifiedUtc = [DateTime]::UtcNow.ToString("o")
    } | ConvertTo-Json | Set-Content -LiteralPath $MarkerTemporaryPath -Encoding UTF8
    Move-Item -LiteralPath $MarkerTemporaryPath -Destination $VerificationMarkerPath -Force
}
finally {
    Remove-Item -LiteralPath $MarkerTemporaryPath -Force -ErrorAction SilentlyContinue
}

if (-not $Quiet) {
    Write-Host "LiveKit C++ SDK $ExpectedVersion verified at $SdkRoot"
}
