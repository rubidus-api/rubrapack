# 키, 인증서, 서명

튜토리얼 16장은 `hello.msi` 에 서명한다. 이 장은 그것이 무슨 뜻인지 설명한다: 서명이 파일을 누가 만들었고 아무도
바꾸지 않았음을 어떻게 증명하는지, 인증서가 무엇인지, 그리고 타임스탬프가 왜 인증서가 만료된 뒤에도 서명을 유효하게
두는지.

## 열쇠 하나 대신 둘

가장 오래된 비밀 글쓰기는 양쪽에 열쇠 하나를 쓴다: 잠글 수 있는 사람은 열 수도 있다. **공개 키 암호**(1970년대)는
대신 **키 쌍**을 쓴다: 함께 만들어지고 서로 짝이 되는 두 수다.

- **개인 키**는 주인에게만 비밀로 남는다 - 비밀번호로 보호한 파일에, 더 좋게는 키를 절대 내보내지 않는 하드웨어
  토큰 안에.
- **공개 키**는 모두에게 준다.

한 키가 한 일은 다른 키만 확인할 수 있다. 서명에서는 이런 뜻이다: 서명을 *만드는* 것은 개인 키만 할 수 있고, 공개
키가 있는 누구나 그것을 *확인*할 수 있다. 공개 키를 안다고 개인 키를 찾는 데 도움이 되지는 않는다 - 오늘날의
크기로는 우주의 수명 안에 되지 않는다.

코드 서명에는 두 방법이 쓰인다:

| 방법 | 흔한 키 크기 | 참고 |
|---|---|---|
| RSA | 2048 또는 3072 비트 | 전통의 방법. rubrapack 의 시험 인증서가 RSA 2048 을 쓴다 |
| ECDSA | 256 또는 384 비트(P-256, P-384) | 타원 곡선: 같은 강도에 훨씬 작은 키 |

## 서명하기와 확인하기

서명은 파일 전체 위에 만들지 않는다 - 200 MB 패키지라면 느리다 - 파일의 **해시**(4장) 위에 만든다:

```text
  signing (the publisher):
    file ---hash---> digest ---private key---> signature      (stored in the file)

  checking (any user's computer):
    file ---hash---> digest'
    signature ---public key---> digest
    digest' = digest ?  yes: unchanged and signed by that key.  no: changed, or another key.
```

파일의 비트 하나만 바뀌어도 해시가 완전히 바뀌므로 어떤 수정도 확인을 통과하지 못한다. 그리고 공개 키가 받아들이는
서명은 개인 키만 만들 수 있으므로, 다른 누구도 바뀐 파일에 게시자의 이름으로 서명할 수 없다.

## 인증서: 이것은 누구의 키인가?

서명은 어떤 공개 키로 확인된다 - 그런데 누구의 키인가? 누구든 키 쌍을 만들어 "Example Software" 라고 부를 수 있다.
**인증서**가 그 답이다: "이 공개 키는 *이* 이름의 것이다"라고 적은 작은 문서로, 그 이름을 확인한 **인증 기관**(CA)이
서명한 것이다.

rubrapack 자체 시험용으로 만든 시험 인증서를 `openssl x509 -text` 가 보이는 대로 옮기면:

| 필드 | 값 |
|---|---|
| 주체(Subject, 누구) | `CN=rubrapack test signer, O=rubrapack tests` |
| 발급자(Issuer, 누가 보증) | `CN=rubrapack test root` |
| 유효 기간 시작 / 끝 | 2026년 9월 30일 / 2036년 9월 27일 |
| 일련번호 | `71AB698C19EACB47D6A00D71A711CF7A0ACDB236` |
| 공개 키 | RSA, 2048 비트 |
| 기본 제약 | `CA:FALSE` - 다른 인증서를 보증할 수 없다 |
| 키 용도 | 디지털 서명 |
| 확장 키 용도 | 코드 서명 |
| 서명 | sha256WithRSAEncryption, 발급자의 개인 키로 만듦 |

rubrapack 은 서명하기 전에 이것들을 확인한다: 인증서는 코드 서명용이어야 하고, CA 의 것이 아니어야 하며, 오늘
유효해야 한다(튜토리얼 16장).

### 이름: CN, O, C

주체는 부분들의 목록으로, 부분마다 종류와 값이 있다: `CN`(일반 이름), `O`(조직), `OU`(부서), `L`(도시),
`S`(주/도), `C`(나라, 두 글자). 인증서는 이것을 어떤 순서로 저장하고, 프로그램마다 쓰는 순서가 다르다. 이 인증서는
`CN` 다음에 `O` 순서로 저장되어 있고, Windows 는 부분들을 마지막 것부터 거꾸로 쓴다:

```text
  as stored:          CN=rubrapack test signer, O=rubrapack tests
  as Windows writes:  O=rubrapack tests, CN=rubrapack test signer
```

MSIX 의 `publisher` 는 정확히 Windows 의 형태여야 한다(튜토리얼 17장) - 그래서 둘이 다르면 rubrapack 이 옮겨 적을
글을 출력해 준다.

## 사슬과 신뢰

