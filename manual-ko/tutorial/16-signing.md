# 서명과 타임스탬프

목표: `hello.msi` 에 서명해, Windows 가 게시자를 "알 수 없는 게시자" 대신 "Example Software" 로 보이게 하고,
누구도 몰래 고치지 못하게 한다 - 키 파일로, Windows 인증서 저장소의 키로, 또는 하드웨어 토큰으로.

## 서명이란

*디지털 서명*은 파일 내용과 비밀인 *개인 키*로 계산한 수다. 짝이 되는 *공개 키*로 누구나 그것을 확인할 수 있다.
공개 키는 *인증서* 안에 든다. 인증서는 "이 공개 키는 Example Software 의 것이다"라고 적은 작은 문서로, 그 자체가
*인증 기관*(CA)의 서명을 받은 것이다. Windows 는 믿는 CA 목록을 가지고 있고, 그중 한 곳의 인증서면 사용자 계정
컨트롤 창에 내 이름이 나온다. 패키지의 바이트 하나만 바뀌어도 서명은 더 이상 맞지 않는다. [해시](../basics/04-guids-and-hashes.md#해시)와 [키, 인증서, 서명](../basics/08-keys-certificates-signatures.md)은 제3부가 차근차근 설명한다.

인증서를 얻는 곳:

- **실제 사용자에게 배포할 때**: CA 에서 사는 *코드 서명 인증서*. 2023년부터 그 개인 키는 하드웨어 - USB 토큰이나
  클라우드 서비스 - 안에만 있어야 하고 파일로 둘 수 없다. rubrapack 은 아래의 PKCS#11 토큰이나 Windows 인증서
  저장소를 통해 그런 키로 서명한다.
- **배우고 시험할 때**: 직접 만든 인증서. Windows 는 그것을 믿지 않지만(시험 컴퓨터의 신뢰 목록에 더하지 않는 한
  창에는 여전히 "알 수 없는 게시자"가 나온다), 나머지는 모두 똑같이 된다. Windows 의 PowerShell 에서:

```text
PS> New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=Example Software" -CertStoreLocation Cert:\CurrentUser\My
```

인증서의 *지문*(thumbprint) - 인증서를 가리키는 16진수 40자리 - 이 출력된다.

## 키 파일로 서명하기

키 파일은 개인 키와 인증서를 비밀번호로 보호해 담는다: Windows 가 내보내는 `.pfx`(`.p12` 라고도 한다.
`Export-PfxCertificate` 에 `-CryptoAlgorithmOption AES256_SHA256`), 또는 PEM 키. 비밀번호는 명령줄에 절대 적지
않는다(다른 프로그램이 거기서 읽을 수 있다): 환경 변수나 파일에서 온다. 비밀번호를 받을 법한 옵션은 그렇게
말하려고만 있다:

```text
C:\work\hello> rubrapack sign hello.msi --key signer.pfx --pass secret
rubrapack: error[RP0004]: a password is never taken on the command line (others can see it); use --pass-env or --pass-file
```

(토큰 PIN 의 `--pin` 도 같은 답을 한다.)

```text
C:\work\hello> set SIGN_PASS=...비밀번호...
C:\work\hello> rubrapack sign hello.msi --key signer.pfx --pass-env SIGN_PASS -o hello-signed.msi
rubrapack: warning[RP0011]: 'hello.msi': no --timestamp: the signature stops being valid when the certificate expires
signed hello-signed.msi
```

`-o` 가 없으면 파일 자체에 서명한다. 이미 서명된 파일은 거부한다 - 서명 안 된 파일에 다시 서명한다. 인증서는
코드 서명용이어야 하고, CA 의 인증서가 아니어야 하며, 오늘 유효해야 한다. `--pass-file pass.txt` 는 비밀번호를
파일에서 읽는다. `--cert chain.pem` 은 키 파일에 없는 인증서들(CA 의 중간 인증서 - Windows 가 내 인증서를 믿는
루트까지 잇는 데 필요하다)을 더한다.

빌드하면서 서명하면 한 번에 된다:

```text
C:\work\hello> rubrapack build hello.toml -o hello.msi --key signer.pfx --pass-env SIGN_PASS
```

`sign` 은 프로그램(`.exe`, `.dll`)과 MSIX 패키지(다음 장)에도 서명한다. 빌드 전에 `hello.exe` 에 서명해 두면
프로그램 자체에도 내 이름이 붙는다.

## 타임스탬프: 인증서보다 오래 사는 서명

인증서는 1~3년 동안 유효하다. 위의 경고가 그 뒤의 일을 말한다: 서명이 더는 효력이 없다. *타임스탬프*가 이것을
해결한다: 타임스탬프 서버(CA 들이 운영하고, 무료로 쓸 수 있다)가 "이 서명은 이 시각에 있었다"고 서명하면, 그 서명은
그 뒤로 계속 유효하다.

```text
C:\work\hello> rubrapack sign hello.msi --key signer.pfx --pass-env SIGN_PASS --timestamp http://timestamp.digicert.com
```

배포하는 것에는 늘 타임스탬프를 넣는다. 특별한 네트워크를 위한 옵션: `--proxy http://host:port`, `--tls-trust`
(`https://` 타임스탬프 서버에 믿을 인증서), `--system-roots`(그 연결에 컴퓨터 자신의 루트 인증서를 믿는다),
`--tsa-trust`(타임스탬프 자체에 믿을 인증서).

## Windows 인증서 저장소의 키

위에서 만든 인증서는 내 인증서 저장소에 있다. 지문으로 그것을 써서 서명한다 - Windows 에서만 된다:

```text
C:\work\hello> rubrapack keys list
CurrentUser\My	thumbprint=1a2b...	RSA	publisher=CN=Example Software	until=...
C:\work\hello> rubrapack sign hello.msi --key-store 1a2b...
```

`--machine-store` 는 내 저장소 대신 컴퓨터의 저장소(LocalMachine)에서 찾는다. 드라이버가 Windows 에 연결되는 USB
토큰의 키도 여기에 보인다.

## 하드웨어 토큰: PKCS#11

토큰 제조사는 PKCS#11 *모듈* - 토큰 대신 말하는 DLL(Linux 에서는 `.so`) - 을 준다. rubrapack 은 그것과 직접
말한다:

```text
C:\work\hello> rubrapack keys list --pkcs11 C:\token\pkcs11.dll --pin-env TOKEN_PIN
C:\work\hello> rubrapack sign hello.msi --pkcs11 C:\token\pkcs11.dll --key-label "Code Signing" --pin-env TOKEN_PIN --timestamp http://timestamp.digicert.com
```

`--key-label` 은 이름표로 키를 고르고, `--token-label` 은 토큰이 여럿 꽂혀 있을 때 토큰을 고르며,
`--pin-env`/`--pin-file` 이 PIN 을 준다(명령줄에는 절대 적지 않는다).

## 서명 확인하기: `verify`

```text
C:\work\hello> rubrapack verify hello-signed.msi --trust ca.pem
hello-signed.msi
  structure:  ok
  digest:     ok
  signature:  ok
  chain:      trusted
  revocation: not-checked
  timestamp:  not-present
```

- `structure`: 서명이 있어야 할 자리에 있다. `digest`: 내용이 바뀌지 않았다. `signature`: 인증서의 키로 만든
  서명이다.
- `chain`: 인증서가 내가 믿는 인증서까지 이어진다 - `--trust` 로 주거나, 컴퓨터가 믿는 루트는
  `--system-roots` 로. 둘 다 없으면 verify 는 판단할 수 없다고 말한다(종료 코드 4).
- `timestamp`: 있고 유효한지, 없는지.

서명 안 된 파일은 `structure: invalid ... the package is not signed` 라고 나온다.

## 외부 캐비닛은 서명되지 않는다

서명은 `.msi` 만 덮는다. `cab = "external"`(15장)이면 누군가 `.cab` 파일을 몰래 바꿔치기할 수 있으므로 `sign` 은
그런 패키지를 거부한다:

```text
C:\work\hello\out-x64> rubrapack sign hello-x64.msi --key signer.pfx --pass-env SIGN_PASS
rubrapack: error[RP0011]: 'hello-x64.msi': the package uses cabinets outside itself, which a signature of the .msi does not cover; build it with embedded cabinets, or sign anyway with --allow-unsigned-cabs
```

`--allow-unsigned-cabs` 는 경고와 함께 `.msi` 에만 서명한다. 서명할 패키지에는 넣어 둔 캐비닛을 쓰는 편이 낫다.

## 안에서 무슨 일이 일어났나

MSI 의 서명은 숨은 스트림 `\005DigitalSignature` 로, PKCS#7 구조를 담는다: 다른 모든 스트림의 해시, 내 인증서
사슬, 그리고 그 위의 서명. 제4부(Authenticode)가 해시를 어떻게 계산하고 각 부분이 어디에 있는지 보여 준다.
