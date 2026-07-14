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
. (Join-Path $PSScriptRoot "livekit-windows-sdk-paths.ps1")

function Get-AdapterSourceSet {
    param([Parameter(Mandatory = $true)][string]$Root)

    $Root = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\', '/')
    $RootPrefix = $Root + [IO.Path]::DirectorySeparatorChar
    $AllowedExtensions = @(".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx")
    $RelativePaths = [Collections.Generic.List[string]]::new()

    foreach ($DirectoryName in @("include", "src")) {
        $Directory = Join-Path $Root $DirectoryName
        if (-not (Test-Path -LiteralPath $Directory -PathType Container)) {
            throw "The Windows adapter source directory is missing: $Directory"
        }
        foreach ($File in Get-ChildItem -LiteralPath $Directory -Recurse -File) {
            if ($AllowedExtensions -notcontains $File.Extension.ToLowerInvariant()) {
                continue
            }
            if (-not $File.FullName.StartsWith($RootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw "The Windows adapter source path escapes its root: $($File.FullName)"
            }
            $RelativePaths.Add($File.FullName.Substring($RootPrefix.Length).Replace('\', '/'))
        }
    }
    if ($RelativePaths.Count -eq 0) {
        throw "The Windows adapter source set is empty: $Root"
    }
    $RelativePaths.Sort([StringComparer]::Ordinal)

    $Entries = @()
    $Canonical = [Text.StringBuilder]::new()
    foreach ($RelativePath in $RelativePaths) {
        $FullPath = Join-Path $Root $RelativePath.Replace('/', [IO.Path]::DirectorySeparatorChar)
        $Hash = (Get-FileHash -LiteralPath $FullPath -Algorithm SHA256).Hash.ToLowerInvariant()
        $Entries += [pscustomobject][ordered]@{
            path = $RelativePath
            sha256 = $Hash
            fullPath = $FullPath
        }
        [void]$Canonical.Append($RelativePath).Append("`t").Append($Hash).Append("`n")
    }

    $Hasher = [Security.Cryptography.SHA256]::Create()
    try {
        $Digest = $Hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($Canonical.ToString()))
    }
    finally {
        $Hasher.Dispose()
    }
    return [pscustomobject]@{
        entries = @($Entries)
        sha256 = ([BitConverter]::ToString($Digest)).Replace("-", "").ToLowerInvariant()
    }
}

$AdapterRoot = Join-Path $PluginRoot "Source\WindowsAdapter"
$AdapterSourceSet = Get-AdapterSourceSet -Root $AdapterRoot
$AdapterGeneratedFiles = @(
    ".adapter-build.json",
    "bin/LiveKitUnrealWindowsAdapter.dll",
    "bin/LiveKitUnrealWindowsAdapter.pdb",
    "lib/LiveKitUnrealWindowsAdapter.lib"
)
$ExpectedAdapterExports = @(
    "lkub_buffer_release"
    "lkub_byte_stream_cancel"
    "lkub_byte_stream_destroy"
    "lkub_byte_stream_get_info"
    "lkub_byte_stream_read_next"
    "lkub_byte_stream_visit_attributes"
    "lkub_get_abi_version"
    "lkub_initialize"
    "lkub_result_reset"
    "lkub_room_connect"
    "lkub_room_create"
    "lkub_room_destroy"
    "lkub_room_detach_callbacks"
    "lkub_room_disconnect"
    "lkub_room_perform_rpc"
    "lkub_room_prepare_audio"
    "lkub_room_publish_data"
    "lkub_room_register_byte_stream_handler"
    "lkub_room_register_rpc_method"
    "lkub_room_set_microphone_enabled"
    "lkub_room_unregister_byte_stream_handler"
    "lkub_room_unregister_rpc_method"
    "lkub_room_visit_remote_participants"
    "lkub_shutdown"
)
if ([string]::IsNullOrWhiteSpace($SdkRoot)) {
    $SdkRoot = Resolve-LiveKitWindowsSdkRoot -WindowsRoot $WindowsRoot
}
else {
    $SdkRoot = Resolve-LiveKitWindowsSdkRoot -WindowsRoot $WindowsRoot -SdkRoot $SdkRoot
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
$AdapterBuildMarkerPath = Join-Path $SdkRoot ".adapter-build.json"
$ManagedInstallMarkerPath = Join-Path $SdkRoot ".managed-install.json"
$InstallsRoot = Get-LiveKitWindowsSdkInstallsRoot -WindowsRoot $WindowsRoot
$SdkParent = [IO.DirectoryInfo]::new($SdkRoot).Parent
$IsImmutableManagedInstall = $null -ne $SdkParent -and
    (Test-LiveKitWindowsPathEqual -Left $SdkParent.FullName -Right $InstallsRoot)
if ($IsImmutableManagedInstall) {
    $InstallName = [IO.Path]::GetFileName($SdkRoot.TrimEnd('\', '/'))
    if (-not (Test-LiveKitWindowsManagedSdkInstallName -InstallName $InstallName) -or
        -not (Test-Path -LiteralPath $ManagedInstallMarkerPath -PathType Leaf)) {
        throw "The immutable LiveKit Windows SDK is missing valid managed-install provenance."
    }
    try {
        $ManagedInstallMarker = Get-Content -LiteralPath $ManagedInstallMarkerPath -Raw |
            ConvertFrom-Json
        $null = [DateTimeOffset]::Parse(
            [string]$ManagedInstallMarker.createdUtc,
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::RoundtripKind)
    }
    catch {
        throw "The immutable LiveKit Windows SDK managed-install provenance is malformed."
    }
    if ([string]$ManagedInstallMarker.schemaVersion -ne "1" -or
        [string]$ManagedInstallMarker.managedBy -ne "LiveKitBridge" -or
        [string]$ManagedInstallMarker.installName -cne $InstallName -or
        [string]$ManagedInstallMarker.installKind -notin @("fetch", "adapter-rebuild")) {
        throw "The immutable LiveKit Windows SDK managed-install provenance does not match its directory."
    }
}
# The build rule independently re-hashes every marker input and manifest file,
# so a stale marker cannot enable the backend. Preserve an already-valid marker
# to keep an active versioned install read-only during repeat verification.

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
    "bin\livekit_ffi.dll",
    "lib\LiveKitUnrealWindowsAdapter.lib",
    "bin\LiveKitUnrealWindowsAdapter.dll",
    "bin\LiveKitUnrealWindowsAdapter.pdb"
)) {
    $Path = Join-Path $SdkRoot $RelativePath
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -eq 0) {
        throw "LiveKit Windows SDK is missing required file: $RelativePath"
    }
}
foreach ($AdapterSourceFile in $AdapterSourceSet.entries) {
    if ((Get-Item -LiteralPath $AdapterSourceFile.fullPath).Length -eq 0) {
        throw "LiveKit Windows adapter source is empty: $($AdapterSourceFile.path)"
    }
}
$AdapterBuild = if (Test-Path -LiteralPath $AdapterBuildMarkerPath -PathType Leaf) {
    Get-Content -LiteralPath $AdapterBuildMarkerPath -Raw | ConvertFrom-Json
}
else {
    throw "LiveKit Windows adapter build provenance is missing: .adapter-build.json. Run Scripts/build-livekit-windows-adapter.ps1."
}
$CurrentAdapterHashes = [ordered]@{
    adapterSourceSetSha256 = $AdapterSourceSet.sha256
    adapterDllSha256 = (Get-FileHash -LiteralPath (Join-Path $SdkRoot "bin\LiveKitUnrealWindowsAdapter.dll") -Algorithm SHA256).Hash.ToLowerInvariant()
    adapterLibSha256 = (Get-FileHash -LiteralPath (Join-Path $SdkRoot "lib\LiveKitUnrealWindowsAdapter.lib") -Algorithm SHA256).Hash.ToLowerInvariant()
    adapterPdbSha256 = (Get-FileHash -LiteralPath (Join-Path $SdkRoot "bin\LiveKitUnrealWindowsAdapter.pdb") -Algorithm SHA256).Hash.ToLowerInvariant()
}
if ([int]$AdapterBuild.schemaVersion -ne 2 -or
    [string]$AdapterBuild.toolchain -ne "Visual Studio 2022 x64" -or
    [string]$AdapterBuild.adapterArchitecture -ne "x64") {
    throw "LiveKit Windows adapter build provenance is invalid. Rebuild the adapter."
}
foreach ($HashName in $CurrentAdapterHashes.Keys) {
    $RecordedHash = ([string]$AdapterBuild.$HashName).ToLowerInvariant()
    if ($RecordedHash -notmatch '^[a-f0-9]{64}$' -or $RecordedHash -ne $CurrentAdapterHashes[$HashName]) {
        throw "LiveKit Windows adapter build provenance is stale: $HashName. Rebuild the adapter."
    }
}
$RecordedAdapterSources = @($AdapterBuild.adapterSources)
if ($RecordedAdapterSources.Count -ne $AdapterSourceSet.entries.Count) {
    throw "LiveKit Windows adapter source-set provenance is stale. Rebuild the adapter."
}
for ($Index = 0; $Index -lt $AdapterSourceSet.entries.Count; ++$Index) {
    $RecordedSource = $RecordedAdapterSources[$Index]
    $CurrentSource = $AdapterSourceSet.entries[$Index]
    $RecordedPath = [string]$RecordedSource.path
    $RecordedHash = ([string]$RecordedSource.sha256).ToLowerInvariant()
    if (-not [string]::Equals($RecordedPath, $CurrentSource.path, [StringComparison]::Ordinal) -or
        $RecordedHash -notmatch '^[a-f0-9]{64}$' -or
        $RecordedHash -ne $CurrentSource.sha256) {
        throw "LiveKit Windows adapter source-set provenance is stale at index $Index. Rebuild the adapter."
    }
}
$RecordedExports = @(
    $AdapterBuild.adapterExports |
        ForEach-Object { [string]$_ } |
        Sort-Object -Unique
)
$ExportDifferences = @(
    Compare-Object -ReferenceObject $ExpectedAdapterExports -DifferenceObject $RecordedExports
)
if ($ExportDifferences.Count -ne 0) {
    throw "LiveKit Windows adapter export provenance is invalid. Rebuild the adapter."
}
$RecordedDependencies = @(
    $AdapterBuild.adapterDependencies |
        ForEach-Object { ([string]$_).ToLowerInvariant() } |
        Sort-Object -Unique
)
foreach ($RequiredDependency in @("livekit.dll", "msvcp140.dll", "vcruntime140.dll")) {
    if ($RecordedDependencies -notcontains $RequiredDependency) {
        throw "LiveKit Windows adapter dependency provenance is invalid: $RequiredDependency is missing."
    }
}
$ForbiddenDependencies = @(
    $RecordedDependencies | Where-Object {
        $_ -match '^(unrealeditor|unrealengine|avatar)(-|\.|$)'
    }
)
if ($ForbiddenDependencies.Count -ne 0) {
    throw "LiveKit Windows adapter dependency provenance crosses the Unreal allocator boundary."
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
    if ($AdapterGeneratedFiles -contains $RelativePath) {
        throw "The SDK archive manifest must not include generated adapter output: $RelativePath"
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
            $_.FullName -ne $VerificationMarkerPath -and
            $_.FullName -ne $ManagedInstallMarkerPath -and
            $AdapterGeneratedFiles -notcontains $_.FullName.Substring($SdkRoot.Length + 1).Replace('\', '/')
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

$ExpectedMarkerValues = [ordered]@{
    version = $ExpectedVersion
    archiveSha256 = $ExpectedHash
    lockSha256 = (Get-FileHash -LiteralPath $LockPath -Algorithm SHA256).Hash.ToLowerInvariant()
    sourceSha256 = (Get-FileHash -LiteralPath $SourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
    manifestSha256 = (Get-FileHash -LiteralPath $ManifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
    adapterBuildSha256 = (Get-FileHash -LiteralPath $AdapterBuildMarkerPath -Algorithm SHA256).Hash.ToLowerInvariant()
    adapterSourceSetSha256 = $CurrentAdapterHashes.adapterSourceSetSha256
    adapterDllSha256 = $CurrentAdapterHashes.adapterDllSha256
    adapterLibSha256 = $CurrentAdapterHashes.adapterLibSha256
    adapterPdbSha256 = $CurrentAdapterHashes.adapterPdbSha256
}
if (Test-Path -LiteralPath $ManagedInstallMarkerPath -PathType Leaf) {
    $ExpectedMarkerValues.managedInstallSha256 =
        (Get-FileHash -LiteralPath $ManagedInstallMarkerPath -Algorithm SHA256).Hash.ToLowerInvariant()
}
$MarkerMatches = $false
if (Test-Path -LiteralPath $VerificationMarkerPath -PathType Leaf) {
    try {
        $ExistingMarker = Get-Content -LiteralPath $VerificationMarkerPath -Raw | ConvertFrom-Json
        $MarkerMatches = $true
        foreach ($Name in $ExpectedMarkerValues.Keys) {
            $Property = $ExistingMarker.PSObject.Properties[$Name]
            if ($null -eq $Property -or
                -not [string]::Equals(
                    [string]$Property.Value,
                    [string]$ExpectedMarkerValues[$Name],
                    [StringComparison]::OrdinalIgnoreCase)) {
                $MarkerMatches = $false
                break
            }
        }
    }
    catch {
        $MarkerMatches = $false
    }
}

if (-not $MarkerMatches) {
    $MarkerTemporaryPath = Join-Path $SdkRoot (".verified.{0}.tmp" -f [Guid]::NewGuid().ToString("N"))
    try {
        $Marker = [ordered]@{}
        foreach ($Name in $ExpectedMarkerValues.Keys) {
            $Marker[$Name] = $ExpectedMarkerValues[$Name]
        }
        $Marker["verifiedUtc"] = [DateTime]::UtcNow.ToString("o")
        $Marker | ConvertTo-Json | Set-Content -LiteralPath $MarkerTemporaryPath -Encoding UTF8
        Move-Item -LiteralPath $MarkerTemporaryPath -Destination $VerificationMarkerPath -Force
    }
    finally {
        Remove-Item -LiteralPath $MarkerTemporaryPath -Force -ErrorAction SilentlyContinue
    }
}

if (-not $Quiet) {
    Write-Host "LiveKit C++ SDK $ExpectedVersion verified at $SdkRoot"
}
