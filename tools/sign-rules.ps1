#Requires -Version 5.1
<#
.SYNOPSIS
    MrProper: подпись набора правил очистки (SPEC §9.2, ADR-008).

.DESCRIPTION
    Набор правил управляет тем, что приложение удаляет, поэтому канал обновления
    считается недоверенным входом: правила применяются только после проверки
    подписи Ed25519 встроенным в бинарник публичным ключом и сверки SHA-256
    каждого файла с манифестом (SPEC §9.2 п.2).

    Скрипт делает три вещи:
      * NewKey       — один раз генерирует пару ключей Ed25519; приватная часть
                       шифруется DPAPI (CurrentUser) и кладётся ВНЕ репозитория,
                       публичная печатается для вставки константой в исходники;
      * Sign         — проверяет, что rules\manifest.json сходится с фактическими
                       файлами набора, и подписывает байты манифеста, записывая
                       rules\rules.sig (base64 от 64 сырых байт);
      * Verify       — проверяет существующий набор: подпись манифеста и хеши
                       всех файлов. То же, что делает приложение, только без
                       сборки бинарника.

    Порядок проверок совпадает с SPEC §9.2 п.2 и с src/core/rulesync.cpp
    (parseManifest → minAppVersion → подпись → SHA-256/размер → разбор правил):
    подпись считается раньше хешей, а файл, не перечисленный в манифесте, —
    отказ, потому что по нему приложение удаляло бы файлы, которые никто не
    подписывал.

.PARAMETER Action
    NewKey | Sign | Verify | PublicKey | CheckManifest | SelfTest.

.PARAMETER RepoRoot
    Корень репозитория. По умолчанию — родительский каталог tools\.

.PARAMETER RulesDir
    Каталог набора правил. По умолчанию <RepoRoot>\rules.

.PARAMETER ManifestPath
    Путь к манифесту. По умолчанию <RulesDir>\manifest.json.

.PARAMETER SignaturePath
    Путь к файлу подписи. По умолчанию <RulesDir>\rules.sig.

.PARAMETER KeyPath
    Файл приватного ключа. По умолчанию %USERPROFILE%\.mrproper\rules-ed25519.key.
    Путь внутри репозитория отвергается: ключ в репозитории — это не шифрование,
    а его отсутствие (ADR-008).

.PARAMETER PublicKeyBase64
    Публичный ключ для -Verify в виде base64 от 32 сырых байт (как в исходниках).
    Если не задан, берётся из файла ключа.

.PARAMETER AppVersion
    Версия приложения для сверки с minAppVersion манифеста, например 1.0.0.

.PARAMETER AllowUnlisted
    Не считать отказом файл *.json в каталоге набора, не перечисленный в
    манифесте. Только для отладки: приложение такой набор всё равно отвергнет
    (requireKnownFilesOnly в rulesync.cpp).

.PARAMETER Force
    Для -Action NewKey: перезаписать существующий ключ (старый набор останется
    неподписываемым — после смены ключа публичный ключ обязан попасть в бинарник).

.PARAMETER Json
    Вместо человекочитаемого отчёта напечатать одну строку JSON (для CI).

.PARAMETER Quiet
    Не печатать человекочитаемый отчёт. На результат проверки не влияет.

.EXAMPLE
    .\tools\sign-rules.ps1 -Action NewKey

.EXAMPLE
    .\tools\sign-rules.ps1 -Action Sign -AppVersion 1.0.0

.EXAMPLE
    .\tools\sign-rules.ps1 -Action Verify -PublicKeyBase64 "3bT7...=" -Json

.NOTES
    Windows PowerShell 5.1 (штатный .NET Framework 4.x) не умеет Ed25519: ни
    CNG, ни System.Security.Cryptography его не содержат. Реализация RFC 8032
    поэтому встроена в скрипт и компилируется в память через Add-Type; внешних
    зависимостей (OpenSSL, ssh-keygen, Python) нет, ключ никуда не уходит.
    -Action SelfTest прогоняет эталонные векторы RFC 8032 §7.1: модуль, который
    подписывает правила удаления, не вправе молча доверять собственной
    криптографии (та же мысль, что sha256SelfTest в ядре).

    Коды возврата: 0 — успех, 1 — проверка не пройдена (набор не подписан,
    подпись/хеши не сошлись), 2 — ошибка использования или окружения.
#>
[CmdletBinding()]
param(
    [ValidateSet('NewKey', 'Sign', 'Verify', 'PublicKey', 'CheckManifest', 'SelfTest')]
    [string]$Action = 'Sign',

    [string]$RepoRoot,
    [string]$RulesDir,
    [string]$ManifestPath,
    [string]$SignaturePath,
    [string]$KeyPath,
    [string]$PublicKeyBase64,
    [string]$AppVersion,
    [switch]$AllowUnlisted,
    [switch]$Force,
    [switch]$Json,
    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Русский текст в отчёте должен доезжать до консоли читаемым, а не ломбардом:
# без этого cmd в кодировке 866/1251 превращает сообщения в мусор. У redirects
# и неконсольных хостов смена кодировки может быть запрещена — тогда оставляем
# как есть, на разбор скрипта это не влияет.
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    $OutputEncoding = [System.Text.Encoding]::UTF8
} catch {
    # хост без смены кодировки вывода — не повод прерывать подписание
}

# Код возврата, который поставит верхний обработчик. Ошибки использования
# (2) отличаются от отказа проверки (1): «забыл -AppVersion» и «набор битый» —
# разные ситуации для CI.
$script:ExitCode = 1

# --------------------------------------------------------------------- вывод --

function Write-Line {
    # Человекочитаемый отчёт. В режиме -Json он уходит в stderr: на stdout
    # должен остаться ровно один JSON-объект, иначе его не разобрать в CI.
    param([string]$Text = '', [string]$Kind = 'info')
    if ($Quiet) { return }
    $line = switch ($Kind) {
        'ok' { "[ok]   $Text" }
        'bad' { "[плохо] $Text" }
        'warn' { "[внимание] $Text" }
        'info' { "[инфо] $Text" }
        'head' { "== $Text" }
        default { $Text }
    }
    if ($Json) {
        [Console]::Error.WriteLine($line)
        return
    }
    $colors = @{ 'ok' = 'DarkGreen'; 'bad' = 'Red'; 'warn' = 'Yellow'; 'info' = 'DarkGray'; 'head' = 'Cyan' }
    $color = $colors[$Kind]
    if ($color) { Write-Host $line -ForegroundColor $color } else { Write-Host $line }
}

function Stop-With {
    # Отказ проверки (1) по умолчанию; $Code 2 — ошибка использования.
    param([Parameter(Mandatory = $true)][string]$Message, [int]$Code = 1)
    $script:ExitCode = $Code
    throw $Message
}

function ConvertTo-AsciiEscaped {
    # JSON для CI должен читаться в любой кодировке, поэтому всё не-ASCII
    # выводится escape-последовательностями (в том числе суррогатные пары).
    param([Parameter(Mandatory = $true)][string]$Text)
    $sb = New-Object System.Text.StringBuilder ($Text.Length + 16)
    foreach ($ch in $Text.ToCharArray()) {
        $code = [int]$ch
        if ($code -ge 0x20 -and $code -le 0x7E) {
            [void]$sb.Append($ch)
        } else {
            [void]$sb.AppendFormat('\u{0:x4}', $code)
        }
    }
    return $sb.ToString()
}

