# ==================================================================
#  font_gen.ps1  -  generate 16x16 Chinese dot-matrix font for the
#                   CH592X mini games (replaces nothing else).
#
#  Usage:   powershell -ExecutionPolicy Bypass -File font_gen.ps1
#
#  Input :  font_texts.txt   (GBK, lines:  GTXT_ID=Chinese words)
#  Output:  game_font16.h / game_font16.c   glyph bitmaps (32 B each)
#           game_text.h   / game_text.c     text table (id -> glyphs)
#           font_preview.png                visual check sheet
#           font_report.txt                 index / codepoint / dots
#
#  NOTE: keep this file pure ASCII (only font_texts.txt holds Chinese).
# ==================================================================

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root      = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
$txtFile   = Join-Path $root 'font_texts.txt'

$GLYPH_W    = 16
$GLYPH_H    = 16
$BYTES_PER  = 32          # 16 rows x 2 bytes, MSB = leftmost pixel
$FONT_NAME  = 'SimSun'
$FONT_SIZE  = 16
$OFFSET_X   = 0
$OFFSET_Y   = -1          # fine tune vertical placement
$PREVIEW_Z  = 3           # preview zoom

$gbk = [System.Text.Encoding]::GetEncoding(936)

# ---------------------------------------------------------------- input
if (-not (Test-Path $txtFile)) { throw "missing $txtFile" }
$raw = $gbk.GetString([System.IO.File]::ReadAllBytes($txtFile))
$lines = $raw -split "`r`n|`n"

$entries = New-Object System.Collections.ArrayList     # @(id, phrase)
$seenIds = @{}
foreach ($ln in $lines) {
    $t = $ln.Trim()
    if ($t -eq '' -or $t.StartsWith('#')) { continue }
    $p = $t.IndexOf('=')
    if ($p -lt 1) { throw "bad line: $t" }
    $id     = $t.Substring(0, $p).Trim()
    $phrase = $t.Substring($p + 1).Trim()
    if ($phrase -eq '') { continue }
    if ($seenIds.ContainsKey($id)) { throw "duplicate id: $id" }
    $seenIds[$id] = 1
    [void]$entries.Add(@($id, $phrase))
}
if ($entries.Count -eq 0) { throw 'no entries in font_texts.txt' }

# ------------------------------------------------- unique glyph order
$chars = New-Object System.Collections.ArrayList
$gidx  = @{}                       # char -> glyph index
foreach ($e in $entries) {
    foreach ($ch in $e[1].ToCharArray()) {
        if (-not $gidx.ContainsKey($ch)) {
            $gidx[$ch] = $chars.Count
            [void]$chars.Add($ch)
        }
    }
}

# ------------------------------------------------------------ render
$font  = New-Object System.Drawing.Font($FONT_NAME, $FONT_SIZE, [System.Drawing.GraphicsUnit]::Pixel)
$brush = [System.Drawing.Brushes]::White
$sf    = [System.Drawing.StringFormat]::GenericTypographic

$glyphs = @()                      # array of byte[32]
$report = New-Object System.Collections.ArrayList
$idx = 0
foreach ($ch in $chars) {
    $bmp = New-Object System.Drawing.Bitmap($GLYPH_W, $GLYPH_H)
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Black)
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::SingleBitPerPixelGridFit
    $g.DrawString([string]$ch, $font, $brush, [single]$OFFSET_X, [single]$OFFSET_Y, $sf)
    $g.Dispose()

    $bytes = New-Object 'byte[]' $BYTES_PER
    $dots  = 0
    for ($y = 0; $y -lt $GLYPH_H; $y++) {
        for ($x = 0; $x -lt $GLYPH_W; $x++) {
            $c = $bmp.GetPixel($x, $y)
            if ($c.R -gt 127) {
                $bytes[$y * 2 + [int][math]::Floor($x / 8)] =
                    $bytes[$y * 2 + [int][math]::Floor($x / 8)] -bor (0x80 -shr ($x % 8))
                $dots++
            }
        }
    }
    $bmp.Dispose()
    $glyphs += , $bytes
    $cp = [int][char]$ch
    [void]$report.Add(('{0,3}  0x{1:X4}  {2}  dots={3}' -f $idx, $cp, [string]$ch, $dots))
    $idx++
}

# ------------------------------------------------------- .h / .c out
function Write-Gbk([string]$path, [string]$text) {
    [System.IO.File]::WriteAllBytes($path, $gbk.GetBytes($text))
}

