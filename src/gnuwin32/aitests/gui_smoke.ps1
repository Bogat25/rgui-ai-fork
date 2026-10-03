<#
.SYNOPSIS
    Drive a running Rgui.exe through Win32 messages and check the AI panel.

.DESCRIPTION
    Called by 'rgui test -Gui' with the pid of an Rgui it has just
    started.  Nothing is clicked with the mouse: menu commands, button
    clicks, text and keystrokes are all sent as window messages, so the
    test runs the same on any screen and needs no focus.

    Exit code: the number of failed checks.
#>
param(
    [Parameter(Mandatory = $true)] [int]$ProcessId,
    [Parameter(Mandatory = $true)] [string]$ProbeDir,
    [Parameter(Mandatory = $true)] [string]$TreeDir,
    [switch]$WithModel,
    [int]$AnswerTimeout = 600,

    # First-run download pass: answer the offer with Yes and expect the
    # model to arrive at DownloadedModel with DownloadBytes bytes.
    [switch]$ExpectDownload,

    # Canned-answer pass: the "model server" is the fake server, which
    # answers prose for NOCODE questions and code otherwise.
    [switch]$Canned,
    [string]$DownloadedModel = '',
    [string]$DownloadedVision = '',
    [long]$DownloadBytes = 0,

    # Where the fake server saves the pictures it receives (canned pass).
    [string]$ServerDir = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class Win {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr h);
    [DllImport("user32.dll")] static extern int GetMenuItemCount(IntPtr m);
    [DllImport("user32.dll")] static extern IntPtr GetSubMenu(IntPtr m, int pos);
    [DllImport("user32.dll")] static extern uint GetMenuItemID(IntPtr m, int pos);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetMenuStringW(IntPtr m, uint item, StringBuilder s, int max, uint flags);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetClassNameW(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] static extern int GetWindowLongW(IntPtr h, int idx);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")]
    static extern IntPtr SmtoInt(IntPtr h, uint m, IntPtr w, IntPtr l, uint f, uint t, out IntPtr r);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)]
    static extern IntPtr SmtoStr(IntPtr h, uint m, IntPtr w, string l, uint f, uint t, out IntPtr r);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)]
    static extern IntPtr SmtoBuf(IntPtr h, uint m, IntPtr w, StringBuilder l, uint f, uint t, out IntPtr r);

    const uint SMTO_ABORTIFHUNG = 2;

    public static List<IntPtr> TopWindows(int pid) {
        var list = new List<IntPtr>();
        EnumWindows(delegate (IntPtr h, IntPtr l) {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p == (uint) pid) list.Add(h);
            return true;
        }, IntPtr.Zero);
        return list;
    }

    public static List<IntPtr> Descendants(IntPtr parent) {
        var list = new List<IntPtr>();
        EnumChildWindows(parent, delegate (IntPtr h, IntPtr l) { list.Add(h); return true; }, IntPtr.Zero);
        return list;
    }

    public static string ClassOf(IntPtr h) {
        var sb = new StringBuilder(256); GetClassNameW(h, sb, 256); return sb.ToString();
    }

    public static int Style(IntPtr h) { return GetWindowLongW(h, -16); }

    public static string Text(IntPtr h) {
        IntPtr r;
        if (SmtoInt(h, 0x000E, IntPtr.Zero, IntPtr.Zero, SMTO_ABORTIFHUNG, 5000, out r) == IntPtr.Zero)
            return null;
        int n = (int) r;
        var sb = new StringBuilder(n + 2);
        if (SmtoBuf(h, 0x000D, (IntPtr) (n + 1), sb, SMTO_ABORTIFHUNG, 5000, out r) == IntPtr.Zero)
            return null;
        return sb.ToString();
    }

    public static bool SetText(IntPtr h, string s) {
        IntPtr r; return SmtoStr(h, 0x000C, IntPtr.Zero, s, SMTO_ABORTIFHUNG, 5000, out r) != IntPtr.Zero;
    }

    // BM_CLICK is posted rather than sent: the button's handler runs on
    // R's GUI thread, and waiting for it here would add nothing.
    public static bool Click(IntPtr h) { return PostMessageW(h, 0x00F5, IntPtr.Zero, IntPtr.Zero); }

    // Does the window's thread answer within ms?  WM_NULL does nothing.
    public static bool Responds(IntPtr h, uint ms) {
        IntPtr r; return SmtoInt(h, 0, IntPtr.Zero, IntPtr.Zero, SMTO_ABORTIFHUNG, ms, out r) != IntPtr.Zero;
    }

    // The chat boxes are Unicode windows (the panel puts a W procedure on
    // top of GraphApp's), so EM_SETSEL / EM_GETSEL go through the W entry
    // point and the positions arrive untranslated.
    public static void SetSel(IntPtr h, int a, int b) {
        IntPtr r; SmtoInt(h, 0x00B1, (IntPtr) a, (IntPtr) b, SMTO_ABORTIFHUNG, 5000, out r);
    }
    public static int[] GetSel(IntPtr h) {
        IntPtr r; SmtoInt(h, 0x00B0, IntPtr.Zero, IntPtr.Zero, SMTO_ABORTIFHUNG, 5000, out r);
        long v = (long) r;
        return new int[] { (int) (v & 0xFFFF), (int) ((v >> 16) & 0xFFFF) };
    }

    public static void TypeChars(IntPtr h, string s) {
        foreach (char c in s) PostMessageW(h, 0x0102, (IntPtr) c, IntPtr.Zero);   // WM_CHAR
    }

    // The task-modal MessageBox that askyesno() shows: class #32770.
    public static IntPtr FindDialog(int pid) {
        foreach (IntPtr h in TopWindows(pid))
            if (IsWindowVisible(h) && ClassOf(h) == "#32770") return h;
        return IntPtr.Zero;
    }
    public static string DialogText(IntPtr dlg) {
        var sb = new StringBuilder();
        foreach (IntPtr h in Descendants(dlg))
            if (ClassOf(h) == "Static") sb.Append(Text(h)).Append(' ');
        return sb.ToString();
    }
    public static bool ClickButton(IntPtr dlg, string label) {
        foreach (IntPtr h in Descendants(dlg))
            if (ClassOf(h) == "Button" && (Text(h) ?? "").Replace("&", "") == label)
                return Click(h);
        return false;
    }

    static string MenuText(IntPtr m, int pos) {
        var sb = new StringBuilder(256);
        GetMenuStringW(m, (uint) pos, sb, 256, 0x400);   // MF_BYPOSITION
        return sb.ToString().Replace("&", "");
    }

    public static List<string> MenuNames(IntPtr bar) {
        var l = new List<string>();
        int n = GetMenuItemCount(bar);
        for (int i = 0; i < n; i++) l.Add(MenuText(bar, i));
        return l;
    }
    // The full label, shortcut included, of the command with this label.
    public static string FullLabel(IntPtr bar, string label) {
        int n = GetMenuItemCount(bar);
        for (int i = 0; i < n; i++) {
            IntPtr sub = GetSubMenu(bar, i);
            if (sub == IntPtr.Zero) continue;
            int k = GetMenuItemCount(sub);
            for (int j = 0; j < k; j++) {
                string t = MenuText(sub, j);
                int tab = t.IndexOf('\t');
                if ((tab >= 0 ? t.Substring(0, tab) : t).Trim() == label) return t;
            }
        }
        return null;
    }

    // Find a command by its label (the part before any tab); returns its
    // id, or -1.  topName receives the name of the top-level menu.
    public static int FindCommand(IntPtr bar, string label, out string topName) {
        topName = null;
        int n = GetMenuItemCount(bar);
        for (int i = 0; i < n; i++) {
            IntPtr sub = GetSubMenu(bar, i);
            if (sub == IntPtr.Zero) continue;
            int k = GetMenuItemCount(sub);
            for (int j = 0; j < k; j++) {
                string t = MenuText(sub, j);
                int tab = t.IndexOf('\t');
                if (tab >= 0) t = t.Substring(0, tab);
                if (t.Trim() == label) {
                    topName = MenuText(bar, i);
                    return (int) GetMenuItemID(sub, j);
                }
            }
        }
        return -1;
    }
}
'@

