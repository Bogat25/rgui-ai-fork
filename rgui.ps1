<#
.SYNOPSIS
    Build, test, run and package RGui with the local AI assistant.

.DESCRIPTION
    One script for the whole cycle.  The source repository is never built
    in place: it is mirrored to a build tree (default D:\rgui-build\tree)
    whose path has no spaces, because R's makefiles do not quote paths.
    Only changed files are copied, so incremental builds stay incremental.

    Everyday use:

        .\rgui dev          rebuild R.dll + the .exe files, then start Rgui
        .\rgui test         compile check + unit and protocol tests (seconds)

    First time:

        .\rgui fetch        download Tcl/Tk bundle, llama.cpp and the model
        .\rgui full         build R, base and recommended packages

    All commands:

        doctor    check prerequisites and show the state of everything
        fetch     download into the cache (-NoModel skips the 2.7 GB model)
        full      complete build; also the right choice after editing src/main
        quick     rebuild R.dll and the front-ends only (the fast path)
        run       start the built Rgui with the AI payload in place
        dev       quick, then run
        test      static checks + tests; -Real drives the real model,
                  -Gui drives the built Rgui.exe (menu, panel, buttons),
                  -Installer installs, upgrades and uninstalls the setup.exe
        package   assemble the pendrive layout in <BuildRoot>\dist
        installer build RGui-AI-<version>-setup.exe (Inno Setup; the model
                  is not inside, RGui downloads it on first use)
        deploy    copy dist to a pendrive (-Drive E:), keeping course notes
        clean     delete the build tree; downloads are kept

.EXAMPLE
    .\rgui dev
.EXAMPLE
    .\rgui test -Real
.EXAMPLE
    .\rgui deploy -Drive E:
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('doctor', 'fetch', 'full', 'quick', 'run', 'dev', 'test',
                 'package', 'installer', 'deploy', 'clean')]
    [string]$Command = 'doctor',

    # Where the build tree, downloads, logs and dist live.  No spaces.
    [string]$BuildRoot = 'D:\rgui-build',

    # Rtools45 location; found from the registry when empty.
    [string]$Rtools = '',

    # Parallel make jobs; 0 means one per logical CPU.
    [int]$Jobs = 0,

    # llama.cpp build to use, e.g. b11153, or 'latest'.
    [string]$LlamaBuild = 'b11153',

    # fetch/package: leave the model out.
    [switch]$NoModel,

    # test: also start llama-server with the real model and ask it something.
    [switch]$Real,

    # test: also drive the built Rgui.exe through its window messages.
    [switch]$Gui,

    # test: also install the last built installer into a folder with a
    # space in its name, start it, upgrade it and uninstall it.
    [switch]$Installer,

    # deploy: target drive, e.g. E:
    [string]$Drive = '',

    # deploy: allow overwriting an R folder this script did not create.
    [switch]$Force,

    # installer: version, e.g. 0.1.1 or 0.2.0-rc1.  Default: the v-tag on
    # HEAD if there is one, else 0.0.0-dev.
    [string]$Version = '',

    # installer: ISCC.exe to use; found automatically when empty.
    [string]$InnoSetup = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# ---------------------------------------------------------------------
# Pinned inputs
# ---------------------------------------------------------------------

$TclBundle   = 'tcltk-6768-6663.zip'
$TclUrl      = "https://cran.r-project.org/bin/windows/Rtools/rtools45/files/$TclBundle"
$ModelFile   = 'Qwen3.5-4B-Q4_K_M.gguf'
$ModelUrl    = "https://huggingface.co/unsloth/Qwen3.5-4B-GGUF/resolve/main/$ModelFile"
$ModelSha256 = '00fe7986ff5f6b463e62455821146049db6f9313603938a70800d1fb69ef11a4'
$ModelSize   = 2740937888
$LlamaRepo   = 'ggml-org/llama.cpp'

# Everything that ends up in the installer is checked against these.
$TclSha256   = 'd83744f6627a8f1facaaf3caa04e5d29764606b8246c7c6d91358a570489f61f'
$LlamaSha256 = @{ 'b11153' = '569d19826f3fb00a3fc2df7bd68ab9ad33e5c0d6d69ce022b24372700cee7931' }

$InnoFile    = 'innosetup-7.1.0-x64.exe'
$InnoUrl     = "https://github.com/jrsoftware/issrc/releases/download/is-7_1_0/$InnoFile"
$InnoSha256  = '0362a383ed217d4c4239b5933866dd96d3eb2102737da92f80f6057a4b40df2f'

# ---------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------

$Repo    = $PSScriptRoot
$Tree    = Join-Path $BuildRoot 'tree'
$Cache   = Join-Path $BuildRoot 'cache'
$Logs    = Join-Path $BuildRoot 'logs'
$Dist    = Join-Path $BuildRoot 'dist'
$DevHome = Join-Path $BuildRoot 'devhome'
$script:LastLog = $null
$script:RtoolsDir = $null

if ($BuildRoot -match '\s') {
    Write-Host "BuildRoot must not contain spaces: $BuildRoot" -ForegroundColor Red
    exit 2
}
if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

# ---------------------------------------------------------------------
# Output helpers
# ---------------------------------------------------------------------

function Say([string]$Message)  { Write-Host "==> $Message" -ForegroundColor Cyan }
function Note([string]$Message) { Write-Host "    $Message" }
function Warn([string]$Message) { Write-Host "WARNING: $Message" -ForegroundColor Yellow }

function Stop-WithError([string]$Message) {
    Write-Host ""
    Write-Host "FAILED: $Message" -ForegroundColor Red
    if ($script:LastLog -and (Test-Path $script:LastLog)) {
        $pattern = '(^|[\s:])(error|Error)\b|\*\*\*|undefined reference|No such file|fatal'
        $hits = @(Select-String -Path $script:LastLog -Pattern $pattern |
                  Select-Object -First 15)
        if ($hits.Count -gt 0) {
            Write-Host "--- first error lines ---" -ForegroundColor Yellow
            foreach ($h in $hits) { Write-Host ("  {0,6}: {1}" -f $h.LineNumber, $h.Line) }
        }
        Write-Host "--- last lines ---" -ForegroundColor Yellow
        Get-Content $script:LastLog -Tail 15 | ForEach-Object { Write-Host "  $_" }
        Write-Host "full log: $script:LastLog"
    }
    exit 1
}

function Format-Elapsed([System.Diagnostics.Stopwatch]$Watch) {
    $t = $Watch.Elapsed
    if ($t.TotalMinutes -ge 1) { return ('{0}m{1:00}s' -f [int][math]::Floor($t.TotalMinutes), $t.Seconds) }
    return ('{0:0.0}s' -f $t.TotalSeconds)
}

