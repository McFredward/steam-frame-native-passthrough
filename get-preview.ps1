# Read-only preview. The pinned bootstrap always receives -InspectionOnly.
& {
    $ErrorActionPreference = 'Stop'
    $source = Invoke-RestMethod -Uri 'https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/b171247a19b39236f764a5ee5525f0ad4cf4f110/get.ps1' -TimeoutSec 30
    if ($source -isnot [string]) { throw 'Unexpected inspection bootstrap response.' }
    $hasher = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = ([BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($source)))).Replace('-', '').ToLowerInvariant()
    } finally { $hasher.Dispose() }
    if ($digest -cne '5384c38d30e19ba794e660475a615baeb9d527b1c800b7b8e8681c4d434028c5') {
        throw 'Inspection bootstrap differs from the reviewed revision. Nothing was run.'
    }
    & ([scriptblock]::Create($source)) -InspectionOnly
}
