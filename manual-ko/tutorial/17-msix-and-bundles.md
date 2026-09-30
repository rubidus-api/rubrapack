# MSIX 패키지와 번들

목표: 같은 원본으로 Hello 를 MSIX 패키지 - Windows 의 더 새로운 패키지 형식 - 로도 빌드하고, 이어서 세
아키텍처를 모두 담은 번들 하나로 빌드한다.

## MSI 인가 MSIX 인가?

| | MSI (`.msi`) | MSIX (`.msix`) |
|---|---|---|
| 설치 | 패키지가 말하는 곳 어디든, 시키는 대로 컴퓨터를 바꾼다 | Windows 가 관리하는 봉인된 폴더에. 앱이 쓰는 파일과 레지스트리는 따로 보관된다 |
| 제거 | 패키지가 만든 만큼 깨끗하게 | 늘 완전히: 아무것도 남지 않는다 |
| 서명 | 권장 | **필수**: Windows 는 서명 안 된 MSIX 를 설치하지 않는다(아래의 시험용은 예외) |
| 할 수 있는 것 | 이 튜토리얼의 모든 것 | 파일, 바로가기, 파일 형식, 레지스트리, 글꼴, 명령줄 별칭, 로그인할 때 시작 |
| 할 수 없는 것 | - | 서비스, 사용자 지정 동작, 환경 변수, INI 파일, 권한, 조건, 대화창 |

많은 제품이 둘 다 낸다. rubrapack 은 원본 하나로 어느 쪽이든 빌드한다: 출력 파일의 확장자가 정한다.

## 폴더

원본 옆 `assets\` 에 로고 셋을 둔다: 정확히 150x150, 44x44, 50x50 픽셀인 PNG 그림이다. 시작 메뉴와 설정 앱이
이것을 보인다. 없으면 패키지는 단색의 밋밋한 로고를 받는다 - 셋을 모두 주거나 하나도 주지 않는다.

```text
C:\work\hello\
    assets\
        Square150x150.png
        Square44x44.png
        StoreLogo.png
    dist\
        x64\hello.exe
        x86\hello.exe
        arm64\hello.exe
        docs\guide.txt
    hello.toml
```

## 원본

```toml
# tutorial 17: hello.toml
[package]
name = "Hello"
manufacturer = "Example Software"
version = "$(VERSION)"
arch = "x64"
upgrade-code = "{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}"
upgrade-code-x86 = "{7D1C2B3A-4E5F-4061-9728-3A4B5C6D7E8F}"
upgrade-code-arm64 = "{0E9F8D7C-6B5A-4948-8372-6150F4E3D2C1}"

[define]
VERSION = "2.1.0"

[msix]
identity-name = "ExampleSoftware.Hello"
publisher = "CN=Example Software"
publisher-display-name = "Example Software"
min-version = "10.0.17763.0"

[msix-app.Hello]
executable = "Hello"
display-name = "Hello"
description = "Says hello."
logo-150 = "assets/Square150x150.png"
logo-44 = "assets/Square44x44.png"
store-logo = "assets/StoreLogo.png"

[msix-extension.Command]
kind = "alias"
alias = "hello.exe"

[msix-extension.AtSignIn]
kind = "startup-task"
display-name = "Hello"
enabled = false

[dir.INSTALLDIR]
path = "ProgramFiles/Hello"

[file.Hello]
dir = "INSTALLDIR"
source = "dist/$(ARCH)/hello.exe"

[files.DocFiles]
dir = "INSTALLDIR"
glob = "dist/docs/**"

[shortcut.StartMenu]
dir = "Programs"
name = "Hello"
target = "file:Hello"

[assoc.HelloDoc]
extension = ".hello"
prog-id = "ExampleSoftware.HelloDocument"
description = "Hello document"
target = "file:Hello"

[registry.Greeting]
root = "HKMU"
key = "Software\\Example Software\\Hello"
name = "Greeting"
value = "Hello"