시험 인증서는 "rubrapack test root" 가 발급했고, 그 루트의 인증서는 자기 자신이 서명했다 - 그것이 **루트**다. 실제
인증서는 **사슬**을 이룬다:

```text
  your code signing certificate    issued by   ->  an intermediate CA certificate
  the intermediate certificate     issued by   ->  a root CA certificate
  the root certificate             issued by   ->  itself
```

Windows 는 믿는 루트 인증서의 목록, **신뢰할 수 있는 루트 저장소**를 두고, Microsoft 가 관리한다. 사슬이 그중 하나에서
끝나면 서명을 믿는다. 중간 인증서는 서명 안에 함께 실려 가므로(`--cert` 가 더한다) Windows 는 누구에게 묻지 않고 사슬을
만들 수 있다.

`rubrapack verify --trust ca.pem` 은 내가 지정한 루트로, `--system-roots` 는 컴퓨터 자신의 목록으로 확인한다. 직접 만든
인증서(튜토리얼 16장)는 사슬이 Windows 가 믿는 어디에서도 끝나지 않는다 - 그래서 시험에는 괜찮지만 사용자에게는
쓸모가 없다.

### 폐지

개인 키를 도둑맞으면 CA 는 그 인증서를 **폐지**한다: 일련번호를 목록(CRL)에 공개하거나 그에 대한 질문(OCSP)에 답한다.
Windows 는 네트워크에 닿을 때 이것을 확인한다. `rubrapack verify` 는 `revocation: not-checked` 라고 알린다: 스스로
온라인에 나가지 않는다.

## 타임스탬프

인증서는 만료된다 - 오늘날 코드 서명 인증서는 길어야 39개월 뒤에. 그 인증서로 서명한 파일들은 어떻게 되나? 아무
도움이 없으면 그날부터 서명은 효력이 없다.

**타임스탬프**가 이것을 해결한다. 서명할 때 rubrapack 은 서명의 해시를 **타임스탬프 기관**(TSA) - CA 가 운영하는
서버 - 에 보낸다. TSA 는 "이 해시는 이 시각에 있었다"를 자기 인증서로 서명해 돌려주고, 그것이 서명 옆에 파일로
들어간다(RFC 3161). 나중에 Windows 는 확인한다: 서명한 인증서가 *타임스탬프가 보이는 시각에* 유효했는가? 그렇다면
서명은 인증서가 만료된 한참 뒤에도 계속 유효하다.

```text
  signed 2026-10-01, certificate valid until 2029-10-01
  checked 2031-05-12:
    without a timestamp: certificate expired -> signature not valid
    with a timestamp:    valid on 2026-10-01 (the TSA says so) -> signature valid
```

TSA 는 파일을 보지 않는다. 해시만 본다.

## 키가 사는 곳: 파일과 토큰

| 형태 | 무엇인가 |
|---|---|
| `.pfx` / `.p12` (PKCS #12) | 개인 키, 인증서, 흔히 사슬까지 한 파일에 담고 비밀번호로 보호한 것. Windows 가 내보내는 형태 |
| `.pem` | 글: 이진 형태를 base64 로 바꿔 `-----BEGIN CERTIFICATE-----` / `-----END ...-----`(또는 `PRIVATE KEY`) 줄 사이에 둔 것. Linux 도구들이 쓴다 |
| `.der`, `.cer` | 같은 것의 이진 형태 |
| Windows 인증서 저장소 | Windows 가 보관하는 키(`certmgr.msc`). 지문으로 쓴다(`--key-store`) |
| 하드웨어 토큰, 클라우드 HSM | 개인 키가 장치를 떠나지 않는다. 프로그램이 장치에 서명을 부탁한다(PKCS #11, `--pkcs11`) |

2023년부터 CA 는 하드웨어 안의 키에만 코드 서명 인증서를 발급하므로, 배포용으로는 마지막 두 줄을 쓰게 된다.

이진(DER) 인증서 자체도 길이가 앞에 붙은 필드들의 둥지다. 시험 인증서는 `30 82 03 d8` 로 시작한다: `30` 은 "묶음이
뒤따른다", `82` 는 "그 길이가 다음 2 바이트를 차지한다", 그리고 `03 d8` 이 그 길이 984 다 - 이번에는 big-endian 이다
(2장). 984 바이트에 이 네 바이트를 더하면 인증서 전체 988 바이트가 된다.

### 지문

인증서의 **지문**(thumbprint)은 그 이진 형태의 SHA-1 해시(4장)다 - 여기서는
`2E AB FE 60 19 0A F6 6C 52 E4 29 31 AF A6 01 BE C0 EF 86 0D`. 비밀이 아니다. 인증서 하나를 가리키는 짧은 이름이고,
Windows 저장소와 `rubrapack keys list` 가 인증서를 이것으로 부른다.

## 쓰이는 곳

- 튜토리얼 [16](../tutorial/16-signing.md)장: `sign`, `verify`, 키, 타임스탬프.
- 튜토리얼 [17](../tutorial/17-msix-and-bundles.md)장: MSIX 게시자.
- 제4부: [Authenticode](../formats/authenticode.md)(정확히 무엇을 해시하고 서명하는지, Windows 가 받아들이는 구조),
  [Windows 와 대조하기](../formats/verify.md).
