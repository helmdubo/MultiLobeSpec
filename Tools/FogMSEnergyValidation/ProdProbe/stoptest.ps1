param([string[]]$Cmds = @(), [string]$Label = "test", [int]$Stops = 3)
$S = Split-Path -Parent $MyInvocation.MyCommand.Path
foreach ($c in $Cmds) { python "$S\uemcp.py" cmd $c | Select-String '"success"' | Out-Null; "cvar: $c" }
$src = @"
using System; using System.Runtime.InteropServices;
public static class U2 {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr e);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@; if (-not ([System.Management.Automation.PSTypeName]'U2').Type) { Add-Type -TypeDefinition $src }
$p = Get-Process UnrealEditor | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
[U2]::SetForegroundWindow($p.MainWindowHandle) | Out-Null; Start-Sleep -Milliseconds 800
$r = New-Object U2+RECT; [U2]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
$cx = [int](($r.L + $r.R) / 2); $cy = [int](($r.T + $r.B) / 2 - 60)
$startUtc = (Get-Date).ToUniversalTime()
Start-Sleep -Milliseconds 1200
for ($k = 0; $k -lt $Stops; $k++) {
  [U2]::SetCursorPos($cx, $cy) | Out-Null; Start-Sleep -Milliseconds 300
  [U2]::mouse_event(0x0008, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 120
  for ($i = 0; $i -lt 60; $i++) { [U2]::mouse_event(0x0001, 3, 1, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 20 }
  [U2]::mouse_event(0x0010, 0, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 2500
}
Start-Sleep -Milliseconds 1500
python "$S\hitchparse.py" $startUtc.ToString("yyyy.MM.dd-HH.mm.ss") $Label