$script:Failures = 0
function Check([string]$What, [bool]$Ok, [string]$Detail = '') {
    $mark = if ($Ok) { 'ok' } else { 'FAIL' }
    Write-Host ("{0,-60} {1}" -f $What, $mark) -ForegroundColor $(if ($Ok) { 'Gray' } else { 'Red' })
    if (-not $Ok) {
        $script:Failures++
        if ($Detail) { Write-Host "      $Detail" -ForegroundColor Yellow }
    }
}

# The clipboard is shared: any process holding it open for a moment
# (RGui, a clipboard manager) makes OpenClipboard fail.  Retry briefly.
function Set-Clip([string]$Text) {
    for ($i = 0; $i -lt 40; $i++) {
        try { Set-Clipboard -Value $Text; return $true } catch { Start-Sleep -Milliseconds 100 }
    }
    return $false
}
function Get-Clip {
    for ($i = 0; $i -lt 40; $i++) {
        try { return [string](Get-Clipboard -Raw) } catch { Start-Sleep -Milliseconds 100 }
    }
    return $null
}

function Wait-Until([scriptblock]$Condition, [double]$Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        if (& $Condition) { return $true }
        Start-Sleep -Milliseconds 200
    }
    return [bool](& $Condition)
}

function Wait-Dialog([double]$Seconds) {
    $script:dlg = [IntPtr]::Zero
    $null = Wait-Until { $script:dlg = [Win]::FindDialog($ProcessId); $script:dlg -ne [IntPtr]::Zero } $Seconds
    return $script:dlg
}

