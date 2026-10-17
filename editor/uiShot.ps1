param([string]$out = "shot.png", [string]$clicks = "", [string]$file = "", [string]$dialogClicks = "", [string]$attribute = "")
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System; using System.Text; using System.Collections.Generic; using System.Runtime.InteropServices;
public class W5 {
 public delegate bool EnumProc(IntPtr h, IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc e, IntPtr l);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
 [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
 [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
 public struct R { public int L,T,Rr,B; }
 public static IntPtr FindFrame(uint pid) { IntPtr res = IntPtr.Zero; EnumWindows((h,l)=>{ uint p; GetWindowThreadProcessId(h, out p); var c=new StringBuilder(64); GetClassName(h,c,64); if (p==pid && IsWindowVisible(h) && c.ToString()=="Frame") res=h; return true; }, IntPtr.Zero); return res; }
 public static List<IntPtr> Visible(uint pid) { var l = new List<IntPtr>(); EnumWindows((h,x)=>{ uint p; GetWindowThreadProcessId(h, out p); R r; GetWindowRect(h, out r); if (p==pid && IsWindowVisible(h) && r.Rr-r.L > 100) l.Add(h); return true; }, IntPtr.Zero); return l; }
 public static List<string> Children(IntPtr f) { var l = new List<string>(); EnumChildWindows(f, (h,x)=>{ var c=new StringBuilder(64); GetClassName(h,c,64); R r; GetWindowRect(h,out r); l.Add(h + " " + c + " " + r.L+","+r.T+","+r.Rr+","+r.B); return true; }, IntPtr.Zero); return l; }
 public static void DblClick(IntPtr h, int x, int y) { IntPtr lp = (IntPtr)((y << 16) | (x & 0xFFFF)); PostMessage(h, 0x0201, (IntPtr)1, lp); PostMessage(h, 0x0202, IntPtr.Zero, lp); PostMessage(h, 0x0203, (IntPtr)1, lp); PostMessage(h, 0x0202, IntPtr.Zero, lp); }
 public static void Click(IntPtr h, int x, int y) { IntPtr lp = (IntPtr)((y << 16) | (x & 0xFFFF)); PostMessage(h, 0x0201, (IntPtr)1, lp); PostMessage(h, 0x0202, IntPtr.Zero, lp); }
 public static string Shot(IntPtr h, string path) { R r; GetWindowRect(h, out r); var bmp = new System.Drawing.Bitmap(r.Rr-r.L, r.B-r.T); var g = System.Drawing.Graphics.FromImage(bmp); var dc = g.GetHdc(); PrintWindow(h, dc, 2); g.ReleaseHdc(dc); bmp.Save(path); return (r.Rr-r.L) + "x" + (r.B-r.T); }
}
"@
[W5]::SetProcessDPIAware() | Out-Null
Get-Process jitnodesEditor -ErrorAction SilentlyContinue | Stop-Process -Force
$exe = "D:\code\jitnodes\out\build\x64-release-editor\editor\jitnodesEditor.exe"
$exeArgs = @(); if ($file) { $exeArgs += $file }; if ($attribute) { $exeArgs += "--attribute"; $exeArgs += $attribute }
if ($exeArgs.Count -gt 0) { $p = Start-Process $exe -ArgumentList $exeArgs -WorkingDirectory (Split-Path $exe) -PassThru } else { $p = Start-Process $exe -WorkingDirectory (Split-Path $exe) -PassThru }
Start-Sleep -Seconds 3
$f = [W5]::FindFrame([uint32]$p.Id); "frame $f"
$kids = [W5]::Children($f); $kids
$child = [IntPtr][int64](($kids[0] -split " ")[0])
$fr = New-Object W5+R; [W5]::GetWindowRect($f, [ref]$fr) | Out-Null; "frame rect $($fr.L),$($fr.T) $($fr.Rr),$($fr.B)"
$cr = New-Object W5+R; [W5]::GetClientRect($f, [ref]$cr) | Out-Null; "client $($cr.Rr) x $($cr.B)"
foreach ($c in ($clicks -split ";" | Where-Object { $_ })) {
  $dbl = $c.StartsWith("d"); if ($dbl) { $c = $c.Substring(1) }
  $xy = $c -split ","; $x = [int]$xy[0]; $y = [int]$xy[1]
  if ($dbl) { [W5]::DblClick($child, $x, $y) } else { [W5]::Click($child, $x, $y) }; Start-Sleep -Milliseconds 400
}
Start-Sleep -Milliseconds 500
if ($dialogClicks) {
  $dlg = [W5]::Visible([uint32]$p.Id) | Where-Object { $_ -ne $f } | Select-Object -First 1
  if ($dlg) {
    $dkids = [W5]::Children($dlg); $dchild = [IntPtr][int64](($dkids[0] -split " ")[0])
    foreach ($c in ($dialogClicks -split ";" | Where-Object { $_ })) { $xy = $c -split ","; [W5]::Click($dchild, [int]$xy[0], [int]$xy[1]); Start-Sleep -Milliseconds 400 }
    Start-Sleep -Milliseconds 600
  }
}
[W5]::Shot($f, $out)
$n = 0
foreach ($w in [W5]::Visible([uint32]$p.Id)) { if ($w -ne $f) { $n++; "extra window $w -> " + [W5]::Shot($w, ($out -replace '\.png$', "-$n.png")) } }