# ---- game_font16.h
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('/**************************************************************************')
[void]$sb.AppendLine(' * game_font16.h - 16x16 中文字模（由 font_gen.ps1 自动生成，不要手改）')
[void]$sb.AppendLine(' *')
[void]$sb.AppendLine(' *   * 数据放在 Flash（const），不占 SRAM：每个字 32 字节')
[void]$sb.AppendLine(' *   * 点阵格式：16 行，每行 2 字节，高位在左（MSB first）')
[void]$sb.AppendLine(' *   * 加字/改字请编辑 font_texts.txt 后重新跑 font_gen.ps1')
[void]$sb.AppendLine(' **************************************************************************/')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('#ifndef __GAME_FONT16_H__')
[void]$sb.AppendLine('#define __GAME_FONT16_H__')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('#include <stdint.h>')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('#define GAME_GLYPH_W            16')
[void]$sb.AppendLine('#define GAME_GLYPH_H            16')
[void]$sb.AppendLine('#define GAME_GLYPH_BYTES        32')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('/* 每个汉字一个编号，名字就是它的 Unicode 码位 */')
[void]$sb.AppendLine('typedef enum')
[void]$sb.AppendLine('{')
foreach ($ch in $chars) {
    $cp = [int][char]$ch
    [void]$sb.AppendLine(('    GF_U{0:X4} = {1},        /* {2} */' -f $cp, $gidx[$ch], [string]$ch))
}
[void]$sb.AppendLine(('    GF_COUNT = {0}' -f $chars.Count))
[void]$sb.AppendLine('} GameGlyph_t;')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('extern const uint8_t game_font16_data[GF_COUNT][GAME_GLYPH_BYTES];')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('#endif /* __GAME_FONT16_H__ */')
Write-Gbk (Join-Path $root 'game_font16.h') $sb.ToString()

# ---- game_font16.c
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('/**************************************************************************')
[void]$sb.AppendLine(' * game_font16.c - 16x16 中文字模数据（由 font_gen.ps1 自动生成，不要手改）')
[void]$sb.AppendLine((' * 共 {0} 个字，{1} 字节' -f $chars.Count, ($chars.Count * $BYTES_PER)))
[void]$sb.AppendLine(' **************************************************************************/')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('#include "game_font16.h"')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('const uint8_t game_font16_data[GF_COUNT][GAME_GLYPH_BYTES] =')
[void]$sb.AppendLine('{')
foreach ($ch in $chars) {
    $cp = [int][char]$ch
    $b  = $glyphs[$gidx[$ch]]
    $line = '    {'
    for ($i = 0; $i -lt $BYTES_PER; $i++) {
        $line += ('0x{0:X2},' -f $b[$i])
        if (($i % 16) -eq 15 -and $i -ne ($BYTES_PER - 1)) { $line += '   /* ' + [string]$ch + ' */' + "`r`n    " }
    }
    $line += '},    /* ' + [string]$ch + ('  U+{0:X4} */' -f $cp)
    [void]$sb.AppendLine($line)
}
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('')
Write-Gbk (Join-Path $root 'game_font16.c') $sb.ToString()

# ---- game_text.h
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('/**************************************************************************')
[void]$sb.AppendLine(' * game_text.h - 游戏中文字符串表（由 font_gen.ps1 自动生成，不要手改）')
[void]$sb.AppendLine(' *')
[void]$sb.AppendLine(' *   代码里不要出现中文字符串字面量（工程是 GBK，编译器按 UTF-8 解会乱），')
[void]$sb.AppendLine(' *   统一用 GameTextId_t 这个编号 + Game_DrawText() 来显示中文。')
[void]$sb.AppendLine(' **************************************************************************/')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('#ifndef __GAME_TEXT_H__')
[void]$sb.AppendLine('#define __GAME_TEXT_H__')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('#include <stdint.h>')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('typedef enum')
[void]$sb.AppendLine('{')
[void]$sb.AppendLine('    GTXT_NONE = 0,')
foreach ($e in $entries) {
    [void]$sb.AppendLine(('    {0},        /* {1} */' -f $e[0], $e[1]))
}
[void]$sb.AppendLine('    GTXT_COUNT')
[void]$sb.AppendLine('} GameTextId_t;')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('/* 每个文本：字模编号序列 + 字数 */')
[void]$sb.AppendLine('extern const uint8_t *const game_text_ptr[GTXT_COUNT];')
[void]$sb.AppendLine('extern const uint8_t        game_text_len[GTXT_COUNT];')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('#endif /* __GAME_TEXT_H__ */')
Write-Gbk (Join-Path $root 'game_text.h') $sb.ToString()

