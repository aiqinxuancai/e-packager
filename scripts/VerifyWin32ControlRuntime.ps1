[CmdletBinding()]
param(
    [string]$Project = "eproj\\e-window-exe-full.e",
    [string]$OutputDirectory = "temp\\win32-control-verification",
    [string]$AdapterRoot = "D:\\git\\BlackMoonKernelStaticLib\\adapter",
    [int]$StartupSeconds = 3,
    [ValidateRange(1, 10)][int]$RepeatCount = 3,
    [string]$X64Project = "eproj\\e-window-exe-new-proj.e",
    [ValidateRange(0, 60000)][int]$AutoCloseMilliseconds = 0,
    [switch]$PropertyProbe,
    [switch]$MemberProbe,
    [switch]$EventProbe
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot ".." )).Path
$projectPath = (Resolve-Path (Join-Path $repo $Project)).Path
$x64ProjectPath = (Resolve-Path (Join-Path $repo $X64Project)).Path
$outputRoot = Join-Path $repo $OutputDirectory
[System.IO.Directory]::CreateDirectory($outputRoot) | Out-Null
$x86Packager = Join-Path $repo "bin\\Win32\\Release\\e-packager.exe"
$x64Packager = Join-Path $repo "bin\\x64\\Release\\e-packager.exe"

function Get-PeMachine([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { return "invalid" }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -lt 0 -or $peOffset + 6 -gt $bytes.Length) { return "invalid" }
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    switch ($machine) { 0x014c { "x86"; break } 0x8664 { "x64"; break } default { "0x{0:X4}" -f $machine } }
}

