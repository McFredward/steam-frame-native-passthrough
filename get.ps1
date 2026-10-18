param([switch] $InspectionOnly)
# Online entry point. Runs from Invoke-RestMethod | Invoke-Expression too.
& {
    param([bool] $sfnpInspection)
    $ErrorActionPreference = 'Stop'
    Set-StrictMode -Version 2
    $sfnpStage = $null
    $sfnpTls = [Net.ServicePointManager]::SecurityProtocol
    $sfnpRepo = 'McFredward/steam-link-native-passthrough'
    $sfnpAsset = 'steam-link-native-passthrough-setup.zip'
    $sfnpEndpoint = '/releases/latest'
    if ($sfnpInspection) {
        $sfnpAsset = 'steam-link-native-passthrough-inspection.zip'
        $sfnpEndpoint = '/releases/tags/setup-inspection-v2'
    }

    function Require-Sfnp($Condition, [string] $Message) {
        if (-not $Condition) { throw $Message }
    }
    function Get-SfnpHash([byte[]] $Bytes) {
        $hasher = [Security.Cryptography.SHA256]::Create()
        try { return ([BitConverter]::ToString($hasher.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
        finally { $hasher.Dispose() }
    }
    function Get-SfnpName([string] $Name) {
        Require-Sfnp ($Name.Length -gt 0 -and $Name.Length -le 240 -and $Name -notmatch '[\\:*?"<>|\x00-\x1f]') 'Unsafe archive path.'
        $parts = $Name.TrimEnd('/').Split('/')
        foreach ($part in $parts) {
            Require-Sfnp ($part -ne '' -and $part -ne '.' -and $part -ne '..' -and $part -notmatch '[. ]$' -and
                $part -notmatch '^(con|prn|aux|nul|conin\$|conout\$|com[1-9\u00b9\u00b2\u00b3]|lpt[1-9\u00b9\u00b2\u00b3])(\..*)?$') 'Unsafe archive component.'
        }
        return ($parts -join '/')
    }
    function Receive-Sfnp([string] $Url, [long] $Limit, [string] $Output = '') {
        $next = [Uri] $Url
        for ($redirect = 0; $redirect -le 5; $redirect++) {
            Require-Sfnp ($next.Scheme -eq 'https' -and $next.Port -eq 443 -and $next.UserInfo -eq '' -and
                $next.Host -in @('api.github.com','github.com','release-assets.githubusercontent.com','objects.githubusercontent.com')) 'Unexpected download host.'
            $request = [Net.HttpWebRequest]::Create($next)
            $request.AllowAutoRedirect = $false
            $request.Timeout = 30000
            $request.ReadWriteTimeout = 30000
            $request.UserAgent = 'Steam-Link-Native-Passthrough-Setup'
            $response = $request.GetResponse()
            try {
                $status = [int] $response.StatusCode
                if ($status -in @(301,302,303,307,308)) {
                    Require-Sfnp ($redirect -lt 5 -and $response.Headers['Location']) 'Download redirect bound exceeded.'
                    $next = New-Object Uri ($next, $response.Headers['Location'])
                    continue
                }
                Require-Sfnp ($status -eq 200 -and $response.ContentLength -le $Limit) 'Download status/size refused.'
                $inputStream = $response.GetResponseStream()
                if ($Output -eq '') { $destination = New-Object IO.MemoryStream }
                else { $destination = [IO.File]::Open($Output, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None) }
                try {
                    $buffer = New-Object byte[] 65536
                    [long] $total = 0
                    $progressClock = [Diagnostics.Stopwatch]::StartNew()
                    while (($count = $inputStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                        $total += $count
                        Require-Sfnp ($total -le $Limit) 'Download exceeds size bound.'
                        $destination.Write($buffer, 0, $count)
                        if ($Output -ne '' -and $progressClock.ElapsedMilliseconds -ge 200) {
                            Write-Progress -Id 1 -Activity 'Preparing the setup assistant' -Status 'Downloading verified tools' -PercentComplete ([Math]::Min(100, [int](100 * $total / $Limit)))
                            $progressClock.Restart()
                        }
                    }
                    if ($Output -eq '') { return ,$destination.ToArray() }
                    return
                } finally {
                    if ($Output -ne '') { Write-Progress -Id 1 -Activity 'Preparing the setup assistant' -Completed }
                    $inputStream.Dispose(); $destination.Dispose()
                }
            } finally { $response.Dispose() }
        }
        throw 'Download redirect bound exceeded.'
    }
    function Read-SfnpEntry($Entry, [long] $Limit) {
        Require-Sfnp ($Entry.Length -ge 0 -and $Entry.Length -le $Limit) 'Archive file exceeds bound.'
        $inputStream = $Entry.Open()
        $memory = New-Object IO.MemoryStream
        try {
            $buffer = New-Object byte[] 65536
            while (($count = $inputStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                Require-Sfnp ($memory.Length + $count -le $Limit) 'Expanded file exceeds bound.'
                $memory.Write($buffer, 0, $count)
            }
            Require-Sfnp ($memory.Length -eq $Entry.Length) 'Archive file size differs.'
            return ,$memory.ToArray()
        } finally { $inputStream.Dispose(); $memory.Dispose() }
    }
    function Test-SfnpInspectionFile([string] $Name) {
        return ($Name -cin @('setup-inspection.json','setup/steam_link_setup.py','setup/steam_link_setup_ui.py',
            'README.md','README.de.md','README.txt','LICENSE','patch-steam-link.cmd','patch-steam-link.sh') -or
            $Name.StartsWith('setup/runtime/python/', [StringComparison]::Ordinal) -or
            $Name.StartsWith('setup/runtime/openssh/', [StringComparison]::Ordinal))
    }
    function Expand-SfnpPackage([string] $Archive, [string] $Target, [bool] $InspectionOnly = $false) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $package = [IO.Compression.ZipFile]::OpenRead($Archive)
        try {
            Require-Sfnp ($package.Entries.Count -gt 0 -and $package.Entries.Count -le 512) 'Archive entry bound exceeded.'
            $files = @{}
            $names = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
            [long] $total = 0
            foreach ($entry in $package.Entries) {
                $name = Get-SfnpName $entry.FullName
                Require-Sfnp ($names.Add($name)) 'Duplicate archive path.'
                $directory = $entry.FullName.EndsWith('/')
                $kind = ($entry.ExternalAttributes -shr 16) -band 61440
                $expectedKind = 32768
                if ($directory) { $expectedKind = 16384 }
                Require-Sfnp ($kind -eq 0 -or $kind -eq $expectedKind) 'Archive links/devices refused.'
                if ($directory) { continue }
                Require-Sfnp ($entry.Length -ge 0 -and $entry.Length -le 33554432) 'Archive file exceeds bound.'
                $total += $entry.Length
                Require-Sfnp ($total -le 201326592) 'Expanded archive exceeds bound.'
                $files[$name] = $entry
            }
            Require-Sfnp ($files.ContainsKey('bootstrap-package.json')) 'Bootstrap manifest missing.'
            $rawManifest = Read-SfnpEntry $files['bootstrap-package.json'] 262144
            $manifest = [Text.Encoding]::UTF8.GetString($rawManifest) | ConvertFrom-Json
            $keys = @($manifest.PSObject.Properties.Name | Sort-Object)
            $schema = 'sfnp-bootstrap-package-v1'; $readiness = 'release'; $profileName = 'steam-link-bundle.json'
            if ($InspectionOnly) { $schema = 'sfnp-bootstrap-inspection-v1'; $readiness = 'inspection-only'; $profileName = 'setup-inspection.json' }
            Require-Sfnp (($keys -join ',') -eq 'files,readiness,schema,windows_python' -and
                $manifest.schema -eq $schema -and $manifest.readiness -eq $readiness) 'Incomplete or mismatched setup package.'
            $pins = @($manifest.files.PSObject.Properties)
            Require-Sfnp ($pins.Count -eq $files.Count - 1 -and
                $files.ContainsKey($profileName) -and $files.ContainsKey('setup/steam_link_setup.py')) 'Setup inventory differs.'
            if ($InspectionOnly) {
                foreach ($property in $pins) {
                    Require-Sfnp (Test-SfnpInspectionFile $property.Name) 'Inspection package contains an installation payload.'
                }
            }
            foreach ($property in $pins) {
                $name = Get-SfnpName $property.Name
                $pin = $property.Value
                Require-Sfnp ($files.ContainsKey($name) -and $name -ne 'bootstrap-package.json' -and
                    ((@($pin.PSObject.Properties.Name | Sort-Object) -join ',') -eq 'bytes,sha256') -and
                    $pin.bytes -is [ValueType] -and $pin.bytes -eq $files[$name].Length -and
                    $pin.sha256 -cmatch '^[0-9a-f]{64}$') 'Invalid inner file pin.'
                $bytes = Read-SfnpEntry $files[$name] 33554432
                Require-Sfnp ((Get-SfnpHash $bytes) -ceq $pin.sha256) 'Inner file hash mismatch.'
                $path = Join-Path $Target $name
                [void] [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
                $output = [IO.File]::Open($path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
                try { $output.Write($bytes, 0, $bytes.Length) } finally { $output.Dispose() }
            }
            if ($InspectionOnly) {
                $profile = [IO.File]::ReadAllText((Join-Path $Target 'setup-inspection.json')) | ConvertFrom-Json
                Require-Sfnp ($profile.schema -eq 'sfnp-setup-inspection-v1' -and
                    ((@($profile.PSObject.Properties.Name | Sort-Object) -join ',') -eq 'expected_pc,runtime_files,schema')) 'Invalid inspection profile.'
            } else {
                $bundle = [IO.File]::ReadAllText((Join-Path $Target 'steam-link-bundle.json')) | ConvertFrom-Json
                Require-Sfnp ($bundle.schema -eq 'sfnp-steam-link-setup-v1' -and $bundle.readiness -eq 'release') 'Only complete release bundles may start setup.'
            }
            Require-Sfnp ($manifest.windows_python -is [string] -and
                $manifest.windows_python -eq 'setup/runtime/python/python.exe' -and
                $files.ContainsKey($manifest.windows_python)) 'Verified portable Python runtime missing.'
            return $manifest.windows_python
        } finally { $package.Dispose() }
    }

    try {
        Require-Sfnp ($env:OS -eq 'Windows_NT') 'Use the Bash setup command on Linux.'
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        if ($sfnpInspection) { Write-Host 'Steam Link Native Passthrough - downloading read-only inspection (no installation or restart)...' }
        else { Write-Host 'Steam Link Native Passthrough - downloading the guided setup...' }
        $releaseBytes = Receive-Sfnp ('https://api.github.com/repos/' + $sfnpRepo + $sfnpEndpoint) 1048576
        $release = [Text.Encoding]::UTF8.GetString($releaseBytes) | ConvertFrom-Json
        Require-Sfnp ($release.draft -eq $false -and $release.prerelease -eq $sfnpInspection -and
            $release.tag_name -cmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$') 'No matching public setup release is available.'
        Require-Sfnp (-not $sfnpInspection -or $release.tag_name -ceq 'setup-inspection-v2') 'Unexpected inspection release tag.'
        $assets = @($release.assets | Where-Object { $_.name -ceq $sfnpAsset })
        Require-Sfnp ($assets.Count -eq 1) 'This release has no complete PC/Frame setup package. Nothing was installed.'
        $asset = $assets[0]
        Require-Sfnp ($asset.state -eq 'uploaded' -and $asset.size -gt 0 -and $asset.size -le 134217728 -and
            $asset.digest -cmatch '^sha256:[0-9a-f]{64}$' -and
            $asset.browser_download_url -ceq ('https://github.com/' + $sfnpRepo + '/releases/download/' + $release.tag_name + '/' + $sfnpAsset)) 'Release asset lacks its exact URL, size or SHA-256 digest.'
        $sfnpStage = Join-Path ([IO.Path]::GetTempPath()) ('sfnp-setup-' + [Guid]::NewGuid().ToString('N'))
        $directory = [IO.Directory]::CreateDirectory($sfnpStage)
        $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User
        $acl = New-Object Security.AccessControl.DirectorySecurity
        $acl.SetOwner($sid)
        $acl.SetAccessRuleProtection($true, $false)
        foreach ($identity in @($sid, (New-Object Security.Principal.SecurityIdentifier 'S-1-5-18'))) {
            $rule = New-Object Security.AccessControl.FileSystemAccessRule ($identity, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')
            [void] $acl.AddAccessRule($rule)
        }
        Set-Acl -LiteralPath $directory.FullName -AclObject $acl
        $archive = Join-Path $sfnpStage 'download.zip'
        Receive-Sfnp $asset.browser_download_url $asset.size $archive
        Require-Sfnp ((Get-Item -LiteralPath $archive).Length -eq $asset.size -and
            (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $asset.digest.Substring(7)) 'Setup download failed its SHA-256 check.'
        $target = Join-Path $sfnpStage 'package'
        [void] [IO.Directory]::CreateDirectory($target)
        $python = Join-Path $target (Expand-SfnpPackage $archive $target $sfnpInspection)
        if ($sfnpInspection) { Write-Host 'Inspection package verified. This mode only reads and reports; it cannot apply changes.' }
        else { Write-Host 'Package verified. The assistant will check both devices before offering changes.' }
        $sfnpArguments = @('-I', '-B', (Join-Path $target 'setup/steam_link_setup.py'), '--bundle', $target)
        if ($sfnpInspection) { $sfnpArguments += '--inspect' }
        & $python @sfnpArguments
        Require-Sfnp ($LASTEXITCODE -eq 0) 'The setup assistant stopped. No automatic restart was performed.'
    } catch {
        Write-Host ('Setup could not finish: ' + $_.Exception.Message) -ForegroundColor Red
        Write-Host 'No automatic restart was performed. You can safely run the setup command again.'
    } finally {
        [Net.ServicePointManager]::SecurityProtocol = $sfnpTls
        if ($null -ne $sfnpStage -and [IO.Directory]::Exists($sfnpStage)) {
            Remove-Item -LiteralPath $sfnpStage -Recurse -Force
        }
    }
} ([bool] $InspectionOnly)
