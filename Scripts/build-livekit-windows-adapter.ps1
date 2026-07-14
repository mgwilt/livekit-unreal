[CmdletBinding()]
param(
    [string]$SdkRoot,
    [string]$VisualStudioRoot,
    [Parameter(DontShow = $true)][switch]$InternalWorker
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$PluginRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$WindowsRoot = Join-Path $PluginRoot "Source\ThirdParty\Windows"
. (Join-Path $PSScriptRoot "livekit-windows-sdk-paths.ps1")

$OperationLock = $null
if ($InternalWorker -and [string]::IsNullOrWhiteSpace($SdkRoot)) {
    throw "The internal adapter build worker requires an explicit inactive SDK root."
}
if (-not $InternalWorker) {
    $OperationLock = Enter-LiveKitWindowsSdkOperationLock -WindowsRoot $WindowsRoot
}

try {
if ([string]::IsNullOrWhiteSpace($SdkRoot)) {
    # A developer rebuild must not mutate the active immutable install. Clone
    # it to a unique inactive directory, build and verify there, then atomically
    # activate it through the same pointer used by the fetch workflow.
    $CurrentSdkRoot = Resolve-LiveKitWindowsSdkRoot -WindowsRoot $WindowsRoot
    if (-not (Test-Path -LiteralPath $CurrentSdkRoot -PathType Container)) {
        throw "LiveKit Windows SDK is not installed at $CurrentSdkRoot. Run Scripts/fetch-livekit-windows.ps1 first."
    }

    $SourceMarkerPath = Join-Path $CurrentSdkRoot ".source.json"
    if (-not (Test-Path -LiteralPath $SourceMarkerPath -PathType Leaf)) {
        throw "The active LiveKit Windows SDK provenance marker is missing: .source.json"
    }
    $SourceMarker = Get-Content -LiteralPath $SourceMarkerPath -Raw | ConvertFrom-Json
    $SourceVersion = [string]$SourceMarker.version
    $SourceHash = ([string]$SourceMarker.sha256).ToLowerInvariant()
    if ($SourceVersion -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$' -or
        $SourceVersion.EndsWith('.', [StringComparison]::Ordinal) -or
        $SourceHash -notmatch '^[a-f0-9]{64}$') {
        throw "The active LiveKit Windows SDK provenance cannot name a safe immutable install."
    }

    $InstallsRoot = Get-LiveKitWindowsSdkInstallsRoot -WindowsRoot $WindowsRoot
    New-Item -ItemType Directory -Path $InstallsRoot -Force | Out-Null
    Assert-LiveKitWindowsSdkPathIsNotReparsePoint -Path $InstallsRoot
    $InstallName = "livekit-{0}-{1}-adapter-{2}" -f `
        $SourceVersion, `
        $SourceHash.Substring(0, 16), `
        [Guid]::NewGuid().ToString("N")
    $CandidateSdkRoot = Join-Path $InstallsRoot $InstallName
    $ActivationSucceeded = $false
    try {
        Copy-Item -LiteralPath $CurrentSdkRoot -Destination $CandidateSdkRoot -Recurse

        $BuildArguments = @{ SdkRoot = $CandidateSdkRoot }
        if (-not [string]::IsNullOrWhiteSpace($VisualStudioRoot)) {
            $BuildArguments.VisualStudioRoot = $VisualStudioRoot
        }
        $BuildArguments.InternalWorker = $true
        & $PSCommandPath @BuildArguments
        New-LiveKitWindowsManagedSdkInstallMarker `
            -WindowsRoot $WindowsRoot `
            -SdkRoot $CandidateSdkRoot `
            -InstallKind "adapter-rebuild"
        & (Join-Path $PSScriptRoot "verify-livekit-windows.ps1") `
            -SdkRoot $CandidateSdkRoot `
            -Quiet
        $null = Set-LiveKitWindowsActiveSdk `
            -WindowsRoot $WindowsRoot `
            -SdkRoot $CandidateSdkRoot
        $ActivationSucceeded = $true
    }
    finally {
        if (-not $ActivationSucceeded -and (Test-Path -LiteralPath $CandidateSdkRoot)) {
            Remove-Item -LiteralPath $CandidateSdkRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    try {
        Remove-LiveKitWindowsObsoleteSdkInstalls `
            -WindowsRoot $WindowsRoot `
            -ActiveSdkRoot $CandidateSdkRoot `
            -PreviousActiveSdkRoot $CurrentSdkRoot
    }
    catch {
        Write-Warning "The rebuilt SDK is active, but obsolete install cleanup failed: $($_.Exception.Message)"
    }
    Write-Host "Built, verified, and activated the LiveKit Windows adapter at $CandidateSdkRoot"
    return
}
$RequestedSdkRoot = [IO.Path]::GetFullPath($SdkRoot)
if (-not $InternalWorker) {
    $ActiveSdkRoot = Resolve-LiveKitWindowsSdkRootBeforeReplacement -WindowsRoot $WindowsRoot
    if ((Test-Path -LiteralPath $ActiveSdkRoot -PathType Container) -and
        (Test-LiveKitWindowsPathEqual -Left $RequestedSdkRoot -Right $ActiveSdkRoot)) {
        throw "Refusing to rebuild the active immutable SDK in place. Omit -SdkRoot to clone, verify, and atomically activate a rebuilt install."
    }
}
$SdkRoot = Resolve-LiveKitWindowsSdkRoot -WindowsRoot $WindowsRoot -SdkRoot $SdkRoot
if (-not (Test-Path -LiteralPath $SdkRoot -PathType Container)) {
    throw "LiveKit Windows SDK is not installed at $SdkRoot. Run Scripts/fetch-livekit-windows.ps1 first."
}
$SdkRoot = (Resolve-Path -LiteralPath $SdkRoot).Path

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
$AdapterCompileSources = @(
    $AdapterSourceSet.entries |
        Where-Object { [IO.Path]::GetExtension($_.path).ToLowerInvariant() -in @(".c", ".cc", ".cpp", ".cxx") }
)
if ($AdapterCompileSources.Count -eq 0) {
    throw "The Windows adapter source set does not contain a compilation unit."
}
$SdkInclude = Join-Path $SdkRoot "include"
$SdkLib = Join-Path $SdkRoot "lib"

$RequiredAdapterFiles = @($AdapterSourceSet.entries | ForEach-Object { $_.fullPath })
$RequiredAdapterFiles += Join-Path $SdkInclude "livekit\livekit.h"
$RequiredAdapterFiles += Join-Path $SdkLib "livekit.lib"
$RequiredAdapterFiles += Join-Path $SdkLib "livekit_ffi.dll.lib"
foreach ($Path in $RequiredAdapterFiles) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "The Windows adapter cannot be built because a required file is missing: $Path"
    }
}

function Resolve-VisualStudio2022Root {
    param([string]$RequestedRoot)

    if (-not [string]::IsNullOrWhiteSpace($RequestedRoot)) {
        if (-not (Test-Path -LiteralPath $RequestedRoot -PathType Container)) {
            throw "Visual Studio root does not exist: $RequestedRoot"
        }
        return (Resolve-Path -LiteralPath $RequestedRoot).Path
    }

    $VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $VsWhere -PathType Leaf)) {
        throw "Visual Studio Installer's vswhere.exe was not found. Install Visual Studio 2022 with the Desktop development with C++ workload, or pass -VisualStudioRoot."
    }

    $InstallationPath = & $VsWhere `
        -latest `
        -version "[17.0,18.0)" `
        -products "*" `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if ($LASTEXITCODE -ne 0) {
        throw "vswhere.exe failed while locating Visual Studio 2022 (exit code $LASTEXITCODE)."
    }
    $InstallationPath = @($InstallationPath | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }) | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($InstallationPath)) {
        throw "Visual Studio 2022 with the Desktop development with C++ workload was not found."
    }
    return [string]$InstallationPath
}

function Quote-ResponseArgument {
    param([Parameter(Mandatory = $true)][string]$Value)

    if ($Value.Contains('"')) {
        throw "A compiler argument contains an unsupported quote character."
    }
    return '"' + $Value + '"'
}

$VisualStudioRoot = Resolve-VisualStudio2022Root -RequestedRoot $VisualStudioRoot
$VcVars64 = Join-Path $VisualStudioRoot "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path -LiteralPath $VcVars64 -PathType Leaf)) {
    throw "Visual Studio 2022 x64 compiler environment was not found: $VcVars64"
}

# Removing the marker before changing any derived binary guarantees that a
# concurrent or interrupted Unreal build cannot enable a stale adapter.
$VerificationMarker = Join-Path $SdkRoot ".verified.json"
$AdapterBuildMarker = Join-Path $SdkRoot ".adapter-build.json"
Remove-Item -LiteralPath $VerificationMarker -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $AdapterBuildMarker -Force -ErrorAction SilentlyContinue

$TransactionRoot = Join-Path (Join-Path $SdkRoot ".adapter-build") ([Guid]::NewGuid().ToString("N"))
$OutputBin = Join-Path $SdkRoot "bin"
$OutputLib = Join-Path $SdkRoot "lib"
$AdapterName = "LiveKitUnrealWindowsAdapter"
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
$FinalDll = Join-Path $OutputBin ($AdapterName + ".dll")
$FinalPdb = Join-Path $OutputBin ($AdapterName + ".pdb")
$FinalLib = Join-Path $OutputLib ($AdapterName + ".lib")

New-Item -ItemType Directory -Path $TransactionRoot -Force | Out-Null
New-Item -ItemType Directory -Path $OutputBin -Force | Out-Null
New-Item -ItemType Directory -Path $OutputLib -Force | Out-Null

try {
    $TemporaryDll = Join-Path $TransactionRoot ($AdapterName + ".dll")
    $TemporaryPdb = Join-Path $TransactionRoot ($AdapterName + ".pdb")
    $TemporaryLib = Join-Path $TransactionRoot ($AdapterName + ".lib")
    $CompilePdb = Join-Path $TransactionRoot ($AdapterName + ".compile.pdb")
    $CompileResponseFiles = @()
    $TemporaryObjects = @()
    $LinkResponseFile = Join-Path $TransactionRoot "link-adapter.rsp"
    $ExportsDump = Join-Path $TransactionRoot "adapter-exports.txt"
    $DependenciesDump = Join-Path $TransactionRoot "adapter-dependencies.txt"
    $HeadersDump = Join-Path $TransactionRoot "adapter-headers.txt"
    $CommandFile = Join-Path $TransactionRoot "build-adapter.cmd"

    $CommonCompileArguments = @(
        "/nologo"
        "/LD"
        "/MD"
        "/c"
        "/std:c++20"
        "/EHsc"
        "/O2"
        "/Zi"
        "/DNDEBUG"
        "/DLIVEKIT_UNREAL_WINDOWS_ADAPTER_EXPORTS=1"
        ('/Fd:{0}' -f (Quote-ResponseArgument -Value $CompilePdb))
        ('/I{0}' -f (Quote-ResponseArgument -Value (Join-Path $PluginRoot "Source\WindowsAdapter\include")))
        ('/I{0}' -f (Quote-ResponseArgument -Value $SdkInclude))
    )
    for ($Index = 0; $Index -lt $AdapterCompileSources.Count; ++$Index) {
        $Source = $AdapterCompileSources[$Index]
        $TemporaryObject = Join-Path $TransactionRoot ("adapter-{0:D2}.obj" -f $Index)
        $CompileResponseFile = Join-Path $TransactionRoot ("compile-adapter-{0:D2}.rsp" -f $Index)
        $TemporaryObjects += $TemporaryObject
        $CompileResponseFiles += $CompileResponseFile
        @(
            $CommonCompileArguments
            ('/Fo:{0}' -f (Quote-ResponseArgument -Value $TemporaryObject))
            (Quote-ResponseArgument -Value $Source.fullPath)
        ) | Set-Content -LiteralPath $CompileResponseFile -Encoding ASCII
    }

    $LinkArguments = @(
        "/NOLOGO"
        "/DLL"
        "/DEBUG:FULL"
        ('/OUT:{0}' -f (Quote-ResponseArgument -Value $TemporaryDll))
        ('/IMPLIB:{0}' -f (Quote-ResponseArgument -Value $TemporaryLib))
        ('/PDB:{0}' -f (Quote-ResponseArgument -Value $TemporaryPdb))
        ('/LIBPATH:{0}' -f (Quote-ResponseArgument -Value $SdkLib))
    )
    $LinkArguments += @($TemporaryObjects | ForEach-Object { Quote-ResponseArgument -Value $_ })
    $LinkArguments += @(
        "livekit.lib"
        "livekit_ffi.dll.lib"
        "ntdll.lib"
        "userenv.lib"
        "winmm.lib"
        "iphlpapi.lib"
        "msdmo.lib"
        "dmoguids.lib"
        "wmcodecdspuuid.lib"
        "ws2_32.lib"
        "secur32.lib"
        "bcrypt.lib"
        "crypt32.lib"
    )
    $LinkArguments | Set-Content -LiteralPath $LinkResponseFile -Encoding ASCII

    $CommandLines = @(
        "@echo off"
        ('call "{0}" >nul' -f $VcVars64)
        "if errorlevel 1 exit /b %errorlevel%"
    )
    foreach ($CompileResponseFile in $CompileResponseFiles) {
        $CommandLines += ('cl.exe @"{0}"' -f $CompileResponseFile)
        $CommandLines += "if errorlevel 1 exit /b %errorlevel%"
    }
    $CommandLines += @(
        ('link.exe @"{0}"' -f $LinkResponseFile)
        "if errorlevel 1 exit /b %errorlevel%"
        ('link.exe /dump /exports "{0}" > "{1}"' -f $TemporaryDll, $ExportsDump)
        "if errorlevel 1 exit /b %errorlevel%"
        ('link.exe /dump /dependents "{0}" > "{1}"' -f $TemporaryDll, $DependenciesDump)
        "if errorlevel 1 exit /b %errorlevel%"
        ('link.exe /dump /headers "{0}" > "{1}"' -f $TemporaryDll, $HeadersDump)
        "exit /b %errorlevel%"
    )
    $CommandLines | Set-Content -LiteralPath $CommandFile -Encoding ASCII

    $PreviousErrorActionPreference = $ErrorActionPreference
    try {
        # Windows PowerShell 5.1 promotes native stderr to NativeCommandError
        # when Stop is active, even when the compiler ultimately succeeds.
        $ErrorActionPreference = "Continue"
        & $CommandFile
        $CompilerExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $PreviousErrorActionPreference
    }
    if ($CompilerExitCode -ne 0) {
        throw "Visual Studio 2022 failed to build the LiveKit Windows adapter (exit code $CompilerExitCode)."
    }

    foreach ($Path in @($TemporaryDll, $TemporaryLib, $TemporaryPdb)) {
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -eq 0) {
            throw "The adapter build did not produce the required output: $Path"
        }
    }

    $ExportsText = Get-Content -LiteralPath $ExportsDump -Raw
    $AdapterExports = @(
        [regex]::Matches(
            $ExportsText,
            '(?m)^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(lkub_[A-Za-z0-9_]+)(?:\s+=.*)?\s*$') |
            ForEach-Object { $_.Groups[1].Value } |
            Sort-Object -Unique
    )
    $ExportDifferences = @(
        Compare-Object -ReferenceObject $ExpectedAdapterExports -DifferenceObject $AdapterExports
    )
    if ($ExportDifferences.Count -ne 0) {
        $Summary = ($ExportDifferences | ForEach-Object {
            "$($_.SideIndicator) $($_.InputObject)"
        }) -join ", "
        throw "The generated adapter exports do not match the C ABI: $Summary"
    }

    $DependenciesText = Get-Content -LiteralPath $DependenciesDump -Raw
    $AdapterDependencies = @(
        [regex]::Matches(
            $DependenciesText,
            '(?m)^\s+([A-Za-z0-9_.-]+\.dll)\s*$') |
            ForEach-Object { $_.Groups[1].Value.ToLowerInvariant() } |
            Sort-Object -Unique
    )
    foreach ($RequiredDependency in @("livekit.dll", "msvcp140.dll", "vcruntime140.dll")) {
        if ($AdapterDependencies -notcontains $RequiredDependency) {
            throw "The generated adapter is missing required dynamic dependency: $RequiredDependency"
        }
    }
    $ForbiddenDependencies = @(
        $AdapterDependencies | Where-Object {
            $_ -match '^(unrealeditor|unrealengine|avatar)(-|\.|$)'
        }
    )
    if ($ForbiddenDependencies.Count -ne 0) {
        throw "The generated adapter crosses the Unreal allocator boundary through: $($ForbiddenDependencies -join ', ')"
    }

    $HeadersText = Get-Content -LiteralPath $HeadersDump -Raw
    if ($HeadersText -notmatch '(?im)^\s+8664 machine \(x64\)\s*$') {
        throw "The generated adapter is not an x64 PE image."
    }

    Move-Item -LiteralPath $TemporaryDll -Destination $FinalDll -Force
    Move-Item -LiteralPath $TemporaryLib -Destination $FinalLib -Force
    Move-Item -LiteralPath $TemporaryPdb -Destination $FinalPdb -Force

    $TemporaryBuildMarker = Join-Path $TransactionRoot ".adapter-build.json"
    [ordered]@{
        schemaVersion = 2
        toolchain = "Visual Studio 2022 x64"
        adapterSourceSetSha256 = $AdapterSourceSet.sha256
        adapterSources = @($AdapterSourceSet.entries | ForEach-Object {
            [ordered]@{
                path = $_.path
                sha256 = $_.sha256
            }
        })
        adapterDllSha256 = (Get-FileHash -LiteralPath $FinalDll -Algorithm SHA256).Hash.ToLowerInvariant()
        adapterLibSha256 = (Get-FileHash -LiteralPath $FinalLib -Algorithm SHA256).Hash.ToLowerInvariant()
        adapterPdbSha256 = (Get-FileHash -LiteralPath $FinalPdb -Algorithm SHA256).Hash.ToLowerInvariant()
        adapterArchitecture = "x64"
        adapterExports = $AdapterExports
        adapterDependencies = $AdapterDependencies
        builtUtc = [DateTime]::UtcNow.ToString("o")
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $TemporaryBuildMarker -Encoding UTF8
    Move-Item -LiteralPath $TemporaryBuildMarker -Destination $AdapterBuildMarker -Force

    Write-Host "Built $AdapterName with Visual Studio 2022. Run Scripts/verify-livekit-windows.ps1 before building Unreal."
}
finally {
    if (Test-Path -LiteralPath $TransactionRoot) {
        Remove-Item -LiteralPath $TransactionRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
    $BuildRoot = Join-Path $SdkRoot ".adapter-build"
    if ((Test-Path -LiteralPath $BuildRoot -PathType Container) -and
        @(Get-ChildItem -LiteralPath $BuildRoot -Force).Count -eq 0) {
        Remove-Item -LiteralPath $BuildRoot -Force -ErrorAction SilentlyContinue
    }
}
}
finally {
    Exit-LiveKitWindowsSdkOperationLock -Lock $OperationLock
}
