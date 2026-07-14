function Get-LiveKitWindowsSdkPointerPath {
    param([Parameter(Mandatory = $true)][string]$WindowsRoot)

    return Join-Path ([IO.Path]::GetFullPath($WindowsRoot)) "active-sdk.txt"
}

function Get-LiveKitWindowsSdkInstallsRoot {
    param([Parameter(Mandatory = $true)][string]$WindowsRoot)

    return [IO.Path]::GetFullPath((Join-Path $WindowsRoot "SDKs"))
}

function Get-LiveKitWindowsSdkOperationLockPath {
    param([Parameter(Mandatory = $true)][string]$WindowsRoot)

    return Join-Path ([IO.Path]::GetFullPath($WindowsRoot)) ".sdk-operation.lock"
}

function Enter-LiveKitWindowsSdkOperationLock {
    param(
        [Parameter(Mandatory = $true)][string]$WindowsRoot,
        [ValidateRange(1, 3600)][int]$TimeoutSeconds = 600
    )

    $WindowsRoot = [IO.Path]::GetFullPath($WindowsRoot)
    New-Item -ItemType Directory -Path $WindowsRoot -Force | Out-Null
    Assert-LiveKitWindowsSdkPathIsNotReparsePoint -Path $WindowsRoot
    $LockPath = Get-LiveKitWindowsSdkOperationLockPath -WindowsRoot $WindowsRoot
    Assert-LiveKitWindowsSdkPathIsNotReparsePoint -Path $LockPath
    $Deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)

    while ($true) {
        try {
            $Stream = [IO.FileStream]::new(
                $LockPath,
                [IO.FileMode]::OpenOrCreate,
                [IO.FileAccess]::ReadWrite,
                [IO.FileShare]::None,
                4096,
                [IO.FileOptions]::WriteThrough)
            try {
                $Payload = [Text.Encoding]::UTF8.GetBytes(
                    "pid=$PID`nacquiredUtc=$([DateTime]::UtcNow.ToString('o'))`n")
                $Stream.SetLength(0)
                $Stream.Write($Payload, 0, $Payload.Length)
                $Stream.Flush($true)
                return [pscustomobject]@{
                    path = $LockPath
                    stream = $Stream
                }
            }
            catch {
                $Stream.Dispose()
                throw
            }
        }
        catch [IO.IOException] {
            if ([DateTime]::UtcNow -ge $Deadline) {
                throw "Timed out waiting for the LiveKit Windows SDK operation lock at $LockPath."
            }
            Start-Sleep -Milliseconds 100
        }
    }
}

function Exit-LiveKitWindowsSdkOperationLock {
    param([AllowNull()][object]$Lock)

    if ($null -ne $Lock -and $null -ne $Lock.stream) {
        $Lock.stream.Dispose()
    }
}

