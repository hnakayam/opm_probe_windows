// opm_probe.cpp
//
// PVP-OPM (Protected Video Path - Output Protection Manager) 実装状況プローブ
//
// 目的:
//   ディスプレイドライバが Output Protection Manager (opmapi.h) の
//   初期化ハンドシェイクにどこまで応答するかを検査し、
//   「PVP-OPM の DDI が実装されているかどうか」を一次診断する。
//
// 検証範囲:
//   OPMGetVideoOutputsFromHMONITOR -> IOPMVideoOutput::StartInitialization まで。
//   FinishInitialization 以降(RSA-OAEP による鍵交換、AES-CMAC(OMAC-1)による
//   コマンド署名)は実装していない。これらはドライバの公開鍵証明書に対する
//   正しい暗号パラメータ(パディング方式・ハッシュアルゴリズム)の実装を要し、
//   ここを誤ると「ドライバ未対応」と「自前実装のバグ」の切り分けができなくなる
//   ため、まずは確実に判定できる StartInitialization までに絞っている。
//
// 判定の目安:
//   1. OPMGetVideoOutputsFromHMONITOR 自体が失敗する
//        -> ドライバが PVP-OPM の DDI エントリポイントを
//           一切公開していない可能性が高い
//   2. 出力オブジェクトは取得できるが StartInitialization が失敗する
//        -> スタブ実装 / 部分実装の可能性。証明書チェーンの提供までは未対応
//   3. StartInitialization が成功し、証明書チェーンが取得できる
//        -> ドライバは PVP-OPM の初期化ハンドシェイクの入り口までは実装している
//           (HDCP保護レベルの実際の有効化可否は本ツールでは未検証)
//
// ビルド方法 (WDK / Windows SDK が入った環境):
//   "x64 Native Tools Command Prompt for VS" または ARM64版のものを開き、
//
//     cl /EHsc /std:c++17 /W4 opm_probe.cpp ^
//        dxva2.lib crypt32.lib ole32.lib user32.lib
//
//   ARM64ネイティブでビルドする場合は "ARM64 Native Tools Command Prompt" を使うこと。
//   x64版バイナリをArm版WindowsでPrism(x64エミュレーション)経由で実行しても
//   一定の診断はできるが、ドライバDDI周りの挙動を厳密に見たい場合は
//   ARM64ネイティブビルドを強く推奨する。
//
// 実行:
//   接続されている全モニターに対して、OPM semantics / COPP semantics の
//   両方で結果を表示する。管理者権限は不要(通常は)。

#include <windows.h>
#include <d3d9.h>
#include <opmapi.h>
#include <wincrypt.h>
#include <iostream>
#include <vector>
#include <string>
#include <cwchar>
#include <locale>

#pragma comment(lib, "dxva2.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

namespace {

std::wstring HResultToString(HRESULT hr) {
    wchar_t buf[512] = {};
    FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        buf, ARRAYSIZE(buf), nullptr);
    std::wstring s(buf);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
    wchar_t hex[32];
    swprintf_s(hex, L" (0x%08X)", static_cast<unsigned>(hr));
    if (s.empty()) {
        s = L"不明なエラー";
    }
    s += hex;
    return s;
}

// COPP semanticsの証明書チェーンはASN.1 DERではなくUTF-8文字列として返される
// (Microsoft Learn: IOPMVideoOutput::StartInitialization の Remarks を参照。
//  OPM semanticsはX.509/DER、COPP semanticsはUTF-8文字列、と明記されている)。
// XML-Signature Syntax and Processingをベースにした独自形式とされ、そもそも
// CryptoAPIでの解析対象ではないため、単純にUTF-8→UTF-16変換して表示する。
void DumpCoppCertificateAsUtf8(const BYTE* pbCert, DWORD cbCert) {
    int wideLen = MultiByteToWideChar(CP_UTF8, 0,
        reinterpret_cast<const char*>(pbCert), static_cast<int>(cbCert), nullptr, 0);
    if (wideLen <= 0) {
        std::wcout << L"      (UTF-8としてのデコードに失敗しました。GetLastError="
                    << GetLastError() << L")\n";
        return;
    }
    std::wstring text(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0,
        reinterpret_cast<const char*>(pbCert), static_cast<int>(cbCert), &text[0], wideLen);

    std::wcout << L"      COPP証明書 (UTF-8文字列, " << text.size() << L" 文字):\n";
    std::wcout << L"      --------------------------------------------------\n";
    std::wcout << L"      " << text << L"\n";
    std::wcout << L"      --------------------------------------------------\n";
}