function New-JsonLine {
    param([Parameter(Mandatory = $true)]$Object)
    return ConvertTo-AsciiEscaped (ConvertTo-Json -InputObject $Object -Compress -Depth 6)
}

function Write-JsonLine {
    # Через Write-Output, а не Write-Host: строку результата должен быть видно
    # в перенаправлении и в CI, а не только на экране.
    param([Parameter(Mandatory = $true)]$Object)
    Write-Output (New-JsonLine $Object)
}

# ------------------------------------------------------------------- пути --

function Get-NormalizedPath {
    # Регистр и разделители в путях Windows значения не имеют, а сравнение
    # «ключ лежит в репозитории» обязано быть регистронезависимым.
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = [System.IO.Path]::GetFullPath($Path)
    return $full.TrimEnd('\', '/')
}

function Get-ResolvedPaths {
    if (-not $RepoRoot) { $RepoRoot = Split-Path -Parent $PSScriptRoot }
    $RepoRoot = Get-NormalizedPath $RepoRoot
    if (-not $RulesDir) { $RulesDir = Join-Path $RepoRoot 'rules' }
    if (-not $ManifestPath) { $ManifestPath = Join-Path $RulesDir 'manifest.json' }
    if (-not $SignaturePath) { $SignaturePath = Join-Path $RulesDir 'rules.sig' }
    if (-not $KeyPath) { $KeyPath = Join-Path $env:USERPROFILE '.mrproper\rules-ed25519.key' }
    $RulesDir = Get-NormalizedPath $RulesDir
    $ManifestPath = Get-NormalizedPath $ManifestPath
    $SignaturePath = Get-NormalizedPath $SignaturePath
    $KeyPath = Get-NormalizedPath $KeyPath
    return [pscustomobject]@{
        RepoRoot     = $RepoRoot
        RulesDir     = $RulesDir
        ManifestPath = $ManifestPath
        SignaturePath = $SignaturePath
        KeyPath      = $KeyPath
    }
}

function Test-IsInside {
    # path внутри root? Сравнение с границей по разделителю, иначе «D:\Proj»
    # посчитался бы родителем «D:\Project\MrProper».
    param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][string]$Root)
    $p = $Path
    $r = $Root
    if (-not $r.EndsWith('\')) { $r = $r + '\' }
    if (-not $p.EndsWith('\')) { $p = $p + '\' }
    return $p.StartsWith($r, [System.StringComparison]::OrdinalIgnoreCase)
}

# ----------------------------------------------------------------- крипто --

function Initialize-Crypto {
    <#
        Ed25519 (RFC 8032) в памяти. Внешних зависимостей нет: Windows
        PowerShell 5.1 работает на .NET Framework 4.x, где Ed25519 нет ни в
        CNG, ни в System.Security.Cryptography.
    #>
    if ('MrProper.RulesSigning.Ed25519' -as [type]) { return }

    $source = @'
using System;
using System.Globalization;
using System.Numerics;
using System.Security.Cryptography;

namespace MrProper.RulesSigning
{
    // Точка в расширенных координатах: x = X/Z, y = Y/Z, T = XY/Z, кривая
    // twisted Edwards с a = -1 (стандартные формулы ref10).
    internal struct Point
    {
        public BigInteger X;
        public BigInteger Y;
        public BigInteger Z;
        public BigInteger T;
    }

    // Только подпись и проверка: без KDF-режимов и без постоянного времени.
    // Инструмент локальный, приватный ключ живёт офлайн у владельца
    // (ADR-008), поэтому побочные каналы по времени здесь не модель угроз;
    // соответствие стандарту проверяется эталонными векторами (SelfTest).
    public static class Ed25519
    {
        private static readonly BigInteger Zero = BigInteger.Zero;
        private static readonly BigInteger One = BigInteger.One;
        private static readonly BigInteger Two = new BigInteger(2);

        // p = 2^255 - 19, порядок группы L, параметр d кривой, sqrt(-1).
        private static readonly BigInteger P = BigInteger.Pow(2, 255) - 19;
        private static readonly BigInteger L = BigInteger.Parse(
            "1000000000000000000000000000000014DEF9DEA2F79CD65812631A5CF5D3ED",
            NumberStyles.HexNumber);
        private static readonly BigInteger D = BigInteger.Parse(
            "37095705934669439343138083508754565189542113879843219016388785533085940283555");
        private static readonly BigInteger SqrtM1 = BigInteger.ModPow(new BigInteger(2), (P - 1) / 4, P);

        // Порядок объявления обязателен: инициализаторы статических полей идут
        // сверху вниз, и Base не декодируется из ещё не заполненного
        // BaseEncoding, если поменять их местами.
        // Байтовое представление базовой точки (y = 4/5, знак x = 0).
        private static readonly byte[] BaseEncoding = CreateBaseEncoding();
        private static readonly Point Base = MakeBase();

        private static byte[] CreateBaseEncoding()
        {
            byte[] b = new byte[32];
            for (int i = 0; i < b.Length; i++) { b[i] = 0x66; }
            b[0] = 0x58;
            return b;
        }

        private static Point MakeBase()
        {
            Point p;
            if (!Decode(BaseEncoding, out p))
            {
                throw new InvalidOperationException("Ed25519: базовая точка не декодируется");
            }
            return p;
        }

        // ------------------------------------------------------------ поля --

        private static BigInteger Norm(BigInteger x)
        {
            x %= P;
            if (x.Sign < 0) { x += P; }
            return x;
        }

        private static BigInteger AddF(BigInteger a, BigInteger b) { return Norm(a + b); }
        private static BigInteger SubF(BigInteger a, BigInteger b) { return Norm(a - b); }
        private static BigInteger MulF(BigInteger a, BigInteger b) { return Norm(a * b); }
        private static BigInteger InvF(BigInteger a) { return BigInteger.ModPow(Norm(a), P - 2, P); }

        // Приведение по модулю порядка группы L (не по модулю поля: это разные
        // числа, и ошибка здесь тихо ломает и r, и k, и S).
        private static BigInteger ModL(BigInteger x)
        {
            x %= L;
            if (x.Sign < 0) { x += L; }
            return x;
        }

        // ---------------------------------------------------------- точки --

        private static Point Identity()
        {
            Point r;
            r.X = Zero; r.Y = One; r.Z = One; r.T = Zero;
            return r;
        }

        // Формулы add-2008-hwcd-3 и dbl-2008-hwcd для a = -1.
        private static Point AddPoints(Point p, Point q)
        {
            BigInteger a = MulF(SubF(p.Y, p.X), SubF(q.Y, q.X));
            BigInteger b = MulF(AddF(p.Y, p.X), AddF(q.Y, q.X));
            BigInteger c = MulF(MulF(p.T, D), MulF(Two, q.T));
            BigInteger d = MulF(MulF(Two, p.Z), q.Z);
            BigInteger e = SubF(b, a);
            BigInteger f = SubF(d, c);
            BigInteger g = AddF(d, c);
            BigInteger h = AddF(b, a);
            Point r;
            r.X = MulF(e, f);
            r.Y = MulF(g, h);
            r.T = MulF(e, h);
            r.Z = MulF(f, g);
            return r;
        }

        private static Point DoublePoint(Point p)
        {
            BigInteger a = MulF(p.X, p.X);
            BigInteger b = MulF(p.Y, p.Y);
            BigInteger c = MulF(MulF(Two, p.Z), p.Z);
            BigInteger d = SubF(Zero, a);              // a кривой = -1
            BigInteger e = SubF(SubF(MulF(AddF(p.X, p.Y), AddF(p.X, p.Y)), a), b);
            BigInteger g = AddF(d, b);
            BigInteger f = SubF(g, c);
            BigInteger h = SubF(d, b);
            Point r;
            r.X = MulF(e, f);
            r.Y = MulF(g, h);
            r.T = MulF(e, h);
            r.Z = MulF(f, g);
            return r;
        }

        private static Point ScalarMult(Point p, BigInteger e)
        {
            Point r = Identity();
            for (int i = 255; i >= 0; i--)
            {
                r = DoublePoint(r);
                if (((e >> i) & One) == One) { r = AddPoints(r, p); }
            }
            return r;
        }

        private static bool SamePoint(Point p, Point q)
        {
            return MulF(p.X, q.Z) == MulF(q.X, p.Z) && MulF(p.Y, q.Z) == MulF(q.Y, p.Z);
        }

        private static byte[] EncodePoint(Point p)
        {
            BigInteger zi = InvF(p.Z);
            BigInteger x = MulF(p.X, zi);
            BigInteger y = MulF(p.Y, zi);
            byte[] encoded = ToLittleEndian(y, 32);
            if ((x & One) == One) { encoded[31] = (byte)(encoded[31] | 0x80); }
            return encoded;
        }

        private static bool Decode(byte[] encoded, out Point point)
        {
            point = Identity();
            if (encoded == null || encoded.Length != 32) { return false; }
            byte[] b = (byte[])encoded.Clone();
            int sign = (b[31] >> 7) & 1;
            b[31] = (byte)(b[31] & 0x7F);
            BigInteger y = FromLittleEndian(b, 0, 32);
            if (y >= P) { return false; }

            BigInteger y2 = MulF(y, y);
            BigInteger u = SubF(y2, One);
            BigInteger v = AddF(MulF(D, y2), One);
            BigInteger v3 = MulF(MulF(v, v), v);
            BigInteger v7 = MulF(MulF(v3, v3), v);
            BigInteger x = MulF(MulF(u, v3), BigInteger.ModPow(MulF(u, v7), (P - 5) / 8, P));
            BigInteger vxx = MulF(MulF(v, x), x);
            if (vxx == SubF(Zero, u))
            {
                x = MulF(x, SqrtM1);
            }
            else if (vxx != u)
            {
                return false;                          // точки нет
            }
            if (x.IsZero && sign == 1) { return false; }  // неканоническое кодирование
            if ((x & One) != new BigInteger(sign)) { x = SubF(Zero, x); }

            point = new Point();
            point.X = x;
            point.Y = y;
            point.Z = One;
            point.T = MulF(x, y);
            return true;
        }

        // --------------------------------------------------------- байты --

        private static byte[] Sha512(byte[] data)
        {
            using (SHA512 hash = SHA512.Create()) { return hash.ComputeHash(data); }
        }

        private static byte[] Concat(byte[] a, byte[] b)
        {
            byte[] r = new byte[a.Length + b.Length];
            Array.Copy(a, 0, r, 0, a.Length);
            Array.Copy(b, 0, r, a.Length, b.Length);
            return r;
        }

        private static byte[] ToLittleEndian(BigInteger value, int length)
        {
            byte[] raw = value.ToByteArray();
            if (raw.Length > length) { throw new InvalidOperationException("Ed25519: значение не помещается"); }
            byte[] result = new byte[length];
            Array.Copy(raw, result, raw.Length);
            return result;
        }

        private static BigInteger FromLittleEndian(byte[] data)
        {
            return FromLittleEndian(data, 0, data.Length);
        }

        private static BigInteger FromLittleEndian(byte[] data, int offset, int count)
        {
            // Конструктор BigInteger(byte[]) и так читает данные в порядке
            // little-endian, поэтому байты копируются как есть. Лишний нулевой
            // байт справа держит число положительным: без него старший бит
            // последнего байта прочитался бы как знак.
            byte[] positive = new byte[count + 1];
            Array.Copy(data, offset, positive, 0, count);
            return new BigInteger(positive);
        }

        // Зажим скаляра по RFC 8032 §5.1.5: обнуляем младшие три бита, стираем
        // бит 255 и выставляем бит 254.
        private static BigInteger Clamp(byte[] hash, int offset)
        {
            byte[] s = new byte[32];
            Array.Copy(hash, offset, s, 0, 32);
            s[0] = (byte)(s[0] & 248);
            s[31] = (byte)((s[31] & 63) | 64);
            return FromLittleEndian(s, 0, 32);
        }

        // --------------------------------------------------------- API ----

        // Публичный ключ (32 байта) из приватного seed (32 байта).
        public static byte[] PublicKeyFromSeed(byte[] seed)
        {
            if (seed == null || seed.Length != 32) { throw new ArgumentException("seed: ровно 32 байта"); }
            BigInteger a = Clamp(Sha512(seed), 0);
            return EncodePoint(ScalarMult(Base, a));
        }

        // Подпись RFC 8032 §5.1.6: 64 байта, R (32) + S (32).
        public static byte[] Sign(byte[] seed, byte[] message)
        {
            if (seed == null || seed.Length != 32) { throw new ArgumentException("seed: ровно 32 байта"); }
            if (message == null) { throw new ArgumentException("message: не null"); }

            byte[] h = Sha512(seed);
            BigInteger a = Clamp(h, 0);
            byte[] prefix = new byte[32];
            Array.Copy(h, 32, prefix, 0, 32);
            byte[] publicKey = EncodePoint(ScalarMult(Base, a));

            BigInteger r = ModL(FromLittleEndian(Sha512(Concat(prefix, message))));
            byte[] rEncoded = EncodePoint(ScalarMult(Base, r));

            BigInteger k = ModL(FromLittleEndian(Sha512(Concat(Concat(rEncoded, publicKey), message))));
            BigInteger s = ModL(r + k * a);

            return Concat(rEncoded, ToLittleEndian(s, 32));
        }

        // Уравнение без кофактора: S*B == R + k*A, 0 <= S < L.
        public static bool Verify(byte[] publicKey, byte[] message, byte[] signature)
        {
            if (publicKey == null || publicKey.Length != 32) { return false; }
            if (message == null || signature == null || signature.Length != 64) { return false; }

            Point a;
            if (!Decode(publicKey, out a)) { return false; }

            byte[] rEncoded = new byte[32];
            Array.Copy(signature, 0, rEncoded, 0, 32);
            Point r;
            if (!Decode(rEncoded, out r)) { return false; }

            BigInteger s = FromLittleEndian(signature, 32, 32);
            if (s >= L) { return false; }

            BigInteger k = ModL(FromLittleEndian(Sha512(Concat(Concat(rEncoded, publicKey), message))));

            Point left = ScalarMult(Base, s);
            Point right = AddPoints(r, ScalarMult(a, k));
            return SamePoint(left, right);
        }
    }
}
'@

    try {
        Add-Type -TypeDefinition $source -ReferencedAssemblies 'System.Numerics.dll' -ErrorAction Stop
    } catch {
        Stop-With ("не удалось скомпилировать встроенную реализацию Ed25519: " + $_.Exception.Message) 2
    }
}

function ConvertTo-ByteArray {
    # Пустая строка — законный случай: вектор RFC 8032 TEST 1 подписывает пустое
    # сообщение. Возврат через запятую: иначе PowerShell развернёт byte[] в
    # отдельные байты, и пустой массив дошёл бы как $null.
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Hex)
    $clean = $Hex.Trim()
    if (($clean.Length % 2) -ne 0) { Stop-With "hex-строка нечётной длины ($($clean.Length) символов)" 2 }
    if ($clean -notmatch '^[0-9a-fA-F]*$') { Stop-With "в строке есть не-hex символы: $clean" 2 }
    $bytes = New-Object byte[] ($clean.Length / 2)
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        $bytes[$i] = [Convert]::ToByte($clean.Substring($i * 2, 2), 16)
    }
    return , $bytes
}

function ConvertTo-Hex {
    param([Parameter(Mandatory = $true)][byte[]]$Bytes)
    $sb = New-Object System.Text.StringBuilder ($Bytes.Length * 2)
    foreach ($b in $Bytes) { [void]$sb.Append($b.ToString('x2')) }
    return $sb.ToString()
}

function ConvertTo-CppBytesLiteral {
    # Готовый фрагмент для вставки в исходники: публичный ключ — это
    # core::rules::kRuleSetPublicKey из SPEC §9.2. Формат — под .clang-format
    # проекта: отступ 4, 8 байт в строке (строка короче ColumnLimit 120), без
    # хвостовых пробелов и без висячей запятой.
    param([Parameter(Mandatory = $true)][byte[]]$Bytes)
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append("inline constexpr std::uint8_t kRuleSetPublicKey[32] = {`n    ")
    for ($i = 0; $i -lt $Bytes.Length; $i++) {
        if ($i -gt 0) { [void]$sb.Append(',') }
        if (($i % 8) -eq 0 -and $i -gt 0) { [void]$sb.Append("`n    ") }
        [void]$sb.Append(("0x{0:x2}" -f $Bytes[$i]))
    }
    [void]$sb.Append("`n};")
    return $sb.ToString()
}

function Get-Sha256Hex {
    param([Parameter(Mandatory = $true)][string]$Path)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $sha.ComputeHash([System.IO.File]::ReadAllBytes($Path))
    } finally {
        $sha.Dispose()
    }
    return (ConvertTo-Hex $bytes)
}