function Answer-Dialog([IntPtr]$Dialog, [string]$Button) {
    [void][Win]::ClickButton($Dialog, $Button)
    return (Wait-Until { -not [Win]::IsWindowVisible($Dialog) } 5)
}

function Post-Command([int]$Id) {
    [void][Win]::PostMessageW($frame, 0x0111, [IntPtr]$Id, [IntPtr]::Zero)
}

# Script windows titled like this; the status line may quote the same
# title, so only MDI documents count.
function Count-Scripts([IntPtr]$Parent, [string]$Needle) {
    $n = 0
    foreach ($h in [Win]::Descendants($Parent)) {
        if ([Win]::ClassOf($h) -ne 'Rgui Document') { continue }
        $t = [Win]::Text($h)
        if ($t -and $t.Contains($Needle)) { $n++ }
    }
    return $n
}

function Find-ByText([IntPtr]$Parent, [string]$Needle) {
    foreach ($h in [Win]::Descendants($Parent)) {
        $t = [Win]::Text($h)
        if ($t -and $t.Contains($Needle)) { return $h }
    }
    return [IntPtr]::Zero
}

# Enter an R expression at the console prompt and wait for its side
# effect.  It goes in through the console's own Edit > Paste command, as
# if the user pressed Ctrl+V: posted keystrokes do not work, because a
# Unicode GraphApp window builds characters from R's own keyboard state.
$script:ProbeSeq = 0
$script:PasteCmd = -1
function Test-ConsoleRuns([IntPtr]$Console, [double]$Seconds) {
    if ($script:PasteCmd -lt 0) { return $false }
    $script:ProbeSeq++
    $file = Join-Path $ProbeDir ("probe{0}.txt" -f $script:ProbeSeq)
    if (Test-Path $file) { Remove-Item $file -Force }
    $rpath = $file -replace '\\', '/'
    $cmd = "writeLines('ok', '$rpath')`n"
    # Windows' clipboard history service opens the clipboard for a moment
    # after each change, and RGui's console Paste tries only once: give it
    # a moment, and if R shows no sign of the input, paste again.  The
    # check still needs R to run the code.
    for ($attempt = 0; $attempt -lt 3; $attempt++) {
        if (-not (Set-Clip $cmd)) { return $false }
        Start-Sleep -Milliseconds 400
        [void][Win]::PostMessageW($frame, 0x0111, [IntPtr]$script:PasteCmd, [IntPtr]::Zero)
        if (Wait-Until { Test-Path $file } ($Seconds / 3)) { return $true }
    }
    return (Test-Path $file)
}

# ---------------------------------------------------------------------

New-Item -ItemType Directory -Force -Path $ProbeDir | Out-Null

# The Copy code check writes to the clipboard; put the user's text back.
$savedClipboard = Get-Clip

# Several checks go through the clipboard (console input, Copy code).  If
# Windows refuses clipboard access to every program, which happens on a
# locked session and elsewhere, say so up front: otherwise those checks
# fail looking like RGui bugs.
$clipUsable = (Set-Clip 'rgui-gui-test') -and ((Get-Clip) -eq 'rgui-gui-test')
Check 'Windows clipboard is usable (needed by several checks)' $clipUsable `
      'even clip.exe is refused: unlock the session or close whatever holds the clipboard, then rerun'

$frame = [IntPtr]::Zero
$found = Wait-Until {
    foreach ($h in [Win]::TopWindows($ProcessId)) {
        if ([Win]::IsWindowVisible($h) -and [Win]::GetMenu($h) -ne [IntPtr]::Zero) {
            $script:frame = $h; return $true
        }
    }
    return $false
} 60
$frame = $script:frame
Check 'Rgui main window with a menu bar appeared' $found
if (-not $found) { exit 99 }

$console = [IntPtr]::Zero
$null = Wait-Until { $script:console = Find-ByText $frame 'R Console'; $script:console -ne [IntPtr]::Zero } 20
$console = $script:console
Check 'R Console window found' ($console -ne [IntPtr]::Zero)
# Let R finish starting up before giving it input.
$null = Wait-Until { [Win]::Responds($frame, 500) } 20
Start-Sleep -Seconds 5
$pasteTop = $null
$script:PasteCmd = [Win]::FindCommand([Win]::GetMenu($frame), 'Paste', [ref]$pasteTop)
Check 'console Edit > Paste command found' ($script:PasteCmd -ge 0)
Check 'console runs R code typed at it (baseline)' (Test-ConsoleRuns $console 30) `
      'pasting into the console did not work, so the concurrency check below means nothing'

$top = $null
$cmd = [Win]::FindCommand([Win]::GetMenu($frame), 'AI assistant', [ref]$top)
Check 'menu entry "AI assistant" exists' ($cmd -ge 0)
Check 'menu entry is in the Misc menu' ($top -eq 'Misc') "found in: $top"
if ($cmd -lt 0) { exit 98 }