// 連結されたDER証明書列を先頭から1枚ずつ手動でデコードする。
// OPM semanticsでも稀に、PKCS#7等の包装を持たずX.509 DER証明書が単純に
// 連結されているだけのことがあり、その場合はCryptQueryObjectの自動判定
// では認識できない。CertCreateCertificateContextで1枚デコードするたびに、
// その証明書の実際のエンコード長(cbCertEncoded)だけポインタを進めて
// 次の証明書を読む。
int DumpCertificatesAsConcatenatedDer(const BYTE* pbCert, DWORD cbCert) {
    DWORD offset = 0;
    int count = 0;
    while (offset < cbCert) {
        PCCERT_CONTEXT pCertContext = CertCreateCertificateContext(
            X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
            pbCert + offset, cbCert - offset);
        if (!pCertContext) {
            break;
        }
        wchar_t nameBuf[256] = {};
        CertGetNameStringW(pCertContext, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr,
                            nameBuf, ARRAYSIZE(nameBuf));
        std::wcout << L"      証明書[" << count++ << L"] Subject = " << nameBuf << L"\n";
        offset += pCertContext->cbCertEncoded;
        CertFreeCertificateContext(pCertContext);
    }

    if (count == 0) {
        std::wcout << L"      (DER証明書としても解析できませんでした。未知の独自形式の可能性があります)\n";
    } else if (offset < cbCert) {
        std::wcout << L"      (" << count << L" 枚解析後、残り " << (cbCert - offset)
                    << L" バイトは証明書として解釈できませんでした)\n";
    }
    return count;
}

// 取得した証明書チェーンを解析し、参考情報としてSubjectを表示する。
// StartInitializationの成否には影響しない、あくまで補助情報。
// vosによって証明書の形式が異なる(上記DumpCoppCertificateAsUtf8のコメント参照)。
void DumpCertificateSubject(const BYTE* pbCert, DWORD cbCert, OPM_VIDEO_OUTPUT_SEMANTICS vos) {
    if (vos == OPM_VOS_COPP_SEMANTICS) {
        DumpCoppCertificateAsUtf8(pbCert, cbCert);
        return;
    }

    CERT_BLOB blob{ cbCert, const_cast<BYTE*>(pbCert) };
    DWORD contentType = 0;
    HCERTSTORE hStore = nullptr;
    HCRYPTMSG hMsg = nullptr;

    BOOL ok = CryptQueryObject(
        CERT_QUERY_OBJECT_BLOB, &blob,
        CERT_QUERY_CONTENT_FLAG_ALL,
        CERT_QUERY_FORMAT_FLAG_ALL,
        0, nullptr, &contentType, nullptr, &hStore, &hMsg, nullptr);

    if (!ok) {
        // CryptQueryObjectは単一証明書・PKCS#7・シリアル化ストアなど
        // 「包装された」形式しか自動判定できない。単純に連結されただけの
        // DER証明書列はここに落ちてくるため、手動の連結DERパースに
        // フォールバックする。
        std::wcout << L"      (CryptQueryObjectでは形式を認識できませんでした。HRESULT="
                    << HResultToString(HRESULT_FROM_WIN32(GetLastError()))
                    << L"。連結DER証明書として手動パースを試みます)\n";
        DumpCertificatesAsConcatenatedDer(pbCert, cbCert);
        return;
    }

    if (hStore) {
        PCCERT_CONTEXT pCert = nullptr;
        int count = 0;
        while ((pCert = CertEnumCertificatesInStore(hStore, pCert)) != nullptr) {
            wchar_t nameBuf[256] = {};
            CertGetNameStringW(pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr,
                                nameBuf, ARRAYSIZE(nameBuf));
            std::wcout << L"      証明書[" << count++ << L"] Subject = " << nameBuf << L"\n";
        }
        CertCloseStore(hStore, 0);
    }
    if (hMsg) CryptMsgClose(hMsg);
}

struct MonitorEntry {
    HMONITOR hMonitor;
    std::wstring deviceName;
};

BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC, LPRECT, LPARAM lParam) {
    auto* list = reinterpret_cast<std::vector<MonitorEntry>*>(lParam);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMonitor, &mi)) {
        list->push_back({ hMonitor, mi.szDevice });
    } else {
        list->push_back({ hMonitor, L"(名前取得失敗)" });
    }
    return TRUE;
}

