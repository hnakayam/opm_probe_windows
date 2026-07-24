<#
.SYNOPSIS
    Release\ 配下のビルド済みバイナリ(x64/ARM64/Win32)を1つのzipにまとめる。

.PARAMETER Version
    zipファイル名に付与するバージョン文字列(例: v0.1.0)。省略時は日付を使う。

.EXAMPLE
    .\make_release_zip.ps1 -Version v0.1.0
#>
param(
    [string]$Version = (Get-Date -Format "yyyyMMdd")
)

$ErrorActionPreference = "Stop"

$repoRoot = $PSScriptRoot
$releaseDir = Join-Path $repoRoot "Release"

$files = @(
    "opm_probe_x64.exe",
    "opm_probe_ARM64.exe",
    "opm_probe_Win32.exe"
) | ForEach-Object { Join-Path $releaseDir $_ }

$missing = $files | Where-Object { -not (Test-Path $_) }
if ($missing) {
    Write-Error "以下のファイルが見つかりません。先にRelease構成でビルドしてください:`n$($missing -join "`n")"
}

$zipName = "opm_probe_windows_$Version.zip"
$zipPath = Join-Path $repoRoot $zipName

if (Test-Path $zipPath) {
    Remove-Item $zipPath -Force
}

# zip内に "Release" ディレクトリを作り、その配下に実行ファイルを格納するため、
# 一時フォルダにReleaseという名前のディレクトリを作ってからそこを圧縮する。
$stagingRoot = Join-Path ([System.IO.Path]::GetTempPath()) "opm_probe_release_staging_$PID"
$stagingRelease = Join-Path $stagingRoot "Release"
New-Item -ItemType Directory -Path $stagingRelease -Force | Out-Null

try {
    foreach ($file in $files) {
        Copy-Item -Path $file -Destination $stagingRelease
    }

    Compress-Archive -Path $stagingRelease -DestinationPath $zipPath
}
finally {
    Remove-Item $stagingRoot -Recurse -Force
}

Write-Output "作成しました: $zipPath"
