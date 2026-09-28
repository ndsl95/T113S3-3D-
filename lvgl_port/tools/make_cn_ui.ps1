<#
.SYNOPSIS
    重新生成中文字库 -> 交叉编译 -> 部署到 T113-S3 拆机板。

.DESCRIPTION
    界面文案改动后重新跑一遍即可。脚本流程：
      1. 从 main.c 里提取所有非 ASCII 字符（即界面用到的全部汉字/符号）
      2. 用 lv_font_conv 从思源黑体重新裁剪出 lv_font_cn_22.c / lv_font_cn_32.c
         并自动打上 LVGL 9 兼容补丁（见下方 NOTE）
      3. 调 WSL 里的 build.sh 交叉编译，产物在 dist/
      4. 在本机起一个临时 HTTP 服务，让板子 wget 拉取，再用串口校验 sha256
         并重启界面服务

.NOTES
    lv_font_conv 1.5.x 生成的是 LVGL 8 时代的代码，直接编译会失败，需要两处修补：
      a) `#if LV_VERSION_CHECK(8, 0, 0)` 这个宏在 LVGL 9 下为假（它是主版本
         严格相等判断），会把字体描述符写成非 const，和 LV_FONT_DECLARE 冲突。
         补成 `|| LVGL_VERSION_MAJOR >= 8`。
      b) `.cache = &cache` —— `lv_font_fmt_txt_dsc_t` 在 LVGL 9 里已经没有
         `cache` 成员了，整块删掉。
    这两处本脚本会自动处理。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\make_cn_ui.ps1
    powershell -ExecutionPolicy Bypass -File tools\make_cn_ui.ps1 -SkipDeploy
    powershell -ExecutionPolicy Bypass -File tools\make_cn_ui.ps1 -SkipFont -SkipBuild
    # 只改排版/配色（文案没变）时可以 -SkipFont 省掉重新裁字库
#>
[CmdletBinding()]
param(
    [string] $ComPort   = 'COM6',
    [string] $BoardIp   = '192.168.10.238',
    [string] $HostIp    = '',
    [string] $NodeDir   = 'Q:\Harness\nodejs',
    [string] $FontPath  = 'Q:\全志T113\hmi_tina\lvgl_port\third_party\lvgl-9.3.0\scripts\built_in_font\SourceHanSansSC-Normal.otf',
    [string] $LvglConv  = 'lv_font_conv@1.5.2',
    [int[]]  $Sizes     = @(22, 32),
    [int]    $HttpPort  = 8000,
    [string] $Distro    = 'Ubuntu-22.04',
    [string] $AppPath   = '/home/wqf/lvgl-demo',
    [string] $Service   = '/etc/init.d/S99lvgl',
    [switch] $SkipFont,
    [switch] $SkipBuild,
    [switch] $SkipDeploy
)

$ErrorActionPreference = 'Stop'
$ProjDir = Split-Path -Parent $PSScriptRoot
$DistDir = Join-Path $ProjDir 'dist'

# 让控制台正确显示 UTF-8 中文
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }

function To-WslPath([string]$p) {
    $full = [IO.Path]::GetFullPath($p)
    $drive = $full.Substring(0, 1).ToLower()
    return '/mnt/' + $drive + ($full.Substring(2) -replace '\\', '/')
}

function Invoke-Board([string]$cmd, [int]$waitMs = 9000) {
    $port = New-Object System.IO.Ports.SerialPort $ComPort, 115200, 'None', 8, 'One'
    $port.ReadTimeout = 300
    $port.Open()
    Start-Sleep -Milliseconds 400
    [void]$port.ReadExisting()
    $port.Write($cmd + "`r`n")
    Start-Sleep -Milliseconds $waitMs
    $out = $port.ReadExisting()
    $port.Close()
    return $out
}

Write-Host "项目目录: $ProjDir" -ForegroundColor Cyan

