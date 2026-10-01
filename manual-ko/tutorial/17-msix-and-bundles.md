# MSIX 패키지와 번들

목표: 같은 원본으로 Hello 를 MSIX 패키지 - Windows 의 더 새로운 패키지 형식 - 로도 빌드하고, 이어서 세
아키텍처를 모두 담은 번들 하나로 빌드한다.

## MSI 인가 MSIX 인가?

| | MSI (`.msi`) | MSIX (`.msix`) |
|---|---|---|
| 설치 | 패키지가 말하는 곳 어디든, 시키는 대로 컴퓨터를 바꾼다 | Windows 가 관리하는 봉인된 폴더에. 앱이 쓰는 파일과 레지스트리는 따로 보관된다 |
| 제거 | 패키지가 만든 만큼 깨끗하게 | 늘 완전히: 아무것도 남지 않는다 |
| 서명 | 권장 | **필수**: Windows 는 서명 안 된 MSIX 를 설치하지 않는다(아래의 시험용은 예외) |
| 할 수 있는 것 | 이 튜토리얼의 모든 것 | 파일, 바로가기, 파일 형식, 레지스트리, INI 파일, 글꼴, 서비스, 명령줄 별칭, 로그인할 때 시작, 앱 자신의 프로세스에 주는 환경 변수 |
| 할 수 없는 것 | - | 사용자 지정 동작, 컴퓨터 전체의 환경 변수, 권한, 조건, 대화창 |

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
format = 1

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

[msix-extension.Web]
kind = "firewall"
direction = "in"
protocol = "tcp"
ports = "8080"
profile = "private"

[dir.INSTALLDIR]
path = "$(ProgramFiles)/Hello"

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
`description` 의 기본값은 패키지 이름이다.

`[msix-app.*]` 표가 여럿이면 항목도 여럿이 된다.

### 여러 언어의 이름, 여러 크기의 로고

`display-name-ko = "헬로"`(와 `description-ko`, 패키지는 `[msix]` 의 `display-name-ko` 와
`publisher-display-name-ko`)는 다른 언어의 이름을 준다. Windows 는 사용자의 언어에 맞는 것을 보이고, 그
밖에서는 접미사 없는 글을 보인다. 로고는 옆에 더 선명한 것을 두면 된다 - `Square44x44.png` 옆에
`Square44x44.scale-200.png`(88x88), 원하면 `scale-125`, `-150`, `-400` 도 - 그러면 200% 화면은 그것을 보인다.
rubrapack 은 둘 다 패키지의 `resources.pri`, 곧 Windows 가 이름과 파일을 찾는 색인에 넣고, 패키지에 그런 것이
있을 때만 그것을 쓴다.

## 덤: `[msix-extension.ID]`

- `kind = "alias"`: 콘솔에서 `hello.exe` 라고 치면 어느 폴더에서든 애플리케이션이 시작된다 - 프로그램을 `PATH`
  에 두는 MSIX 식 방법이다.
- `kind = "startup-task"`: 사용자가 로그인할 때 애플리케이션이 시작된다(한 번 실행한 뒤부터). `enabled = false`
  는 사용자가 작업 관리자의 시작 앱에서 켤 때까지 꺼 둔다. 그 목록에는 `display-name` 이 보인다. `task-id` 는
  Windows 가 부를 작업 이름이고, 기본값은 표의 ID 다(아래 매니페스트에 보이듯 여기서는 `AtSignIn`).

- `kind = "firewall"`: 패키지가 설치돼 있는 동안 있는 Windows 방화벽 규칙 - 여기서는 사설 네트워크에서 Hello 로
  들어오는 TCP 8080 연결. `direction` 은 `in` 이나 `out`, `protocol` 은 `tcp` 나 `udp`, `ports` 는 포트 하나나
  `8000-8100` 같은 범위, `profile` 은 `all`(기본값), `domain`, `private`, `public`. `file = "file:ID"` 는 패키지의
  다른 프로그램을 가리킨다.

- `kind = "com-server"`, `"toast"`, `"context-menu"` 는 COM 클래스를 등록한다: 프로그램이나 DLL 이 제공하는 클래스
  (`file`, `class = "{GUID}"`, `args`, DLL 이면 `threading`), 사용자가 알림을 누를 때 Windows 가 프로그램을 시작하는
  클래스, 그리고 어떤 파일 `types` 에 대한 탐색기 오른쪽 메뉴 항목(DLL 과 `verb`). 참조의 "MSIX 전용"에 각각의
  예가 있다.

