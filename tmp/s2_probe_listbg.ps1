# S2 probe: какой фон нативный SysListView32 рисует на самом деле.
# Зачем: в задаче сказано, что ListView_SetBkColor, а затем enableDarkModeForWindow
# дают белый список. Прежде чем чинить, надо знать, какой порядок вызовов (или его
# отсутствие) оставляет наш цвет фона. Окно создаётся СКРЫТЫМ и снимается
# WM_PRINT / PrintWindow — передний план чужого окна не трогаем.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tmp\s2_probe_listbg.ps1
#   ... -Mode printwindow      показать окно на 0,3 с и снять PrintWindow
param(
    [ValidateSet('print', 'printwindow')]
    [string]$Mode = 'print'
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$src = @'
using System;
using System.Runtime.InteropServices;
using System.Drawing;
using System.Drawing.Imaging;
using System.Collections.Generic;

public static class Probe {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int L, T, R, B; }

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateWindowExW(uint ex, string cls, string title, uint style, int x, int y,
                                          int w, int h, IntPtr parent, IntPtr id, IntPtr inst, IntPtr param);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")]
    static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")]
    static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")]
    static extern bool DestroyWindow(IntPtr h);
    [DllImport("user32.dll")]
    static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr GetModuleHandleW(string name);
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true, EntryPoint = "GetProcAddress")]
    static extern IntPtr GetProcAddressByName(IntPtr module, [MarshalAs(UnmanagedType.LPStr)] string proc);
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true, EntryPoint = "GetProcAddress")]
    static extern IntPtr GetProcAddressByOrdinal(IntPtr module, IntPtr ordinal);
    [DllImport("uxtheme.dll", SetLastError = true)]
    static extern IntPtr SetWindowTheme(IntPtr h, string app, string sub);

    delegate int AppModeFn(int mode);
    delegate bool AllowDarkFn(IntPtr hwnd, bool allow);

    const uint LVM_SETBKCOLOR = 0x101F, LVM_SETTEXTBKCOLOR = 0x1022, LVM_SETTEXTCOLOR = 0x1023;
    const uint WM_PRINT = 0x0317;
    const uint PRF_NONCLIENT = 0x2, PRF_CLIENT = 0x4, PRF_ERASEBKGND = 0x8, PRF_CHILDREN = 0x10;

    static AppModeFn _appMode;
    static AllowDarkFn _allow;
    public static readonly List<string> Lines = new List<string>();

    static IntPtr Resolve(IntPtr mod, string name, short ordinal) {
        IntPtr p = GetProcAddressByName(mod, name);
        if (p != IntPtr.Zero) return p;
        return GetProcAddressByOrdinal(mod, new IntPtr((ushort)ordinal));
    }

    public static bool BindDark() {
        IntPtr ux = GetModuleHandleW("uxtheme.dll");
        if (ux == IntPtr.Zero) { Lines.Add("uxtheme.dll НЕ загружена"); return false; }
        IntPtr am = Resolve(ux, "SetPreferredAppMode", 135);
        IntPtr al = Resolve(ux, "AllowDarkModeForWindow", 136);
        if (am != IntPtr.Zero) _appMode = (AppModeFn)Marshal.GetDelegateForFunctionPointer(am, typeof(AppModeFn));
        if (al != IntPtr.Zero) _allow = (AllowDarkFn)Marshal.GetDelegateForFunctionPointer(al, typeof(AllowDarkFn));
        Lines.Add("SetPreferredAppMode=" + (_appMode != null) + " AllowDarkModeForWindow=" + (_allow != null));
        return _appMode != null || _allow != null;
    }

    public static int AppMode(int m) { return _appMode == null ? -999 : _appMode(m); }

    public static bool MakeDark(IntPtr hwnd) {
        IntPtr ux = GetModuleHandleW("uxtheme.dll");
        if (ux == IntPtr.Zero) return false;
        IntPtr refresh = Resolve(ux, "RefreshImmersiveColorPolicyState", 104);
        if (refresh != null) {
            RefreshFn f = (RefreshFn)Marshal.GetDelegateForFunctionPointer(refresh, typeof(RefreshFn));
            f();
        }
        if (_appMode != null) _appMode(1);
        if (_allow != null) _allow(hwnd, true);
        IntPtr applied = SetWindowTheme(hwnd, "DarkMode_Explorer", null);
        return applied != IntPtr.Zero;
    }

    delegate void RefreshFn();

    public static void SetColors(IntPtr hwnd, uint bk, uint text, uint textBk) {
        SendMessageW(hwnd, LVM_SETBKCOLOR, new IntPtr((long)bk), IntPtr.Zero);
        SendMessageW(hwnd, LVM_SETTEXTBKCOLOR, new IntPtr((long)textBk), IntPtr.Zero);
        SendMessageW(hwnd, LVM_SETTEXTCOLOR, new IntPtr((long)text), IntPtr.Zero);
    }

    public static IntPtr MakeParent(int x, int y, int w, int h) {
        return CreateWindowExW(0, "STATIC", "s2probe", 0x90C80000u /*WS_POPUP*/, x, y, w, h,
                               IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
    }
    public static IntPtr MakeList(IntPtr parent, int x, int y, int w, int h) {
        return CreateWindowExW(0, "SysListView32", "list", 0x50000001u /*WS_CHILD|WS_VISIBLE*/ | 0x0001u /*LVS_REPORT*/ | 0x0020u,
                               x, y, w, h, parent, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
    }
    public static void Show(IntPtr h) { ShowWindow(h, 5); }
    public static void Kill(IntPtr h) { DestroyWindow(h); }

    static double Lum(int r, int g, int b) {
        Func<int, double> ch = v => { double x = v / 255.0; return x <= 0.03928 ? x / 12.92 : Math.Pow((x + 0.055) / 1.055, 2.4); };
        return 0.2126 * ch(r) + 0.7152 * ch(g) + 0.0722 * ch(b);
    }

    // Замер: самый частый цвет = фон, «чернила» = пиксели с контрастом к фону >= 3:1.
    public static void Measure(IntPtr hwnd, int capW, int capH, string tag, bool usePrintWindow) {
        using (Bitmap bmp = new Bitmap(capW, capH, PixelFormat.Format32bppArgb))
        using (Graphics g = Graphics.FromImage(bmp)) {
            IntPtr dc = g.GetHdc();
            bool ok = usePrintWindow
                ? PrintWindow(hwnd, dc, 2)
                : SendMessageW(hwnd, WM_PRINT, dc,
                      new IntPtr(PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND | PRF_NONCLIENT)) != IntPtr.Zero;
            g.ReleaseHdc(dc);
            var counts = new Dictionary<int, int>();
            int total = 0;
            for (int y = 0; y < capH; y += 2)
                for (int x = 0; x < capW; x += 2) {
                    Color c = bmp.GetPixel(x, y);
                    int key = (c.R << 16) | (c.G << 8) | c.B;
                    int n; counts.TryGetValue(key, out n); counts[key] = n + 1; total++;
                }
            int modal = 0, modalCount = 0;
            foreach (var kv in counts) if (kv.Value > modalCount) { modalCount = kv.Value; modal = kv.Key; }
            int mr = (modal >> 16) & 255, mg = (modal >> 8) & 255, mb = modal & 255;
            int ink = 0;
            foreach (var kv in counts) {
                double l1 = Lum((kv.Key >> 16) & 255, (kv.Key >> 8) & 255, kv.Key & 255);
                double l2 = Lum(mr, mg, mb);
                double hi = Math.Max(l1, l2), lo = Math.Min(l1, l2);
                if ((hi + 0.05) / (lo + 0.05) >= 3.0) ink += kv.Value;
            }
            RECT r; GetClientRect(hwnd, out r);
            Lines.Add(string.Format("{0}: ok={1} фон=#{2:X2}{3:X2}{4:X2} доля={5}% чернила>=3:1={6} цветов={7} клиент={8}x{9} (шаг 2, всего {10})",
                tag, ok, mr, mg, mb, total == 0 ? 0 : (100 * modalCount / total), ink, counts.Count,
                r.R - r.L, r.B - r.T, total));
        }
    }
}
'@

Add-Type -TypeDefinition $src -Language CSharp -ReferencedAssemblies System.Drawing

Write-Host "[probe] режим снимка: $Mode"
[void][Probe]::BindDark()
Write-Host ("[probe] SetPreferredAppMode(AllowDark) = {0}" -f [Probe]::AppMode(1))
Write-Host ("[probe] OS: {0}" -f (Get-CimInstance Win32_OperatingSystem).Caption)

function Invoke-Variant {
    param([string]$Name, [scriptblock]$Setup)
    $parent = [Probe]::MakeParent(0, 0, 420, 220)
    if ($parent -eq [IntPtr]::Zero) { Write-Host "[probe] $Name : не создалось родительское окно"; return }
    $list = [Probe]::MakeList($parent, 0, 0, 420, 200)
    if ($list -eq [IntPtr]::Zero) { Write-Host "[probe] $Name : не создался SysListView32"; return }
    & $Setup $list
    if ($Mode -eq 'printwindow') { [Probe]::Show($parent); Start-Sleep -Milliseconds 400 }
    [Probe]::Measure($parent, 420, 220, $Name, ($Mode -eq 'printwindow'))
    [Probe]::Kill($parent)
}

# A: как в коде сейчас — сначала цвет фона, потом тёмный режим.
Invoke-Variant 'A colors-then-darkmode' {
    param($list)
    [Probe]::SetColors($list, 0x2B2B2B, 0xFFFFFF, 0x2B2B2B)
    $ok = [Probe]::MakeDark($list)
    Write-Host "[probe] A: DarkMode_Explorer применён = $ok"
}
# B: сначала тёмный режим, потом цвет фона.
Invoke-Variant 'B darkmode-then-colors' {
    param($list)
    $ok = [Probe]::MakeDark($list)
    [Probe]::SetColors($list, 0x2B2B2B, 0xFFFFFF, 0x2B2B2B)
    Write-Host "[probe] B: DarkMode_Explorer применён = $ok"
}
# C: цвет фона без тёмного режима.
Invoke-Variant 'C colors-only' {
    param($list)
    [Probe]::SetColors($list, 0x2B2B2B, 0xFFFFFF, 0x2B2B2B)
}

foreach ($line in [Probe]::Lines) { Write-Host "[probe] $line" }
exit 0