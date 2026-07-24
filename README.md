# opm_probe_windows
A test program probing Windows PVP-OPM (Output Protection Manager) driver support for DTCP-IP/HDCP.

最終更新日: 2026-07-25

このプログラムは WindowsのPVP-OPM(Output Protection Manager)に対するディスプレイドライバの対応状況を調べるテストプログラムです。\
DTCP-IP/HDCP保護されたコンテンツの再生に必要なディスプレイドライバ機能の実装状況を確認します。

## 概要

本プログラムは、ディスプレイドライバがWindowsのOutput Protection Manager(OPM)をどこまで実装しているかを確認する診断ツールです。DTCP-IP/HDCP保護されたビデオコンテンツの再生には、ディスプレイドライバがOPM機能のDDI(Display Driver Interface)を実装している必要があり、これが未実装だと再生アプリが著作権保護エラーで起動できない現象が発生します。

具体的には、接続されている各モニターに対して以下を確認します。

1. `OPMGetVideoOutputsFromHMONITOR` でOPM/COPPそれぞれのセマンティクスの出力オブジェクトを取得できるか
2. `IOPMVideoOutput::StartInitialization` でドライバの証明書チェーンを取得できるか

現時点では検証範囲は初期化ハンドシェイクの入り口(`StartInitialization`)までに絞っており、その先の鍵交換やHDCP保護レベルの実際の有効化までは検証しません。

## ビルド済みバイナリ

自分でビルドしなくても、[Releases](https://github.com/hnakayam/opm_probe_windows/releases) からビルド済みのzip(例: `opm_probe_windows_v0.1.0.zip`)をダウンロードして使うことができます。zipを展開すると `Release` ディレクトリの下にx64/ARM64/x86それぞれの実行ファイルが入っています。

## ビルド環境

- Visual Studio 2026、「C++によるデスクトップ開発」ワークロードが必要 (Windows SDKもインストールしてください)
- **WDK(Windows Driver Kit)は不要**。使用しているOPM API(`opmapi.h`)・関連ライブラリ(`dxva2.lib`等)はいずれもユーザーモード向けで、Windows SDKに含まれる

## ビルド方法

1. 任意のディレクトリで本リポジトリをclone
   ```
   git clone https://github.com/hnakayam/opm_probe_windows.git
   ```
2. 生成された `opm_probe_windows` ディレクトリ内の `opm_probe_windows.slnx` をVisual Studio 2026で開く
3. メニューの「ビルド」→「バッチ ビルド」を開く
4. ビルドしたい **Configuration**(Debug/Release)× **Platform**(x64/ARM64/x86)の組み合わせにチェックを入れる
5. 「ビルド」(または「リビルド」)をクリック

ビルドが成功すると `<Configuration>\opm_probe_<Platform>.exe` のファイル名で実行ファイルが出力されます (例: `Release\opm_probe_x64.exe`)。
これを実行するときに管理者権限は不要です。

コマンドラインの場合は `msbuild opm_probe_windows.slnx /p:Configuration=Release /p:Platform=x64` 等でもビルド可能です。

## 実行方法

- Visual Studio IDEで Platform (x86 / x64 / ARM64)と Configuration (Debug / Release) を選んだあと、 `Ctrl+F5` でデバッグなし実行を開始します。
- 直接実行: エクスプローラーで該当フォルダを開き、アドレスバーで "cmd" + Enter を入力してコマンドプロンプトを開いた後、`opm_probe_<Platform>.exe` を入力して実行します。

**実行例**

```
Microsoft Windows [Version 10.0.22631.6199]
(c) Microsoft Corporation. All rights reserved.

D:\TEMP\opm_probe_windows>dir Release
 ドライブ D のボリューム ラベルは DATA です
 ボリューム シリアル番号は B80A-5806 です

 D:\TEMP\opm_probe_windows\Release のディレクトリ

2026/07/25  04:54    <DIR>          .
2026/07/25  04:54    <DIR>          ..
2026/07/25  04:54            29,184 opm_probe_ARM64.exe
2026/07/25  04:54         1,200,128 opm_probe_ARM64.pdb
2026/07/25  04:54            25,600 opm_probe_Win32.exe
2026/07/25  04:54         1,118,208 opm_probe_Win32.pdb
2026/07/25  04:54            29,184 opm_probe_x64.exe
2026/07/25  04:54         1,150,976 opm_probe_x64.pdb
               6 個のファイル           3,553,280 バイト
               2 個のディレクトリ  27,320,537,088 バイトの空き領域

D:\TEMP\opm_probe_windows>Release\opm_probe_x64
=== PVP-OPM 実装状況プローブ ===
StartInitialization までの応答を見て、ディスプレイドライバが
PVP-OPM の DDI を実装しているかを一次判定します。
(HDCP保護レベルの実際の有効化可否まではこのツールでは検証しません)

[モニター: \\.\DISPLAY1]
  [OPM semantics]
    OPMGetVideoOutputsFromHMONITOR: 成功 (出力数=1)
    -- 出力 #0 --
    StartInitialization: 成功 (証明書 3,659 バイト)
      証明書[0] Subject = iKGF-AZSKGFDCS
      証明書[1] Subject = Microsoft Digital Media Authority 2005
      証明書[2] Subject = IntelVpgOpm2011
  [COPP semantics (後方互換モード)]
    OPMGetVideoOutputsFromHMONITOR: 成功 (出力数=1)
    -- 出力 #0 --
    StartInitialization: 成功 (証明書 6,441 バイト)
      COPP証明書 (UTF-8文字列, 6,439 文字):
      --------------------------------------------------
      <c:CertificateCollection xmlns="http://www.w3.org/2000/09/xmldsig#" xmlns:c="http://schemas.microsoft.com/DRM/2004

(以下略)

```

## 結果の見方

接続されている各モニターについて、OPM方式・COPP方式(実行結果では「OPM semantics」「COPP semantics」と表示されます)の両方の結果が表示されます。

| 結果 | 意味 |
|---|---|
| `OPMGetVideoOutputsFromHMONITOR` が両セマンティクスとも失敗 | ドライバがPVP-OPMのDDIエントリポイントを一切公開していない可能性が高い |
| 出力オブジェクトは取得できるが `StartInitialization` が失敗 | スタブ実装/部分実装の可能性 |
| `StartInitialization` まで成功(証明書チェーン取得可) | ドライバはPVP-OPMの初期化ハンドシェイクの入り口を実装している |

いずれのセマンティクスでも `StartInitialization` が成功すれば、そのディスプレイドライバはPVP-OPMの基本的なDDIを実装していると判断できる。