# ---------------------------------------------------------------- 1. 字库
if (-not $SkipFont) {
    Write-Host "`n[1/3] 生成中文字库" -ForegroundColor Cyan
    if (-not (Test-Path -LiteralPath $FontPath)) { throw "找不到字体: $FontPath" }

    # 只提取字符串字面量里的字符；注释里的中文不需要进字库
    $src = [IO.File]::ReadAllText((Join-Path $ProjDir 'main.c'))
    $src = [regex]::Replace($src, '/\*[\s\S]*?\*/', ' ')
    $src = [regex]::Replace($src, '//[^\n]*', ' ')

    $cps = New-Object 'System.Collections.Generic.HashSet[int]'
    foreach ($m in [regex]::Matches($src, '"(\\.|[^"\\])*"')) {
        foreach ($ch in $m.Value.ToCharArray()) {
            $c = [int]$ch
            if ($c -gt 0x7F) { [void]$cps.Add($c) }
        }
    }
    $sorted = $cps | Sort-Object
    $range  = '0x20-0x7F,' + (($sorted | ForEach-Object { '0x{0:X}' -f $_ }) -join ',')
    Write-Host ("      界面用到的非 ASCII 字符 {0} 个" -f $sorted.Count)

    $env:Path = "$NodeDir;" + $env:Path

    # 需要从 lv_font_conv 输出里剔除的 LVGL 8 片段
    $blockAOld = "#if LV_VERSION_CHECK(8, 0, 0) || LVGL_VERSION_MAJOR >= 8`n" +
                 "/*Store all the custom data of the font*/`n" +
                 "static  lv_font_fmt_txt_glyph_cache_t cache;`n" +
                 "static const lv_font_fmt_txt_dsc_t font_dsc = {`n" +
                 "#else`n" +
                 "static lv_font_fmt_txt_dsc_t font_dsc = {`n" +
                 "#endif`n"
    $blockAnew = "static const lv_font_fmt_txt_dsc_t font_dsc = {"
    $blockBOld = "#if LV_VERSION_CHECK(8, 0, 0) || LVGL_VERSION_MAJOR >= 8`n" +
                 "    .cache = &cache`n" +
                 "#endif`n};`n"
    $blockBnew = "};`n"

    foreach ($sz in $Sizes) {
        $out = Join-Path $ProjDir ("lv_font_cn_{0}.c" -f $sz)
        Write-Host ("      lv_font_conv size={0} ..." -f $sz)
        & npx -y $LvglConv --font $FontPath --range $range --size $sz `
              --bpp 4 --format lvgl --no-compress -o $out 2>&1 |
            Select-Object -Last 2 | Out-Host
        if (-not (Test-Path -LiteralPath $out)) { throw "lv_font_conv 失败: size=$sz" }

        $t = [IO.File]::ReadAllText($out).Replace("`r`n", "`n")
        $t = $t.Replace('#if LV_VERSION_CHECK(8, 0, 0)', '#if LV_VERSION_CHECK(8, 0, 0) || LVGL_VERSION_MAJOR >= 8')
        $t = $t.Replace($blockAOld, $blockAnew)
        $t = $t.Replace($blockBOld, $blockBnew)
        if ($t.Contains('glyph_cache')) { throw "补丁未完全生效（仍存在 glyph_cache）: $out" }
        [IO.File]::WriteAllText($out, $t)
        Write-Host ("         -> {0} ({1} KB)" -f (Split-Path -Leaf $out), [int]((Get-Item $out).Length / 1KB))
    }
} else {
    Write-Host "`n[1/3] 跳过字库生成" -ForegroundColor DarkGray
}

# ---------------------------------------------------------------- 2. 编译
if ($SkipBuild) {
    Write-Host "`n[2/3] 跳过编译（直接用 dist/ 里已有的产物）" -ForegroundColor DarkGray
} else {
    Write-Host "`n[2/3] 交叉编译" -ForegroundColor Cyan
    $buildSh = To-WslPath (Join-Path $ProjDir 'build.sh')
    & wsl.exe -d $Distro bash $buildSh
    if ($LASTEXITCODE -ne 0) { throw "构建失败" }
}

$sha = (Get-Content -LiteralPath (Join-Path $DistDir 'lvgl-demo.sha256') -Raw).Trim()
Write-Host "      本地 sha256: $sha"

# ---------------------------------------------------------------- 3. 部署
if ($SkipDeploy) {
    Write-Host "`n[3/3] 跳过部署。产物在 $DistDir" -ForegroundColor DarkGray
    return
}

Write-Host "`n[3/3] 部署到板子" -ForegroundColor Cyan

if (-not $HostIp) {
    $prefix = ($BoardIp -split '\.')[0..2] -join '.'
    $HostIp = (Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
               Where-Object {
                   $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' -and
                   (($_.IPAddress -split '\.')[0..2] -join '.') -eq $prefix
               } | Select-Object -First 1).IPAddress
}
if (-not $HostIp) { throw "找不到与 $BoardIp 同网段的本机 IP，请用 -HostIp 指定" }
Write-Host "      本机 IP: $HostIp"

$pythonExe = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $pythonExe) { throw "找不到 python，无法起临时 HTTP 服务" }

$srv = Start-Process -FilePath $pythonExe -WindowStyle Hidden -PassThru `
       -WorkingDirectory $DistDir -ArgumentList @('-m', 'http.server', "$HttpPort", '--bind', '0.0.0.0')

$ready = $false
for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep -Milliseconds 500
    if (Get-NetTCPConnection -LocalPort $HttpPort -State Listen -ErrorAction SilentlyContinue) { $ready = $true; break }
    if ($srv.HasExited) { break }
}
if (-not $ready) { throw "临时 HTTP 服务未能在端口 $HttpPort 上监听" }
Write-Host "      HTTP 服务已就绪 (PID $($srv.Id))"

try {
    $url = 'http://{0}:{1}/lvgl-demo.gz' -f $HostIp, $HttpPort
    $tpl = @'
__SVC__ stop; sleep 1; wget -T 20 -O /tmp/lvgl-demo.gz __URL__ 2>&1 | tail -1; gunzip -c /tmp/lvgl-demo.gz > __APP__.new; echo -n "board sha256: "; sha256sum __APP__.new; mv __APP__.new __APP__; chmod +x __APP__; __SVC__ start; sleep 6; ps | grep -i lvgl-demo; echo ---LOG---; tail -6 /var/log/lvgl.log
'@
    $cmd = $tpl.Replace('__URL__', $url).Replace('__APP__', $AppPath).Replace('__SVC__', $Service)
    $out = Invoke-Board $cmd 30000
    Write-Host $out

    if ($out -match 'board sha256: ([0-9a-f]{64})') {
        $boardSha = $Matches[1]
        if ($boardSha -eq $sha) {
            Write-Host "`n校验通过，部署完成。" -ForegroundColor Green
        } else {
            Write-Host "`n警告：sha256 不一致！本地=$sha 板端=$boardSha" -ForegroundColor Red
        }
    } else {
        Write-Host "`n警告：没有读到板端 sha256，请手动确认。" -ForegroundColor Yellow
    }
}
finally {
    if ($srv -and -not $srv.HasExited) { Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue }
}