# ---------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------

function Find-Rtools {
    if ($Rtools) { return $Rtools }
    foreach ($key in 'HKLM:\SOFTWARE\R-core\Rtools', 'HKCU:\SOFTWARE\R-core\Rtools') {
        try {
            $p = (Get-ItemProperty $key -ErrorAction Stop).InstallPath
            if ($p -and (Test-Path $p)) { return $p.TrimEnd('\') }
        } catch { }
    }
    foreach ($c in 'C:\rtools45', 'D:\rtools45') { if (Test-Path $c) { return $c } }
    return $null
}

function Initialize-Toolchain {
    $script:RtoolsDir = Find-Rtools
    if (-not $script:RtoolsDir) {
        Stop-WithError 'Rtools45 not found. Install it, or pass -Rtools <dir>.'
    }
    foreach ($rel in 'usr\bin\bash.exe', 'usr\bin\make.exe',
                     'x86_64-w64-mingw32.static.posix\bin\gcc.exe') {
        if (-not (Test-Path (Join-Path $script:RtoolsDir $rel))) {
            Stop-WithError "Rtools at $script:RtoolsDir is incomplete: $rel is missing."
        }
    }
}

function ConvertTo-MsysPath([string]$Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    return '/' + $full.Substring(0, 1).ToLowerInvariant() + ($full.Substring(2) -replace '\\', '/')
}

# Run a bash script under Rtools with the environment CRAN's build how-to
# prescribes, echoing live and teeing to a log.  Returns the exit code.
# The script must not contain double quotes (Windows PowerShell 5.1
# mangles them when passing arguments to native programs).
function Invoke-Bash([string]$Code, [string]$LogName) {
    if ($Code.Contains('"')) { throw "Invoke-Bash: use single quotes only" }
    New-Item -ItemType Directory -Force -Path $Logs | Out-Null
    $log = Join-Path $Logs ('{0}-{1}.log' -f $LogName, (Get-Date -Format 'yyyyMMdd-HHmmss'))
    $script:LastLog = $log
    $sys = ConvertTo-MsysPath $env:SystemRoot
    $prologue = "export PATH=/x86_64-w64-mingw32.static.posix/bin:/usr/bin:$sys/System32:$sys; " +
                "export TAR=/usr/bin/tar TAR_OPTIONS=--force-local; " +
                "unset R_HOME R_LIBS R_LIBS_USER R_LIBS_SITE; set -o pipefail; "
    $cmd = $prologue + "{ $Code ; } 2>&1 | tee '" + (ConvertTo-MsysPath $log) + "'"
    $bash = Join-Path $script:RtoolsDir 'usr\bin\bash.exe'
    & $bash --noprofile --norc -c $cmd | Out-Host
    return $LASTEXITCODE
}

# ---------------------------------------------------------------------
# Source sync
# ---------------------------------------------------------------------

# Mirror the repo into the build tree.  /XO skips files that are older in
# the repo than in the tree, so files the build regenerates are left
# alone, while anything edited in the repo (always newer) is copied.
function Sync-Tree {
    New-Item -ItemType Directory -Force -Path $Tree | Out-Null
    # /XX: do not report build outputs that exist only in the tree.
    $out = & robocopy.exe $Repo $Tree /E /XO /XX /XD .git .vs .vscode .claude `
                          /NDL /NJH /NJS /NP /NC /NS /R:1 /W:1 /MT:16
    $rc = $LASTEXITCODE
    if ($rc -ge 8) { Stop-WithError "copying sources to $Tree failed (robocopy code $rc)" }
    $files = @($out | ForEach-Object { "$_".Trim() } | Where-Object { $_ })
    if ($files.Count -eq 0) {
        Note 'sources: no changes'
    } elseif ($files.Count -le 8) {
        Note ("sources: {0} changed" -f $files.Count)
        foreach ($f in $files) { Note ('  ' + $f.Replace($Repo + '\', '')) }
    } else {
        Note ("sources: {0} files copied" -f $files.Count)
    }
}

# One-off preparation the official recipe does by hand.
function Initialize-Tree {
    if (-not (Test-Path (Join-Path $Tree 'Tcl\bin'))) {
        $zip = Join-Path $Cache $TclBundle
        if (-not (Test-Path $zip)) { Stop-WithError "Tcl/Tk bundle not downloaded; run '.\rgui fetch'." }
        Say "unpacking $TclBundle into the build tree"
        & (Join-Path $env:SystemRoot 'System32\tar.exe') -xf $zip -C $Tree
        if ($LASTEXITCODE -ne 0) { Stop-WithError 'unpacking the Tcl/Tk bundle failed' }
        if (-not (Test-Path (Join-Path $Tree 'Tcl\bin'))) {
            Stop-WithError "the Tcl/Tk bundle did not create $Tree\Tcl\bin"
        }
    }
    if (-not (Test-Path (Join-Path $Tree 'src\library\Recommended\MASS.tgz'))) {
        Say 'linking recommended packages'
        $rc = Invoke-Bash ("cd '{0}' && sh tools/link-recommended" -f (ConvertTo-MsysPath $Tree)) 'link-recommended'
        if ($rc -ne 0) { Stop-WithError 'tools/link-recommended failed' }
    }
}

# Anything started from the build tree holds R.dll open; relinking then
# fails with "Permission denied".  Only processes whose executable lives
# inside the build tree are touched.
function Stop-DevProcesses {
    $prefix = [System.IO.Path]::GetFullPath($Tree).TrimEnd('\') + '\'
    $procs = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
        $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
    })
    foreach ($p in $procs) {
        Note ("stopping {0} (pid {1}) from the build tree" -f $p.Name, $p.ProcessId)
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
    if ($procs.Count -gt 0) { Start-Sleep -Milliseconds 700 }
}

function Test-Built { return (Test-Path (Join-Path $Tree 'bin\x64\Rgui.exe')) }

# ---------------------------------------------------------------------
# Downloads
# ---------------------------------------------------------------------

function Get-Download([string]$Url, [string]$Dest) {
    $part = "$Dest.part"
    & curl.exe --fail --location --progress-bar --retry 3 --retry-delay 2 --continue-at - --output $part $Url
    if ($LASTEXITCODE -ne 0) { Stop-WithError "download failed: $Url" }
    Move-Item -Force $part $Dest
}

# Download unless already cached, then insist on the pinned checksum.
function Get-VerifiedDownload([string]$Url, [string]$Dest, [string]$Sha256, [string]$Label) {
    if (-not (Test-Path $Dest)) { Say "downloading $Label"; Get-Download $Url $Dest }
    if ($Sha256) {
        $got = (Get-FileHash -Algorithm SHA256 $Dest).Hash.ToLowerInvariant()
        if ($got -ne $Sha256) {
            Remove-Item -Force $Dest
            Stop-WithError "$Label does not match its pinned SHA-256 (got $got); deleted, run fetch again"
        }
        Note "$Label`: verified"
    } else {
        Note "$Label`: cached (no pinned checksum for this version)"
    }
}

function Find-InnoSetup {
    if ($InnoSetup) { return $InnoSetup }
    $candidates = @((Join-Path $BuildRoot 'tools\innosetup\ISCC.exe'))
    foreach ($key in 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 7_is1',
                     'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 7_is1',
                     'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 7_is1') {
        try {
            $loc = (Get-ItemProperty $key -ErrorAction Stop).InstallLocation
            if ($loc) { $candidates += (Join-Path $loc 'ISCC.exe') }
        } catch { }
    }
    $candidates += "$env:ProgramFiles\Inno Setup 7\ISCC.exe"
    foreach ($c in $candidates) { if ($c -and (Test-Path $c)) { return $c } }
    return $null
}

# Inno Setup, per user and without administrator rights, into the build
# root, so every machine (CI included) builds with the same version.
function Install-InnoSetup {
    $iscc = Join-Path $BuildRoot 'tools\innosetup\ISCC.exe'
    if (Test-Path $iscc) { Note 'Inno Setup: installed'; return }
    $exe = Join-Path $Cache $InnoFile
    Get-VerifiedDownload $InnoUrl $exe $InnoSha256 'Inno Setup 7.1.0'
    Say 'installing Inno Setup for the current user'
    $dir = Join-Path $BuildRoot 'tools\innosetup'
    $p = Start-Process -FilePath $exe -Wait -PassThru -ArgumentList `
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER', '/NOICONS', "/DIR=$dir"
    if ($p.ExitCode -ne 0 -or -not (Test-Path $iscc)) {
        Stop-WithError "installing Inno Setup failed (exit code $($p.ExitCode))"
    }
}

function Resolve-LlamaBuild {
    if ($LlamaBuild -ne 'latest') { return $LlamaBuild }
    Note 'asking GitHub for the newest llama.cpp Windows CPU build'
    $rels = Invoke-RestMethod -Uri "https://api.github.com/repos/$LlamaRepo/releases?per_page=20" `
                              -Headers @{ 'User-Agent' = 'rgui-build' }
    foreach ($r in $rels) {
        foreach ($a in $r.assets) {
            if ($a.name -match '^llama-(b\d+)-bin-win-cpu-x64\.zip$') { return $Matches[1] }
        }
    }
    Stop-WithError 'no llama.cpp release with a win-cpu-x64 build was found'
}

# The llama.cpp zip to install: the pinned one, or with -LlamaBuild latest
# the newest already in the cache (so 'run' works offline).
function Get-LlamaZip {
    if ($LlamaBuild -ne 'latest') {
        return (Join-Path $Cache "llama-$LlamaBuild-bin-win-cpu-x64.zip")
    }
    $zips = @(Get-ChildItem $Cache -Filter 'llama-b*-bin-win-cpu-x64.zip' -ErrorAction SilentlyContinue |
              Sort-Object { [int]($_.Name -replace '^llama-b(\d+)-.*$', '$1') } -Descending)
    if ($zips.Count -eq 0) { return $null }
    return $zips[0].FullName
}

function Test-ModelFile([string]$Path) {
    if (-not (Test-Path $Path)) { return $false }
    $item = Get-Item $Path
    if ($item.Length -ne $ModelSize) { return $false }
    # Hashing 2.7 GB takes a while; remember a successful check.
    $ok = "$Path.sha256-ok"
    if ((Test-Path $ok) -and ((Get-Content $ok -Raw).Trim() -eq "$($item.Length) $($item.LastWriteTimeUtc.Ticks)")) {
        return $true
    }
    Note 'verifying model checksum'
    $hash = (Get-FileHash -Algorithm SHA256 $Path).Hash.ToLowerInvariant()
    if ($hash -ne $ModelSha256) { return $false }
    Set-Content -Path $ok -Value "$($item.Length) $($item.LastWriteTimeUtc.Ticks)"
    return $true
}

function Invoke-Fetch {
    New-Item -ItemType Directory -Force -Path $Cache | Out-Null

    Get-VerifiedDownload $TclUrl (Join-Path $Cache $TclBundle) $TclSha256 'Tcl/Tk bundle'

    $build = Resolve-LlamaBuild
    $zipName = "llama-$build-bin-win-cpu-x64.zip"
    Get-VerifiedDownload "https://github.com/$LlamaRepo/releases/download/$build/$zipName" `
        (Join-Path $Cache $zipName) $LlamaSha256[$build] "llama.cpp $build"

    Install-InnoSetup

    if ($NoModel) { Note 'model: skipped (-NoModel)'; return }
    $model = Join-Path $Cache $ModelFile
    if (Test-ModelFile $model) { Note 'model: cached and verified'; return }
    if (Test-Path $model) { Warn 'cached model is incomplete or corrupt; downloading again'; Remove-Item -Force $model }
    Say "downloading $ModelFile (2.7 GB; resumable, rerun if interrupted)"
    Get-Download $ModelUrl $model
    if (-not (Test-ModelFile $model)) {
        Remove-Item -Force $model
        Stop-WithError 'the model download does not match the published SHA-256'
    }
    Note 'model: downloaded and verified'
}

# ---------------------------------------------------------------------
# AI payload inside the build tree
# ---------------------------------------------------------------------

function Install-Payload {
    $ai = Join-Path $Tree 'ai'
    $llamaDir = Join-Path $ai 'llama'
    $modelsDir = Join-Path $ai 'models'
    New-Item -ItemType Directory -Force -Path $llamaDir, $modelsDir | Out-Null

    $zip = Get-LlamaZip
    if (-not $zip -or -not (Test-Path $zip)) {
        Warn "llama.cpp not downloaded; the assistant will say the server is missing. Run '.\rgui fetch'."
    } else {
        $marker = Join-Path $llamaDir '.llama-build'
        $want = [System.IO.Path]::GetFileName($zip)
        $have = ''
        if (Test-Path $marker) { $have = (Get-Content $marker -Raw).Trim() }
        if ($have -ne $want -or -not (Test-Path (Join-Path $llamaDir 'llama-server.exe'))) {
            Say "installing $want"
            $tmp = Join-Path $BuildRoot 'tmp-llama'
            if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
            New-Item -ItemType Directory -Force -Path $tmp | Out-Null
            & (Join-Path $env:SystemRoot 'System32\tar.exe') -xf $zip -C $tmp
            if ($LASTEXITCODE -ne 0) { Stop-WithError "unpacking $want failed" }
            $server = Get-ChildItem $tmp -Recurse -Filter 'llama-server.exe' | Select-Object -First 1
            if (-not $server) { Stop-WithError "$want contains no llama-server.exe" }
            Get-ChildItem $llamaDir -Force | Remove-Item -Recurse -Force
            Copy-Item -Path (Join-Path $server.DirectoryName '*') -Destination $llamaDir -Recurse -Force
            Set-Content -Path $marker -Value $want
            Remove-Item -Recurse -Force $tmp
        }
    }

    $src = Join-Path $Cache $ModelFile
    $dst = Join-Path $modelsDir $ModelFile
    if (-not (Test-Path $src)) {
        Warn "model not downloaded; the assistant will say the model is missing. Run '.\rgui fetch'."
        return
    }
    if ((Test-Path $dst) -and (Get-Item $dst).Length -eq (Get-Item $src).Length) { return }
    if (Test-Path $dst) { Remove-Item -Force $dst }
    try {
        # Same volume: a hard link costs no space and no time.
        New-Item -ItemType HardLink -Path $dst -Target $src | Out-Null
    } catch {
        Say 'copying the model into the build tree'
        Copy-Item $src $dst
    }
}

# ---------------------------------------------------------------------
# Build steps
# ---------------------------------------------------------------------

function Invoke-Make([string]$Targets, [string]$LogName) {
    $gw = ConvertTo-MsysPath (Join-Path $Tree 'src\gnuwin32')
    $rc = Invoke-Bash ("cd '{0}' && make -j{1} {2}" -f $gw, $Jobs, $Targets) $LogName
    if ($rc -ne 0) { Stop-WithError "make $Targets" }
}

function Invoke-Full {
    Say 'syncing sources'
    Sync-Tree
    Initialize-Tree
    Stop-DevProcesses
    # Separate makes: with -j, 'make all recommended' starts the
    # recommended packages before 'all' has created MkRules.
    Say "building R (make -j$Jobs all); the first run takes a while"
    Invoke-Make 'all' 'full-all'
    Say "building the recommended packages (make -j$Jobs recommended)"
    Invoke-Make 'recommended' 'full-recommended'
}

function Invoke-Quick {
    if (-not (Test-Built)) { Stop-WithError "no complete build yet; run '.\rgui full' once first." }
    Say 'syncing sources'
    Sync-Tree
    Stop-DevProcesses
    Say 'rebuilding core libraries and R.dll'
    # Two separate makes: with -j, goals on one command line may overlap.
    Invoke-Make 'rlibs' 'quick-rlibs'
    Invoke-Make 'rbuild' 'quick-rbuild'
}

# -NoPayload: start as is.  The GUI tests need the model to stay missing,
# and Install-Payload would quietly put it back.
function Start-DevRgui([switch]$NoPayload) {
    if (-not (Test-Built)) { Stop-WithError "no build yet; run '.\rgui full' first." }
    if (-not $NoPayload) { Install-Payload }
    foreach ($d in $DevHome, (Join-Path $DevHome 'library'), (Join-Path $DevHome 'tmp')) {
        New-Item -ItemType Directory -Force -Path $d | Out-Null
    }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $Tree 'bin\x64\Rgui.exe'
    $psi.WorkingDirectory = $DevHome
    $psi.UseShellExecute = $false
    $vars = $psi.EnvironmentVariables
    foreach ($k in 'R_HOME', 'R_LIBS', 'R_LIBS_SITE') { if ($vars.ContainsKey($k)) { $vars.Remove($k) } }
    $vars['R_USER']         = $DevHome
    $vars['HOME']           = $DevHome
    $vars['R_LIBS_USER']    = Join-Path $DevHome 'library'
    $vars['R_ENVIRON_USER'] = Join-Path $DevHome '.Renviron'
    $vars['R_PROFILE_USER'] = Join-Path $DevHome '.Rprofile'
    $vars['TMPDIR']         = Join-Path $DevHome 'tmp'
    $vars['TMP']            = $vars['TMPDIR']
    $vars['TEMP']           = $vars['TMPDIR']
    $p = [System.Diagnostics.Process]::Start($psi)
    Say ("Rgui started (pid {0}).  Misc > AI assistant, or Ctrl+T." -f $p.Id)
    Note "R_HOME  $Tree"
    Note "R_USER  $DevHome"
    return $p
}

function Invoke-GuiTests {
    if (-not (Test-Built)) { Stop-WithError "the GUI test needs a build; run '.\rgui full' first." }
    $smoke = Join-Path $Tree 'src\gnuwin32\aitests\gui_smoke.ps1'
    $probe = Join-Path $DevHome 'probe'
    Install-Payload
    $model = Join-Path $Tree "ai\models\$ModelFile"
    $hidden = "$model.hidden-for-test"

    # Pass 1: no model on disk.  The assistant must fail politely and R
    # must not notice.
    Stop-DevProcesses
    if (Test-Path $model) { Move-Item -Force $model $hidden }
    try {
        Say 'GUI test, pass 1: model missing'
        $p = Start-DevRgui -NoPayload
        & $smoke -ProcessId $p.Id -ProbeDir $probe -TreeDir $Tree
        $rc1 = $LASTEXITCODE
    } finally {
        Stop-DevProcesses
        if (Test-Path $hidden) { Move-Item -Force $hidden $model }
    }
    if ($rc1 -ne 0) { Stop-WithError "GUI test without a model ($rc1 failed checks)" }

    # Pass 1b: the first-run download, from a local server into a scratch
    # model path, through a scratch copy of Rai.conf.
    Say 'GUI test, pass 1b: first-run model download'
    Stop-DevProcesses
    $out = Join-Path $BuildRoot 'testout'
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    $conf = Join-Path $Tree 'etc\Rai.conf'
    $backup = "$conf.gui-test-backup"
    $testModel = Join-Path $Tree 'ai\models\rgui-gui-test.gguf'
    $python = (Get-Command python -ErrorAction Stop).Source
    $server = Join-Path $Tree 'src\gnuwin32\aitests\fakeserver.py'
    $port = Get-FreePort
    Push-Location $out
    try { & $python $server $port --expected } finally { Pop-Location }
    $sha = (Get-Content (Join-Path $out 'download.sha256') -Raw).Trim()
    $size = [long](Get-Content (Join-Path $out 'download.size') -Raw).Trim()
    foreach ($f in $testModel, "$testModel.part") { if (Test-Path $f) { [System.IO.File]::Delete($f) } }
    Copy-Item $conf $backup -Force
    $srv = $null
    try {
        Add-Content -Path $conf -Encoding ASCII -Value @(
            '', '## gui test overrides',
            'model = ai/models/rgui-gui-test.gguf',
            "model_url = http://127.0.0.1:$port/file/model.gguf?slow",
            "model_sha256 = $sha",
            "model_bytes = $size")
        $srv = Start-Process -FilePath $python -ArgumentList "`"$server`"", $port `
                             -WorkingDirectory $out -WindowStyle Hidden -PassThru
        if (-not (Wait-Port $port 15)) { Stop-WithError "fake server did not start on port $port" }
        $p = Start-DevRgui -NoPayload
        & $smoke -ProcessId $p.Id -ProbeDir $probe -TreeDir $Tree -ExpectDownload `
                 -DownloadedModel $testModel -DownloadBytes $size
        $rc1b = $LASTEXITCODE
    } finally {
        Stop-DevProcesses
        if ($srv) { Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue }
        Move-Item -Force $backup $conf
        foreach ($f in $testModel, "$testModel.part") { if (Test-Path $f) { [System.IO.File]::Delete($f) } }
    }
    if ($rc1b -ne 0) { Stop-WithError "GUI test of the first-run download ($rc1b failed checks)" }

    # Pass 1c: canned answers from the fake server standing in for the
    # model server, so the checks do not depend on what a 4B model writes.
    Say 'GUI test, pass 1c: canned answers (no code; To editor into an open script)'
    Stop-DevProcesses
    $port = Get-FreePort
    Copy-Item $conf $backup -Force
    $srv = $null
    try {
        Add-Content -Path $conf -Encoding ASCII -Value @(
            '', '## gui test overrides',
            "port = $port",
            'model = ai/system_prompt.txt',
            'model_url =')
        $srv = Start-Process -FilePath $python -ArgumentList "`"$server`"", $port `
                             -WorkingDirectory $out -WindowStyle Hidden -PassThru
        if (-not (Wait-Port $port 15)) { Stop-WithError "fake server did not start on port $port" }
        $p = Start-DevRgui -NoPayload
        & $smoke -ProcessId $p.Id -ProbeDir $probe -TreeDir $Tree -Canned
        $rc1c = $LASTEXITCODE
    } finally {
        Stop-DevProcesses
        if ($srv) { Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue }
        Move-Item -Force $backup $conf
    }
    if ($rc1c -ne 0) { Stop-WithError "GUI test with canned answers ($rc1c failed checks)" }

    if (-not (Test-Path $model)) { Warn 'no model downloaded: skipping GUI pass 2'; return }
    Say 'GUI test, pass 2: real model'
    $p = Start-DevRgui
    try {
        & $smoke -ProcessId $p.Id -ProbeDir $probe -TreeDir $Tree -WithModel
        $rc2 = $LASTEXITCODE
    } finally { Stop-DevProcesses }
    if ($rc2 -ne 0) { Stop-WithError "GUI test with the model ($rc2 failed checks)" }
}

