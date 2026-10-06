# Authenticode 서명

Windows 가 서명된 PE 파일(`.exe`, `.dll`)이나 MSI 패키지를 받아들이게 하려면 프로그램이 무엇을 써야 하는가.
출처 표시는 [README.md](README.md)와 같다: **[spec]** 은 Microsoft 의 "Windows Authenticode Portable
Executable Signature Format", PE/COFF 명세, RFC 5652(CMS), **[observed]** 는 Windows 자신의 서명기
(`mssign32.dll` 을 쓰는 PowerShell `Set-AuthenticodeSignature`)가 서명하고 Windows 11 에서
`Get-AuthenticodeSignature` / `WinVerifyTrust` 로 확인한 파일이다.

## PE 파일에서 서명이 사는 곳

- 인증서 표: 선택 머리의 데이터 디렉터리 항목 4(항목은 8바이트: 파일 오프셋, 크기). 디렉터리는 PE32(magic
  0x10B)면 선택 머리 + 96, PE32+(magic 0x20B)면 + 112 에서 시작하고, `NumberOfRvaAndSizes` 는 5 이상이어야
  한다. 다른 모든 디렉터리 항목과 달리 그 "주소"는 RVA 가 아니라 **파일 오프셋**이다. [spec]
- 그 오프셋, 파일 끝에: `WIN_CERTIFICATE { u32 dwLength, u16 wRevision = 0x0200,
  u16 wCertificateType = 0x0002 (PKCS_SIGNED_DATA), bCertificate[] }`. `bCertificate` 는 DER CMS
  `ContentInfo` 다. `dwLength` 는 8바이트 머리를 포함하고 8의 배수로 올리며 늘어난 바이트는 0 이다.
  디렉터리 항목의 크기는 `dwLength` 와 같다. [spec] [observed]
- 표를 붙이기 전에 파일을 8의 배수까지 0 으로 채운다. 채운 바이트도 해시한다(아래 "섹션 뒤의 데이터").
  [spec]
- 선택 머리의 `CheckSum`(선택 머리 + 64)을 고친다. Windows 의 서명기는 다시 계산하지 않고 있던 값을
  조정한다: 체크섬이 PE 알고리즘 값이 아니던 파일이면 Windows 는 그 차이를 지키고, 새로 계산하면 그렇지
  않다. `CheckSum` 은 다이제스트 밖이라 둘 다 검증된다. [observed]

## PE 다이제스트

SHA-256 으로 이 순서대로: [spec]

1. 머리, 0 부터 `SizeOfHeaders` 까지. `CheckSum`(4바이트)과 인증서 표 디렉터리 항목(8바이트)은 뺀다.
2. `SizeOfRawData > 0` 인 모든 섹션을 `PointerToRawData` 가 커지는 순서로, `PointerToRawData` 부터
   `SizeOfRawData` 바이트.
3. 그 뒤의 데이터: `SizeOfHeaders + sum(SizeOfRawData)` 부터 파일 끝에서 인증서 표를 뺀 곳까지.

구멍 둘을 두고 파일을 앞에서부터 해시해도 섹션 사이에 틈이 없을 때만 같은 결과가 나온다. 위 단계가 정의다.

## CMS SignedData

Windows 의 서명기가 쓰고 받아들이는 것: [observed]

```text
ContentInfo { signedData (1.2.840.113549.1.7.2), [0] SignedData {
  version 1,
  digestAlgorithms SET { sha256 + NULL },
  encapContentInfo { SPC_INDIRECT_DATA (1.3.6.1.4.1.311.2.1.4),
                     [0] SpcIndirectDataContent }          -- SEQUENCE 그대로, OCTET STRING 없음
  certificates [0] { 서명자의 인증서, 그다음 중간 인증서; 자체 서명 루트 없음 },
  signerInfos SET { SignerInfo {
    version 1, 서명자의 IssuerAndSerialNumber,
    digestAlgorithm sha256 + NULL,
    signedAttrs [0] { SpcSpOpusInfo, contentType, SpcStatementType, messageDigest },
    signatureAlgorithm rsaEncryption + NULL,
    signature (RSA PKCS#1 v1.5) } } } }
```