# --------------------------------------------------------- приватный ключ --

function Get-KeyEntropy {
    # Энтропия DPAPI: свой домен приложения. Не секрет — защищает от чужого
    # приложения, которое зашифровало что-то своей учётной записью без энтропии.
    return , ([System.Text.Encoding]::UTF8.GetBytes('MrProper rules signing key v1'))
}

function New-RuleSetKey {
    $seed = New-Object byte[] 32
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $rng.GetBytes($seed)
    } finally {
        $rng.Dispose()
    }
    $publicKey = [MrProper.RulesSigning.Ed25519]::PublicKeyFromSeed($seed)
    return [pscustomobject]@{ Seed = $seed; PublicKey = $publicKey }
}

function Save-PrivateKey {
    # Формат файла: 8 байт magic "MRPRK1\0\0", 32 байта публичного ключа,
    # дальше — seed, зашифрованный DPAPI (CurrentUser). Открытая половина в
    # файле не секрет: она нужна, чтобы ключ был самодостаточным для проверки
    # и чтобы нельзя было подсунуть файл с чужим seed.
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][byte[]]$Seed,
        [Parameter(Mandatory = $true)][byte[]]$PublicKey
    )
    Add-Type -AssemblyName System.Security -ErrorAction Stop
    $protected = [System.Security.Cryptography.ProtectedData]::Protect(
        $Seed, (Get-KeyEntropy), [System.Security.Cryptography.DataProtectionScope]::CurrentUser)

    $magic = [byte[]]@(0x4D, 0x52, 0x50, 0x52, 0x4B, 0x31, 0x00, 0x00)
    $buffer = New-Object byte[] (8 + 32 + $protected.Length)
    [Array]::Copy($magic, 0, $buffer, 0, 8)
    [Array]::Copy($PublicKey, 0, $buffer, 8, 32)
    [Array]::Copy($protected, 0, $buffer, 40, $protected.Length)

    $directory = Split-Path -Parent $Path
    if ($directory -and -not (Test-Path -LiteralPath $directory)) {
        [void](New-Item -ItemType Directory -Path $directory -Force)
    }
    [System.IO.File]::WriteAllBytes($Path, $buffer)

    # Права на файл — только владелец. Лучшее усилие: если icacls недоступен,
    # об этом честно сказано, а не сделано вид, что закрыто.
    try {
        $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
        & icacls.exe $Path /inheritance:r /grant:r "${identity}:(F)" | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Line "icacls вернул $LASTEXITCODE — права на ключ оставлены по умолчанию" 'warn'
        }
    } catch {
        Write-Line "не удалось сузить права на ключ: $($_.Exception.Message)" 'warn'
    }
    # Массив с seed в памяти сколько можно живёт короче.
    [Array]::Clear($Seed, 0, $Seed.Length)
}

