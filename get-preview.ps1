# Compatibility alias for the same paired-only setup. The assistant checks both devices before offering changes.
& {
    $ErrorActionPreference = 'Stop'
    $source = Invoke-RestMethod -Uri 'https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/f6a29075a7426323a02372a951af82145b8f8fad/get.ps1' -TimeoutSec 30
    if ($source -isnot [string]) { throw 'Unexpected setup bootstrap response.' }
    $hasher = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = ([BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($source)))).Replace('-', '').ToLowerInvariant()
    } finally { $hasher.Dispose() }
    if ($digest -cne '10cead622d116d048a6d7567c4332265cbfcfd012eeeada19e0dff989eec12dc') {
        throw 'Setup bootstrap differs from the reviewed revision. Nothing was run.'
    }
    & ([scriptblock]::Create($source)) -BridgePreview
}