- `SpcIndirectDataContent ::= SEQUENCE { data, messageDigest DigestInfo }`. PE 파일이면 `data` 는
  `SEQUENCE { SPC_PE_IMAGE_DATA (1.3.6.1.4.1.311.2.1.15), SpcPeImageData }` 이고 Windows 는
  `SpcPeImageData` 를 `30 09 03 01 00 A0 04 A2 02 80 00` 으로 쓴다: 플래그 없음, 그리고 빈 유니코드
  문자열인 파일 링크. `messageDigest` 가 PE 다이제스트를 담는다. [observed]
- **messageDigest 속성**은 `SpcIndirectDataContent` 의 내용을 그 SEQUENCE 꼬리표와 길이 **없이** 해시한
  것이다 - 요소 전체도 아니고 PE 파일도 아니다. [observed]
- 서명 속성은 꼬리표 `[0]`(0xA0)로 저장하지만 서명은 같은 바이트를 꼬리표 SET(0x31)으로, DER SET OF 로
  정렬해 계산한다. Windows 가 쓰는 속성 넷: `SpcSpOpusInfo`(1.3.6.1.4.1.311.2.1.12) =
  `SEQUENCE { [0] { [0] "" }, [1] { [0] "" } }`, `contentType` = SPC_INDIRECT_DATA,
  `SpcStatementType`(1.3.6.1.4.1.311.2.1.11) = `SEQUENCE { individual code signing
  (1.3.6.1.4.1.311.2.1.21) }`, `messageDigest`. 서명 시각은 없다. [observed]
- RSA PKCS#1 v1.5 는 결정적이다: 같은 키로 정확히 이 구조를 쓰는 프로그램은 Windows 와 같은 바이트를
  만든다. rubrapack 이 그렇다(위의 `CheckSum` 차이만 빼고). [observed]

## MSI 패키지

Microsoft 는 MSI 패키지를 어떻게 해시하는지 공개하지 않는다. 아래 규칙은 Windows 의 서명기로 패키지에
서명하고 비교해서 알아냈다: 같은 패키지와 키에 대해 rubrapack 의 서명과 Ex 스트림은 Windows 의 것과 바이트까지
같았고, 디렉터리 항목에 0 아닌 상태 비트·시각·CLSID 를 준 패키지들로 필드를 가려냈다. [observed]

- 서명은 복합 파일 루트의 `\005DigitalSignature` 스트림이다: PE 파일과 같은 CMS `ContentInfo` 에
  `data` = `SEQUENCE { SPC_SIPINFO (1.3.6.1.4.1.311.2.1.30), SpcSipInfo }`. Windows 는 `SpcSipInfo` 를
  `SEQUENCE { 2, OCTET STRING <MSI SIP GUID {000C10F1-0000-0000-C000-000000000046} 을 리틀엔디언
  바이트 순서로>, 0, 0, 0, 0, 0 }` 으로 쓴다.
- Windows 는 `\005MsiDigitalSignatureEx` 도 쓴다: 32바이트, 복합 파일 디렉터리의 "사전 해시"에 대한
  SHA-256:
  - 루트 항목의 CLSID(16바이트)와 상태 비트(4바이트, 리틀엔디언);
  - 그다음 루트의 모든 스트림 - 서명 스트림 둘은 빼고 - 을 이름의 UTF-16LE 바이트 비교 순서로:
    이름(UTF-16LE, 끝 NUL 없음), 스트림 크기 8바이트 리틀엔디언, 항목의 만든 시각과 고친 시각을 저장된
    그대로(8 + 8바이트). 스트림의 CLSID 와 상태 비트는 들지 않는다.
  루트 자신의 시각은 들지 않는다(Windows 는 서명한 파일을 저장할 때 루트의 고친 시각을 정한다).
- `SpcIndirectDataContent` 의 다이제스트는 다음의 해시다: 32바이트 `MsiDigitalSignatureEx` 값(그 스트림을
  쓸 때), 그다음 같은 이름 순서의 모든 스트림 내용(역시 서명 스트림 둘은 빼고), 그다음 루트 CLSID.