[env.HelloHome]
name = "HELLO_HOME"
value = "[INSTALLDIR]"
msi-only = true
```

## 패키지의 정체: `[msix]`

| 키 | 뜻 |
|---|---|
| `identity-name` | Windows 가 부르는 패키지 이름. `A-Z a-z 0-9 . -` 로 된 3~50자이고, 관례는 `회사.제품` 이다. MSI 의 업그레이드 코드처럼 절대 바꾸지 않는다: 이름과 게시자가 같은 새 판이 옛 판을 대신한다. |
| `publisher` | 서명할 인증서의 *주체*(subject)를 Windows 가 쓰는 방식 그대로(다음 절) |
| `publisher-display-name` | 사람들이 보는 게시자 이름. 기본값은 `[package] manufacturer` |
| `min-version` | 설치할 수 있는 가장 오래된 Windows, `10.0.<빌드>.0` 꼴. 기본값 `10.0.17763.0` 은 Windows 10 버전 1809 다 |

판 번호는 `[package] version` 에 네 번째 부분을 붙인 것이다: `2.1.0` 은 `2.1.0.0` 이 된다.

## 애플리케이션: `[msix-app.ID]`

MSIX 는 자기가 담은 *애플리케이션* - 시작 메뉴의 항목 - 을 나열한다. `executable` 은 그것을 시작하는
`[file.*]` 를 가리키고, 그 파일의 폴더(여기서는 `INSTALLDIR`)가 패키지 자신의 폴더가 된다. `display-name` 과
`description` 의 기본값은 패키지 이름이다. `[msix-app.*]` 표가 여럿이면 항목도 여럿이 된다.

## 덤: `[msix-extension.ID]`

- `kind = "alias"`: 콘솔에서 `hello.exe` 라고 치면 어느 폴더에서든 애플리케이션이 시작된다 - 프로그램을 `PATH`
  에 두는 MSIX 식 방법이다.
- `kind = "startup-task"`: 사용자가 로그인할 때 애플리케이션이 시작된다(한 번 실행한 뒤부터). `enabled = false`
  는 사용자가 작업 관리자의 시작 앱에서 켤 때까지 꺼 둔다. 그 목록에는 `display-name` 이 보인다. `task-id` 는
  Windows 가 부를 작업 이름이고, 기본값은 표의 ID 다(아래 매니페스트에 보이듯 여기서는 `AtSignIn`).

`[msix-app.*]` 표가 여럿이면 `app = "Hello"` 로 덤이 어느 애플리케이션의 것인지 밝힌다. 기본값은 첫 번째다. MSI
빌드는 이 표들을 뺀다.

## 다른 표들은 무엇이 되나

- `Programs` 에 있고 애플리케이션의 실행 파일을 가리키는 `[shortcut.StartMenu]` 는 애플리케이션의 시작 메뉴 항목
  *그 자체*다. `Desktop` 의 바로가기도 `min-version = "10.0.19645.0"` 부터 된다. 다른 곳의 바로가기, 인수나
  `icon` 이 있는 바로가기는 MSI 전용이다(`RP1613`).
- `[assoc.HelloDoc]` 는 애플리케이션의 파일 형식 연결이 된다. 그 파일 형식은 애플리케이션의 로고로 보인다.
- `[registry.Greeting]` 은 패키지의 *가상 레지스트리*에 들어간다: 애플리케이션은 값이 실제 레지스트리에 있는
  것처럼 보지만, 컴퓨터의 레지스트리는 그대로다. `Software` 아래의 키만 들어갈 수 있다(`RP1612`).
- `[env.HelloHome]`: MSIX 는 환경 변수를 정하지 못한다. `msi-only = true` 가 없으면 빌드가 멈춘다:

```text
C:\work\hello> rubrapack build hello.toml -o hello.msix --unsigned-test
hello.toml:65:1: error[RP1605]: [env.HelloHome] cannot go into an MSIX; add msi-only = true to build the MSIX without it
```

`msi-only = true` 는 그 표를 MSI 에는 두고 MSIX 에서는 뺀다. rubrapack 은 아무것도 몰래 빼지 않는다: MSIX 가
담지 못하는 것은 내가 뜻을 밝힐 때까지 오류다.

기능, 속성, 대화창, `[arp]` 는 Windows Installer 에만 해당하므로 쓰이지 않는다.
`rubrapack lint hello.toml --target msix` 는 빌드하지 않고 원본을 MSIX 기준으로 검사한다.

## 시험용 빌드: `--unsigned-test`

```text
C:\work\hello> rubrapack build hello.toml -o hello.msix --unsigned-test
C:\work\hello> rubrapack build hello.toml -o hello.msi
```

Windows 는 패키지가 시험용이라고 밝히고 설치하는 쪽도 그것을 받아들인다고 할 때만 서명 안 된 MSIX 를 설치한다.
`--unsigned-test` 가 그 표시를 더한다(게시자가 바뀌므로 서명한 패키지와는 다른 패키지다). 프로그램이 들어 있으니
관리자 권한 PowerShell 에서 설치한다:

```text
PS C:\work\hello> Add-AppxPackage -Path hello.msix -AllowUnsigned
PS C:\work\hello> hello.exe
PS C:\work\hello> Get-AppxPackage ExampleSoftware.Hello | Remove-AppxPackage
```

이런 패키지는 내 시험 컴퓨터에서만 쓴다.

## 서명하기

다른 사람이 설치하게 하려면 서명한다(16장). 이때 MSIX 의 `publisher` 는 인증서의 주체와 정확히 같아야 한다.
다르면 서명이 멈추고 써야 할 주체를 알려 준다:

```text
C:\work\hello> rubrapack build hello.toml -o hello.msix --key signer.pfx --pass-env SIGN_PASS
rubrapack: error[RP0011]: 'hello.msix': the package's publisher "CN=Example Software" is not the certificate's subject "O=Example Software Ltd, CN=Example Software"; set [msix] publisher to it (without --unsigned-test)
```

그 주체를 `publisher` 에 옮겨 적는다. Windows 는 주체의 부분들을 마지막 것부터 거꾸로 쓰므로, 어떤 도구가
보여 주는 순서와 다를 수 있다.

## 모든 아키텍처를 파일 하나에: 번들

`.msixbundle` 로 끝나는 출력은 15장처럼 `--arch` 의 아키텍처마다 원본을 한 번씩 빌드해 패키지들을 파일 하나에
담는다:

```text
C:\work\hello> rubrapack build hello.toml -o hello.msixbundle --arch x64,x86,arm64 --unsigned-test
C:\work\hello> rubrapack inspect hello.msixbundle --files
AppxMetadata\AppxBundleManifest.xml	1616	deflate
ExampleSoftware.Hello_2.1.0.0_x64.msix	11009	stored
ExampleSoftware.Hello_2.1.0.0_x86.msix	11096	stored
ExampleSoftware.Hello_2.1.0.0_arm64.msix	11012	stored
```

Windows 는 그중 자기 처리기에 맞는 패키지를 설치한다. `--key` 를 주면 번들과 그 안의 패키지 모두에 서명한다.

## 압축

파일은 deflate 로 압축되고, 그림처럼 이미 압축된 파일은 그대로 저장된다. `--msix-compress store` 는 모두 그대로
저장한다. 패키지는 커지지만(여기서는 11009 에서 29961 바이트로) 여는 것은 빨라진다. MSIX 에는 날짜도 시각도
들어 있지 않다: 같은 원본은 어느 컴퓨터에서든 같은 바이트를 낸다.

## 안에서 무슨 일이 일어났나

MSIX 는 ZIP 압축 파일이다. 파일 목록을 본다:

```text
C:\work\hello> rubrapack inspect hello.msix --files
hello.exe	17920	deflate
guide.txt	6	deflate
Assets\Square150x150.png	301	stored
Assets\Square44x44.png	111	stored
Assets\StoreLogo.png	117	stored
Registry.dat	8192	deflate
AppxManifest.xml	2537	deflate
```

`Registry.dat` 가 가상 레지스트리 - 레지스트리 *하이브* 파일 - 다. `AppxManifest.xml` 은 패키지를 설명하고,
`--manifest` 가 그것을 출력한다:

```text
C:\work\hello> rubrapack inspect hello.msix --manifest
...
  <Identity Name="ExampleSoftware.Hello" Publisher="CN=Example Software, OID.2.25.311729368913984317654407730594956997722=1" Version="2.1.0.0" ProcessorArchitecture="x64" />
...
    <Application Id="Hello" Executable="hello.exe" EntryPoint="Windows.FullTrustApplication">
...
          <uap3:FileTypeAssociation Name="examplesoftware.hellodocument" Parameters="&quot;%1&quot;">
...
          <desktop:StartupTask TaskId="AtSignIn" Enabled="false" DisplayName="Hello" />
...
            <desktop:ExecutionAlias Alias="hello.exe" />
...
    <rescap:Capability Name="runFullTrust" />
```

게시자의 `OID.2.25...=1` 부분이 `--unsigned-test` 가 더한 시험 표시다. `runFullTrust` 는 프로그램이 스토어 앱의
제한된 샌드박스가 아니라 여느 데스크톱 프로그램처럼 돈다는 뜻이다. `--files` 가 보이지 않지만 모든 패키지에
두 항목이 더 있다: 모든 파일의 64 KB 블록마다 해시를 적은 `AppxBlockMap.xml`(서명이 덮는 것이 이것이다)과,
파일마다 형식을 적은 `[Content_Types].xml`. 서명한 패키지에는 서명인 `AppxSignature.p7x` 도 있다. 제4부가 ZIP
구조와 블록 맵을 바이트 단위로 보여 준다.
