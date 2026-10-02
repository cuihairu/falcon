# Falcon 一键安装脚本（Windows PowerShell）——从每日构建（nightly release）安装桌面版。
#
# 用法（匿名下载，无需登录；PowerShell 5.1+）：
#   irm https://raw.githubusercontent.com/cuihairu/falcon/main/scripts/install.ps1 | iex
#
# 特性：
#   - 安装 Inno Setup 安装包（falcon-desktop-setup-nightly.exe）到 Program Files
#   - 自动检测 CPU 架构（AMD64 支持；ARM64/x86 明确报错不猜测）
#   - 幂等：重跑即升级（安装器固定 AppId，覆盖安装同一目录与卸载条目）
#   - 失败即停（throw；irm|iex 下禁用 exit——exit 会关闭用户会话）
#   - 提权：安装 Program Files 需要管理员；/SILENT 下由安装器触发 UAC，
#     拒绝提权按退出码明确报错（不猜测安装结果）
#   - 验证：MZ 头完整性 + 安装目录 falcon-cli.exe `--version` 真验证（控制台
#     子系统进程；桌面二进制绝不执行——旧产物不识别 --version 会直接
#     拉起 GUI 挂住安装会话）
#   - 用户 PATH 注册表追加（幂等，自动剔除旧版 LOCALAPPDATA 入口）
#   - 开始菜单快捷方式与卸载入口由安装器创建（[Icons] 组「Falcon」）
#   - 兼容旧版 zip 布局：自动清理 %LOCALAPPDATA%\Falcon 遗留，避免 PATH 双份

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # irm|iex 下 IWR 进度条极慢且无意义

$Repo = 'cuihairu/falcon'
$BaseUrl = "https://github.com/$Repo/releases/download/nightly"
$Asset = 'falcon-desktop-setup-nightly.exe'

function Info([string]$msg) { Write-Host ">>> $msg" }
function Die([string]$msg) { throw "安装失败：$msg" }

# ---- 架构检测（不认识的组合明确报错，绝不猜测下载地址） ----
$arch = $env:PROCESSOR_ARCHITECTURE
switch ($arch) {
    'AMD64' { }
    'ARM64' { Die "不支持的 CPU 架构：Windows/$arch——目前仅提供 x86_64 构建" }
    'x86'   { Die "不支持的 CPU 架构：Windows/$arch——目前仅提供 x86_64 (64 位) 构建" }
    default { Die "不支持的 CPU 架构：Windows/$arch" }
}

