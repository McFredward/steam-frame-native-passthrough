# Compatibility alias for the same paired-only setup. The assistant checks both devices before offering changes.
& {
    $ErrorActionPreference = 'Stop'
    $source = Invoke-RestMethod -Uri 'https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/2b0bcb97c56cf391b3e00c6cec0c55439495d686/get.ps1' -TimeoutSec 30
    if ($source -isnot [string]) { throw 'Unexpected setup bootstrap response.' }
    $hasher = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = ([BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($source)))).Replace('-', '').ToLowerInvariant()
    } finally { $hasher.Dispose() }
    if ($digest -cne '3babf4839b629a2e019cb5b7673bd8e88827d1df07ca4b923b62a102c2f88f9a') {
        throw 'Setup bootstrap differs from the reviewed revision. Nothing was run.'
    }
    & ([scriptblock]::Create($source)) -BridgePreview
}