function Toggle { [void][Win]::PostMessageW($frame, 0x0111, [IntPtr]$cmd, [IntPtr]::Zero) }  # WM_COMMAND

Toggle
$panel = [IntPtr]::Zero
$null = Wait-Until { $script:panel = Find-ByText $frame 'R AI assistant'; $script:panel -ne [IntPtr]::Zero } 15
$panel = $script:panel
Check 'first toggle opens the panel' (($panel -ne [IntPtr]::Zero) -and [Win]::IsWindowVisible($panel))
if ($panel -eq [IntPtr]::Zero) { exit 97 }

# Identify the controls.
$edits = @(); $buttons = @{}
foreach ($h in [Win]::Descendants($panel)) {
    $cls = [Win]::ClassOf($h)
    if ($cls -match 'Edit') { $edits += $h }            # RichEdit20W
    elseif ($cls -ieq 'Button') { $buttons[[Win]::Text($h)] = $h }
}
$hist = $edits | Where-Object { ([Win]::Style($_) -band 0x0800) -ne 0 } | Select-Object -First 1   # ES_READONLY
$inbox = $edits | Where-Object { ([Win]::Style($_) -band 0x0800) -eq 0 } | Select-Object -First 1
Check 'panel has a read-only transcript and an input box' ($hist -and $inbox)
foreach ($b in 'Send', 'Stop', 'Copy code', 'To editor', 'New chat') {
    Check ("button '{0}' present" -f $b) $buttons.ContainsKey($b)
}
if (-not ($hist -and $inbox -and $buttons.ContainsKey('Send'))) { exit 96 }
$send = $buttons['Send']; $stop = $buttons['Stop']
Check 'transcript shows the welcome text' (([Win]::Text($hist)) -like '*Local R assistant*')

# The status line is the panel's GraphApp label, window class "Rgui".
$statusLabel = [Win]::Descendants($panel) | Where-Object { [Win]::ClassOf($_) -eq 'Rgui' -and [Win]::IsWindowVisible($_) } | Select-Object -First 1
function Status { if ($statusLabel) { return [string]([Win]::Text($statusLabel)) } else { return '' } }
# The "Attached picture: ..." line above the question box, when shown.
function Attach-Text {
    foreach ($h in [Win]::Descendants($panel)) {
        if ($h -eq $statusLabel -or [Win]::ClassOf($h) -ne 'Rgui' -or -not [Win]::IsWindowVisible($h)) { continue }
        $t = [string]([Win]::Text($h))
        if ($t.StartsWith('Attached')) { return $t }
    }
    return ''
}
# Draw a plot at the console and wait for its window.
function Draw-Plot {
    if (-not (Set-Clip "boxplot(len ~ supp * dose, data = ToothGrowth, col = c('orange', 'skyblue'))`n")) { return $false }
    Start-Sleep -Milliseconds 400
    Post-Command $script:PasteCmd
    return (Wait-Until { (Find-ByText $frame 'R Graphics') -ne [IntPtr]::Zero } 20)
}