`[msix-app.*]` 표가 여럿이면 `app = "Hello"` 로 덤이 어느 애플리케이션의 것인지 밝힌다. 기본값은 첫 번째다. MSI
빌드는 이 표들을 뺀다.

## 패키지가 요구하는 것: 능력과 의존 패키지

MSIX 애플리케이션은 데스크톱 프로그램으로 도는 것 말고 쓰는 것을 Windows 에 요구하고, 필요한 프레임워크 패키지를
적는다:

```toml
[msix]
identity-name = "ExampleSoftware.Hello"
publisher = "CN=Example Software, O=Example Software, C=KR"
capabilities = ["internetClient", "allowElevation"]

[msix-dependency.VCLibs]
name = "Microsoft.VCLibs.140.00.UWPDesktop"
publisher = "CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US"
min-version = "14.0.24217.0"

[msix-app.Helper]
executable = "HelperExe"
hidden = true                  # 시작 메뉴 항목 없음
background-color = "#1E3A5F"   # 타일 색
```

- `capabilities` 에는 `internetClient`, `documentsLibrary`, `webcam`, `allowElevation` 같은 이름을 적는다. rubrapack 이
  매니페스트가 원하는 꼴로 쓴다. 목록은 참조에 있다.
- 런타임을 담지 않은 Visual C++ 프로그램은 VCLibs 프레임워크가 필요하다: `[msix-dependency.VCLibs]` 가 Windows 에
  그것을 요구하게 한다(Microsoft Store 에서는 애플리케이션과 함께 설치된다).
- 자기 설치 폴더나 `HKCU` 에 실제로 써야 하는 애플리케이션은 패키지의 쓰기 가상화를 끌 수 있다:
  `file-system-virtualization = false`, `registry-virtualization = false`(Windows 10 1903 이상).

## 선택적 패키지와 수정 패키지

패키지는 다른 패키지에 딸릴 수 있다. *선택적 패키지*는 주 패키지에 콘텐츠 - 더 많은 단계, 플러그인 - 를 더하고, 그
패키지가 있는 곳에만 설치된다:

```toml
[msix]
identity-name = "ExampleSoftware.HelloExtras"
publisher = "CN=Example Software, O=Example Software, C=KR"
main-package = "ExampleSoftware.Hello"
```

*수정 패키지*는 설치된 애플리케이션의 설정 - 기업의 설정 파일과 레지스트리 값 - 을 애플리케이션 자신의 패키지를
건드리지 않고 바꾼다:

```toml
[msix]
identity-name = "ExampleSoftware.HelloSiteSettings"
publisher = "CN=Example Software, O=Example Software, C=KR"
main-package = "ExampleSoftware.Hello"
modification = true
min-version = "10.0.18362.0"
```

둘 다 `[msix-app.*]` 표가 없어도 된다. 선택적 패키지의 파일은 자기 폴더(dir `INSTALLDIR`)에, 수정 패키지의 파일은
가상 파일 시스템에 들어가 주 애플리케이션이 본다. 주 패키지의 게시자가 다른 사람이면 `main-publisher` 로 적는다.

## 다른 표들은 무엇이 되나

- `Programs` 에 있고 애플리케이션의 실행 파일을 가리키는 `[shortcut.StartMenu]` 는 애플리케이션의 시작 메뉴 항목
  *그 자체*다. `Desktop` 의 바로가기도 `min-version = "10.0.19645.0"` 부터 된다. 다른 곳의 바로가기, 인수나
  `icon` 이 있는 바로가기는 MSI 전용이다(`RP1613`).
- `[assoc.HelloDoc]` 는 애플리케이션의 파일 형식 연결이 된다. 그 파일 형식은 애플리케이션의 로고로 보인다.
- `[registry.Greeting]` 은 패키지의 *가상 레지스트리*에 들어간다: 애플리케이션은 값이 실제 레지스트리에 있는
  것처럼 보지만, 컴퓨터의 레지스트리는 그대로다. `Software` 아래의 키만 들어갈 수 있다(`RP1612`).
- `[service.*]`(12장)는 패키지 서비스가 된다: 이름·시작·계정은 같고, Windows 10 2004 판부터 된다
  (`min-version = "10.0.19041.0"`). 패키지를 지우면 함께 사라진다.
