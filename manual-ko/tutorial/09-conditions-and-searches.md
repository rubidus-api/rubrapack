# 조건과 검색

목표: 무엇을 설치할지 *사용자 컴퓨터에서* 정한다. 64비트 Windows 에서만 설치하고, 메모장이 있는 곳에서만 한 부분을
권하고, 바탕화면 바로가기를 사용자가 끌 수 있게 하고, 다음 판은 사용자가 이번 판에 고른 폴더에 설치한다.

## 속성

Windows Installer 는 실행하는 동안 *속성*이라는 이름 붙은 값을 지닌다: `ProductName`, `INSTALLDIR`,
`VersionNT64`(64비트 Windows 에서 정해짐), 그리고 내 것들. 이름이 모두 대문자인 속성은 *공개*다: 명령줄
(`msiexec /i hello.msi DESKTOP_SHORTCUT=0`)과 대화창이 정할 수 있다. *조건*은 속성에 대한 작은 식으로, 참이거나
거짓이다:

| 조건 | 참일 때 |
|---|---|
| `VersionNT64` | 속성이 정해져 있을 때(비어 있지 않음): 여기서는 64비트 Windows |
| `NOT Installed` | 제품이 아직 설치되지 않았을 때(첫 설치) |
| `DESKTOP_SHORTCUT = "1"` | 속성 값이 정확히 `1` 일 때 |
| `VersionNT >= 603` | Windows 판 번호가 6.3 이상일 때 |
| `A AND (B OR NOT C)` | 생각하는 대로의 조합 |

## 원본

```toml
# tutorial 09: hello.toml
format = 1

[package]
name = "Hello"
manufacturer = "Example Software"
version = "$(VERSION)"
arch = "x64"
upgrade-code = "{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}"
ui = "features"

[define]
VERSION = "1.7.0"

[property.DESKTOP_SHORTCUT]
value = "1"
secure = true

[require.Windows64]
condition = "VersionNT64"
message = "[ProductName] needs 64-bit Windows."

[search.Notepad]
property = "NOTEPAD_PATH"
kind = "file"
path = "$(SystemRoot)"
file = "notepad.exe"

[search.PreviousDir]
property = "INSTALLDIR"
kind = "registry"
root = "HKLM"
key = "Software\\Example Software\\Hello"
name = "InstallDir"

[feature.Main]
title = "Hello"
required = true

[feature.NotepadHelper]
title = "Open notes in Notepad"
description = "Only offered where Notepad is installed."
when = "NOTEPAD_PATH"

[dir.INSTALLDIR]
path = "$(ProgramFiles)/Hello"
feature = "Main"

[file.Hello]
dir = "INSTALLDIR"
source = "dist/hello.exe"

[file.Settings]
dir = "INSTALLDIR"
source = "dist/settings.ini"
feature = "NotepadHelper"

[registry.RememberDir]
root = "HKLM"
key = "Software\\Example Software\\Hello"
name = "InstallDir"
value = "[INSTALLDIR]"
feature = "Main"

[shortcut.StartMenu]
dir = "Programs"
name = "Hello"
target = "file:Hello"

[shortcut.Desktop]
dir = "Desktop"
name = "Hello"
target = "file:Hello"
when = "DESKTOP_SHORTCUT = \"1\""
```

## 내 속성: `[property.NAME]`

`[property.DESKTOP_SHORTCUT]` 은 기본값 `1` 로 속성을 만든다. 공개가 되려면 이름이 모두 대문자여야 한다.
Windows 나 rubrapack 이 스스로 쓰는 이름(`ALLUSERS`, `ARP...`, `MSI...` 등)은 거부한다.

- `secure = true`: 모든 사용자를 위한 설치는 두 부분으로 돈다 - 사용자 부분(대화창)과, 컴퓨터를 바꾸는 권한 높은
  부분. 첫 부분이나 명령줄에서 정한 값은 속성이 *보안(secure)*일 때만 둘째 부분에 닿는다. `when` 조건이 보는
  속성은 모두 보안으로 한다. (내 대화창 페이지의 속성과 검색 결과는 rubrapack 이 알아서 그렇게 한다.)