if (-not $WithModel -and -not $Canned) {
    # No model on disk: the first open offers to download it.
    $dlg = Wait-Dialog 10
    $offer = ''
    if ($dlg -ne [IntPtr]::Zero) { $offer = [Win]::DialogText($dlg) }
    Check 'missing model: opening the panel offers the download' `
          ($offer -like '*not on this computer*' -or $offer -like '*already downloaded*') "dialog: $offer"

    if ($ExpectDownload) {
        Check 'the offer says how big it is' ($offer -like '*GB*')
        Check 'answering Yes closes the offer' (Answer-Dialog $dlg 'Yes')
        Check 'download runs (Send disabled, Stop enabled)' `
              (Wait-Until { -not [Win]::IsWindowEnabled($send) -and [Win]::IsWindowEnabled($stop) } 10)
        Check 'status line shows download progress' (Wait-Until { (Status) -like '*Downloading the AI model*' } 15) "status: $(Status)"
        $ran = Test-ConsoleRuns $console 30
        Check 'R runs console code while the model downloads' ($ran -and -not [Win]::IsWindowEnabled($send))
        Check 'download completes' (Wait-Until { [Win]::IsWindowEnabled($send) } 180)
        $len = -1
        if (Test-Path $DownloadedModel) { $len = (Get-Item $DownloadedModel).Length }
        Check 'model file in place with the published size' ($len -eq $DownloadBytes) "size: $len"
        Check 'no .part file left behind' (-not (Test-Path "$DownloadedModel.part"))
        if ($DownloadedVision) {
            $vlen = -1
            if (Test-Path $DownloadedVision) { $vlen = (Get-Item $DownloadedVision).Length }
            Check 'the picture reader arrives with the model' ($vlen -eq $DownloadBytes) "size: $vlen"
        }
        # The test file is not a real model, so the server must refuse it
        # and the status line must say why.
        Check 'a bad model file is reported in the status line' `
              (Wait-Until { (Status) -like '*stopped while starting*' -or (Status) -like '*exited unexpectedly*' } 120) "status: $(Status)"
        Check 'console still runs R code' (Test-ConsoleRuns $console 30)
        Stop-Process -Id $ProcessId -Force
        if ($savedClipboard) { [void](Set-Clip $savedClipboard) }
        Write-Host ''
        if ($script:Failures -eq 0) { Write-Host 'GUI: PASSED' -ForegroundColor Green }
        else { Write-Host ("GUI: FAILED ({0})" -f $script:Failures) -ForegroundColor Red }
        exit $script:Failures
    }
    Check 'answering No closes the offer' (Answer-Dialog $dlg 'No')
}

# While the panel is the active window, RGui's top bar is its menu bar.
$bar = [Win]::GetMenu($frame)
$names = @([Win]::MenuNames($bar))
if (-not $Canned) {
    foreach ($m in 'File', 'Edit', 'Attach', 'Misc', 'Packages', 'Windows', 'Help') {
        Check ("panel menu bar has {0}" -f $m) ($names -contains $m) ("menus: " + ($names -join ', '))
    }
    $label = [Win]::FullLabel($bar, 'AI assistant')
    Check 'panel Misc menu has AI assistant with its Ctrl+T' ($label -and $label.Contains('Ctrl+T')) "label: $label"
}
$ptop = $null
$panelToggle = [Win]::FindCommand($bar, 'AI assistant', [ref]$ptop)
$attErr = [Win]::FindCommand($bar, 'Last error from the console', [ref]$ptop)
$attScript = [Win]::FindCommand($bar, 'Current script', [ref]$ptop)
$attCon = [Win]::FindCommand($bar, 'Recent console output', [ref]$ptop)
$attPlot = [Win]::FindCommand($bar, 'Current plot', [ref]$ptop)
$attClip = [Win]::FindCommand($bar, 'Picture from the clipboard', [ref]$ptop)
$attRemove = [Win]::FindCommand($bar, 'Remove pictures', [ref]$ptop)
$panelPaste = [Win]::FindCommand($bar, 'Paste', [ref]$ptop)

if (-not $WithModel -and -not $Canned) {
    Post-Command $panelToggle
    Check "the panel's own AI assistant command hides it" (Wait-Until { -not [Win]::IsWindowVisible($panel) } 5)
    Toggle
    $null = Wait-Until { [Win]::IsWindowVisible($panel) } 5

    # Attach: a real error typed at the console.
    if ((Set-Clip "stop('rgui-test-error')`n")) {
        Start-Sleep -Milliseconds 400
        Post-Command $script:PasteCmd
        Start-Sleep -Seconds 2
    }
    [void][Win]::SetText($inbox, '')
    Post-Command $attErr
    $got = ''
    $null = Wait-Until { $script:got = [string]([Win]::Text($inbox)); $script:got.Contains('rgui-test-error') } 5
    Check 'Attach > Last error adds the console error' ($script:got.Contains('rgui-test-error') -and $script:got.Contains('Error')) "input: $($script:got)"
    Check 'the attached error includes the command' ($script:got.Contains("stop('rgui-test-error')"))
    [void][Win]::SetText($inbox, '')
    Post-Command $attCon
    $null = Wait-Until { ([string]([Win]::Text($inbox))).Contains('rgui-test-error') } 5
    Check 'Attach > Recent console output adds the output' (([string]([Win]::Text($inbox))).Contains('rgui-test-error'))
    [void][Win]::SetText($inbox, '')
    Post-Command $attScript
    Start-Sleep -Seconds 1
    Check 'Attach > Current script with no script says so' `
          (([string]([Win]::Text($inbox))) -eq '' -and (Status) -like '*No script is open*') "status: $(Status)"
}

if ($Canned) {
    Check 'no download offer when the model file is there' ((Wait-Dialog 3) -eq [IntPtr]::Zero)
    function Ask([string]$Q) {
        [void][Win]::SetText($inbox, $Q)
        [void][Win]::Click($send)
        $null = Wait-Until { -not [Win]::IsWindowEnabled($send) } 5
        return (Wait-Until { [Win]::IsWindowEnabled($send) } 60)
    }
    # An answer without a code block.
    [void](Set-Clip 'clipboard-sentinel')
    Check 'canned prose answer arrives' (Ask 'NOCODE: what is a p-value?')
    Check 'it is in the transcript' (([Win]::Text($hist)).Contains('probability'))
    [void][Win]::Click($buttons['Copy code'])
    Check 'Copy code says the answer has no code' (Wait-Until { (Status) -like '*no code block*' } 5) "status: $(Status)"
    Check 'and leaves the clipboard alone' ((Get-Clip) -eq 'clipboard-sentinel')
    [void][Win]::Click($buttons['To editor'])
    Check 'To editor says so too' (Wait-Until { (Status) -like '*no code block*' } 5) "status: $(Status)"
    Check 'and opens no script window' ((Count-Scripts $frame 'AI answer') -eq 0)

    # Every character: set into the box, sent, shown; and Markdown rendered.
    # (escapes: this file is read as ANSI by Windows PowerShell 5.1)
    $uq = [regex]::Unescape('\u00c1rv\u00edzt\u0171r\u0151 t\u00fck\u00f6rf\u00far\u00f3g\u00e9p \u2013 ' +
          '\u03b1\u03b2\u03b3 \u2264 \u2265 \u2211 \u2013 \u4e2d\u6587 \u2013 \ud83d\ude00 \u2013 t-test?')
    [void][Win]::SetText($inbox, $uq)
    Check 'a question in any script round-trips through the input box' ([Win]::Text($inbox) -eq $uq)

    # An answer with code: the first To editor opens a script ...
    Check 'canned code answer arrives' (Ask $uq)
    $ht = [string]([Win]::Text($hist))
    Check 'the question reaches the transcript whole' ($ht.Contains($uq))
    Check "the answer's accents and emoji reach the transcript" ($ht.Contains([regex]::Unescape('\u00e9\u00e1 and \ud83d\ude00')))
    Check 'the answer is rendered: no raw ``` or backticks' (-not $ht.Contains('```') -and -not $ht.Contains('`t.test()`'))
    Check 'the code block is shown' ($ht.Contains('t.test(len ~ supp, data = ToothGrowth)'))
    [void][Win]::Click($buttons['To editor'])
    Check 'To editor opens a script when none is open' (Wait-Until { (Count-Scripts $frame 'AI answer') -ge 1 } 10)
    # ... Attach picks it up ...
    [void][Win]::SetText($inbox, '')
    Post-Command $attScript
    $null = Wait-Until { ([string]([Win]::Text($inbox))).Contains('t.test(') } 5
    Check 'Attach > Current script adds the open script' (([string]([Win]::Text($inbox))).Contains('t.test(')) "input: $([Win]::Text($inbox))"
    [void][Win]::SetText($inbox, '')
    # ... and the next To editor inserts into it rather than opening another.
    Check 'second canned code answer arrives' (Ask 'And once more, please?')
    [void][Win]::Click($buttons['To editor'])
    Check 'the second To editor inserts into the open script' (Wait-Until { (Status) -like '*inserted into*' } 5) "status: $(Status)"
    Check 'no second script window' ((Count-Scripts $frame 'AI answer') -eq 1)
    $ed = Find-ByText $frame 'AI answer'
    $edText = (@([Win]::Descendants($ed) | ForEach-Object { [Win]::Text($_) }) -join "`n")
    $n = ([regex]::Matches($edText, [regex]::Escape('t.test('))).Count
    Check 'the script now holds the code twice' ($n -ge 2) "t.test( occurs $n times"

    # Pictures.  The plot window is drawn under the panel on purpose: what
    # is sent must be the plot, not whatever covers it.
    Check 'Attach menu has Current plot, Picture file and the clipboard' `
          ($attPlot -ge 0 -and $attClip -ge 0 -and $attRemove -ge 0)
    Post-Command $attPlot
    Check 'Current plot with no plot says so' (Wait-Until { (Status) -like '*No plot window*' } 5) "status: $(Status)"
    Check 'a plot is drawn at the console' (Draw-Plot)
    Post-Command $attPlot
    Check 'Current plot attaches it' (Wait-Until { (Attach-Text) -like 'Attached picture: plot, device*' } 10) "attach line: $(Attach-Text)  status: $(Status)"
    Check 'the picture goes with the question' (Ask 'What kind of plot is this?')
    $ht = [string]([Win]::Text($hist))
    Check 'the transcript names the picture' ($ht.Contains('Picture: plot, device'))
    Check 'the server got one picture' ($ht.Contains('PICTURES last=1 total=1')) "tail: $($ht.Substring([Math]::Max(0, $ht.Length - 200)))"
    Check 'the attached line goes once it is sent' ((Attach-Text) -eq '')
    $recv = Join-Path $ServerDir 'received-1.png'
    $orange = 0; $blue = 0
    if (Test-Path $recv) {
        Add-Type -AssemblyName System.Drawing
        $bmp = [System.Drawing.Bitmap]::FromFile($recv)
        for ($y = 0; $y -lt $bmp.Height; $y += 3) {
            for ($x = 0; $x -lt $bmp.Width; $x += 3) {
                $c = $bmp.GetPixel($x, $y)
                if ($c.R -gt 230 -and $c.G -gt 140 -and $c.G -lt 190 -and $c.B -lt 60) { $orange++ }
                elseif ($c.R -gt 110 -and $c.R -lt 160 -and $c.G -gt 190 -and $c.B -gt 215) { $blue++ }
            }
        }
        $bmp.Dispose()
    }
    Check 'the picture sent is the plot itself, not what covers it' ($orange -gt 200 -and $blue -gt 200) "orange $orange, sky blue $blue samples"
    Check 'a follow-up still carries the picture' (Ask 'And which box is highest?')
    $ht = [string]([Win]::Text($hist))
    Check 'the server got it again with the follow-up' ($ht.Contains('PICTURES last=0 total=1'))

    # A screenshot on the clipboard, pasted into the question box.
    Add-Type -AssemblyName System.Windows.Forms
    $img = New-Object System.Drawing.Bitmap 240, 120
    $g = [System.Drawing.Graphics]::FromImage($img)
    $g.Clear([System.Drawing.Color]::White)
    $g.FillRectangle([System.Drawing.Brushes]::Red, 20, 20, 100, 60)
    $g.Dispose()
    $put = $false
    for ($i = 0; $i -lt 20 -and -not $put; $i++) {
        try { [System.Windows.Forms.Clipboard]::SetImage($img); $put = $true } catch { Start-Sleep -Milliseconds 100 }
    }
    $img.Dispose()
    Start-Sleep -Milliseconds 400
    Post-Command $panelPaste
    Check 'Paste with a picture on the clipboard attaches it' `
          (Wait-Until { (Attach-Text) -like '*picture from the clipboard (240 x 120)*' } 10) "attach line: $(Attach-Text)  status: $(Status)"
    Post-Command $attRemove
    Check 'Remove pictures drops it' (Wait-Until { (Attach-Text) -eq '' -and (Status) -like 'Pictures removed*' } 5) "status: $(Status)"

    Stop-Process -Id $ProcessId -Force
    if ($savedClipboard) { [void](Set-Clip $savedClipboard) }
    Write-Host ''
    if ($script:Failures -eq 0) { Write-Host 'GUI: PASSED' -ForegroundColor Green }
    else { Write-Host ("GUI: FAILED ({0})" -f $script:Failures) -ForegroundColor Red }
    exit $script:Failures
}
Check 'Send enabled, Stop disabled when idle' ([Win]::IsWindowEnabled($send) -and -not [Win]::IsWindowEnabled($stop))

