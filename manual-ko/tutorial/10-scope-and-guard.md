# 누구를 위해 설치하나, 그리고 보호되는 폴더

목표: 관리자 권한이 없는 사용자도 자기만을 위해 Hello 를 설치하게 하되, 관리자는 여전히 모든 사용자를 위해 설치할
수 있게 한다 - 그리고 남이 미리 만들어 둔 설치 폴더는 거부한다.

## 세 가지 범위

지금까지의 패키지는 모두 *컴퓨터 전체*에 설치했다: Program Files 에, 컴퓨터의 모든 사용자를 위해. 그래서 관리자
권한(사용자 계정 컨트롤 창)이 필요했다. `[package]` 의 `scope` 가 이것을 바꾼다:

| `scope` | 설치 대상 | 관리자 필요 | `ProgramFiles/Hello` 는 | 시작 메뉴, 바탕화면 | 레지스트리 `HKMU` |
|---|---|---|---|---|---|
| `machine`(기본) | 모든 사용자 | 예 | `C:\Program Files\Hello` | 모든 사용자의 것 | `HKLM` |
| `user` | 지금 사용자 | 아니요 | `%LOCALAPPDATA%\Programs\Hello` | 사용자의 것 | `HKCU` |
| `dual` | 기본은 사용자, 고르면 모든 사용자 | 모든 사용자일 때만 | 위의 둘 중 하나 | 둘 중 하나 | 둘 중 하나 |

`%LOCALAPPDATA%` 는 사용자 자신의 `C:\Users\<이름>\AppData\Local` 이다. 사용자별 설치는 허락이 필요 없고 컴퓨터의
다른 사용자에게 보이지 않는다 - 작은 도구에 흔히 알맞다.

어떤 것은 컴퓨터 전체에만 있어서 `scope = "machine"` 이 필요하다: 서비스, 글꼴, 권한, 그리고 `Windows`, `System`,
`Fonts`, `CommonAppData` 폴더. rubrapack 은 `user` 나 `dual` 패키지에서 이것들을 거부한다.

## 원본

```toml
# tutorial 10: hello.toml
[package]
name = "Hello"
manufacturer = "Example Software"
version = "$(VERSION)"
arch = "x64"
upgrade-code = "{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}"
scope = "dual"
ui = "installdir"

[define]
VERSION = "1.8.0"

[dir.INSTALLDIR]
path = "ProgramFiles/Hello"
guard = true

[file.Hello]
dir = "INSTALLDIR"
source = "dist/hello.exe"

[shortcut.StartMenu]
dir = "Programs"
name = "Hello"
target = "file:Hello"
```

## 겸용(dual) 패키지

`scope = "dual"` 에 대화창이 있으면, 약관 다음 페이지가 묻는다:

```text
( ) Just me             (기본, 나만)
( ) Everyone on this computer   (이 컴퓨터의 모든 사용자)
```

"모든 사용자"는 관리자 권한을 묻는다. 설치 폴더, 시작 메뉴 항목, "설치된 앱" 항목이 선택을 따른다. 대화창이 없으면
겸용 패키지는 지금 사용자를 위해 설치되고, 관리자 터미널에서 이렇게 하면 모든 사용자를 위해 설치된다:

```text
msiexec /i hello.msi ALLUSERS=1 MSIINSTALLPERUSER=""
```

(`MSIINSTALLPERUSER=""` 는 "비움"이다: 속성을 지운다.) `user` 패키지는 모든 사용자를 위한 설치를 거부한다.

## 보호되는 설치 폴더: `guard`

어떤 프로그램은 다른 프로그램이 불러 쓰거나 높은 권한으로 돈다 - 입력기, 셸 확장, 서비스. 이런 프로그램에게 *이미
있는* 설치 폴더는 위험하다: 그것을 만든 누군가가 파일을 남겨 두었을 수 있다(심어 둔 DLL 은 프로그램의 권한으로
불려 온다). dir 에 `guard = true` 를 달면, 그 폴더가 다음과 같을 때 첫 설치가 파일을 하나도 두기 전에 멈춘다:

- 이미 있고, 주인이 SYSTEM, Administrators, TrustedInstaller 가 아니다.
- 교차점(junction)이나 다른 링크를 거쳐 닿는다(그 폴더든 위의 어느 폴더든).

아직 없는 폴더는 통과한다 - 설치 파일이 만든다. 복구, 제거, 그리고 옛 판이 만든 폴더로의 업그레이드는 영향받지
않는다. 설치는 `DirGuardText` 안내문(대화창에서 고른 언어로)과 함께 끝나며, 조용한 설치에서도 마찬가지이고, 기록에는
찾은 주인이 적힌다. 검사는 패키지가 싣고 가는 rubrapack 의 작은 도우미 DLL 이 한다.

## 해 보기

일반 사용자(관리자가 아님)로 `hello.msi` 를 두 번 누르고 "Just me" 를 그대로 둔다: 허락 창이 없다. Hello 는
`%LOCALAPPDATA%\Programs\Hello` 에 들어가고, 시작 메뉴와 "설치된 앱"에서 나에게만 보인다. 지운 뒤 "Everyone" 을
골라 다시 설치하면 Windows 가 관리자 권한을 묻고 Program Files 에 설치한다.

폴더 보호를 보려면: 일반 사용자로 `C:\Users\Public\Hello` 폴더를 만든다(그러면 주인이 그 사용자다). 관리자
터미널에서 그 폴더에 모든 사용자용으로 설치한다:

```text
msiexec /i hello.msi ALLUSERS=1 MSIINSTALLPERUSER="" INSTALLDIR="C:\Users\Public\Hello\"
```

설치가 보호 안내문과 함께 멈추고 아무것도 설치하지 않는다. 폴더를 지우고 같은 명령을 다시 하면 이번에는 설치 파일이
폴더를 직접 만들고 설치가 끝까지 간다.

## 안에서 무슨 일이 일어났나

```text
C:\work\hello> rubrapack inspect hello.msi Property
...
ALLUSERS	2
MSIINSTALLPERUSER	1
RP_GUARD	INSTALLDIR
...
```

`ALLUSERS = 2` 와 `MSIINSTALLPERUSER = 1` 의 짝은 Windows Installer 의 "둘 다를 위한 단일 패키지"다: 따로 말하지
않으면 사용자별이다. 컴퓨터 전체 패키지는 `ALLUSERS = 1` 이다. `RP_GUARD` 는 도우미 DLL 에게 보호할 dir 을
알린다. 묻는 페이지는 `Dialog` 표의 `RpScopeDlg` 다.
