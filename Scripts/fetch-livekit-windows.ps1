[CmdletBinding()]
param(
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$PluginRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$WindowsRoot = Join-Path $PluginRoot "Source\ThirdParty\Windows"
$LockPath = Join-Path $WindowsRoot "dependencies.lock"
$SdkRoot = Join-Path $WindowsRoot "SDK"
$VerifyScript = Join-Path $PSScriptRoot "verify-livekit-windows.ps1"

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

if (-not $Force -and (Test-Path -LiteralPath $SdkRoot -PathType Container)) {
    try {
        & $VerifyScript -SdkRoot $SdkRoot -Quiet
        Write-Host "LiveKit C++ SDK $Version is already installed and verified."
        exit 0
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
$PreviousSdk = Join-Path $TransactionRoot "SDK-previous"

New-Item -ItemType Directory -Path $TransactionRoot -Force | Out-Null

try {
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

    $HadPreviousSdk = Test-Path -LiteralPath $SdkRoot
    if ($HadPreviousSdk) {
        Move-Item -LiteralPath $SdkRoot -Destination $PreviousSdk
    }

    try {
        Move-Item -LiteralPath $StagedSdk -Destination $SdkRoot
        & $VerifyScript -SdkRoot $SdkRoot -Quiet
    }
    catch {
        if (Test-Path -LiteralPath $SdkRoot) {
            Remove-Item -LiteralPath $SdkRoot -Recurse -Force
        }
        if ($HadPreviousSdk -and (Test-Path -LiteralPath $PreviousSdk)) {
            Move-Item -LiteralPath $PreviousSdk -Destination $SdkRoot
        }
        throw
    }

    Write-Host "LiveKit C++ SDK $Version installed and verified at $SdkRoot"
}
finally {
    if (Test-Path -LiteralPath $TransactionRoot) {
        Remove-Item -LiteralPath $TransactionRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