function Test-LiveKitWindowsPathEqual {
    param(
        [Parameter(Mandatory = $true)][string]$Left,
        [Parameter(Mandatory = $true)][string]$Right
    )

    return [string]::Equals(
        [IO.Path]::GetFullPath($Left).TrimEnd('\', '/'),
        [IO.Path]::GetFullPath($Right).TrimEnd('\', '/'),
        [StringComparison]::OrdinalIgnoreCase)
}

function Assert-LiveKitWindowsSdkPathIsNotReparsePoint {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ((Test-Path -LiteralPath $Path) -and
        (((Get-Item -LiteralPath $Path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw "LiveKit Windows SDK paths must not use junctions or symbolic links: $Path"
    }
}

function Resolve-LiveKitWindowsSdkPointerValue {
    param(
        [Parameter(Mandatory = $true)][string]$WindowsRoot,
        [Parameter(Mandatory = $true)][string]$PointerValue
    )

    $WindowsRoot = [IO.Path]::GetFullPath($WindowsRoot).TrimEnd('\', '/')
    $RawPointerValue = $PointerValue
    $PointerValue = $PointerValue.Trim()
    if ([string]::IsNullOrWhiteSpace($PointerValue) -or
        -not [string]::Equals($RawPointerValue, $PointerValue, [StringComparison]::Ordinal) -or
        [IO.Path]::IsPathRooted($PointerValue) -or
        $PointerValue.Split([char[]]@('/', '\'), [StringSplitOptions]::None) -contains ".." -or
        $PointerValue.EndsWith('.', [StringComparison]::Ordinal) -or
        -not [regex]::IsMatch(
            $PointerValue,
            '\ASDKs/[A-Za-z0-9][A-Za-z0-9._-]*\z',
            [Text.RegularExpressions.RegexOptions]::CultureInvariant -bor
                [Text.RegularExpressions.RegexOptions]::IgnoreCase)) {
        throw "Invalid LiveKit Windows SDK pointer. Expected SDKs/<immutable-install-name>."
    }

    $InstallsRoot = Get-LiveKitWindowsSdkInstallsRoot -WindowsRoot $WindowsRoot
    $SdkRoot = [IO.Path]::GetFullPath((Join-Path $WindowsRoot $PointerValue.Replace('/', '\')))
    $InstallsPrefix = $InstallsRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $SdkParent = [IO.DirectoryInfo]::new($SdkRoot).Parent
    if ($null -eq $SdkParent -or
        -not $SdkRoot.StartsWith($InstallsPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-LiveKitWindowsPathEqual -Left $SdkParent.FullName -Right $InstallsRoot)) {
        throw "The LiveKit Windows SDK pointer escapes its immutable install directory."
    }

    Assert-LiveKitWindowsSdkPathIsNotReparsePoint -Path $InstallsRoot
    if (-not (Test-Path -LiteralPath $SdkRoot -PathType Container)) {
        throw "The active LiveKit Windows SDK install does not exist: $SdkRoot"
    }
    Assert-LiveKitWindowsSdkPathIsNotReparsePoint -Path $SdkRoot
    return $SdkRoot
}

function Resolve-LiveKitWindowsSdkRoot {
    param(
        [Parameter(Mandatory = $true)][string]$WindowsRoot,
        [string]$SdkRoot
    )

    if (-not [string]::IsNullOrWhiteSpace($SdkRoot)) {
        return [IO.Path]::GetFullPath($SdkRoot)
    }

    $WindowsRoot = [IO.Path]::GetFullPath($WindowsRoot)
    $PointerPath = Get-LiveKitWindowsSdkPointerPath -WindowsRoot $WindowsRoot
    if (Test-Path -LiteralPath $PointerPath -PathType Leaf) {
        $PointerValue = [IO.File]::ReadAllText($PointerPath)
        return Resolve-LiveKitWindowsSdkPointerValue `
            -WindowsRoot $WindowsRoot `
            -PointerValue $PointerValue
    }

    # Compatibility for installations created before immutable SDK directories
    # and the active pointer were introduced.
    $LegacySdkRoot = [IO.Path]::GetFullPath((Join-Path $WindowsRoot "SDK"))
    Assert-LiveKitWindowsSdkPathIsNotReparsePoint -Path $LegacySdkRoot
    return $LegacySdkRoot
}

function Resolve-LiveKitWindowsSdkRootBeforeReplacement {
    param(
        [Parameter(Mandatory = $true)][string]$WindowsRoot,
        [switch]$AllowInvalidActivePointer
    )

    try {
        return Resolve-LiveKitWindowsSdkRoot -WindowsRoot $WindowsRoot
    }
    catch {
        if ($AllowInvalidActivePointer) {
            return $null
        }
        throw
    }
}

function Test-LiveKitWindowsManagedSdkInstallName {
    param([Parameter(Mandatory = $true)][string]$InstallName)

    return [regex]::IsMatch(
        $InstallName,
        '\Alivekit-[A-Za-z0-9][A-Za-z0-9._-]*-[a-f0-9]{16}-(?:adapter-)?[a-f0-9]{32}\z',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant -bor
            [Text.RegularExpressions.RegexOptions]::IgnoreCase)
}

function New-LiveKitWindowsManagedSdkInstallMarker {
    param(
        [Parameter(Mandatory = $true)][string]$WindowsRoot,
        [Parameter(Mandatory = $true)][string]$SdkRoot,
        [ValidateSet("fetch", "adapter-rebuild")][string]$InstallKind,
        [DateTimeOffset]$CreatedUtc = [DateTimeOffset]::UtcNow
    )

    $InstallsRoot = Get-LiveKitWindowsSdkInstallsRoot -WindowsRoot $WindowsRoot
    $SdkRoot = [IO.Path]::GetFullPath($SdkRoot)
    $SdkParent = [IO.DirectoryInfo]::new($SdkRoot).Parent
    $InstallName = [IO.Path]::GetFileName($SdkRoot.TrimEnd('\', '/'))
    if ($null -eq $SdkParent -or
        -not (Test-LiveKitWindowsPathEqual -Left $SdkParent.FullName -Right $InstallsRoot) -or
        -not (Test-LiveKitWindowsManagedSdkInstallName -InstallName $InstallName)) {
        throw "Only a recognized immutable LiveKit SDK install can receive managed provenance."
    }
    if (-not (Test-Path -LiteralPath $SdkRoot -PathType Container)) {
        throw "The managed LiveKit SDK install does not exist: $SdkRoot"
    }
    Assert-LiveKitWindowsSdkPathIsNotReparsePoint -Path $SdkRoot

    $MarkerPath = Join-Path $SdkRoot ".managed-install.json"
    $TemporaryMarkerPath = "$MarkerPath.$([Guid]::NewGuid().ToString('N')).tmp"
    $BackupMarkerPath = "$MarkerPath.$([Guid]::NewGuid().ToString('N')).bak"
    try {
        $MarkerJson = [ordered]@{
            schemaVersion = "1"
            managedBy = "LiveKitBridge"
            installName = $InstallName
            installKind = $InstallKind
            createdUtc = $CreatedUtc.ToUniversalTime().ToString("o")
        } | ConvertTo-Json
        [IO.File]::WriteAllText($TemporaryMarkerPath, $MarkerJson, [Text.UTF8Encoding]::new($false))
        if ([IO.File]::Exists($MarkerPath)) {
            [IO.File]::Replace($TemporaryMarkerPath, $MarkerPath, $BackupMarkerPath)
        }
        else {
            [IO.File]::Move($TemporaryMarkerPath, $MarkerPath)
        }
    }
    finally {
        Remove-Item -LiteralPath $TemporaryMarkerPath -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $BackupMarkerPath -Force -ErrorAction SilentlyContinue
    }
}

function Get-LiveKitWindowsManagedSdkInstall {
    param(
        [Parameter(Mandatory = $true)][string]$WindowsRoot,
        [Parameter(Mandatory = $true)][string]$SdkRoot
    )

    try {
        $InstallsRoot = Get-LiveKitWindowsSdkInstallsRoot -WindowsRoot $WindowsRoot
        $SdkRoot = [IO.Path]::GetFullPath($SdkRoot)
        $SdkItem = Get-Item -LiteralPath $SdkRoot -Force -ErrorAction Stop
        $InstallName = $SdkItem.Name
        if (-not $SdkItem.PSIsContainer -or
            (($SdkItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) -or
            -not (Test-LiveKitWindowsManagedSdkInstallName -InstallName $InstallName) -or
            -not (Test-LiveKitWindowsPathEqual -Left $SdkItem.Parent.FullName -Right $InstallsRoot)) {
            return $null
        }

        foreach ($RequiredMarker in @(
            ".managed-install.json",
            ".source.json",
            ".files.sha256",
            ".adapter-build.json",
            ".verified.json"
        )) {
            if (-not (Test-Path -LiteralPath (Join-Path $SdkRoot $RequiredMarker) -PathType Leaf)) {
                return $null
            }
        }

        $Marker = Get-Content -LiteralPath (Join-Path $SdkRoot ".managed-install.json") -Raw |
            ConvertFrom-Json
        if ([string]$Marker.schemaVersion -ne "1" -or
            [string]$Marker.managedBy -ne "LiveKitBridge" -or
            [string]$Marker.installName -cne $InstallName -or
            [string]$Marker.installKind -notin @("fetch", "adapter-rebuild")) {
            return $null
        }
        $CreatedUtc = [DateTimeOffset]::Parse(
            [string]$Marker.createdUtc,
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::RoundtripKind).ToUniversalTime()
        return [pscustomobject]@{
            root = $SdkRoot
            createdUtc = $CreatedUtc
            installKind = [string]$Marker.installKind
        }
    }
    catch {
        return $null
    }
}

function Set-LiveKitWindowsActiveSdk {
    param(
        [Parameter(Mandatory = $true)][string]$WindowsRoot,
        [Parameter(Mandatory = $true)][string]$SdkRoot
    )

    $WindowsRoot = [IO.Path]::GetFullPath($WindowsRoot)
    $InstallsRoot = Get-LiveKitWindowsSdkInstallsRoot -WindowsRoot $WindowsRoot
    $SdkRoot = [IO.Path]::GetFullPath($SdkRoot)
    if (-not (Test-Path -LiteralPath $SdkRoot -PathType Container)) {
        throw "The LiveKit Windows SDK cannot be activated because it does not exist: $SdkRoot"
    }

    $SdkParent = [IO.DirectoryInfo]::new($SdkRoot).Parent
    $InstallName = [IO.Path]::GetFileName($SdkRoot.TrimEnd('\', '/'))
    if ($null -eq $SdkParent -or
        -not (Test-LiveKitWindowsPathEqual -Left $SdkParent.FullName -Right $InstallsRoot) -or
        $InstallName.EndsWith('.', [StringComparison]::Ordinal) -or
        -not [regex]::IsMatch(
            $InstallName,
            '\A[A-Za-z0-9][A-Za-z0-9._-]*\z',
            [Text.RegularExpressions.RegexOptions]::CultureInvariant)) {
        throw "Only a direct, safely named child of $InstallsRoot can be activated."
    }

    Assert-LiveKitWindowsSdkPathIsNotReparsePoint -Path $InstallsRoot
    Assert-LiveKitWindowsSdkPathIsNotReparsePoint -Path $SdkRoot
    $PointerValue = "SDKs/$InstallName"
    $ResolvedSdkRoot = Resolve-LiveKitWindowsSdkPointerValue `
        -WindowsRoot $WindowsRoot `
        -PointerValue $PointerValue
    if (-not (Test-LiveKitWindowsPathEqual -Left $ResolvedSdkRoot -Right $SdkRoot)) {
        throw "The LiveKit Windows SDK pointer did not resolve to the requested install."
    }

    New-Item -ItemType Directory -Path $WindowsRoot -Force | Out-Null
    $PointerPath = Get-LiveKitWindowsSdkPointerPath -WindowsRoot $WindowsRoot
    $PointerTransactionId = [Guid]::NewGuid().ToString('N')
    $TemporaryPointerPath = "$PointerPath.$PointerTransactionId.tmp"
    $BackupPointerPath = "$PointerPath.$PointerTransactionId.bak"
    try {
        $Encoding = [Text.UTF8Encoding]::new($false)
        $Stream = $null
        $Writer = $null
        try {
            $Stream = [IO.FileStream]::new(
                $TemporaryPointerPath,
                [IO.FileMode]::CreateNew,
                [IO.FileAccess]::Write,
                [IO.FileShare]::None,
                4096,
                [IO.FileOptions]::WriteThrough)
            $Writer = [IO.StreamWriter]::new($Stream, $Encoding)
            try {
                $Writer.Write($PointerValue)
                $Writer.Flush()
                $Stream.Flush($true)
            }
            finally {
                if ($null -ne $Writer) {
                    $Writer.Dispose()
                }
            }
        }
        finally {
            if ($null -ne $Stream) {
                $Stream.Dispose()
            }
        }

        if ([IO.File]::Exists($PointerPath)) {
            # File.Replace is a single-volume atomic replacement. A failure leaves
            # the previous pointer, and therefore the previous SDK, untouched.
            [IO.File]::Replace($TemporaryPointerPath, $PointerPath, $BackupPointerPath)
        }
        else {
            # The first activation is an atomic same-volume rename.
            [IO.File]::Move($TemporaryPointerPath, $PointerPath)
        }
    }
    finally {
        Remove-Item -LiteralPath $TemporaryPointerPath -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $BackupPointerPath -Force -ErrorAction SilentlyContinue
    }

    return $PointerValue
}

function Remove-LiveKitWindowsObsoleteSdkInstalls {
    param(
        [Parameter(Mandatory = $true)][string]$WindowsRoot,
        [Parameter(Mandatory = $true)][string]$ActiveSdkRoot,
        [string]$PreviousActiveSdkRoot,
        [ValidateRange(0, 8760)][int]$MinimumAgeHours = 24
    )

    $WindowsRoot = [IO.Path]::GetFullPath($WindowsRoot)
    $ActiveSdkRoot = [IO.Path]::GetFullPath($ActiveSdkRoot)
    $ResolvedActiveSdkRoot = Resolve-LiveKitWindowsSdkRoot -WindowsRoot $WindowsRoot
    if (-not (Test-LiveKitWindowsPathEqual -Left $ResolvedActiveSdkRoot -Right $ActiveSdkRoot)) {
        throw "Obsolete SDK cleanup requires the requested install to be active."
    }

    $InstallsRoot = Get-LiveKitWindowsSdkInstallsRoot -WindowsRoot $WindowsRoot
    Assert-LiveKitWindowsSdkPathIsNotReparsePoint -Path $InstallsRoot
    $ProtectedRoots = [Collections.Generic.List[string]]::new()
    $ProtectedRoots.Add($ActiveSdkRoot)
    if (-not [string]::IsNullOrWhiteSpace($PreviousActiveSdkRoot)) {
        $ProtectedRoots.Add([IO.Path]::GetFullPath($PreviousActiveSdkRoot))
    }

    $ManagedInstalls = @(
        Get-ChildItem -LiteralPath $InstallsRoot -Directory -Force |
            ForEach-Object {
                Get-LiveKitWindowsManagedSdkInstall `
                    -WindowsRoot $WindowsRoot `
                    -SdkRoot $_.FullName
            } |
            Where-Object { $null -ne $_ } |
            Sort-Object createdUtc -Descending
    )
    # Even when the caller cannot identify a prior pointer (for example while
    # repairing malformed state), retain the newest previous managed install.
    $NewestPrevious = @(
        $ManagedInstalls | Where-Object {
            -not (Test-LiveKitWindowsPathEqual -Left $_.root -Right $ActiveSdkRoot)
        }
    ) | Select-Object -First 1
    if ($null -ne $NewestPrevious) {
        $ProtectedRoots.Add($NewestPrevious.root)
    }

    $DeleteBefore = [DateTimeOffset]::UtcNow.AddHours(-$MinimumAgeHours)
    foreach ($Install in $ManagedInstalls) {
        $IsProtected = $false
        foreach ($ProtectedRoot in $ProtectedRoots) {
            if (Test-LiveKitWindowsPathEqual -Left $Install.root -Right $ProtectedRoot) {
                $IsProtected = $true
                break
            }
        }
        if ($IsProtected -or $Install.createdUtc -gt $DeleteBefore) {
            continue
        }
        try {
            Remove-Item -LiteralPath $Install.root -Recurse -Force
        }
        catch {
            Write-Warning "Could not remove obsolete LiveKit SDK install $($Install.root): $($_.Exception.Message)"
        }
    }
}