# ---------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------

function Get-FreePort {
    foreach ($port in 45712, 45713, 47811, 49231, 52341, 53117) {
        $l = $null
        try {
            $l = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $port)
            $l.Start()
            return $port
        } catch { } finally { if ($l) { $l.Stop() } }
    }
    Stop-WithError 'no free loopback port found for the test server'
}

function Wait-Port([int]$Port, [int]$Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        $c = New-Object System.Net.Sockets.TcpClient
        try { $c.Connect('127.0.0.1', $Port); return $true } catch { Start-Sleep -Milliseconds 200 }
        finally { $c.Close() }
    }
    return $false
}

function Invoke-Test {
    Say 'syncing sources'
    Sync-Tree

    # Headers that 'make fixfiles' would normally put in src/include.
    $inc = Join-Path $BuildRoot 'testinc'
    $out = Join-Path $BuildRoot 'testout'
    New-Item -ItemType Directory -Force -Path $inc, $out | Out-Null
    Copy-Item (Join-Path $Tree 'src\gnuwin32\fixed\h\*.h') $inc -Force
    Set-Content -Path (Join-Path $inc 'libintl.h') -Encoding ASCII -Value @(
        '#ifndef RGUI_TEST_LIBINTL_H', '#define RGUI_TEST_LIBINTL_H',
        'char *libintl_gettext(const char *);',
        'char *libintl_dgettext(const char *, const char *);',
        'char *libintl_ngettext(const char *, const char *, unsigned long);',
        '#endif')

    $t = ConvertTo-MsysPath $Tree
    $i = ConvertTo-MsysPath $inc
    $o = ConvertTo-MsysPath $out
    $cflags = "-std=gnu2x -I$i -I$t/src/include -I$t/src/gnuwin32 -I$t/src/extra " +
              '-DHAVE_CONFIG_H -DR_DLL_BUILD -DWin32'

    Say 'static checks (production flags; warnings in aichat.c are errors)'
    $sh = "cd $t && sh tools/GETVERSION > $i/Rversion.h && " +
              "cd src/gnuwin32 && " +
              "gcc -fsyntax-only -O3 -Wall -pedantic -Werror $cflags aichat.c && " +
              "gcc -fsyntax-only -O3 -Wall -pedantic $cflags rui.c && " +
              "gcc -fsyntax-only -O3 -Wall -pedantic $cflags system.c && echo static-checks-ok"
    if ((Invoke-Bash $sh 'test-static') -ne 0) { Stop-WithError 'static checks' }

    Say 'building the test harness'
    $sh = "cd $t/src/gnuwin32/aitests && " +
              "gcc -O1 -g -I. -I.. $cflags test_aichat.c -o $o/test_aichat.exe -lws2_32 -lwinhttp -lbcrypt"
    if ((Invoke-Bash $sh 'test-build') -ne 0) { Stop-WithError 'test harness build' }
    $exe = Join-Path $out 'test_aichat.exe'

    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not $python) {
        Warn 'python not found: running unit tests only, not the protocol tests'
        & $exe
        if ($LASTEXITCODE -ne 0) { Stop-WithError 'unit tests' }
    } else {
        $aitests = Join-Path $Tree 'src\gnuwin32\aitests'
        $server = Join-Path $aitests 'fakeserver.py'
        # First the synthetic stream, built to hit every framing edge case;
        # then each captured stream from a real llama-server, replayed.
        $runs = @(@{ name = 'synthetic stream'; extra = @() })
        foreach ($cap in @(Get-ChildItem $aitests -Filter '*.sse' -ErrorAction SilentlyContinue)) {
            $runs += @{ name = "replay of $($cap.Name)"; extra = @('--replay', "`"$($cap.FullName)`"") }
        }
        foreach ($run in $runs) {
            $port = Get-FreePort
            Push-Location $out
            try {
                & $python.Source $server $port --expected @($run.extra)
                $srv = Start-Process -FilePath $python.Source `
                                     -ArgumentList (@("`"$server`"", $port) + $run.extra) `
                                     -WorkingDirectory $out -WindowStyle Hidden -PassThru
                try {
                    if (-not (Wait-Port $port 15)) { Stop-WithError "fake server did not start on port $port" }
                    Say ("unit and protocol tests, {0} (port {1})" -f $run.name, $port)
                    & $exe $port 'expected.txt'
                    $rc = $LASTEXITCODE
                } finally {
                    Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue
                }
            } finally { Pop-Location }
            if ($rc -ne 0) { Stop-WithError ("tests against the {0}" -f $run.name) }
        }
    }

    if ($Real) {
        Install-Payload
        $serverExe = Join-Path $Tree 'ai\llama\llama-server.exe'
        $model = Join-Path $Tree "ai\models\$ModelFile"
        if (-not (Test-Path $serverExe) -or -not (Test-Path $model)) {
            Stop-WithError "the real-model test needs llama.cpp and the model: run '.\rgui fetch'."
        }
        $port = Get-FreePort
        Say "real model test: starts llama-server the way Rgui does (port $port)"
        Note 'loading 2.7 GB takes a minute or two'
        & $exe --real $serverExe $model $port
        if ($LASTEXITCODE -ne 0) { Stop-WithError 'real model test' }
    }

    if ($Gui) { Invoke-GuiTests }
    if ($Installer) { Invoke-InstallerTests }
}

# Install the newest setup.exe for the current user into a folder whose
# name has a space in it -- like the default, "...\Programs\RGui AI" -- and
# check what a user would notice.  Leaves nothing behind: the last step is
# the uninstaller.
function Invoke-InstallerTests {
    $setup = Get-ChildItem (Join-Path $BuildRoot 'installer') -Filter 'RGui-AI-*-setup.exe' -ErrorAction SilentlyContinue |
             Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $setup) { Stop-WithError "no installer built yet; run '.\rgui installer' first." }
    $app = Join-Path $BuildRoot 'itest\RGui AI'
    $key = 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{A1E92B8A-2AFD-45A8-BAF7-C1803C90FB17}_is1'
    $lnk = Join-Path ([Environment]::GetFolderPath('Programs')) 'RGui AI.lnk'
    $script:ItFail = 0
    function It([string]$What, [bool]$Ok, [string]$Detail = '') {
        Write-Host ("{0,-60} {1}" -f $What, $(if ($Ok) { 'ok' } else { 'FAIL' })) `
                   -ForegroundColor $(if ($Ok) { 'Gray' } else { 'Red' })
        if (-not $Ok) { $script:ItFail++; if ($Detail) { Write-Host "      $Detail" -ForegroundColor Yellow } }
    }
    function Install-Setup {
        $p = Start-Process -FilePath $setup.FullName -Wait -PassThru -ArgumentList `
             '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/DIR=`"$app`""
        return $p.ExitCode
    }
    function Stop-Installed {
        Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -like "$app\*" } |
            ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Milliseconds 800
    }

    Say "installer test: $($setup.Name) into '$app'"
    Stop-Installed
    if (Test-Path (Join-Path $app 'unins000.exe')) {
        Start-Process -FilePath (Join-Path $app 'unins000.exe') -Wait -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES'
    }

    It 'silent install succeeds' ((Install-Setup) -eq 0)
    foreach ($rel in 'Start-R.cmd', 'R\bin\x64\Rgui.exe', 'R\etc\Rai.conf', 'R\ai\system_prompt.txt',
                     'R\ai\llama\llama-server.exe', 'R\ai\models', 'R\library\MASS', 'R\Tcl\bin') {
        It ("installed: $rel") (Test-Path (Join-Path $app $rel))
    }
    It 'no model inside the installer' (@(Get-ChildItem (Join-Path $app 'R\ai\models') -Filter '*.gguf').Count -eq 0)
    It 'no source tree or build scripts installed' `
       (-not (Test-Path (Join-Path $app 'R\src')) -and -not (Test-Path (Join-Path $app 'R\rgui.ps1')))
    It 'Start menu shortcut created' (Test-Path $lnk)
    It 'per-user uninstall entry registered' (Test-Path $key)

    # The launcher, from a path with a space: R used to die at start with
    # "'R_TempDir' contains space" here.
    Start-Process -FilePath (Join-Path $app 'Start-R.cmd') -WorkingDirectory $app -WindowStyle Minimized
    $proc = $null
    for ($i = 0; $i -lt 40 -and -not $proc; $i++) {
        Start-Sleep -Milliseconds 500
        $proc = Get-CimInstance Win32_Process -Filter "Name='Rgui.exe'" |
                Where-Object { $_.ExecutablePath -like "$app\*" } | Select-Object -First 1
    }
    It 'Start-R.cmd starts Rgui from the install folder' ($null -ne $proc)
    Start-Sleep -Seconds 10
    It 'Rgui is still running 10 s later (R started up)' `
       ($proc -and [bool](Get-Process -Id $proc.ProcessId -ErrorAction SilentlyContinue))
    Stop-Installed

    # Upgrade: the files people edit must survive.
    Add-Content -Path (Join-Path $app 'R\etc\Rai.conf') -Value '# my own setting'
    Set-Content -Path (Join-Path $app 'R\ai\system_prompt.txt') -Value 'my custom prompt'
    Set-Content -Path (Join-Path $app 'R\ai\context\05-anova.md') -Value '# my notes'
    It 'reinstalling over it succeeds' ((Install-Setup) -eq 0)
    It 'upgrade keeps an edited Rai.conf' ((Get-Content (Join-Path $app 'R\etc\Rai.conf') -Raw) -match 'my own setting')
    It 'upgrade keeps an edited prompt' `
       ((Get-Content (Join-Path $app 'R\ai\system_prompt.txt') -Raw).Trim() -eq 'my custom prompt')
    It 'upgrade keeps course notes' (Test-Path (Join-Path $app 'R\ai\context\05-anova.md'))

    # Uninstall: the program and the downloaded model go, the work folder stays.
    Set-Content -Path (Join-Path $app 'R\ai\models\stand-in.gguf') -Value 'model'
    New-Item -ItemType Directory -Force -Path (Join-Path $app 'work') | Out-Null
    Set-Content -Path (Join-Path $app 'work\my-analysis.R') -Value 'x <- 1'
    $p = Start-Process -FilePath (Join-Path $app 'unins000.exe') -Wait -PassThru -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES'
    Start-Sleep -Seconds 2
    It 'silent uninstall succeeds' ($p.ExitCode -eq 0)
    It 'program files removed' (-not (Test-Path (Join-Path $app 'R\bin\x64\Rgui.exe')))
    It 'downloaded model removed' (-not (Test-Path (Join-Path $app 'R\ai\models\stand-in.gguf')))
    It 'work folder kept' (Test-Path (Join-Path $app 'work\my-analysis.R'))
    It 'Start menu shortcut removed' (-not (Test-Path $lnk))
    It 'uninstall entry removed' (-not (Test-Path $key))

    if ($script:ItFail -ne 0) { Stop-WithError "installer test ($script:ItFail failed checks)" }
    Write-Host 'INSTALLER: PASSED' -ForegroundColor Green
}

# ---------------------------------------------------------------------
# Packaging
# ---------------------------------------------------------------------

# What a user needs at run time, into <Root>\R, and Start-R.cmd into
# <Root>.  The source tree and build scaffolding stay behind.  Used by both
# 'package' and 'installer', so the two cannot drift apart.
# -WithoutModel leaves the .gguf out but keeps ai\models, which the
# first-run download writes into.
function Copy-Runtime([string]$Root, [switch]$WithoutModel) {
    $dstR = Join-Path $Root 'R'
    New-Item -ItemType Directory -Force -Path $dstR | Out-Null
    $xd = @('src', 'tests', 'tools', 'm4', 'po', 'packaging', '.github') |
          ForEach-Object { Join-Path $Tree $_ }
    # build-only files at the top of the tree; the installer's imagedir
    # target leaves these out as well
    $top = @('Makeconf', 'INSTALL', 'rgui.ps1', 'rgui.cmd', '.gitattributes', '.gitignore')
    $xf = @('configure', 'configure.ac', 'config.site', 'Makefile.in', 'Makefile.fw',
            'Makeconf.in', 'SVN-REVISION.bak', '*.sha256-ok', '*.gguf.part', '*.hidden-for-test') +
          @($top | ForEach-Object { Join-Path $Tree $_ })
    if ($WithoutModel) { $xf += '*.gguf' }
    & robocopy.exe $Tree $dstR /MIR /XD @xd /XF @xf /NFL /NDL /NJH /NJS /NP /R:1 /W:1 /MT:16 | Out-Null
    if ($LASTEXITCODE -ge 8) { Stop-WithError "robocopy into $dstR failed (code $LASTEXITCODE)" }
    # /MIR ignores excluded names on both sides, so it never removes one
    # an earlier copy left behind; do that here.
    foreach ($f in $top) {
        $old = Join-Path $dstR $f
        if (Test-Path $old) { [System.IO.File]::Delete($old) }
    }
    if ($WithoutModel) {
        Get-ChildItem (Join-Path $dstR 'ai\models') -Filter '*.gguf' -ErrorAction SilentlyContinue |
            ForEach-Object { [System.IO.File]::Delete($_.FullName) }
    }
    Copy-Item (Join-Path $Tree 'ai\Start-R.cmd') (Join-Path $Root 'Start-R.cmd') -Force
}

function Invoke-Package {
    if (-not (Test-Built)) { Stop-WithError "no build yet; run '.\rgui full' first." }
    Install-Payload
    Say "assembling $Dist"
    Copy-Runtime $Dist -WithoutModel:$NoModel
    $bytes = (Get-ChildItem $Dist -Recurse -File | Measure-Object -Sum Length).Sum
    Note ('size {0:N1} GB' -f ($bytes / 1GB))
    Note "copy the contents of $Dist to the root of the stick, or use '.\rgui deploy -Drive E:'"
}

# 0.1.1 / 0.2.0-rc1, from -Version, else from a v-tag on HEAD.
function Resolve-Version {
    $v = $Version
    if (-not $v) {
        $tag = (& git -C $Repo describe --tags --exact-match 2>$null)
        if ($LASTEXITCODE -eq 0 -and $tag) { $v = "$tag".Trim() } else { $v = '0.0.0-dev' }
    }
    $v = $v -replace '^v', ''
    if ($v -notmatch '^(\d+)\.(\d+)\.(\d+)(-[0-9A-Za-z.]+)?$') {
        Stop-WithError "version '$v' is not like 0.1.1 or 0.2.0-rc1"
    }
    return @{ Text = $v; Numeric = ('{0}.{1}.{2}.0' -f $Matches[1], $Matches[2], $Matches[3]) }
}

function Invoke-Installer {
    if (-not (Test-Built)) { Stop-WithError "no build yet; run '.\rgui full' first." }
    $iscc = Find-InnoSetup
    if (-not $iscc) { Stop-WithError "Inno Setup not found; run '.\rgui fetch' (it installs it per user)" }
    $ver = Resolve-Version
    Install-Payload
    $stage = Join-Path $BuildRoot 'stage'
    Say "staging the installer contents (without the model) in $stage"
    Copy-Runtime $stage -WithoutModel
    $outDir = Join-Path $BuildRoot 'installer'
    New-Item -ItemType Directory -Force -Path $outDir, $Logs | Out-Null
    $log = Join-Path $Logs ('installer-{0}.log' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
    $script:LastLog = $log
    Say ("compiling the installer, version {0}" -f $ver.Text)
    $iss = Join-Path $Repo 'packaging\rgui-ai.iss'
    $p = Start-Process -FilePath $iscc -Wait -PassThru -NoNewWindow `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err" -ArgumentList @(
            ('"/DAppVersion={0}"' -f $ver.Text), ('"/DNumericVersion={0}"' -f $ver.Numeric),
            ('"/DStageDir={0}"' -f $stage), ('"/DOutputDir={0}"' -f $outDir), ('"{0}"' -f $iss))
    if (Test-Path "$log.err") { Get-Content "$log.err" | Add-Content $log; Remove-Item "$log.err" }
    if ($p.ExitCode -ne 0) { Stop-WithError "Inno Setup compiler (exit code $($p.ExitCode))" }
    $exe = Join-Path $outDir ('RGui-AI-{0}-setup.exe' -f $ver.Text)
    if (-not (Test-Path $exe)) { Stop-WithError "the compiler reported success but $exe is missing" }
    $script:InstallerExe = $exe
    $sha = (Get-FileHash -Algorithm SHA256 $exe).Hash.ToLowerInvariant()
    # One LF-terminated line, as sha256sum writes it: with CRLF, 'sha256sum -c'
    # takes the CR for part of the file name and reports the file missing.
    [System.IO.File]::WriteAllText("$exe.sha256", ("{0}  {1}`n" -f $sha, (Split-Path $exe -Leaf)))
    Note ('{0}  {1:N0} MB' -f $exe, ((Get-Item $exe).Length / 1MB))
    Note "sha256 $sha"
}