- 저장소(패치의 변환이 저장소다. 안에 넣은 변환과 하위 데이터베이스도 그렇다)는 같은 이름 순서에서, 둘레의
  스트림과 함께 비교한 제자리에 든다:
  - 사전 해시에는 그 이름, CLSID(16바이트), 상태 비트(4바이트), 두 시각(16바이트) - 크기는 없다 - 이 들고,
    바로 뒤에 그 안 항목들의 사전 해시 항목이 그 이름 순서로, 같은 방식으로 아래까지 이어진다;
  - 다이제스트에는 그 안 항목들의 내용이 그 이름 순서로, 같은 방식으로 아래까지 들고, 끝에 그 CLSID 가 든다 -
    루트의 CLSID 가 전체를 끝내듯이.
  Windows 의 signtool 로 서명한 패치(rubrapack 이 쓴 것 하나, Microsoft 의 MsiMsp.exe 가 쓴 것 하나 - 그 저장소에는
  0 이 아닌 시각이 있다)와 맞춰 알아냈다: 시험한 후보 가운데 저장된 `MsiDigitalSignatureEx` 값 둘을 모두 내는
  배치는 이것 하나이고, 이것으로 두 다이제스트도 맞는다. 그 패치들에는 루트 바로 아래의 저장소만 있었다.
- 서명은 `.msi` 안에 든 것만 덮는다: 밖의 캐비닛(`#` 으로 시작하지 않는 Media 표 `Cabinet` 값)은
  다이제스트에 들지 않는다.

## 타임스탬프

타임스탬프 없는 서명은 서명 인증서가 만료되면 더는 유효하지 않다. RFC 3161 타임스탬프가 있으면 Windows 는
대신 찍힌 시각에 인증서를 확인한다. `SIGNER_TIMESTAMP_RFC3161` 을 준 `mssign32!SignerTimeStampEx2` 가 쓰는
것: [observed]

- 요청의 메시지 지문은 SignerInfo 의 `signature` 값 - OCTET STRING 의 내용, 꼬리표와 길이 없이 - 의 해시
  (여기서는 SHA-256)다.
- 서버의 `TimeStampToken`(`TSTInfo` 위의 CMS SignedData, RFC 3161)은 서명 뒤에 SignerInfo 의 서명 안 된
  속성으로 들어간다: `[1] { SEQUENCE { 1.3.6.1.4.1.311.3.3.1, SET { TimeStampToken } } }`.
- 서명의 다른 것은 바뀌지 않는다: 서명 속성, 서명, 바깥 인증서는 전과 같은 바이트이고, 서버의 인증서는
  토큰 안에 실려 다닌다. MSI 패키지에서는 `\005DigitalSignature` 스트림이 그만큼 커지고(몇 킬로바이트 토큰이면
  미니 스트림에서 보통 섹터로 옮겨 간다), `\005MsiDigitalSignatureEx` 와 다이제스트는 바뀌지 않는다 -
  서명 스트림은 거기 들지 않기 때문이다.
- rubrapack 은 정확히 이것을 쓴다. 같은 키와 토큰이면 그 PE 파일은 Windows 의 것과 바이트까지 같고, MSI
  서명 스트림도 그렇다.

`Set-AuthenticodeSignature -TimestampServer` 는 대신 옛 형태를 쓴다: PKCS#9 counterSignature
속성(1.2.840.113549.1.9.6)과, 바깥 인증서에 더한 서버의 인증서. Windows 는 둘 다 받아들인다. [observed]

## ECDSA

ECDSA P-256 서명자는 PE 파일, MSI 패키지, MSIX 패키지 모두에 통한다(Windows 11 26100 은 루트를 신뢰하면 셋
모두 `Valid` 라고 한다). Windows 의 서명기는 SignerInfo 의 `signatureAlgorithm` 을 `ecdsa-with-SHA256` 이
아니라 NULL 매개변수의 `id-ecPublicKey`(1.2.840.10045.2.1)로 쓰고, 서명 값은 DER
`SEQUENCE { INTEGER r, INTEGER s }` 로 쓴다. rubrapack 도 결정적 nonce(RFC 6979)로 같게 쓰고, 두 OID 를 모두
읽는다. [observed]