function Read-PrivateKey {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Stop-With "приватного ключа нет: $Path (создайте: -Action NewKey)" 1
    }
    $buffer = [System.IO.File]::ReadAllBytes($Path)
    if ($buffer.Length -lt 40) { Stop-With "файл ключа повреждён (короче заголовка): $Path" 1 }
    $magic = @(0x4D, 0x52, 0x50, 0x52, 0x4B, 0x31, 0x00, 0x00)
    for ($i = 0; $i -lt 8; $i++) {
        if ($buffer[$i] -ne $magic[$i]) { Stop-With "это не файл ключа MrProper: $Path" 2 }
    }
    $publicKey = New-Object byte[] 32
    [Array]::Copy($buffer, 8, $publicKey, 0, 32)
    $protected = New-Object byte[] ($buffer.Length - 40)
    [Array]::Copy($buffer, 40, $protected, 0, $protected.Length)

    Add-Type -AssemblyName System.Security -ErrorAction Stop
    try {
        $seed = [System.Security.Cryptography.ProtectedData]::Unprotect(
            $protected, (Get-KeyEntropy), [System.Security.Cryptography.DataProtectionScope]::CurrentUser)
    } catch {
        # DPAPI привязан к учётной записи и машине: ключ, созданный другим
        # пользователем или на другой машине, здесь не расшифруется — и это
        # правильное поведение, а не ошибка скрипта.
        Stop-With ("ключ не расшифровывается под текущей учётной записью ($($_.Exception.Message)). " +
            "DPAPI-ключ привязан к пользователю и машине: скопируйте на другую машину через " +
            "безопасный канал и пересоздайте ключ") 1
    }
    if ($seed.Length -ne 32) { Stop-With "в файле ключа повреждён seed: $Path" 1 }

    # Открытая половина обязана соответствовать приватной: иначе файл подменили.
    $derived = [MrProper.RulesSigning.Ed25519]::PublicKeyFromSeed($seed)
    if ((ConvertTo-Hex $derived) -ne (ConvertTo-Hex $publicKey)) {
        Stop-With "файл ключа не согласован: публичная половина не соответствует приватной" 1
    }
    return [pscustomobject]@{ Seed = $seed; PublicKey = $publicKey }
}

