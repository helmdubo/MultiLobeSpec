# Brings the running UnrealEditor main window to the foreground (viewport renders/measures only when visible).
Add-Type @"
using System; using System.Runtime.InteropServices;
public class FogMSFg { [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd); }
"@
$p = Get-Process UnrealEditor -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { throw 'No UnrealEditor window.' }
[FogMSFg]::ShowWindow($p.MainWindowHandle, 9) | Out-Null   # SW_RESTORE
[FogMSFg]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 800
"FOREGROUND pid=$($p.Id)"