# ---- game_text.c
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('/**************************************************************************')
[void]$sb.AppendLine(' * game_text.c - 游戏中文字符串表数据（由 font_gen.ps1 自动生成，不要手改）')
[void]$sb.AppendLine(' **************************************************************************/')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('#include "game_text.h"')
[void]$sb.AppendLine('#include "game_font16.h"')
[void]$sb.AppendLine('')
$n = 0
foreach ($e in $entries) {
    $id     = $e[0]
    $phrase = $e[1]
    $body = ''
    foreach ($ch in $phrase.ToCharArray()) {
        $cp = [int][char]$ch
        $body += ('GF_U{0:X4}, ' -f $cp)
    }
    [void]$sb.AppendLine(('static const uint8_t s_text_{0}[{1}] = {{ {2}}};   /* {3} */' -f $n, $phrase.Length, $body.TrimEnd(), $phrase))
    $n++
}
[void]$sb.AppendLine('')
[void]$sb.AppendLine('const uint8_t *const game_text_ptr[GTXT_COUNT] =')
[void]$sb.AppendLine('{')
[void]$sb.AppendLine('    0,                                  /* GTXT_NONE */')
$n = 0
foreach ($e in $entries) {
    [void]$sb.AppendLine(('    s_text_{0},                          /* {1} */' -f $n, $e[0]))
    $n++
}
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('const uint8_t game_text_len[GTXT_COUNT] =')
[void]$sb.AppendLine('{')
[void]$sb.AppendLine('    0,                                  /* GTXT_NONE */')
$n = 0
foreach ($e in $entries) {
    [void]$sb.AppendLine(('    {0},                                  /* {1} */' -f $e[1].Length, $e[0]))
    $n++
}
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('')
Write-Gbk (Join-Path $root 'game_text.c') $sb.ToString()

# ----------------------------------------------------------- preview
$cols = 16
$rows = [int][math]::Ceiling($chars.Count / $cols)
$cell = $GLYPH_W * $PREVIEW_Z
$bmp = New-Object System.Drawing.Bitmap(($cols * $cell), ($rows * $cell))
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::White)
$pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(220, 220, 220))
$bmp2 = New-Object System.Drawing.Bitmap($GLYPH_W, $GLYPH_H)
for ($i = 0; $i -lt $chars.Count; $i++) {
    $b = $glyphs[$i]
    for ($y = 0; $y -lt $GLYPH_H; $y++) {
        for ($x = 0; $x -lt $GLYPH_W; $x++) {
            $bit = ($b[$y * 2 + [int][math]::Floor($x / 8)] -shr (7 - ($x % 8))) -band 1
            if ($bit) { $bmp2.SetPixel($x, $y, [System.Drawing.Color]::Black) }
            else      { $bmp2.SetPixel($x, $y, [System.Drawing.Color]::White) }
        }
    }
    $cx = ($i % $cols) * $cell
    $cy = [int][math]::Floor($i / $cols) * $cell
    $g.DrawImage($bmp2, $cx, $cy, $cell, $cell)
    $g.DrawRectangle($pen, $cx, $cy, $cell - 1, $cell - 1)
}
$g.Dispose(); $bmp2.Dispose(); $pen.Dispose()
$bmp.Save((Join-Path $root 'font_preview.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

# ------------------------------------------------------------ report
Write-Gbk (Join-Path $root 'font_report.txt') (($report -join "`r`n") + "`r`n")

# ------------------------------------------------------------ summary
$blank = 0; $fat = 0
foreach ($b in $glyphs) {
    $d = 0
    foreach ($v in $b) { $d += ([Convert]::ToString($v, 2).ToCharArray() | Where-Object { $_ -eq '1' }).Count }
    if ($d -eq 0)    { $blank++ }
    if ($d -gt 160)  { $fat++ }
}
Write-Output ("glyphs      : {0}" -f $chars.Count)
Write-Output ("font bytes  : {0}" -f ($chars.Count * $BYTES_PER))
Write-Output ("texts       : {0}" -f $entries.Count)
Write-Output ("blank glyph : {0}   (>160 dots: {1})" -f $blank, $fat)
Write-Output ("written     : game_font16.h/.c  game_text.h/.c  font_preview.png  font_report.txt")