- `[ini.*]` 는 패키지 안의 INI 파일이 된다. 패키지를 빌드할 때 쓴다.
- `[env.HelloHome]`: MSIX 는 컴퓨터의 환경을 바꾸지 못한다. 할 수 있는 것은 애플리케이션 자신의 프로세스에 변수를
  주는 것이다: 애플리케이션이 rubrapack 의 작은 실행기(패키지 안의 `rubrapack\Hello.exe`)를 거쳐 시작하고, 실행기가
  변수를 정한 뒤 `hello.exe` 를 시작한다. `HELLO_HOME` 은 다른 프로그램도 보라고 둔 것이라 이 원본은
  `msi-only = true` 로 MSI 에만 둔다. 그 줄이 없으면 MSIX 는 그 변수를 Hello 에게만 준다.

`msi-only = true` 는 표를 MSI 에는 두고 MSIX 에서는 뺀다. MSIX 가 아예 담지 못하는 것 - `[require.*]`,
`[action.*]` - 은 내가 뜻을 밝힐 때까지 빌드를 멈춘다:

```text
C:\work\hello> rubrapack build hello.toml -o hello.msix --unsigned-test
hello.toml:80:1: error[RP1605]: [require.Win81] cannot go into an MSIX; add msi-only = true to build the MSIX without it
```

rubrapack 은 아무것도 몰래 빼지 않는다.

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

프로그램이 든 패키지는 서명하면 Windows 자신의 서명기가 하듯 `AppxMetadata\CodeIntegrity.cat` - 그 프로그램들의
해시를 같은 키로 서명한 카탈로그 - 도 갖는다. 서명된 코드만 돌 수 있는 곳(S 모드, 응용 프로그램 제어)에서 Windows
의 코드 무결성은 패키지 안의 서명 없는 프로그램을 이것과 대조한다. rubrapack 의 카탈로그는 Windows 자신의 해시와
`signtool` 로 확인했고, 그런 기기에서는 확인하지 않았다.

## 모든 아키텍처를 파일 하나에: 번들

`.msixbundle` 로 끝나는 출력은 15장처럼 `--arch` 의 아키텍처마다 원본을 한 번씩 빌드해 패키지들을 파일 하나에
담는다:

```text
C:\work\hello> rubrapack build hello.toml -o hello.msixbundle --arch x64,x86,arm64 --unsigned-test
C:\work\hello> rubrapack inspect hello.msixbundle --files
AppxMetadata\AppxBundleManifest.xml	1616	deflate
ExampleSoftware.Hello_2.1.0.0_x64.msix	11119	stored
ExampleSoftware.Hello_2.1.0.0_x86.msix	11208	stored
ExampleSoftware.Hello_2.1.0.0_arm64.msix	11119	stored
```

Windows 는 그중 자기 처리기에 맞는 패키지를 설치한다. `--key` 를 주면 번들과 그 안의 패키지 모두에 서명한다.

## 웹 사이트에서 업데이트: `.appinstaller`

Windows 는 MSIX 를 올려 둔 웹 사이트(또는 공유 폴더)에서 스스로 최신으로 유지할 수 있다. 패키지를 둘 곳을 적으면
rubrapack 이 그 옆에 *App Installer 파일*을 쓴다:

```text
[msix]
...
appinstaller-uri = "https://example.com/hello/hello.appinstaller"
package-uri = "https://example.com/hello/hello.msixbundle"
update-hours = 24                 # Windows 가 새 판을 찾는 간격(0: 시작할 때마다)
```

```text
C:\work\hello> rubrapack build hello.toml -o hello.msixbundle --arch x64,x86,arm64 --key ...
```

은 `hello.msixbundle` 과 `hello.appinstaller` 를 쓴다. 둘을 그 주소에 둔다. 사람들은 `.appinstaller` 로 설치하고
(열거나 `Add-AppxPackage -AppInstallerFile`), 그 뒤로 Windows 는 앱이 시작될 때 그 주소를 확인한다. 다음 판을 새
`.appinstaller` 와 함께 같은 주소에 올리면 스스로 업데이트된다. `update-prompt = true` 는 사용자에게 먼저 묻고,
`update-blocks = true` 는 업데이트가 끝날 때까지 앱을 기다리게 하며, `update-background = true` 는 여덟 시간마다
뒤에서도 확인한다. App Installer 는 서명된 패키지만 받는다(16장).

## 압축

파일은 deflate 로 압축되고, 그림처럼 이미 압축된 파일은 그대로 저장된다. `--msix-compress store` 는 모두 그대로
저장한다. 패키지는 커지지만(여기서는 11119 에서 30072 바이트로) 여는 것은 빨라진다. MSIX 에는 날짜도 시각도
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
AppxManifest.xml	2960	deflate
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