## Windows 가 알려 주는 것

`Get-AuthenticodeSignature` 는 서명하는 쪽에 필요한 경우들을 가려 준다(타임스탬프가 있으면
`TimeStamperCertificate` 에 타임스탬프 인증서를 적는다): `Valid`(다이제스트, 서명, 신뢰하는 루트까지의 사슬),
"the certificate chain ... not trusted" 를 단 `UnknownError`(다이제스트와 서명은 맞고, 루트를 이 컴퓨터가
신뢰하지 않음), `HashMismatch`(서명 뒤에 파일이 바뀜), `NotSigned`. 그러니 어떤 루트도 신뢰시키기 전에 서명이
맞는지 확인할 수 있다. [observed]

## 실제 예: 튜토리얼의 hello.msi

튜토리얼 첫 패키지의 다이제스트를 위의 규칙대로 키 없이 여기서 계산했다 - 어떤 키로 서명하든 서명에는 이 값이 그대로 들어간다. UTF-16LE 이름 순서로 놓은 스트림들:

| # | 이름(UTF-16LE) | 이름 | 크기 |
|---|---|---|---|
| 1 | `05 00 53 00 75 00` ... | `\005SummaryInformation` | 336 |
| 2 | `0b 43 31 41 35 47` ... | `Binary.RpCa` | 91136 |
| 3 | `26 41 65 38 be 41` ... | `cab1.cab` | 6776 |
| 4 | `40 48 0b 43 31 41` ... | `Binary` | 4 |
| 5 | `40 48 0c 46 f6 45` ... | `CustomAction` | 56 |
| 6 | `40 48 0d 43 35 42` ... | `Directory` | 18 |
| 7 | `40 48 0f 42 e4 45` ... | `FeatureComponents` | 4 |
| 8 | `40 48 0f 42 e4 45` ... | `Feature` | 16 |
| 9 | `40 48 0f 43 2f 42` | `File` | 20 |
| 10 | `40 48 16 42 27 43` ... | `Media` | 14 |
| 11 | `40 48 3f 3b f2 43` ... | `_Columns` | 648 |
| 12 | `40 48 3f 3f 77 45` ... | `_StringData` | 3706 |
| 13 | `40 48 3f 3f 77 45` ... | `_StringPool` | 684 |
| 14 | `40 48 52 44 f6 45` ... | `InstallExecuteSequence` | 156 |
| 15 | `40 48 52 44 f6 45` ... | `InstallUISequence` | 42 |
| 16 | `40 48 59 45 f2 44` ... | `Property` | 88 |
| 17 | `40 48 7f 3f 64 41` ... | `_Tables` | 36 |
| 18 | `40 48 8c 44 f0 44` ... | `Component` | 12 |
| 19 | `40 48 ca 41 30 43` ... | `AdminExecuteSequence` | 48 |
| 20 | `40 48 ca 41 30 43` ... | `AdminUISequence` | 24 |
| 21 | `40 48 ca 41 f9 45` ... | `AdvtExecuteSequence` | 42 |
| 22 | `40 48 de 44 6a 45` ... | `Upgrade` | 32 |
| 23 | `40 48 ff 3f e4 43` ... | `_Validation` | 1944 |

프리해시는 뿌리의 CLSID 와 상태 비트(20 바이트)에, 스트림마다 이름, 8 바이트 크기, 0 인 시각 둘을 이은 것으로 모두 908 바이트다. 그 SHA-256 이
`\005MsiDigitalSignatureEx` 의 값이다:

```text
bf 98 be c2 62 21 8d b3 c4 a7 35 04 bc 9d 3a 32 c9 ec f1 5d 9b 42 bd ef 1d 5e 02 6c f9 d5 6c 34
```

`SpcIndirectDataContent` 의 다이제스트 - 그 값, 같은 순서의 모든 스트림 바이트, 뿌리 CLSID 에 대한 SHA-256:

```text
7b ac b1 dd 8f 88 03 47 da b6 62 18 d6 07 b7 56 2f ec 83 dc c4 e8 12 a8 e7 54 15 77 f9 9e 5e eb
```