- `hidden = true`: 값을 설치 기록에 쓰지 않는다 - 암호와 키를 위한 것이다.

## 이럴 때만 설치하기: `when`

기능, 파일이나 파일 묶음, 바로가기, 레지스트리 값, 환경 변수, INI 값에 `when` 을 달면 첫 설치 때 조건이 참일
때만 설치한다:

- 바탕화면 바로가기의 `when = "DESKTOP_SHORTCUT = \"1\""`: 기본으로는 만들고, `msiexec /i hello.msi
  DESKTOP_SHORTCUT=0` 이면 만들지 않는다. 14장에서 이 속성을 내 페이지의 확인란으로 바꾼다.
- 기능의 `when = "NOTEPAD_PATH"`: 조건이 거짓인 곳에서는 그 기능을 설치하지도 트리에 보이지도 않는다.

복구는 설치된 것을 그대로 두고, 메이저 업그레이드(다음 판)는 다시 정한다.

## 설치 거부하기: `[require.ID]`

요구 조건은 `condition` 이 거짓이면 첫 설치를 `message` 와 함께 멈춘다 - 여기서는 32비트 Windows 에서: "Hello
needs 64-bit Windows." 복구와 제거는 절대 막지 않는다.

## 먼저 둘러보기: `[search.ID]`

검색은 다른 무엇보다 먼저 돌고, 찾은 것을 속성에 넣는다(못 찾으면 비움). 조건이 그것을 볼 수 있다:

| `kind` | 찾는 것 | 키 |
|---|---|---|
| `file` | 파일의 전체 경로 | `path`(알려진 폴더와 상대 경로), `file`, `min-version`(프로그램용) |
| `dir` | 폴더 | `path` |
| `registry` | 레지스트리의 값 | `root`, `key`, `name`, `view`(32비트 보기는 `32`) |
| `component` | 다른 제품 컴포넌트의 키 파일 | `component-guid` |

`[search.Notepad]` 는 Windows 폴더에서 `notepad.exe` 를 찾아 경로를 `NOTEPAD_PATH` 에 넣는다.

## 설치 폴더 기억하기

사용자가 Hello 를 `D:\Tools\Hello` 에 설치했다면 다음 판도 Program Files 로 돌아가지 말고 그리 가야 한다. 두
조각으로 한다:

1. `[registry.RememberDir]` 는 설치할 때 폴더를 레지스트리에 적는다: 값 안의 `[INSTALLDIR]` 은 폴더로 바뀐다
   (레지스트리는 11장).
2. `[search.PreviousDir]` 는 그것을 다시 읽는다. `property` 가 dir ID `INSTALLDIR` 이므로 찾은 것이 그 dir 의
   *기본값*이 된다 - 그 폴더가 아직 있을 때만이고, 명령줄로 준 폴더가 여전히 이긴다.

## 안에서 무슨 일이 일어났나

```text
C:\work\hello> rubrapack inspect hello.msi LaunchCondition
Installed OR (VersionNT64)	[ProductName] needs 64-bit Windows.
C:\work\hello> rubrapack inspect hello.msi AppSearch
NOTEPAD_PATH	Notepad
RpFound_PreviousDir	PreviousDir
C:\work\hello> rubrapack inspect hello.msi Condition
NotepadHelper	0	NOT Installed AND NOT (NOTEPAD_PATH)
```

(머리줄은 뺐다.) rubrapack 은 요구 조건에 `Installed OR` 를 붙여, 설치된 제품은 늘 복구하거나 지울 수 있게 한다.
기능의 `when` 은 `Condition` 표의 한 행이 되어 조건이 거짓이면 수준을 0 - "설치 안 함, 안 보임" - 으로 만든다.
`NOT Installed` 는 속성이 사라진 제거 때 이 일이 일어나지 않게 막는다. 바로가기는 모두 컴포넌트를 따로
받는다(5장). 바탕화면 바로가기의 것은 `Condition` 열에 `when` 을 담는다:

```text
C_2ea6d5bfd9b4a9073b30	{BCBFF063-...}	DesktopFolder	260	DESKTOP_SHORTCUT = "1"	R_7f1795ed8aa7ac1a1c60
```