function Invoke-Deploy {
    if (-not $Drive) { Stop-WithError 'deploy needs -Drive, e.g. -Drive E:' }
    $root = $Drive.TrimEnd('\', ':') + ':\'
    if (-not (Test-Path $root)) { Stop-WithError "drive $root not found" }
    $sysRoot = [System.IO.Path]::GetPathRoot($env:SystemRoot)
    if ($root -ieq $sysRoot) { Stop-WithError "refusing to deploy to the system drive $root" }
    if (-not (Test-Path (Join-Path $Dist 'R\bin\x64\Rgui.exe'))) {
        Stop-WithError "nothing to deploy; run '.\rgui package' first."
    }

    $target = Join-Path $root 'R'
    $marker = Join-Path $target '.rgui-deploy'
    if ((Test-Path $target) -and -not (Test-Path $marker) -and -not $Force) {
        Stop-WithError "$target exists and was not created by this script; use -Force to replace it."
    }

    # Mirror the program, but never delete or overwrite what the user keeps
    # on the stick: course material, the prompt and the settings.
    $srcR = Join-Path $Dist 'R'
    Say "deploying to $target"
    & robocopy.exe $srcR $target /MIR `
        /XD (Join-Path $srcR 'ai\context') (Join-Path $target 'ai\context') `
        /XF 'Rai.conf' 'system_prompt.txt' /NFL /NDL /NJH /NJS /R:1 /W:1 | Out-Host
    if ($LASTEXITCODE -ge 8) { Stop-WithError "robocopy to $target failed (code $LASTEXITCODE)" }
    foreach ($rel in 'etc\Rai.conf', 'ai\system_prompt.txt') {
        $s = Join-Path $srcR $rel
        $d = Join-Path $target $rel
        if (-not (Test-Path $d)) { Copy-Item $s $d }
        elseif ((Get-FileHash $s).Hash -ne (Get-FileHash $d).Hash) {
            Note "kept your $rel on the stick (the build has a different default)"
        }
    }
    & robocopy.exe (Join-Path $srcR 'ai\context') (Join-Path $target 'ai\context') /E /XC /XN /XO `
        /NFL /NDL /NJH /NJS /R:1 /W:1 | Out-Null
    Copy-Item (Join-Path $Dist 'Start-R.cmd') (Join-Path $root 'Start-R.cmd') -Force
    Set-Content -Path $marker -Value (Get-Date -Format 's')
    Say "done: run ${root}Start-R.cmd"
}

function Invoke-Clean {
    Stop-DevProcesses
    foreach ($d in $Tree, (Join-Path $BuildRoot 'testout'), (Join-Path $BuildRoot 'testinc')) {
        if (Test-Path $d) { Say "removing $d"; Remove-Item -Recurse -Force $d }
    }
    Note "downloads kept in $Cache"
}

# ---------------------------------------------------------------------
# doctor
# ---------------------------------------------------------------------

function Invoke-Doctor {
    Say 'toolchain'
    Note "Rtools  $script:RtoolsDir"
    $gcc = Join-Path $script:RtoolsDir 'x86_64-w64-mingw32.static.posix\bin\gcc.exe'
    Note ('gcc     ' + (& $gcc --version | Select-Object -First 1))
    Note "jobs    $Jobs"
    $py = Get-Command python -ErrorAction SilentlyContinue
    if ($py) { Note "python  $($py.Source)" } else { Warn 'python not found (only needed by test)' }

    $iscc = Find-InnoSetup
    if ($iscc) { Note "inno    $iscc" } else { Note "inno    missing ('rgui fetch' installs it per user)" }

    Say 'disk'
    $drv = Get-PSDrive -Name $BuildRoot.Substring(0, 1)
    Note ('{0}: {1:N0} GB free' -f $drv.Name, ($drv.Free / 1GB))
    if ($drv.Free -lt 15GB) { Warn 'a full build plus the model wants about 15 GB' }

    Say 'downloads'
    $items = @(
        @{ n = 'Tcl/Tk bundle'; p = (Join-Path $Cache $TclBundle) },
        @{ n = 'model';         p = (Join-Path $Cache $ModelFile) })
    foreach ($it in $items) {
        if (Test-Path $it.p) { Note ("{0,-14} ok" -f $it.n) } else { Note ("{0,-14} missing" -f $it.n) }
    }
    $z = Get-LlamaZip
    if ($z -and (Test-Path $z)) { Note ("{0,-14} {1}" -f 'llama.cpp', [System.IO.Path]::GetFileName($z)) }
    else { Note ("{0,-14} missing" -f 'llama.cpp') }

    Say 'build'
    Note "tree    $Tree"
    if (Test-Built) {
        $dll = Get-Item (Join-Path $Tree 'bin\x64\R.dll')
        Note ('R.dll   built {0:yyyy-MM-dd HH:mm}' -f $dll.LastWriteTime)
    } else {
        Note "not built yet: '.\rgui fetch' then '.\rgui full'"
    }
}

# ---------------------------------------------------------------------

# Where each command left its results; printed at the end of every run.
function Show-Output([string]$Cmd) {
    $rgui = Join-Path $Tree 'bin\x64\Rgui.exe'
    $lines = switch ($Cmd) {
        { $_ -in 'full', 'quick', 'dev', 'run' } { @("Rgui.exe    $rgui") }
        'package'   { @("stick       $Dist   (copy its contents to the stick's root)") }
        'installer' { @("installer   $script:InstallerExe", "checksum    $script:InstallerExe.sha256") }
        'deploy'    { @("stick       " + $Drive.TrimEnd('\', ':') + ':\') }
        'fetch'     { @("downloads   $Cache") }
        'test'      { @("logs        $Logs") }
        'doctor'    { @("build root  $BuildRoot",
                        "Rgui.exe    $rgui",
                        "stick       $Dist",
                        "installer   $(Join-Path $BuildRoot 'installer')",
                        "downloads   $Cache",
                        "logs        $Logs") }
        default     { @() }
    }
    if (@($lines).Count -gt 0) {
        Write-Host 'output:' -ForegroundColor Green
        foreach ($l in $lines) { Write-Host "  $l" -ForegroundColor Green }
    }
}

Initialize-Toolchain
$script:InstallerExe = $null
$watch = [System.Diagnostics.Stopwatch]::StartNew()
switch ($Command) {
    'doctor'  { Invoke-Doctor }
    'fetch'   { Invoke-Fetch }
    'full'    { Invoke-Full }
    'quick'   { Invoke-Quick }
    'run'     { $null = Start-DevRgui }
    'dev'     { Invoke-Quick; $null = Start-DevRgui }
    'test'    { Invoke-Test }
    'package' { Invoke-Package }
    'installer' { Invoke-Installer }
    'deploy'  { Invoke-Deploy }
    'clean'   { Invoke-Clean }
}
Show-Output $Command
Write-Host ("done: {0} in {1}" -f $Command, (Format-Elapsed $watch)) -ForegroundColor Green