if (-not ('Win32ControlProbe' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Diagnostics;
public static class Win32ControlProbe {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr h, EnumProc f, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern bool SetWindowText(IntPtr h, string value);
    [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool IsWindow(IntPtr h);
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("user32.dll")] static extern uint GetGuiResources(IntPtr process, uint flags);
    const uint PROCESS_QUERY_LIMITED_INFORMATION=0x1000, GR_GDIOBJECTS=0, GR_USEROBJECTS=1;
    public static int[] Resources(int pid) {
        var h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,false,pid);
        if(h==IntPtr.Zero)return new[]{-1,-1};
        try { return new[]{(int)GetGuiResources(h,GR_GDIOBJECTS),(int)GetGuiResources(h,GR_USEROBJECTS)}; }
        finally { CloseHandle(h); }
    }
    public static string[] Snapshot(IntPtr root) {
        var result = new List<string>();
        EnumChildWindows(root, (h, l) => {
            var text = new StringBuilder(512); var cls = new StringBuilder(64);
            GetWindowText(h, text, text.Capacity); GetClassName(h, cls, cls.Capacity);
            uint countMessage = 0, textMessage = 0;
            if (cls.ToString().Equals("ListBox", StringComparison.OrdinalIgnoreCase)) { countMessage=0x018B; textMessage=0x0189; }
            else if (cls.ToString().Equals("ComboBox", StringComparison.OrdinalIgnoreCase)) { countMessage=0x0146; textMessage=0x0148; }
            var items = new List<string>();
            if (countMessage != 0) {
                int count = SendMessage(h, countMessage, IntPtr.Zero, IntPtr.Zero).ToInt32();
                IntPtr buffer = Marshal.AllocHGlobal(2048);
                try { for (int i=0; i<count; ++i) { Marshal.Copy(new byte[2048], 0, buffer, 2048); SendMessage(h, textMessage, (IntPtr)i, buffer); items.Add(Marshal.PtrToStringUni(buffer) ?? ""); } }
                finally { Marshal.FreeHGlobal(buffer); }
            }
            result.Add(GetDlgCtrlID(h) + "|" + cls + "|" + text
                + (items.Count==0 ? "" : "|items=" + String.Join(",", items)));
            return true;
        }, IntPtr.Zero);
        return result.ToArray();
    }
    public static void Exercise(IntPtr root) {
        EnumChildWindows(root, (h, l) => {
            var cls = new StringBuilder(64); var text = new StringBuilder(256);
            GetClassName(h, cls, cls.Capacity); GetWindowText(h, text, text.Capacity);
            var name = cls.ToString();
            // Exercise only deterministic, non-modal controls.  Empty BUTTON
            // controls are color selectors in the fixture and must not open a
            // modal color dialog during unattended verification.
            // Do not click push buttons here: the fixture intentionally has
            // handlers which open modal information dialogs.  Those dialogs
            // block unattended probing and do not add coverage beyond the
            // command-routing tests.  Checkbox/radio state is exercised via
            // their non-modal state messages below.
            if (name.Equals("Button", StringComparison.OrdinalIgnoreCase)) {
                // no-op (avoid modal BN_CLICKED handlers)
            }
            else if (name.Equals("ListBox", StringComparison.OrdinalIgnoreCase))
                SendMessage(h, 0x0186, (IntPtr)1, IntPtr.Zero); // LB_SETCURSEL
            else if (name.Equals("ComboBox", StringComparison.OrdinalIgnoreCase))
                SendMessage(h, 0x014E, (IntPtr)1, IntPtr.Zero); // CB_SETCURSEL
            else if (name.Equals("SysTabControl32", StringComparison.OrdinalIgnoreCase)) {
                SendMessage(h, 0x130C, (IntPtr)4, IntPtr.Zero); // TCM_SETCURSEL, last page
                SendMessage(h, 0x130C, IntPtr.Zero, IntPtr.Zero); // and back
            }
            else if (name.Equals("msctls_trackbar32", StringComparison.OrdinalIgnoreCase))
                SendMessage(h, 0x0405, (IntPtr)1, (IntPtr)5); // TBM_SETPOS(TRUE, 5)
            else if (name.Equals("Edit", StringComparison.OrdinalIgnoreCase) && text.Length == 0)
                SetWindowText(h, "runtime edit");
            return true;
        }, IntPtr.Zero);
    }
    public static string[] Query(IntPtr root) {
        var result = new List<string>();
        EnumChildWindows(root, (h, l) => {
            var cls = new StringBuilder(64); GetClassName(h, cls, cls.Capacity);
            var name = cls.ToString();
            if (name.Equals("ListBox", StringComparison.OrdinalIgnoreCase))
                result.Add("list_sel=" + SendMessage(h, 0x0188, IntPtr.Zero, IntPtr.Zero).ToInt32());
            else if (name.Equals("ComboBox", StringComparison.OrdinalIgnoreCase))
                result.Add("combo_sel=" + SendMessage(h, 0x0147, IntPtr.Zero, IntPtr.Zero).ToInt32());
            else if (name.Equals("SysTabControl32", StringComparison.OrdinalIgnoreCase)) {
                result.Add("tab_count=" + SendMessage(h, 0x1304, IntPtr.Zero, IntPtr.Zero).ToInt32());
                result.Add("tab_sel=" + SendMessage(h, 0x130B, IntPtr.Zero, IntPtr.Zero).ToInt32());
            }
            else if (name.Equals("msctls_trackbar32", StringComparison.OrdinalIgnoreCase))
                result.Add("track_pos=" + SendMessage(h, 0x0400, IntPtr.Zero, IntPtr.Zero).ToInt32());
            else if (name.Equals("Edit", StringComparison.OrdinalIgnoreCase)) {
                var text = new StringBuilder(256); GetWindowText(h, text, text.Capacity);
                result.Add("edit_text=" + text);
            }
            return true;
        }, IntPtr.Zero);
        return result.ToArray();
    }
}
'@
}

$whiteList = @(
    "编辑框","图片框","外形框","画板","分组框","标签","按钮","选择框","单选框",
    "组合框","列表框","选择列表框","横向滚动条","纵向滚动条","进度条","滑块条",
    "选择夹","影像框","日期框","月历","驱动器框","目录框","文件框","颜色选择器",
    "超级链接框","调节器"
)
$blackList = @("数据报","客户","服务器","端口","表格","数据源","通用提供者","数据库提供者","图形按钮","外部数据库","外部数据提供者")

