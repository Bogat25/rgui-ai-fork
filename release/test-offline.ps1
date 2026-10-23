param([string]$WorkRoot = ([IO.Path]::GetTempPath()))
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'offline.ps1')
$fixture = Join-Path $WorkRoot ('offline-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture -Force | Out-Null
$file = Join-Path $fixture 'fixture.gguf'
[IO.File]::WriteAllText($file,'synthetic model fixture')
$manifest = Join-Path $fixture 'assets.json'
@{ assets = @(@{ name='fixture.gguf'; bytes=(Get-Item $file).Length;
    sha256=(Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant() }) } |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifest -Encoding UTF8
try {
    $argument = Get-OfflineInstallerArguments -CacheDirectory $fixture -OutputDirectory $fixture -ModelDestination '{app}\models' -Manifest $manifest
    if ($argument -notlike '/DOfflineModelsInclude=*') { throw 'Missing compiler include argument.' }
    $include = Get-Content (Join-Path $fixture 'offline-models.iss') -Raw
    if ($include -notmatch 'nocompression solidbreak' -or $include -notmatch 'Hash:' -or $include -notmatch 'Qwen3.5-4B-LICENSE') {
        throw 'The offline installer lacks verification, seek boundaries, or its license.'
    }
    Assert-InstalledModels -Directory $fixture -Manifest $manifest
    $rejected = $false
    try { Assert-InstallerSize -Path $file -MaximumBytes 1 } catch { $rejected = $true }
    if (-not $rejected) { throw 'Size budget did not reject an oversized payload.' }
    [IO.File]::WriteAllText($file,'corrupt fixture')
    $rejected = $false
    try { Get-OfflineInstallerArguments -CacheDirectory $fixture -OutputDirectory $fixture -ModelDestination '{app}\models' -Manifest $manifest | Out-Null } catch { $rejected = $true }
    if (-not $rejected) { throw 'Corrupt model was accepted.' }
    Remove-Item -LiteralPath $file
    $rejected = $false
    try { Get-OfflineInstallerArguments -CacheDirectory $fixture -OutputDirectory $fixture -ModelDestination '{app}\models' -Manifest $manifest | Out-Null } catch { $rejected = $true }
    if (-not $rejected) { throw 'Missing model was accepted.' }
    Write-Host 'Offline packaging verification checks passed.'
} finally {
    $resolved = [IO.Path]::GetFullPath($fixture)
    $prefix = [IO.Path]::GetFullPath($WorkRoot).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe test cleanup path.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
