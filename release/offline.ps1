# Shared by the three Windows forks. Keep release/ infrastructure in sync.
function Get-OfflineInstallerArguments {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)][string]$CacheDirectory,
        [Parameter(Mandatory=$true)][string]$OutputDirectory,
        [Parameter(Mandatory=$true)][string]$ModelDestination,
        [string]$Manifest = (Join-Path $PSScriptRoot 'assets.json')
    )
    $spec = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
    $entries = @('[Files]')
    foreach ($asset in $spec.assets) {
        if ($asset.name -notmatch '^[A-Za-z0-9_.-]+\.gguf$' -or $asset.sha256 -notmatch '^[a-f0-9]{64}$') {
            throw 'Invalid offline asset manifest.'
        }
        $file = Join-Path $CacheDirectory $asset.name
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
            throw "Offline model missing: $($asset.name). Run fetch without -NoModel."
        }
        if ((Get-Item -LiteralPath $file).Length -ne [long]$asset.bytes -or
            (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $asset.sha256) {
            throw "Offline model does not match its size/SHA-256 pin: $($asset.name)."
        }
        if ($file -match '["\r\n]' -or $ModelDestination -match '["\r\n]') { throw 'Invalid installer model path.' }
        $entries += ('Source: "{0}"; DestDir: "{1}"; Hash: "{2}"; Flags: ignoreversion nocompression solidbreak' -f
            [IO.Path]::GetFullPath($file),$ModelDestination,$asset.sha256)
    }
    $license = Join-Path $PSScriptRoot 'Qwen3.5-4B-LICENSE.txt'
    if (-not (Test-Path -LiteralPath $license)) { throw 'Model license is missing.' }
    $entries += ('Source: "{0}"; DestDir: "{{app}}\licenses"; Flags: ignoreversion' -f $license)
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    $include = Join-Path $OutputDirectory 'offline-models.iss'
    [IO.File]::WriteAllText($include,($entries -join "`n") + "`n",[Text.UTF8Encoding]::new($false))
    return "/DOfflineModelsInclude=$include"
}

function Assert-InstallerSize {
    param([Parameter(Mandatory=$true)][string]$Path, [long]$MaximumBytes = 4100000000)
    if ((Get-Item -LiteralPath $Path).Length -gt $MaximumBytes) {
        throw 'Installer exceeds the single-EXE size budget. Improve application compression before release.'
    }
}

function Assert-InstalledModels {
    param([Parameter(Mandatory=$true)][string]$Directory,
          [string]$Manifest = (Join-Path $PSScriptRoot 'assets.json'))
    $spec = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
    foreach ($asset in $spec.assets) {
        $file = Join-Path $Directory $asset.name
        if (-not (Test-Path -LiteralPath $file) -or
            (Get-Item -LiteralPath $file).Length -ne [long]$asset.bytes -or
            (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $asset.sha256) {
            throw "Installed offline model failed verification: $($asset.name)."
        }
    }
}