function Invoke-CompileAndSmoke([string]$Architecture, [string]$Packager, [string]$InputProject, [string[]]$AdapterArguments, [int]$RunNumber) {
    $stem = "controls-$Architecture-$RunNumber"
    $exe = Join-Path $outputRoot "$stem.exe"
    # 编译器自身会创建 <stem>.compile.log；避免 Tee-Object 同时占用同一文件。
    $log = Join-Path $outputRoot "$stem.stdout.log"
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $compileLines = @()
    try {
        $compileLines = @(& $Packager compile $InputProject $exe --compile-mode semantic --arch $Architecture @AdapterArguments 2>&1 |
            ForEach-Object { $_.ToString() })
        $compileLines | Tee-Object -FilePath $log | Out-Null
        $compileExit = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previousPreference }
    if ($compileExit -ne 0 -or -not (Test-Path $exe)) { throw "compile_failed:$Architecture" }
    $peMachine = Get-PeMachine $exe
    if ($peMachine -ne $Architecture) { throw "pe_architecture_mismatch:${Architecture}:$peMachine" }
    $source = [System.IO.Path]::ChangeExtension($exe, ".generated.cpp")
    if (-not (Test-Path $source)) { throw "generated_source_missing:$Architecture" }
    $generated = Get-Content $source -Raw
    $expectedSpecCount = ([regex]::Matches($generated, '(?m)^\{(?!0u,)\d+u,')).Count
    $expectedTabCount = ([regex]::Matches($generated, 'TCM_INSERTITEMW')).Count
    $hasListSpec = $generated -match '(?m)^\{\d+u,[^\r\n]*,"(?:list|checklist)"'
    $hasComboSpec = $generated -match '(?m)^\{\d+u,[^\r\n]*,"combo"'
    $hasTrackbarSpec = $generated -match '(?m)^\{\d+u,[^\r\n]*,"trackbar"'
    $hasEditSpec = $generated -match '(?m)^\{\d+u,[^\r\n]*,"edit"'
    foreach ($name in $blackList) {
        if ($generated.Contains($name)) { throw "unsupported_control_leaked:${Architecture}:$name" }
    }
    $tokens = @('edit','image','shape','canvas','group','label','button','checkbox','radio','combo','list','checklist','hscroll','vscroll','progress','trackbar','tab','animate','date','month','drive','directory','file','color','hyperlink','spin')
    foreach ($token in $tokens) {
        if ($generated -notmatch ('"' + [regex]::Escape($token) + '"')) { throw "native_control_token_missing:${Architecture}:$token" }
    }
    foreach ($marker in @('NEGATIVE_DATAGRAM','NEGATIVE_CLIENT','NEGATIVE_SERVER','NEGATIVE_PORT','NEGATIVE_GRID','NEGATIVE_DATASOURCE','NEGATIVE_PROVIDER','NEGATIVE_DB_PROVIDER','NEGATIVE_PICTURE_BUTTON','NEGATIVE_EXTERNAL_DB','NEGATIVE_EXTERNAL_PROVIDER','NEGATIVE_UNKNOWN')) {
        if ($generated.Contains($marker)) { throw "unsupported_text_leaked:${Architecture}:$marker" }
    }
    # The codec diagnostic prints the persisted project path for comparison;
    # only a successful load of that path indicates an actual architecture
    # mismatch.  Do not reject harmless `resolved=` fields.
    if ($compileLines -match 'property probe library loaded .*C:\\Users\\aiqin\\OneDrive\\e5\.6\\lib') {
        throw "wrong_persisted_fne_path_used:${Architecture}"
    }
    $loadedAdapterLine = @($compileLines | Where-Object { $_ -match 'property probe library loaded .*adapter\\lib\\(x86|x64)\\' }) | Select-Object -First 1
    $dependencyArchitecture = if ($loadedAdapterLine -match 'adapter\\lib\\(x86|x64)\\') { $Matches[1] } else { "static-or-none" }
    if ($dependencyArchitecture -ne "static-or-none" -and $dependencyArchitecture -ne $Architecture) {
        throw "dependency_architecture_mismatch:${Architecture}:${dependencyArchitecture}"
    }
    $eventLog = Join-Path $outputRoot "$stem.events.log"
    if (Test-Path $eventLog) { Remove-Item -LiteralPath $eventLog -Force }
    $propertyLog = Join-Path $outputRoot "$stem.properties.log"
    if (Test-Path $propertyLog) { Remove-Item -LiteralPath $propertyLog -Force }
    $memberLog = Join-Path $outputRoot "$stem.members.log"
    if (Test-Path $memberLog) { Remove-Item -LiteralPath $memberLog -Force }
    $oldEventLog = $env:E_PACKAGER_EVENT_LOG
    $oldAutoClose = $env:E_PACKAGER_RUNTIME_AUTOCLOSE_MS
    $oldPropertyProbe = $env:E_PACKAGER_RUNTIME_PROPERTY_PROBE
    $oldPropertyLog = $env:E_PACKAGER_PROPERTY_LOG
    $oldMemberProbe = $env:E_PACKAGER_RUNTIME_MEMBER_PROBE
    $oldMemberLog = $env:E_PACKAGER_MEMBER_LOG
    $oldEventProbe = $env:E_PACKAGER_RUNTIME_EVENT_PROBE
    $env:E_PACKAGER_EVENT_LOG = $eventLog
    if ($AutoCloseMilliseconds -gt 0) { $env:E_PACKAGER_RUNTIME_AUTOCLOSE_MS = [string]$AutoCloseMilliseconds }
    if ($PropertyProbe) {
        $env:E_PACKAGER_RUNTIME_PROPERTY_PROBE = "1"
        $env:E_PACKAGER_PROPERTY_LOG = $propertyLog
    }
    if ($MemberProbe) {
        $env:E_PACKAGER_RUNTIME_MEMBER_PROBE = "1"
        $env:E_PACKAGER_MEMBER_LOG = $memberLog
    }
    if ($EventProbe) { $env:E_PACKAGER_RUNTIME_EVENT_PROBE = "1" }
    try {
        $proc = Start-Process -FilePath $exe -PassThru
        Start-Sleep -Seconds $StartupSeconds
    } finally {
        if ($null -eq $oldEventLog) { Remove-Item Env:E_PACKAGER_EVENT_LOG -ErrorAction SilentlyContinue }
        else { $env:E_PACKAGER_EVENT_LOG = $oldEventLog }
        if ($null -eq $oldAutoClose) { Remove-Item Env:E_PACKAGER_RUNTIME_AUTOCLOSE_MS -ErrorAction SilentlyContinue }
        else { $env:E_PACKAGER_RUNTIME_AUTOCLOSE_MS = $oldAutoClose }
        if ($null -eq $oldPropertyProbe) { Remove-Item Env:E_PACKAGER_RUNTIME_PROPERTY_PROBE -ErrorAction SilentlyContinue }
        else { $env:E_PACKAGER_RUNTIME_PROPERTY_PROBE = $oldPropertyProbe }
        if ($null -eq $oldPropertyLog) { Remove-Item Env:E_PACKAGER_PROPERTY_LOG -ErrorAction SilentlyContinue }
        else { $env:E_PACKAGER_PROPERTY_LOG = $oldPropertyLog }
        if ($null -eq $oldMemberProbe) { Remove-Item Env:E_PACKAGER_RUNTIME_MEMBER_PROBE -ErrorAction SilentlyContinue }
        else { $env:E_PACKAGER_RUNTIME_MEMBER_PROBE = $oldMemberProbe }
        if ($null -eq $oldMemberLog) { Remove-Item Env:E_PACKAGER_MEMBER_LOG -ErrorAction SilentlyContinue }
        else { $env:E_PACKAGER_MEMBER_LOG = $oldMemberLog }
        if ($null -eq $oldEventProbe) { Remove-Item Env:E_PACKAGER_RUNTIME_EVENT_PROBE -ErrorAction SilentlyContinue }
        else { $env:E_PACKAGER_RUNTIME_EVENT_PROBE = $oldEventProbe }
    }
    $snapshot = @()
    if (-not $proc.HasExited -and $proc.MainWindowHandle -ne [IntPtr]::Zero) {
        $snapshot = @([Win32ControlProbe]::Snapshot($proc.MainWindowHandle))
        [Win32ControlProbe]::Exercise($proc.MainWindowHandle)
        Start-Sleep -Milliseconds 200
    }
    $query = if (-not $proc.HasExited -and $proc.MainWindowHandle -ne [IntPtr]::Zero) { @([Win32ControlProbe]::Query($proc.MainWindowHandle)) } else { @() }
    $resources = if (-not $proc.HasExited) { @([Win32ControlProbe]::Resources($proc.Id)) } else { @(-1,-1) }
    if (-not $proc.HasExited -and $AutoCloseMilliseconds -gt 0) {
        $proc.WaitForExit([Math]::Max(2000, $AutoCloseMilliseconds + 2000)) | Out-Null
    }
    $alive = -not $proc.HasExited
    $exitCode = if ($proc.HasExited) { $proc.ExitCode } else { $null }
    $normalExit = $proc.HasExited -and $proc.ExitCode -eq 0
    if ($alive) { Stop-Process -Id $proc.Id -Force }
    if ($snapshot.Count -lt $expectedSpecCount) { throw "runtime_control_count_too_low:${Architecture}:$($snapshot.Count):expected=$expectedSpecCount" }
    $propertyFailures = @($compileLines | Where-Object { $_ -match "window property (decode failed|metadata_invalid)" })
    if ($Architecture -eq "x86") {
        $joined = $snapshot -join "`n"
        if ($joined -notmatch "\|Static\|标签") { throw "runtime_text_missing:${Architecture}:label" }
        if ($joined -notmatch "\|Button\|按钮1标题") { throw "runtime_text_missing:${Architecture}:button" }
        if ($joined -notmatch "\|Button\|选择框") { throw "runtime_text_missing:${Architecture}:checkbox" }
        if ($joined -notmatch "\|Button\|单选框") { throw "runtime_text_missing:${Architecture}:radio" }
        if ($joined -notmatch "\|Button\|按钮-在分组框中的") { throw "runtime_text_missing:${Architecture}:nested_button" }
        if ($joined -notmatch "items=1,2,3") { throw "runtime_items_missing:${Architecture}:list" }
        if ($joined -notmatch "items=1,2,3,4") { throw "runtime_items_missing:${Architecture}:combo" }
        if ($propertyFailures.Count -ne 0) { throw "property_decode_failed:${Architecture}:$($propertyFailures.Count)" }
    }
    $queryText = $query -join "`n"
    if ($hasListSpec -and $queryText -notmatch 'list_sel=1') { throw "runtime_selection_failed:${Architecture}:list" }
    if ($hasComboSpec -and $queryText -notmatch 'combo_sel=1') { throw "runtime_selection_failed:${Architecture}:combo" }
    if ($expectedTabCount -gt 0) {
        if ($queryText -notmatch "tab_count=$expectedTabCount") { throw "runtime_tab_count_failed:${Architecture}" }
        if ($queryText -notmatch 'tab_sel=0') { throw "runtime_tab_roundtrip_failed:${Architecture}" }
    }
    if ($hasTrackbarSpec -and $queryText -notmatch 'track_pos=5') { throw "runtime_trackbar_failed:${Architecture}" }
    if ($AutoCloseMilliseconds -gt 0 -and $hasEditSpec -and $queryText -notmatch 'edit_text=runtime edit') { throw "runtime_edit_failed:${Architecture}" }
    $markers = @(if (Test-Path $eventLog) { Get-Content -LiteralPath $eventLog | Where-Object { $_ -like 'marker*' } } else { @() })
    $eventLines = @(if (Test-Path $eventLog) { Get-Content -LiteralPath $eventLog } else { @() })
    $eventRows = @($eventLines | Where-Object { $_ -like 'event*' -or $_ -like 'invoke*' } | ForEach-Object {
        $parts = $_ -split "`t"
        [pscustomobject]@{ kind=$parts[0]; id=if ($parts.Count -gt 1) { $parts[1] } else { "" }; name=if ($parts.Count -gt 2) { $parts[2] } else { "" }; type=if ($parts.Count -gt 3) { $parts[3] } else { "" }; event=if ($parts.Count -gt 4) { $parts[4] } else { "" }; trigger=if ($parts.Count -gt 5) { $parts[5] } else { "" }; native_code=if ($parts.Count -gt 6) { $parts[6] } else { "" }; method_id=if ($parts.Count -gt 7 -and $_ -like 'invoke*') { $parts[7] } else { "" }; param_count=if ($parts.Count -gt 7 -and $_ -like 'event*') { $parts[7] } elseif ($parts.Count -gt 8) { $parts[8] } else { "" }; arguments=if ($parts.Count -gt 8 -and $_ -like 'event*') { $parts[8] } elseif ($parts.Count -gt 9) { $parts[9] } else { "" } }
    })
    $malformedEventRows = @($eventRows | Where-Object { ($_.kind -eq 'event' -or $_.kind -eq 'invoke') -and $_.param_count -notmatch '^\d+$' })
    if (($AutoCloseMilliseconds -gt 0 -or $MemberProbe) -and $malformedEventRows.Count -ne 0) { throw "runtime_event_log_malformed:${Architecture}:$($malformedEventRows.Count)" }
    if ($EventProbe) {
        $probeEvents = @($eventRows | Where-Object { $_.kind -eq 'event' })
        $unknownEvents = @($probeEvents | Where-Object { $_.event -eq 'unknown' })
        if ($unknownEvents.Count -ne 0) { throw "runtime_event_unknown:${Architecture}:$($unknownEvents.Count)" }
        foreach ($eventName in @('focus_gained','focus_lost','mouse_move','mouse_down','mouse_up','right_mouse_down','right_mouse_up','mouse_enter','mouse_leave','clicked','double_clicked','selection_changed','changed','drop_down','list_closed','position_changed','selection_changing','paint','check_changed')) {
            if (@($probeEvents | Where-Object { $_.event -eq $eventName }).Count -eq 0) { throw "runtime_event_missing:${Architecture}:$eventName" }
        }
        $spinRows = @($probeEvents | Where-Object { $_.type -eq 'spin' -and $_.event -eq 'position_changed' })
        if ($spinRows.Count -eq 0 -or $spinRows[0].arguments -notmatch '^integer:-?\d+$') { throw "runtime_event_abi_failed:${Architecture}:spin" }
        $paintRows = @($probeEvents | Where-Object { $_.event -eq 'paint' })
        if ($paintRows.Count -eq 0 -or $paintRows[0].param_count -ne '4' -or $paintRows[0].arguments -notmatch '^integer:-?\d+,integer:-?\d+,integer:-?\d+,integer:-?\d+$') { throw "runtime_event_abi_failed:${Architecture}:paint" }
    }
    $propertyLines = @(if ($PropertyProbe -and (Test-Path $propertyLog)) { Get-Content -LiteralPath $propertyLog } else { @() })
    $memberLines = @(if ($MemberProbe -and (Test-Path $memberLog)) { Get-Content -LiteralPath $memberLog } else { @() })
    $propertyRows = @($propertyLines | Where-Object { $_ -like 'property*' } | ForEach-Object {
        $parts = $_ -split "`t", 6
        [pscustomobject]@{ id=$parts[1]; type=$parts[2]; name=$parts[3]; value_type=$parts[4]; value=if ($parts.Count -gt 5) { $parts[5] } else { "" } }
    })
    $propertyMissing = @($propertyRows | Where-Object { $_.value_type -eq "missing" })
    if ($PropertyProbe -and $propertyMissing.Count -ne 0) { throw "runtime_property_missing:${Architecture}:$($propertyMissing.Count)" }
    $memberRows = @($memberLines | Where-Object { $_ -like 'member*' } | ForEach-Object {
        $parts = $_ -split "`t", 9
        [pscustomobject]@{ id=$parts[1]; type=$parts[2]; operation=$parts[3]; status=$parts[4]; arg_count=$parts[5]; arg_summary=$parts[6]; return_type=$parts[7]; return_value=if ($parts.Count -gt 8) { $parts[8] } else { "" } }
    })
    if ($MemberProbe -and $memberRows.Count -eq 0 -and $expectedSpecCount -gt 0) { throw "runtime_member_probe_empty:${Architecture}" }
    if ($MemberProbe -and $expectedSpecCount -gt 0) {
        foreach ($operation in @("GetHWnd", "GetClientWidth", "GetClientHeight", "IsFocus", "SetFocus", "Invalidate", "UpdateWindow")) {
            if (@($memberRows | Where-Object { $_.operation -eq $operation -and $_.status -eq "called" }).Count -eq 0) {
                throw "runtime_member_operation_missing:${Architecture}:$operation"
            }
        }
        if ($hasListSpec -and @($memberRows | Where-Object { $_.operation -eq "GetTopIndex" -and $_.status -eq "called" }).Count -eq 0) {
            throw "runtime_member_operation_missing:${Architecture}:GetTopIndex"
        }
        if ($expectedTabCount -gt 0 -and @($memberRows | Where-Object { $_.operation -eq "GetTabName" -and $_.status -eq "called" }).Count -eq 0) {
            throw "runtime_member_operation_missing:${Architecture}:GetTabName"
        }
        if (@($memberRows | Where-Object { $_.operation -eq "SetWritePos" -and $_.status -eq "called" }).Count -eq 0) {
            throw "runtime_member_operation_missing:${Architecture}:SetWritePos"
        }
    }
    $negativeSnapshotRows = @($snapshot | Where-Object { $_ -match "NEGATIVE_" })
    if ($negativeSnapshotRows.Count -ne 0) { throw "unsupported_control_runtime_leaked:${Architecture}:$($negativeSnapshotRows.Count)" }
    if ($AutoCloseMilliseconds -gt 0) {
        if (($markers -join "`n") -notmatch 'marker\tinitialize_begin\t') { throw "runtime_marker_missing:${Architecture}:initialize_begin" }
        if (($markers -join "`n") -notmatch 'marker\tinitialize_end\t') { throw "runtime_marker_missing:${Architecture}:initialize_end" }
        if (($markers -join "`n") -notmatch 'marker\tcreated_dispatched\t') { throw "runtime_marker_missing:${Architecture}:created_dispatched" }
        $firstMarker = [Array]::IndexOf($eventLines, ($eventLines | Where-Object { $_ -like 'marker*initialize_begin*' } | Select-Object -First 1))
        if ($firstMarker -gt 0) {
            $beforeInit = @($eventLines | Select-Object -First $firstMarker | Where-Object { $_ -like 'event*' })
            if ($beforeInit.Count -ne 0) { throw "initialization_event_leaked:${Architecture}:$($beforeInit.Count)" }
        }
    }
    if ($AutoCloseMilliseconds -gt 0 -and -not $normalExit) { throw "runtime_exit_failed:${Architecture}:$exitCode" }
    if ($AutoCloseMilliseconds -eq 0 -and -not $alive) { throw "runtime_exited:${Architecture}:$exitCode" }
    [pscustomobject]@{ architecture=$Architecture; pe_machine=$peMachine; dependency_architecture=$dependencyArchitecture; executable=$exe; expected_spec_count=$expectedSpecCount; alive_after_seconds=$alive; normal_exit=$normalExit; exit_code=$exitCode; gui_resources=$resources; window_snapshot=$snapshot; query=$query; event_log=$eventLog; event_lines=$eventLines; event_rows=$eventRows; event_invocation_count=@($eventRows | Where-Object { $_.kind -eq "invoke" }).Count; malformed_event_count=$malformedEventRows.Count; markers=$markers; property_log=$propertyLog; property_rows=$propertyRows; property_missing=$propertyMissing; property_compat_count=@($propertyRows | Where-Object { $_.value_type -eq "compat" }).Count; property_decode_failures=$propertyFailures; member_log=$memberLog; member_rows=$memberRows; member_called_count=@($memberRows | Where-Object { $_.status -eq "called" }).Count; member_skipped_count=@($memberRows | Where-Object { $_.status -like "skipped*" }).Count }
}

