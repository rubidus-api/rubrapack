# 서비스, 글꼴, 권한

목표: 스스로 시작하는 백그라운드 서비스, 모든 프로그램이 쓸 수 있는 글꼴, 접근 권한을 내가 정한 데이터 폴더를
설치한다 - 컴퓨터 전체 패키지만 할 수 있는 세 가지다.

## 원본

`dist\hellosvc.exe` 는 서비스 프로그램, `dist\HelloSans.ttf` 는 글꼴 파일이다.

```toml
# tutorial 12: hello.toml
format = 1

[package]
name = "Hello"
manufacturer = "Example Software"
version = "$(VERSION)"
arch = "x64"
upgrade-code = "{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}"
reboot = "suppress"

[define]
VERSION = "1.10.0"

[dir.INSTALLDIR]
path = "$(ProgramFiles)/Hello"

[dir.DataDir]
path = "$(ProgramData)/Hello"

[dir.FontsDir]
path = "$(Fonts)"

[file.Hello]
dir = "INSTALLDIR"
source = "dist/hello.exe"

[file.Service]
dir = "INSTALLDIR"
source = "dist/hellosvc.exe"

[file.Font]
dir = "FontsDir"
source = "dist/HelloSans.ttf"

[service.HelloService]
file = "file:Service"
name = "HelloService"
display-name = "Hello background service"
description = "Keeps Hello's greetings up to date."
start = "auto"
account = "LocalService"
args = "--service"
start-on-install = true

[font.HelloSans]
file = "file:Font"
title = "Hello Sans"

[permission.DataFolder]
target = "dir:DataDir"
sddl = "D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1301bf;;;BU)"
```

## 서비스: `[service.ID]`

*서비스*는 Windows 가 창 없이 뒤에서, 흔히 누군가 로그인하기도 전에 돌리는 프로그램이다(서비스 관리 도구
`services.msc` 가 목록을 보인다).

| 키 | 뜻 |
|---|---|
| `file` | 서비스 프로그램: `file:` 과 `[file.*]` ID |
| `name` | 서비스의 내부 이름(`sc start HelloService` 에 쓴다) |
| `display-name`, `description` | 서비스 관리 도구가 보이는 것 |
| `start` | `auto`(Windows 가 시작할 때마다), `demand`(무언가가 시작할 때), `disabled` |
| `account` | 누구로 도나: `LocalSystem`(모든 권한), `LocalService`(적은 권한 - 이것을 권한다), `NetworkService` |
| `args` | 명령줄 인자 |
| `start-on-install = true` | 설치 끝에 시작 |

설치 파일은 서비스의 파일을 바꾸기 전(업그레이드나 복구)과 제거 때 서비스를 멈추고, 제거 때 서비스를 지운다.
프로그램은 정말 서비스여야 한다 - Windows 서비스 관리자에 응답하는 것. 보통 프로그램이면 시작이 실패하고, 그러면
설치도 실패한다.

## 글꼴: `[font.ID]`

글꼴은 파일을 글꼴 폴더에 두고 - `path = "$(Fonts)"`, 폴더 하나만 쓴 `[dir.FontsDir]` - `[font.ID]` 로 등록해
설치한다. `title` 은 Windows 가 보이는 이름이다. 없으면 Windows 가 글꼴 파일에서 이름을 읽는다. 설치 뒤 모든
프로그램이 그 글꼴을 보고, 제거는 등록을 풀고 파일을 지운다.

## 접근 권한: `[permission.ID]`

Windows 의 모든 파일, 폴더, 레지스트리 키에는 누가 읽고 바꿀 수 있는지 말하는 접근 목록이 있다. `[permission.ID]` 는
*SDDL*(Security Descriptor Definition Language)로 쓴 그 목록을, 패키지가 만드는 폴더(`dir:ID`), 패키지의 파일
(`file:ID`), 패키지가 쓰는 레지스트리 값(`registry:ID`)에 정한다. 권한을 준 폴더는 파일이 없어도 만들어진다.

위의 SDDL 문자열을 조각조각 보면:

| 조각 | 뜻 |
|---|---|
| `D:` | 접근 목록(DACL)이 이어진다 |
| `PAI` | 보호됨(부모 폴더에서 물려받지 않음), 그리고 안에 든 것에 물려줌 |
| `(A;OICI;FA;;;SY)` | 허용(Allow), 파일과 하위 폴더에(Object·Container Inherit), 모든 권한(Full Access), SYSTEM 에게 |
| `(A;OICI;FA;;;BA)` | 같은 것을 기본 제공 Administrators 에게 |
| `(A;OICI;0x1301bf;;;BU)` | 기본 제공 Users 는 읽고 쓰고 지울 수 있지만 권한은 바꾸지 못함 |

그래서 `C:\ProgramData\Hello` 는 모든 사용자가 쓸 수 있지만, 누가 쓸 수 있는지는 관리자만 바꾼다. 손으로 설정한
폴더의 문자열은 `icacls` 나 PowerShell 의 `(Get-Acl C:\경로).Sddl` 로 얻는다.

## 아직 실행 중인 프로그램: 사전 점검

패키지는 무엇이든 바꾸기 전에 어떤 프로그램이 제품의 파일을 쓰고 있는지 - 제품 자신의 프로그램과, 제품의 DLL 을 싣고
있는 프로그램 - 살피고 모두 종료할지 묻는다(예는 종료, 아니요는 종료하지 않고 계속, 취소는 중단). 창이 없으면(`/qn`)
대신 오류로 멈춘다. 명령줄이 `RPCLOSE=yes` 나 `RPCLOSE=no` 를 주면 그대로 한다:

```text
C:\work\hello> msiexec /i hello-2.0.0.msi /qn RPCLOSE=yes
```

`[package]` 의 `close-programs = "always"` 나 `"never"` 는 답을 정해 두고, `preflight = false` 는 이 단계를 뺀다(나머지
점검은 참조의 "진행하기 전에").

## 다시 시작하기: `reboot`

어떤 파일은 Windows 를 다시 시작해야 바꿀 수 있다. `reboot = "suppress"`(기본)는 스스로 다시 시작하지 않는다:
설치가 종료 코드 3010("다시 시작 필요")으로 끝나고 사용자가 나중에 다시 시작한다. `reboot = "allow"` 는 필요할 때
Windows Installer 가 다시 시작을 묻게(`/qn` 에서는 다시 시작하게) 한다.

재시작을 기다리는 것은 대개 실행 중인 프로그램이 아직 쥐고 있는 파일이다. 패키지의 정리 작업이 그보다 먼저 - 그
프로그램이 닫히는 대로 - 지우고 스스로 사라진다. `[package]` 의 `cleanup = false` 는 정리 작업을 뺀다(참조의
"나중에 정리하기").

## 해 보기

(관리자로) 설치한 뒤: `services.msc` 에 "Hello background service" 가 돌고 있고, 글 편집기가 "Hello Sans" 글꼴을
권하고, `C:\ProgramData\Hello` 의 보안 탭에 항목 셋이 보인다. Hello 를 지우면 서비스, 글꼴, 폴더가 사라진다.

## 안에서 무슨 일이 일어났나

```text
C:\work\hello> rubrapack inspect hello.msi ServiceInstall
HelloService	HelloService	Hello background service	16	2	1			NT AUTHORITY\LocalService		--service	C_d677...	Keeps Hello's greetings up to date.
C:\work\hello> rubrapack inspect hello.msi ServiceControl
HelloService	HelloService	163		1	C_d677...
```

`16` 은 "자기 프로세스의 서비스", `2` 는 "자동 시작", `1` 은 "오류를 보통으로 알림"이다. `ServiceControl` 의
`163` 은 비트 플래그의 합이다: 1(설치 때 시작) + 2(설치 때 멈춤) + 32(제거 때 멈춤) + 128(제거 때 삭제). 이런
합이 어떻게 되는지는 제3부의 [비트 플래그](../basics/02-numbers-and-flags.md#비트-플래그)가 설명한다. 권한이 있는 패키지는 Windows Installer 5.0 을 밝힌다(요약 정보의 스키마,
`rubrapack inspect hello.msi --summary` 의 `14 500`). `MsiLockPermissionsEx` 표가 나머지보다 새것이기 때문이다.