function Assert-KeyOutsideRepo {
    param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][string]$RepoRoot)
    if (Test-IsInside -Path $Path -Root $RepoRoot) {
        Stop-With ("приватный ключ не должен лежать в репозитории: $Path (ADR-008). " +
            "Держите его в %USERPROFILE%\.mrproper\ или в зашифрованном хранилище") 2
    }
}

# --------------------------------------------------------------- манифест --

function Get-JsonProperty {
    # Доступ по имени без ошибки на отсутствующем поле: под StrictMode
    # обращение к несуществующему свойству исключение и бросает, а неизвестное
    # или необязательное поле здесь — норма.
    param($Object, [Parameter(Mandatory = $true)][string]$Name)
    if ($null -eq $Object) { return $null }
    if ($Object -is [System.Collections.IDictionary]) {
        if ($Object.Contains($Name)) { return $Object[$Name] }
        return $null
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Test-IsSafeRelativePath {
    # Повтор правил isSafeRelativePath из src/core/rulesync.cpp: путь внутри
    # набора, без «..», без диска, без хвостовых пробелов и точек (Windows их
    # молча срезает, и «a.json.» указал бы на другой файл).
    param([string]$Path)
    if ([string]::IsNullOrEmpty($Path)) { return $false }
    if ($Path.Length -gt 200) { return $false }
    if ($Path.StartsWith('/') -or $Path.StartsWith('\')) { return $false }
    foreach ($c in '<>:"|?*') {
        if ($Path.Contains($c)) { return $false }
    }
    foreach ($ch in $Path.ToCharArray()) {
        if ([int]$ch -lt 0x20 -or [int]$ch -eq 0x7F) { return $false }
    }
    foreach ($component in $Path.Split([char[]]@('/', '\'), [System.StringSplitOptions]::RemoveEmptyEntries)) {
        if ($component -eq '.' -or $component -eq '..') { return $false }
        if ($component.EndsWith(' ') -or $component.EndsWith('.')) { return $false }
    }
    # Разделители вида «//» или хвостовой тоже запрещены: пустой компонент
    # означает, что имя файла неоднозначно.
    if ($Path.Contains('//') -or $Path.Contains('\\') -or $Path.EndsWith('/') -or $Path.EndsWith('\')) {
        return $false
    }
    return $true
}

function Get-RequiredString {
    # Строковые поля манифеста: requireString в rulesync.cpp отвергает не строку
    # и пустое значение. Подпись должна защищать ровно тот набор, который
    # примет приложение, — иначе получится подпись под манифест, который
    # отвергнут на первой же проверке.
    param($Value, [Parameter(Mandatory = $true)][string]$Name, [Parameter(Mandatory = $true)][string]$Origin)
    if ($Value -isnot [string]) {
        Stop-With "${Origin}: поле `"$Name`" должно быть строкой" 1
    }
    if ($Value.Length -eq 0) {
        Stop-With "${Origin}: поле `"$Name`" пустое" 1
    }
    return $Value
}

function Get-VersionComponents {
    # Повтор versionComponents из rulesync.cpp: только цифры, никакого тихого
    # превращения «1.0.0-бета» в ноль.
    param([Parameter(Mandatory = $true)][string]$Version)
    $parts = New-Object System.Collections.Generic.List[long]
    foreach ($piece in $Version.Split('.')) {
        if ($piece.Length -eq 0) { Stop-With "версия `"$Version`" пуста или заканчивается точкой" 1 }
        $value = 0L
        foreach ($ch in $piece.ToCharArray()) {
            if ($ch -lt '0' -or $ch -gt '9') { Stop-With "версия `"$Version`" не числовая" 1 }
            $value = $value * 10 + [int][char]$ch
            if ($value -gt 1000000000) { Stop-With "компонент версии в `"$Version`" неправдоподобно велик" 1 }
        }
        $parts.Add($value)
    }
    return , $parts
}

function Compare-Version {
    param([Parameter(Mandatory = $true)][string]$Left, [Parameter(Mandatory = $true)][string]$Right)
    $a = Get-VersionComponents $Left
    $b = Get-VersionComponents $Right
    $count = [Math]::Max($a.Count, $b.Count)
    for ($i = 0; $i -lt $count; $i++) {
        $left = 0L
        $right = 0L
        if ($i -lt $a.Count) { $left = $a[$i] }
        if ($i -lt $b.Count) { $right = $b[$i] }
        if ($left -lt $right) { return -1 }
        if ($left -gt $right) { return 1 }
    }
    return 0
}

function Read-Manifest {
    # Байты манифеста читаются как есть: подписывается ровно тот текст, который
    # лежит в файле, без перекодировки и переформатирования.
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Origin
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Stop-With "манифест не найден: $Path (SPEC §9.2: набор версионируется манифестом)" 1
    }
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -gt 1048576) { Stop-With "манифест больше 1 МиБ ($($bytes.Length) байт) — это не набор правил" 1 }
    $text = [System.Text.Encoding]::UTF8.GetString($bytes)
    try {
        $document = $text | ConvertFrom-Json -ErrorAction Stop
    } catch {
        Stop-With "$Origin не разбирается как JSON: $($_.Exception.Message)" 1
    }
    if ($null -eq $document -or $document -is [string] -or $document -is [System.Collections.IEnumerable]) {
        Stop-With "${Origin}: ожидался объект манифеста" 1
    }

    $known = @('schema', 'schemaVersion', 'version', 'minAppVersion', 'files')
    foreach ($property in $document.PSObject.Properties) {
        if ($known -notcontains $property.Name) {
            Stop-With "${Origin}: неизвестное поле `"$($property.Name)`"" 1
        }
    }

    $schema = Get-JsonProperty $document 'schema'
    $schemaVersion = Get-JsonProperty $document 'schemaVersion'
    if ($null -ne $schema -and $null -ne $schemaVersion -and ([int]$schema -ne [int]$schemaVersion)) {
        Stop-With "${Origin}: поля schema и schemaVersion расходятся" 1
    }
    $schemaValue = $null
    if ($null -ne $schema) { $schemaValue = [int]$schema }
    elseif ($null -ne $schemaVersion) { $schemaValue = [int]$schemaVersion }
    if ($null -ne $schemaValue -and $schemaValue -ne 1) {
        Stop-With "${Origin}: версия схемы $schemaValue, поддерживается только 1" 1
    }

    $versionValue = Get-JsonProperty $document 'version'
    if ($null -eq $versionValue) { Stop-With "${Origin}: нет поля version" 1 }
    $version = Get-RequiredString $versionValue 'version' $Origin
    if ($version.Length -gt 64) {
        Stop-With "${Origin}: версия набора обязательна и не длиннее 64 символов" 1
    }
    foreach ($ch in $version.ToCharArray()) {
        if ([int]$ch -lt 0x20 -or [int]$ch -eq 0x7F) {
            Stop-With "${Origin}: в версии набора есть управляющие символы" 1
        }
    }

    $minAppVersion = Get-JsonProperty $document 'minAppVersion'
    if ($null -ne $minAppVersion) {
        if ($minAppVersion -isnot [string]) { Stop-With "${Origin}: minAppVersion должен быть строкой" 1 }
        $minAppVersion = [string]$minAppVersion
        if ($minAppVersion.Length -gt 0) { [void](Get-VersionComponents $minAppVersion) }
    }

    $files = Get-JsonProperty $document 'files'
    if ($null -eq $files) { Stop-With "${Origin}: нет поля files" 1 }
    if ($files -is [string]) { Stop-With "${Origin}: files должен быть массивом" 1 }
    if ($files -is [System.Collections.IEnumerable]) {
        $list = @($files)
    } elseif ($null -ne (Get-JsonProperty $files 'path')) {
        # PowerShell 5.1 разворачивает массив из одного элемента в сам элемент:
        # запись с полем path читается как массив из одной записи.
        $list = @($files)
    } else {
        Stop-With "${Origin}: files должен быть массивом записей" 1
    }
    if ($list.Count -eq 0) { Stop-With "${Origin}: files пуст — набор без файлов не подписывается" 1 }
    if ($list.Count -gt 4096) { Stop-With "${Origin}: в files $($list.Count) записей, граница — 4096" 1 }

    $entries = New-Object System.Collections.Generic.List[object]
    $seen = New-Object System.Collections.Generic.HashSet[string] ([StringComparer]::OrdinalIgnoreCase)
    foreach ($item in $list) {
        $itemKnown = @('path', 'sha256', 'size')
        foreach ($property in $item.PSObject.Properties) {
            if ($itemKnown -notcontains $property.Name) {
                Stop-With "${Origin}: неизвестное поле `"$($property.Name)`" в записи files" 1
            }
        }
        if ($null -eq (Get-JsonProperty $item 'path')) { Stop-With "${Origin}: нет поля path в записи files" 1 }
        $rawPath = Get-RequiredString (Get-JsonProperty $item 'path') 'path' $Origin
        $normalized = $rawPath.Replace('\', '/')
        if (-not (Test-IsSafeRelativePath $normalized)) {
            Stop-With ("$Origin`: недопустимый путь `"$normalized`" " +
                '(нужен относительный путь внутри набора без `"..`")') 1
        }
        if (-not $seen.Add($normalized)) {
            Stop-With "$Origin`: файл `"$normalized`" перечислен дважды" 1
        }
        if ($null -eq (Get-JsonProperty $item 'sha256')) { Stop-With "${Origin}: нет поля sha256 в записи files" 1 }
        $hash = (Get-RequiredString (Get-JsonProperty $item 'sha256') 'sha256' $Origin)
        if ($hash -notmatch '^[0-9a-fA-F]{64}$') {
            Stop-With "$Origin`: в `"$normalized`" ожидается SHA-256 — 64 hex-символа, получено `"$hash`"" 1
        }
        $sizeValue = Get-JsonProperty $item 'size'
        $size = $null
        if ($null -ne $sizeValue) {
            $size = [long]$sizeValue
            if ($size -lt 0) { Stop-With "$Origin`: в `"$normalized`" отрицательный размер" 1 }
        }
        $entries.Add([pscustomobject]@{
                Path = $normalized
                Sha256 = $hash.ToLowerInvariant()
                Size = $size
                SizeDeclared = ($null -ne $size)
            })
    }

    return [pscustomobject]@{
        Version      = $version
        MinAppVersion = $minAppVersion
        Files        = $entries
        Bytes        = $bytes
    }
}

function Test-RuleSetFiles {
    # Сверка манифеста с фактическим набором: это ровно шаги 4-5 SPEC §9.2 п.2,
    # которые приложение делает после проверки подписи.
    param(
        [Parameter(Mandatory = $true)]$Manifest,
        [Parameter(Mandatory = $true)][string]$RulesDirectory,
        [Parameter(Mandatory = $true)][string]$Origin
    )
    $problems = New-Object System.Collections.Generic.List[string]
    $details = New-Object System.Collections.Generic.List[object]

    if (-not (Test-Path -LiteralPath $RulesDirectory -PathType Container)) {
        Stop-With "каталог набора не найден: $RulesDirectory" 1
    }

    foreach ($entry in $Manifest.Files) {
        $full = Join-Path $RulesDirectory ($entry.Path.Replace('/', '\'))
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
            $problems.Add("файл из манифеста отсутствует: $($entry.Path)")
            $details.Add([pscustomobject]@{ Path = $entry.Path; State = 'missing' })
            continue
        }
        $actual = [System.IO.FileInfo]::new($full)
        $actualSize = $actual.Length
        if ($entry.SizeDeclared -and $actualSize -ne $entry.Size) {
            $problems.Add("размер не совпал: $($entry.Path) — манифест $($entry.Size), файл $actualSize")
            $details.Add([pscustomobject]@{ Path = $entry.Path; State = 'size' })
            continue
        }
        $actualHash = Get-Sha256Hex $full
        if ($actualHash -ne $entry.Sha256) {
            $problems.Add("SHA-256 не совпал: $($entry.Path) — манифест $($entry.Sha256), файл $actualHash")
            $details.Add([pscustomobject]@{ Path = $entry.Path; State = 'hash' })
            continue
        }
        $details.Add([pscustomobject]@{ Path = $entry.Path; State = 'ok' })
    }

    # Файл, не перечисленный в манифесте, — отказ: приложение всё равно его
    # отвергнет (requireKnownFilesOnly), а подписывать набор, который нельзя
    # применить, бессмысленно.
    if (-not $AllowUnlisted) {
        $known = New-Object System.Collections.Generic.HashSet[string] ([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $Manifest.Files) { [void]$known.Add($entry.Path.Replace('/', '\')) }
        [void]$known.Add('manifest.json')
        foreach ($file in (Get-ChildItem -LiteralPath $RulesDirectory -File -Filter '*.json')) {
            if (-not $known.Contains($file.Name)) {
                $problems.Add("файл не перечислен в манифесте: $($file.Name)")
                $details.Add([pscustomobject]@{ Path = $file.Name; State = 'unlisted' })
            }
        }
    }

    return [pscustomobject]@{
        Ok        = ($problems.Count -eq 0)
        Problems  = $problems
        Details   = $details
        Checked   = $Manifest.Files.Count
        Origin    = $Origin
    }
}

function Assert-AppVersion {
    param([string]$ManifestMinAppVersion)
    if ([string]::IsNullOrEmpty($ManifestMinAppVersion)) { return }
    if (-not $AppVersion) {
        Write-Line "minAppVersion=`"$ManifestMinAppVersion`": сверка пропущена, -AppVersion не задан" 'warn'
        return
    }
    if ((Compare-Version $AppVersion $ManifestMinAppVersion) -lt 0) {
        Stop-With ("приложение $AppVersion старее, чем требует набор (minAppVersion=" +
            "$ManifestMinAppVersion) — такой набор обновление не примет") 1
    }
}

# ------------------------------------------------------------------ действия --

function Invoke-ActionNewKey {
    param($Paths)
    Initialize-Crypto
    Assert-KeyOutsideRepo -Path $Paths.KeyPath -RepoRoot $Paths.RepoRoot
    if ((Test-Path -LiteralPath $Paths.KeyPath) -and -not $Force) {
        Stop-With ("ключ уже есть: $($Paths.KeyPath); перезапись только с -Force, " +
            'и после смены ключа прежние наборы останутся неподписываемыми') 1
    }
    $key = New-RuleSetKey
    Save-PrivateKey -Path $Paths.KeyPath -Seed $key.Seed -PublicKey $key.PublicKey

    $hex = ConvertTo-Hex $key.PublicKey
    $fingerprint = (Get-Sha256Hex $Paths.KeyPath)
    $result = [pscustomobject]@{
        action       = 'NewKey'
        ok           = $true
        keyPath      = $Paths.KeyPath
        publicKeyHex = $hex
        publicKeyBase64 = [Convert]::ToBase64String($key.PublicKey)
        keyFileSha256 = $fingerprint
    }
    if (-not $Json) {
        Write-Line "Ключ создан: $($Paths.KeyPath)" 'ok'
        Write-Line "Файл закрыт DPAPI (текущая учётная запись, эта машина) и лежит вне репозитория."
        Write-Line "Отпечаток файла ключа (SHA-256): $fingerprint"
        Write-Line ''
        Write-Line 'Публичный ключ (base64, 32 байта) — это core::rules::kRuleSetPublicKey из SPEC §9.2:'
        Write-Line ("  " + [Convert]::ToBase64String($key.PublicKey))
        Write-Line "Публичный ключ (hex):" 'info'
        Write-Line ("  " + $hex)
        Write-Line 'Фрагмент для исходников:' 'info'
        Write-Line (ConvertTo-CppBytesLiteral $key.PublicKey)
        Write-Line ''
        Write-Line 'Ключ в репозиторий не коммитится. Публичный ключ — коммитится.' 'warn'
    }
    return $result
}

function Invoke-ActionPublicKey {
    param($Paths)
    Initialize-Crypto
    Assert-KeyOutsideRepo -Path $Paths.KeyPath -RepoRoot $Paths.RepoRoot
    $key = Read-PrivateKey -Path $Paths.KeyPath
    $hex = ConvertTo-Hex $key.PublicKey
    $result = [pscustomobject]@{
        action          = 'PublicKey'
        ok              = $true
        publicKeyHex    = $hex
        publicKeyBase64 = [Convert]::ToBase64String($key.PublicKey)
    }
    [Array]::Clear($key.Seed, 0, $key.Seed.Length)
    if (-not $Json) {
        Write-Line 'Публичный ключ (base64, 32 байта):'
        Write-Line ("  " + [Convert]::ToBase64String($key.PublicKey))
        Write-Line 'Публичный ключ (hex):' 'info'
        Write-Line ("  " + $hex)
    }
    return $result
}

function Invoke-ActionCheckManifest {
    param($Paths)
    $manifest = Read-Manifest -Path $Paths.ManifestPath -Origin (Split-Path -Leaf $Paths.ManifestPath)
    Assert-AppVersion -ManifestMinAppVersion $manifest.MinAppVersion
    $files = Test-RuleSetFiles -Manifest $manifest -RulesDirectory $Paths.RulesDir -Origin 'набор'
    $result = [pscustomobject]@{
        action   = 'CheckManifest'
        ok       = $files.Ok
        version  = $manifest.Version
        checked  = $files.Checked
        problems = @($files.Problems)
    }
    if (-not $Json) {
        Write-Line "Манифест: $($Paths.ManifestPath)" 'head'
        Write-Line "версия набора: $($manifest.Version); файлов в манифесте: $($manifest.Files.Count)"
        if ($files.Ok) {
            Write-Line "все файлы на месте, SHA-256 совпал ($($files.Checked) шт.)" 'ok'
        } else {
            foreach ($problem in $files.Problems) { Write-Line $problem 'bad' }
        }
    }
    return $result
}

function Invoke-ActionSign {
    param($Paths)
    Initialize-Crypto
    Assert-KeyOutsideRepo -Path $Paths.KeyPath -RepoRoot $Paths.RepoRoot
    $manifest = Read-Manifest -Path $Paths.ManifestPath -Origin (Split-Path -Leaf $Paths.ManifestPath)
    Assert-AppVersion -ManifestMinAppVersion $manifest.MinAppVersion

    # Подписывать имеет смысл только то, что приложение примет: сначала сходится
    # сам набор, потом уже подпись.
    $files = Test-RuleSetFiles -Manifest $manifest -RulesDirectory $Paths.RulesDir -Origin 'набор'
    if (-not $files.Ok) {
        foreach ($problem in $files.Problems) { Write-Line $problem 'bad' }
        Stop-With "набор не сходится с манифестом — подпись не создана" 1
    }

    $key = Read-PrivateKey -Path $Paths.KeyPath
    $signature = [MrProper.RulesSigning.Ed25519]::Sign($key.Seed, $manifest.Bytes)
    $base64 = [Convert]::ToBase64String($signature)

    # Сверка собственной подписи публичным ключом до записи на диск: файл
    # подписи, который нельзя проверить, опаснее отсутствующего.
    if (-not [MrProper.RulesSigning.Ed25519]::Verify($key.PublicKey, $manifest.Bytes, $signature)) {
        Stop-With 'подпись не прошла собственную проверку — файл не записан' 1
    }
    [Array]::Clear($key.Seed, 0, $key.Seed.Length)

    # Запись атомарная: временный файл рядом и замена, чтобы оборванная
    # подпись не осталась вместо рабочей.
    $temporary = "$($Paths.SignaturePath).tmp"
    [System.IO.File]::WriteAllText($temporary, $base64, (New-Object System.Text.UTF8Encoding($false)))
    Move-Item -LiteralPath $temporary -Destination $Paths.SignaturePath -Force

    # Контроль записи: читаем файл обратно и проверяем уже записанную подпись.
    # Оборванная запись или сбившееся перемещение файла поймаются здесь, а не
    # у того, кто через месяц скачает набор по тегу.
    $written = [Convert]::FromBase64String(([System.IO.File]::ReadAllText($Paths.SignaturePath)).Trim())
    if (-not [MrProper.RulesSigning.Ed25519]::Verify($key.PublicKey, $manifest.Bytes, $written)) {
        Stop-With "записанный файл подписи не проходит проверку — удалите $temporary и $Paths.SignaturePath" 1
    }

    $result = [pscustomobject]@{
        action     = 'Sign'
        ok         = $true
        version    = $manifest.Version
        checked    = $files.Checked
        signature  = $base64
        signaturePath = $Paths.SignaturePath
        publicKeyBase64 = [Convert]::ToBase64String($key.PublicKey)
    }
    if (-not $Json) {
        Write-Line "Подписан набор $($manifest.Version); файлов в манифесте: $($files.Checked); хеши совпали" 'ok'
        Write-Line "файл подписи: $($Paths.SignaturePath)"
        Write-Line "подпись (base64, 64 байта): $base64"
        Write-Line 'Опубликуйте набор по тегу rules/vX.Y: rules.sig едет рядом с манифестом.' 'info'
    }
    return $result
}

function Invoke-ActionVerify {
    param($Paths)
    Initialize-Crypto

    $publicKey = $null
    if ($PublicKeyBase64) {
        try {
            $publicKey = [Convert]::FromBase64String($PublicKeyBase64.Trim())
        } catch {
            Stop-With "-PublicKeyBase64 не декодируется как base64" 2
        }
        if ($publicKey.Length -ne 32) {
            Stop-With "публичный ключ должен быть 32 байта, получено $($publicKey.Length)" 2
        }
    } else {
        Assert-KeyOutsideRepo -Path $Paths.KeyPath -RepoRoot $Paths.RepoRoot
        $key = Read-PrivateKey -Path $Paths.KeyPath
        $publicKey = $key.PublicKey
        [Array]::Clear($key.Seed, 0, $key.Seed.Length)
    }

    if (-not (Test-Path -LiteralPath $Paths.SignaturePath -PathType Leaf)) {
        Stop-With "файл подписи не найден: $($Paths.SignaturePath)" 1
    }
    $signatureText = ([System.IO.File]::ReadAllText($Paths.SignaturePath)).Trim()
    if ($signatureText.Length -eq 0) { Stop-With "файл подписи пуст: $($Paths.SignaturePath)" 1 }
    try {
        $signature = [Convert]::FromBase64String($signatureText)
    } catch {
        Stop-With "файл подписи не декодируется как base64: $($Paths.SignaturePath)" 1
    }
    if ($signature.Length -ne 64) {
        Stop-With "подпись должна быть 64 байта, получено $($signature.Length)" 1
    }

    $manifest = Read-Manifest -Path $Paths.ManifestPath -Origin (Split-Path -Leaf $Paths.ManifestPath)
    Assert-AppVersion -ManifestMinAppVersion $manifest.MinAppVersion

    $signatureOk = [MrProper.RulesSigning.Ed25519]::Verify($publicKey, $manifest.Bytes, $signature)
    $files = Test-RuleSetFiles -Manifest $manifest -RulesDirectory $Paths.RulesDir -Origin 'набор'
    $ok = $signatureOk -and $files.Ok
    $problems = New-Object System.Collections.Generic.List[string]
    if (-not $signatureOk) { $problems.Add("подпись манифеста не сошлась с публичным ключом ($signatureText)") }
    foreach ($problem in $files.Problems) { $problems.Add($problem) }

    $result = [pscustomobject]@{
        action   = 'Verify'
        ok       = $ok
        version  = $manifest.Version
        signatureOk = $signatureOk
        checked  = $files.Checked
        problems = @($problems)
    }
    if (-not $Json) {
        Write-Line "Проверка набора $($manifest.Version)" 'head'
        if ($signatureOk) {
            Write-Line "подпись манифеста верна, публичный ключ $((ConvertTo-Hex $publicKey))" 'ok'
        } else {
            Write-Line 'подпись манифеста не сошлась с публичным ключом' 'bad'
        }
        if ($files.Ok) {
            Write-Line "файлы на месте, SHA-256 совпал ($($files.Checked) шт.)" 'ok'
        } else {
            foreach ($problem in $files.Problems) { Write-Line $problem 'bad' }
        }
    }
    return $result
}

function Invoke-ActionSelfTest {
    Initialize-Crypto
    if (-not $Json) {
        Write-Line 'Самопроверка Ed25519 по эталонным векторам RFC 8032 §7.1' 'head'
    }

    # Векторы приведены буквально из RFC 8032: seed, публичный ключ, сообщение
    # и подпись. Пока они не сходятся, подписывать набор нельзя.
    $vectors = @(
        [pscustomobject]@{
            Name = 'TEST 1 (пустое сообщение)'
            Seed = '9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60'
            Public = 'd75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a'
            Message = ''
            Signature = 'e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b'
        },
        [pscustomobject]@{
            Name = 'TEST 2 (сообщение 0x72)'
            Seed = '4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb'
            Public = '3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c'
            Message = '72'
            Signature = '92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00'
        },
        [pscustomobject]@{
            Name = 'TEST 3 (сообщение 0xaf82)'
            Seed = 'c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7'
            Public = 'fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025'
            Message = 'af82'
            Signature = '6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a'
        }
    )

    $failures = New-Object System.Collections.Generic.List[string]
    foreach ($vector in $vectors) {
        $seed = ConvertTo-ByteArray $vector.Seed
        $public = ConvertTo-ByteArray $vector.Public
        $message = ConvertTo-ByteArray $vector.Message
        $signature = ConvertTo-ByteArray $vector.Signature

        $derived = [MrProper.RulesSigning.Ed25519]::PublicKeyFromSeed($seed)
        if ((ConvertTo-Hex $derived) -ne (ConvertTo-Hex $public)) {
            $failures.Add("$($vector.Name): публичный ключ не совпал")
        }
        $produced = [MrProper.RulesSigning.Ed25519]::Sign($seed, $message)
        if ((ConvertTo-Hex $produced) -ne (ConvertTo-Hex $signature)) {
            $failures.Add("$($vector.Name): подпись не совпала")
        }
        if (-not [MrProper.RulesSigning.Ed25519]::Verify($public, $message, $signature)) {
            $failures.Add("$($vector.Name): эталонная подпись не проверяется")
        }
        # Подделка: тот же ключ, изменённый байт сообщения.
        $tampered = New-Object byte[] ($message.Length + 1)
        [Array]::Copy($message, 0, $tampered, 0, $message.Length)
        $tampered[$message.Length] = 0x41
        if ([MrProper.RulesSigning.Ed25519]::Verify($public, $tampered, $signature)) {
            $failures.Add("$($vector.Name): подделка под сообщение прошла проверку")
        }
        if (-not $Json) { Write-Line "$($vector.Name): сходится" 'ok' }
    }

    # Собственный ключ: подпись проверяется и не проходит с чужим ключом.
    $key = New-RuleSetKey
    $message = [System.Text.Encoding]::UTF8.GetBytes('MrProper rules manifest')
    $own = [MrProper.RulesSigning.Ed25519]::Sign($key.Seed, $message)
    if (-not [MrProper.RulesSigning.Ed25519]::Verify($key.PublicKey, $message, $own)) {
        $failures.Add('собственная подпись не проверяется своим же ключом')
    }
    $other = New-RuleSetKey
    if ([MrProper.RulesSigning.Ed25519]::Verify($other.PublicKey, $message, $own)) {
        $failures.Add('подпись прошла проверку чужим ключом')
    }
    [Array]::Clear($key.Seed, 0, $key.Seed.Length)
    [Array]::Clear($other.Seed, 0, $other.Seed.Length)

    $result = [pscustomobject]@{
        action   = 'SelfTest'
        ok       = ($failures.Count -eq 0)
        vectors  = $vectors.Count
        problems = @($failures)
    }
    if (-not $Json) {
        if ($failures.Count -eq 0) {
            Write-Line 'Ed25519 соответствует RFC 8032, подделка не проходит.' 'ok'
        } else {
            foreach ($failure in $failures) { Write-Line $failure 'bad' }
        }
    }
    return $result
}

# ------------------------------------------------------------------ запуск --

$paths = $null
$report = $null
try {
    $paths = Get-ResolvedPaths
    switch ($Action) {
        'NewKey' { $report = Invoke-ActionNewKey -Paths $paths }
        'Sign' { $report = Invoke-ActionSign -Paths $paths }
        'Verify' { $report = Invoke-ActionVerify -Paths $paths }
        'PublicKey' { $report = Invoke-ActionPublicKey -Paths $paths }
        'CheckManifest' { $report = Invoke-ActionCheckManifest -Paths $paths }
        'SelfTest' { $report = Invoke-ActionSelfTest }
        default { Stop-With "неизвестное действие: $Action" 2 }
    }
    if ($Json) { Write-JsonLine $report }
    if ($report.ok) { exit 0 }
    exit 1
} catch {
    $message = $_.Exception.Message
    if ($Json) {
        Write-JsonLine ([pscustomobject]@{ action = $Action; ok = $false; error = $message })
    } else {
        Write-Host "[ошибка] $message" -ForegroundColor Red
    }
    exit $script:ExitCode
}