void ProbeVideoOutput(IOPMVideoOutput* pVideoOutput, OPM_VIDEO_OUTPUT_SEMANTICS vos) {
    OPM_RANDOM_NUMBER random{};
    BYTE* pbCertificate = nullptr;
    ULONG cbCertificate = 0;

    HRESULT hr = pVideoOutput->StartInitialization(&random, &pbCertificate, &cbCertificate);

    if (SUCCEEDED(hr)) {
        std::wcout << L"    StartInitialization: 成功 (証明書 " << cbCertificate << L" バイト)\n";
        if (pbCertificate && cbCertificate > 0) {
            DumpCertificateSubject(pbCertificate, cbCertificate, vos);
        } else {
            std::wcout << L"      (証明書サイズが0でした。ハンドシェイクの体裁だけ整えている可能性があります)\n";
        }
    } else {
        std::wcout << L"    StartInitialization: 失敗 HRESULT=" << HResultToString(hr) << L"\n";
    }

    if (pbCertificate) CoTaskMemFree(pbCertificate);
}

void ProbeMonitor(const MonitorEntry& mon) {
    std::wcout << L"\n[モニター: " << mon.deviceName << L"]\n";

    struct Mode { OPM_VIDEO_OUTPUT_SEMANTICS vos; const wchar_t* label; };
    const Mode modes[] = {
        { OPM_VOS_OPM_SEMANTICS,  L"OPM semantics" },
        { OPM_VOS_COPP_SEMANTICS, L"COPP semantics (後方互換モード)" },
    };

    for (const auto& mode : modes) {
        std::wcout << L"  [" << mode.label << L"]\n";
        ULONG numOutputs = 0;
        IOPMVideoOutput** ppOutputs = nullptr;

        HRESULT hr = OPMGetVideoOutputsFromHMONITOR(
            mon.hMonitor, mode.vos, &numOutputs, &ppOutputs);

        if (FAILED(hr)) {
            std::wcout << L"    OPMGetVideoOutputsFromHMONITOR: 失敗 HRESULT="
                        << HResultToString(hr) << L"\n";
            std::wcout << L"    -> このセマンティクスではPVP-OPMのビデオ出力オブジェクトを取得できません。\n";
            continue;
        }

        std::wcout << L"    OPMGetVideoOutputsFromHMONITOR: 成功 (出力数=" << numOutputs << L")\n";

        for (ULONG i = 0; i < numOutputs; ++i) {
            std::wcout << L"    -- 出力 #" << i << L" --\n";
            if (ppOutputs[i]) {
                ProbeVideoOutput(ppOutputs[i], mode.vos);
                ppOutputs[i]->Release();
            }
        }
        if (ppOutputs) CoTaskMemFree(ppOutputs);
    }
}

} // namespace

int main() {
    // コンソール出力(std::wcout)は既定では"C"ロケールのため、日本語などの
    // ワイド文字をマルチバイトに変換できず失敗し、以降すべての出力が
    // 無言で失われる(failbitが立ったまま)。OS既定のロケール(日本語Windows
    // ではコードページ932)をwcout/wcerrに適用して回避する。
    std::locale::global(std::locale(""));
    std::wcout.imbue(std::locale());
    std::wcerr.imbue(std::locale());

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hrCo)) {
        std::wcerr << L"CoInitializeEx に失敗しました。\n";
        return 1;
    }

    std::wcout << L"=== PVP-OPM 実装状況プローブ ===\n";
    std::wcout << L"StartInitialization までの応答を見て、ディスプレイドライバが\n"
                  L"PVP-OPM の DDI を実装しているかを一次判定します。\n";
    std::wcout << L"(HDCP保護レベルの実際の有効化可否まではこのツールでは検証しません)\n";

    std::vector<MonitorEntry> monitors;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc,
                         reinterpret_cast<LPARAM>(&monitors));

    if (monitors.empty()) {
        std::wcout << L"モニターが見つかりませんでした。\n";
    }

    for (const auto& mon : monitors) {
        ProbeMonitor(mon);
    }

    std::wcout << L"\n=== 判定の目安 ===\n";
    std::wcout << L"・OPMGetVideoOutputsFromHMONITOR 自体が失敗する\n"
                  L"    -> ドライバがPVP-OPMのDDIエントリポイントを一切公開していない可能性が高い\n";
    std::wcout << L"・出力オブジェクトは取得できるがStartInitializationが失敗する\n"
                  L"    -> スタブ実装/部分実装の可能性。証明書チェーンの提供までは未対応\n";
    std::wcout << L"・StartInitializationが成功し、証明書チェーンが取得できる\n"
                  L"    -> ドライバはPVP-OPMの初期化ハンドシェイクの入り口までは実装している\n"
                  L"       (この先のHDCP有効化可否は未検証)\n";

    CoUninitialize();
    return 0;
}
