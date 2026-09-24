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
    [string]$DownloadedModel = '',
    [long]$DownloadBytes = 0
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

    // The transcript is an ANSI edit control.  Sent through the W entry
    // point, EM_GETSEL comes back mangled by the system's A/W position
    // translation; through the A entry point it round-trips exactly.
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutA")]
    static extern IntPtr SmtoA(IntPtr h, uint m, IntPtr w, IntPtr l, uint f, uint t, out IntPtr r);
    public static void SetSel(IntPtr h, int a, int b) {
        IntPtr r; SmtoA(h, 0x00B1, (IntPtr) a, (IntPtr) b, SMTO_ABORTIFHUNG, 5000, out r);
    }
    public static int[] GetSel(IntPtr h) {
        IntPtr r; SmtoA(h, 0x00B0, IntPtr.Zero, IntPtr.Zero, SMTO_ABORTIFHUNG, 5000, out r);
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
    if (-not (Set-Clip "writeLines('ok', '$rpath')`n")) { return $false }
    [void][Win]::PostMessageW($frame, 0x0111, [IntPtr]$script:PasteCmd, [IntPtr]::Zero)
    return (Wait-Until { Test-Path $file } $Seconds)
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
Start-Sleep -Seconds 2
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
    if ($cls -ieq 'Edit') { $edits += $h }
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
$statusLabel = [Win]::Descendants($panel) | Where-Object { [Win]::ClassOf($_) -eq 'Rgui' } | Select-Object -First 1
function Status { if ($statusLabel) { return [string]([Win]::Text($statusLabel)) } else { return '' } }

if (-not $WithModel) {
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
    $at = $text.LastIndexOf('R assistant:')
    if ($at -ge 0) { $reply = $text.Substring($at + 12).Trim() }
    Check 'transcript has the question' ($text.Contains($question))
    Check 'transcript has a non-empty answer' ($reply.Length -gt 0) "transcript tail: $($text.Substring([Math]::Max(0, $text.Length - 300)))"
    Check 'answer contains mean(' ($reply.Contains('mean('))
    Check 'answer has a fenced code block' ($reply.Contains('```'))
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
    $at2 = $text2.LastIndexOf('R assistant:')
    $reply2 = ''
    if ($at2 -ge 0) { $reply2 = $text2.Substring($at2 + 12).Trim() }
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