# ---- 下载（404 与网络失败分开报错） ----
$url = "$BaseUrl/$Asset"
$tmpRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("falcon-install-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmpRoot | Out-Null
$setupPath = Join-Path $tmpRoot $Asset

try {
    Info "下载 $Asset"
    try {
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $setupPath
    } catch {
        $code = $null
        try {
            $resp = $_.Exception.Response
            if ($resp -ne $null) { $code = [int]$resp.StatusCode }
        } catch { }
        if ($code -eq 404) {
            Die "资产不存在（HTTP 404）：$Asset——nightly 产物尚未发布或地址已变更"
        }
        Die "下载失败：$url（$($_.Exception.Message)）"
    }
    if (-not (Test-Path $setupPath) -or (Get-Item $setupPath).Length -eq 0) {
        Die "下载文件为空：$setupPath"
    }

    # ---- 验证：MZ 头（PE 完整性；安装器本身也是 PE） ----
    $fs = [System.IO.File]::OpenRead($setupPath)
    try {
        $mz = New-Object byte[] 2
        [void]$fs.Read($mz, 0, 2)
        if ($mz[0] -ne 0x4D -or $mz[1] -ne 0x5A) {
            Die "安装产物不完整：$Asset 缺少 MZ 头（PE 文件损坏）"
        }
    } finally {
        $fs.Dispose()
    }
    Info "验证：MZ 头通过"

    # ---- 安装（Inno Setup 静默安装 = 覆盖升级，幂等） ----
    Info "运行安装器（/SILENT；未提权会话可能弹出 UAC）"
    $proc = Start-Process -FilePath $setupPath -ArgumentList '/SILENT','/SUPPRESSMSGBOXES','/NORESTART' -Wait -PassThru
    if ($proc.ExitCode -ne 0) {
        if ($proc.ExitCode -eq 740) {
            # ERROR_ELEVATION_REQUIRED：UAC 提权请求被拒绝
            Die "安装需要管理员权限（退出码 740：提权被拒绝）——请在管理员 PowerShell 中重试"
        }
        Die "安装器退出码 $($proc.ExitCode)——安装可能中断（UAC 被拒或写 Program Files 失败）"
    }

    $installDir = Join-Path $env:ProgramFiles 'Falcon'
    $mainExe = Join-Path $installDir 'falcon-desktop.exe'
    if (-not (Test-Path $mainExe)) {
        Die "安装器已退出但未找到 $mainExe——产物结构可能已变更"
    }
    Info "已安装到 $installDir"

    # ---- 旧版 zip 布局清理（此前装在 %LOCALAPPDATA%\Falcon\falcon-desktop，
    #      其 PATH 入口会先于 Program Files 命中造成双份） ----
    $legacyDir = Join-Path $env:LOCALAPPDATA 'Falcon\falcon-desktop'
    if (Test-Path $legacyDir) {
        try {
            Remove-Item -Path $legacyDir -Recurse -Force
            Info "已清理旧版 zip 安装目录：$legacyDir"
        } catch {
            Write-Warning "旧版安装目录清理失败（$($_.Exception.Message)）——可手动删除：$legacyDir"
        }
    }

    # ---- 验证：falcon-cli.exe --version（控制台子进程，真执行） ----
    $cliExe = Join-Path $installDir 'falcon-cli.exe'
    if (Test-Path $cliExe) {
        $verFile = Join-Path $tmpRoot 'cli-version.txt'
        $verProc = Start-Process -FilePath $cliExe -ArgumentList '--version' -NoNewWindow -Wait -PassThru `
                   -RedirectStandardOutput $verFile
        $verText = ''
        if (Test-Path $verFile) { $verText = (Get-Content $verFile -Raw -ErrorAction SilentlyContinue) }
        if ($verProc.ExitCode -ne 0 -or [string]::IsNullOrWhiteSpace($verText)) {
            Write-Warning "falcon-cli --version 未通过（exit=$($verProc.ExitCode)）——安装保留，请手动复核 $cliExe"
        } else {
            Info "验证：falcon-cli --version → $($verText.Trim())"
        }
    } else {
        Write-Warning "包内未附带 falcon-cli.exe，跳过 --version 直验（以 MZ 头 + 安装器退出码为准）"
    }

    # ---- 用户 PATH（注册表，幂等；先剔除旧版入口）+ 当前会话即时生效 ----
    try {
        $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
        if (-not [string]::IsNullOrEmpty($userPath)) {
            $kept = ($userPath -split ';' | Where-Object { $_ -and $_ -ne $legacyDir }) -join ';'
            $userPath = $kept.TrimEnd(';')
        }
        if ([string]::IsNullOrEmpty($userPath)) {
            [Environment]::SetEnvironmentVariable('Path', $installDir, 'User')
        } elseif ($userPath -notlike "*$installDir*") {
            [Environment]::SetEnvironmentVariable('Path', ($userPath.TrimEnd(';') + ';' + $installDir), 'User')
        }
        if (($env:Path -split ';') -notcontains $installDir) {
            $env:Path = $env:Path + ';' + $installDir
        }
        Info "用户 PATH 已包含 $installDir（已打开的终端需重开生效）"
    } catch {
        Write-Warning "写入用户 PATH 失败（$($_.Exception.Message)）——可手动添加：$installDir"
    }

    # ---- 开始菜单快捷方式：安装器已在组「Falcon」下创建（含卸载项）；
    #      此处只清理旧版脚本建在 Programs 根的同名快捷方式避免重复 ----
    try {
        $sm = [Environment]::GetFolderPath('Programs')
        $legacyLnk = Join-Path $sm 'Falcon Desktop.lnk'
        if (Test-Path $legacyLnk) {
            $target = (New-Object -ComObject WScript.Shell).CreateShortcut($legacyLnk).TargetPath
            if ($target -like "*Falcon\falcon-desktop*") {
                Remove-Item $legacyLnk -Force
                Info "已清理旧版开始菜单快捷方式"
            }
        }
    } catch {
        Write-Warning "旧版快捷方式清理失败（$($_.Exception.Message)）——不影响安装"
    }

    Write-Host ''
    Info "安装完成：$mainExe"
    Info "启动方式：开始菜单「Falcon」组 Falcon Desktop，或新开终端 falcon-desktop（当前会话 PATH 已更新也可直接运行）"
    Info "卸载方式：开始菜单「Falcon」组 Uninstall Falcon Desktop，或系统「应用和功能」"
} finally {
    Remove-Item -Path $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue
}