Toggle
Check 'second toggle hides the panel' (Wait-Until { -not [Win]::IsWindowVisible($panel) } 5)
Toggle
Check 'third toggle shows it again' (Wait-Until { [Win]::IsWindowVisible($panel) } 5)

[void][Win]::PostMessageW($panel, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE
Check 'closing the panel hides it instead of destroying it' `
      ((Wait-Until { -not [Win]::IsWindowVisible($panel) } 5) -and [Win]::IsWindow($panel))
Toggle
Check 'toggle after close brings the same panel back' (Wait-Until { [Win]::IsWindowVisible($panel) } 5)
Check 'console still runs R code with the panel open' (Test-ConsoleRuns $console 30)

$question = 'Write one line of R code that computes the mean of c(2, 4, 9).'

if ($WithModel) {
    [Win]::SetSel($hist, 0, 0)
    [void][Win]::SetText($inbox, $question)
    [void][Win]::Click($send)
    Check 'Send starts a turn (Send disabled, Stop enabled)' `
          (Wait-Until { -not [Win]::IsWindowEnabled($send) -and [Win]::IsWindowEnabled($stop) } 10)

    # The requirement: R stays usable while the model loads and answers.
    $busyAtStart = -not [Win]::IsWindowEnabled($send)
    $gui = [Win]::Responds($frame, 2000)
    $ran = Test-ConsoleRuns $console 30
    $stillBusy = -not [Win]::IsWindowEnabled($send)
    Check 'GUI thread answers within 2 s while the model works' $gui
    if ($busyAtStart -and $stillBusy) {
        Check 'R runs console code while the model is still working' $ran
    } else {
        Check 'R runs console code during the answer' $ran
        Write-Host '      (the answer finished before the probe did, so overlap is not proven)' -ForegroundColor Yellow
    }

    $done = Wait-Until { [Win]::IsWindowEnabled($send) } $AnswerTimeout
    Check "the answer completes within $AnswerTimeout s" $done
    $text = [Win]::Text($hist)
    $reply = ''
    $at = $text.LastIndexOf('R assistant')
    if ($at -ge 0) { $reply = $text.Substring($at + 11).Trim() }
    Check 'transcript has the question' ($text.Contains($question))
    Check 'transcript has a non-empty answer' ($reply.Length -gt 0) "transcript tail: $($text.Substring([Math]::Max(0, $text.Length - 300)))"
    Check 'answer contains mean(' ($reply.Contains('mean('))
    Check 'the code is shown without its ``` fences' ($reply.Contains('mean(') -and -not $reply.Contains('```'))
    Check 'no <think> text in the transcript' (-not $text.Contains('<think>'))

    [void](Set-Clip 'clipboard-before')
    [void][Win]::Click($buttons['Copy code'])
    $clipOk = Wait-Until { $c = Get-Clip; $c -and $c.Contains('mean(') } 5
    $clip = Get-Clip
    Check 'Copy code puts the code on the clipboard' $clipOk "clipboard: '$clip'  status: $(Status)"
    Check 'Copy code strips the ``` fences' ($clip -and -not $clip.Contains('```'))

    [void][Win]::Click($buttons['To editor'])
    $ed = [IntPtr]::Zero
    $null = Wait-Until { $script:ed = Find-ByText $frame 'AI answer'; $script:ed -ne [IntPtr]::Zero } 10
    $ed = $script:ed
    Check 'To editor opens a script window' ($ed -ne [IntPtr]::Zero)
    if ($ed -ne [IntPtr]::Zero) {
        $edText = (@([Win]::Descendants($ed) | ForEach-Object { [Win]::Text($_) }) -join "`n")
        Check 'the script window holds the code' ($edText.Contains('mean('))
    }

    [Win]::SetSel($hist, 0, 5)
    [void][Win]::SetText($inbox, 'Now the median of the same numbers, one line of R.')
    [void][Win]::Click($send)
    $null = Wait-Until { -not [Win]::IsWindowEnabled($send) } 10
    $done2 = Wait-Until { [Win]::IsWindowEnabled($send) } $AnswerTimeout
    $text2 = [Win]::Text($hist)
    $at2 = $text2.LastIndexOf('R assistant')
    $reply2 = ''
    if ($at2 -ge 0) { $reply2 = $text2.Substring($at2 + 11).Trim() }
    $sel = [Win]::GetSel($hist)
    Check 'second answer completes' $done2
    Check 'second answer lands after its header' ($reply2.Contains('median')) "reply: $reply2"
    Check 'a selection in the transcript survives a streamed answer' `
          (($sel[0] -eq 0) -and ($sel[1] -eq 5)) ("selection now {0}..{1}" -f $sel[0], $sel[1])
    Check 'first answer still in place' ($text2.Contains('mean('))

    $before = [Win]::Text($hist)
    Toggle; $null = Wait-Until { -not [Win]::IsWindowVisible($panel) } 5
    Toggle; $null = Wait-Until { [Win]::IsWindowVisible($panel) } 5
    Check 'hiding and reopening keeps the conversation' ([Win]::Text($hist) -eq $before)

    # A plot for the real model.
    Check 'a plot is drawn at the console' (Draw-Plot)
    Post-Command $attPlot
    Check 'Current plot attaches it' (Wait-Until { (Attach-Text) -like 'Attached picture: plot*' } 10) "status: $(Status)"
    [void][Win]::SetText($inbox, 'What kind of plot is this? Answer in one sentence.')
    [void][Win]::Click($send)
    $null = Wait-Until { -not [Win]::IsWindowEnabled($send) } 10
    $done3 = Wait-Until { [Win]::IsWindowEnabled($send) } $AnswerTimeout
    $text3 = [Win]::Text($hist)
    $at3 = $text3.LastIndexOf('R assistant')
    $reply3 = ''
    if ($at3 -ge 0) { $reply3 = $text3.Substring($at3 + 11).Trim() }
    Check 'the model answers about the picture' $done3
    Check 'and recognises the boxplot' ($reply3 -match '(?i)box') "reply: $reply3"
} else {
    # No model on disk: Send offers the download again instead of failing.
    [void][Win]::SetText($inbox, $question)
    [void][Win]::Click($send)
    $dlg = Wait-Dialog 10
    Check 'Send without a model offers the download again' ($dlg -ne [IntPtr]::Zero)
    if ($dlg -ne [IntPtr]::Zero) { [void](Answer-Dialog $dlg 'No') }
    Check 'declining leaves the panel idle' (Wait-Until { [Win]::IsWindowEnabled($send) } 10)
    Check 'the question stays in the input box' ([Win]::Text($inbox) -eq $question)
    Check 'console still runs R code after the failed turn' (Test-ConsoleRuns $console 30)
}

# Killing Rgui must take llama-server with it (job object).
$prefix = [System.IO.Path]::GetFullPath($TreeDir).TrimEnd('\') + '\'
function Get-TreeServers {
    @(Get-CimInstance Win32_Process -Filter "Name = 'llama-server.exe'" |
      Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) })
}
$servers = @(Get-TreeServers)
Stop-Process -Id $ProcessId -Force
if ($WithModel) {
    Check 'a llama-server was running for this Rgui' ($servers.Count -gt 0)
    Check 'killing Rgui also ends llama-server' (Wait-Until { @(Get-TreeServers).Count -eq 0 } 10)
}

if ($savedClipboard) { [void](Set-Clip $savedClipboard) }

Write-Host ''
if ($script:Failures -eq 0) { Write-Host 'GUI: PASSED' -ForegroundColor Green }
else { Write-Host ("GUI: FAILED ({0})" -f $script:Failures) -ForegroundColor Red }
exit $script:Failures