$results = @()
for ($run = 1; $run -le $RepeatCount; $run++) {
    $results += Invoke-CompileAndSmoke "x86" $x86Packager $projectPath @("--blackmoon-x86-dir", $AdapterRoot) $run
    $results += Invoke-CompileAndSmoke "x64" $x64Packager $x64ProjectPath @("--blackmoon-x64-dir", $AdapterRoot) $run
}
$report = [pscustomobject]@{
    project=$projectPath
    x64_project=$x64ProjectPath
    repeat_count=$RepeatCount
    whitelist_count=$whiteList.Count
    whitelist=$whiteList
    blacklist_count=$blackList.Count
    blacklist=$blackList
    smoke=$results
    auto_close_milliseconds=$AutoCloseMilliseconds
    property_probe_status=if ($PropertyProbe) { "runtime_property_probe_no_missing" } elseif (@($results | ForEach-Object { @($_.property_decode_failures) } | Where-Object { $_ }).Count -ne 0) { "failed_decode_diagnostics" } elseif ($AutoCloseMilliseconds -gt 0) { "runtime_readback_partial" } else { "smoke_only_no_probe" }
    member_probe_status=if ($MemberProbe) { "runtime_member_probe_recorded" } else { "not_requested" }
    event_probe_status=if ($EventProbe) { "runtime_event_probe_complete" } elseif ($AutoCloseMilliseconds -gt 0) { "runtime_event_trace_partial" } else { "not_present_in_input_project" }
}
$report | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $outputRoot "summary.json") -Encoding UTF8
$report | ConvertTo-Json -Depth 5
