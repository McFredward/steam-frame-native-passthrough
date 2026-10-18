# Read-only preview. The pinned bootstrap always receives -InspectionOnly.
& {
    $ErrorActionPreference = 'Stop'
    $source = Invoke-RestMethod -Uri 'https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/2c2fdeeef5ce623c55b2c8489872531f8226a238/get.ps1' -TimeoutSec 30
    if ($source -isnot [string]) { throw 'Unexpected inspection bootstrap response.' }
    $hasher = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = ([BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($source)))).Replace('-', '').ToLowerInvariant()
    } finally { $hasher.Dispose() }
    if ($digest -cne '750287601f18420a4adbf300aad8ec911c3955cb0fd076afe93bf924920846b8') {
        throw 'Inspection bootstrap differs from the reviewed revision. Nothing was run.'
    }
    & ([scriptblock]::Create($source)) -InspectionOnly
}
