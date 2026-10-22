# Compatibility alias for the same paired-only setup. The assistant checks both devices before offering changes.
& {
    $ErrorActionPreference = 'Stop'
    $source = Invoke-RestMethod -Uri 'https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/dcb6a273ccb25e159e1d4fd2410c28fea64f70db/get.ps1' -TimeoutSec 30
    if ($source -isnot [string]) { throw 'Unexpected setup bootstrap response.' }
    $hasher = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = ([BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($source)))).Replace('-', '').ToLowerInvariant()
    } finally { $hasher.Dispose() }
    if ($digest -cne '6c760bdaf2ccc7c76dcf32e3bbd25801432f93a50f60d2ff2b8c31f95bf13705') {
        throw 'Setup bootstrap differs from the reviewed revision. Nothing was run.'
    }
    & ([scriptblock]::Create($source)) -BridgePreview
}
