# 레지스트리, 환경 변수, INI 파일, 파일 형식과 링크

목표: 다른 프로그램이 찾는 설정을 Hello 에 준다 - 모든 형식의 레지스트리 값, 환경 변수, INI 파일의 한 줄 - 그리고
Windows 가 `.hello` 파일과 `hello:` 링크를 Hello 로 열게 한다.

## 2분 만에 보는 레지스트리

Windows *레지스트리*는 폴더처럼 짜인 설정 데이터베이스다: *키*는 다른 키와 *값*을 담고, 값에는 이름, 형식, 데이터가
있다. 맨 위 키("루트")는:

| 루트 | 담는 것 |
|---|---|
| `HKLM`(HKEY_LOCAL_MACHINE) | 컴퓨터 전체의 설정; 쓰려면 관리자 권한이 필요하다 |
| `HKCU`(HKEY_CURRENT_USER) | 지금 사용자의 설정 |
| `HKCR`(HKEY_CLASSES_ROOT) | 파일 형식과 링크(둘을 합쳐 보인 것) |
| `HKMU` | rubrapack(과 Windows Installer)의 "모든 사용자용이면 HKLM, 한 사용자용이면 HKCU" |

Windows 에서 `regedit` 를 실행하면 둘러볼 수 있다(모르는 것은 바꾸지 않는다).

## 원본

```toml
# tutorial 11: hello.toml
format = 1

[package]
name = "Hello"
manufacturer = "Example Software"
version = "$(VERSION)"
arch = "x64"
upgrade-code = "{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}"

[define]
VERSION = "1.9.0"

[dir.INSTALLDIR]
path = "$(ProgramFiles)/Hello"

[file.Hello]
dir = "INSTALLDIR"
source = "dist/hello.exe"

[file.Settings]
dir = "INSTALLDIR"
source = "dist/settings.ini"

[registry.InstallDir]
root = "HKLM"
key = 'SOFTWARE\Example Software\Hello'
name = "InstallDir"
value = "[INSTALLDIR]"

[registry.Greeting]
root = "HKLM"
key = 'SOFTWARE\Example Software\Hello'
name = "Greeting"
value = "Hello, world"
with = "file:Hello"

[registry.Runs]
root = "HKLM"
key = 'SOFTWARE\Example Software\Hello'
name = "MaxRuns"
type = "dword"
value = 100

[registry.BigNumber]
root = "HKLM"
key = 'SOFTWARE\Example Software\Hello'
name = "Limit"
type = "qword"
value = "0x100000000"

[registry.Colors]
root = "HKLM"
key = 'SOFTWARE\Example Software\Hello'
name = "Colors"
type = "multi"
value = ["red", "green", "blue"]

[registry.LogPath]
root = "HKLM"
key = 'SOFTWARE\Example Software\Hello'
name = "LogPath"
type = "expand"
value = "%TEMP%\\hello.log"

[registry.Legacy32]
root = "HKLM"
key = 'SOFTWARE\Example Software\Hello'
name = "Mode"
value = "compatible"
view = "32"

[registry.OldKey]
root = "HKLM"
key = 'SOFTWARE\Example Software\Hello Old'
remove = true

[registry.UserChoice]
root = "HKLM"
key = 'SOFTWARE\Example Software\Hello'
name = "FirstRun"
type = "dword"
value = 1
keep = true

[env.HelloHome]
name = "HELLO_HOME"
value = "[INSTALLDIR]"

[env.Path]
name = "PATH"
value = "[INSTALLDIR]"
mode = "append"

[ini.InstalledVersion]
dir = "INSTALLDIR"
file = "settings.ini"
section = "hello"
key = "installed"
value = "[ProductVersion]"

[ini.Plugins]
dir = "INSTALLDIR"
file = "settings.ini"
section = "hello"
key = "plugins"
value = "core"
mode = "add"

[assoc.HelloDoc]
extension = ".hello"
prog-id = "Example.HelloDocument"
description = "Hello document"
target = "file:Hello"
icon = "file:Hello"
args = "--open \"%1\""

[protocol.HelloLink]
name = "hello"
description = "Hello link"
target = "file:Hello"
```

TOML 메모: *작은*따옴표 문자열은 쓴 그대로 쓰이므로 `'SOFTWARE\Example Software\Hello'` 에는 백슬래시를 겹쳐 쓸
필요가 없다. 큰따옴표 안에서는 백슬래시가 이스케이프를 시작한다(`"%TEMP%\\hello.log"`).

## 레지스트리 값: `[registry.ID]`

| `type` | TOML 값 | 레지스트리 형식 |
|---|---|---|
| `string`(기본) | 문자열 | REG_SZ |
| `expand` | `%변수%` 가 든 문자열 | REG_EXPAND_SZ - 값을 읽을 때 Windows 가 `%TEMP%` 를 펼친다 |
| `dword` | 정수 0 .. 4294967295 | REG_DWORD, 32비트 |
| `qword` | 정수, 또는 `"0x"` 와 16진수 16자리까지 | REG_QWORD, 64비트 |
| `binary` | 16진수, `"01ff"` | REG_BINARY |
| `multi` | 문자열 배열 | REG_MULTI_SZ |

- `name` 이 없으면: 키의 기본값(regedit 에서 "(기본값)").
- `value` 는 서식 문자열이다: `[INSTALLDIR]` 은 설치 폴더, `[#Hello]` 는 파일 `Hello` 의 경로가 된다.
- 모든 값은 제품과 함께 다시 지워진다. `keep = true` 는 남긴다(여기서는 프로그램이 바꿀 수 있는 `FirstRun`).
- `with = "file:Hello"` 는 값을 그 파일에 묶는다: 파일과 함께 설치되고 지워진다(`Greeting`).
- `view = "32"`: 64비트 Windows 는 32비트 프로그램을 위한 레지스트리를 따로 둔다. `SOFTWARE\Example
  Software\Hello` 를 읽는 32비트 프로그램은 그쪽을 본다. x64 패키지의 값은 `view = "32"` 가 아니면 64비트
  쪽으로 간다.
- `remove = true` 는 설치하는 동안 `name` 이 가리키는 값을 - `name` 이 없으면 키 전체를 - 지운다(`OldKey`: 옛
  판이 남긴 것).

Windows Installer 는 REG_QWORD 를 스스로 쓰지 못한다. `qword` 값을 위해 rubrapack 은 작은 도우미 DLL 을 패키지에
더하고, 이것이 값을 쓰고 설치가 실패하면 이전 값을 되돌린다.

## 환경 변수: `[env.ID]`

컴퓨터 전체의 변수(사용자별 설치에서는 사용자의 것)다. `mode = "set"`(기본)은 정하고, `append` 는 있는 것의 끝에
`;값` 을, `prepend` 는 앞에 `값;` 을 더한다. 제품을 지우면 정확히 그만큼 되돌린다: `HELLO_HOME` 은 지워지고,
`PATH` 에서는 `;C:\Program Files\Hello\` 만 빠진다. `keep = true` 는 남긴다. 설치 뒤에 시작한 프로그램이
바뀐 것을 본다(이미 열려 있던 터미널은 못 본다).

## INI 파일: `[ini.ID]`

INI 파일은 `[섹션]` 과 `키=값` 줄로 된 글 파일이다. `mode = "set"`(기본)은 섹션에 `키=값` 을 쓰고, `add` 는 값을
쉼표로 나눈 목록에 더하고(`plugins=core`, 또는 `plugins=extra` 가 있었다면 `plugins=extra,core`), `remove` 는
키를 지운다 - 파일을 설치하기 전에 하므로 옛 판이 남긴 INI 파일을 치운다. 제거는 `set` 과 `add` 가 쓴 것을
빼낸다. `file` 은 `dir` 안의 파일 이름이며, 패키지가 설치하는 파일이 아니어도 된다.

설치 뒤 `settings.ini` 는:

```text
[hello]
greeting=Hello
installed=1.9.0
plugins=core
```

## 파일 형식: `[assoc.ID]`

`[assoc.HelloDoc]` 는 Windows 가 `.hello` 파일을 Hello 로 열게 한다:

- `extension` 은 점을 포함한 소문자 파일 끝이다.
- `prog-id` 는 문서 종류의 이름이다(`회사.종류`). 여러 확장자가 하나를 함께 쓸 수 있다.
- `description` 은 탐색기가 그 형식을 부르는 이름("Hello document")이다.
- `target` 은 프로그램, `args` 는 인자다(`"%1"` 은 파일 경로이며 기본값이다).
- `icon`(`file:ID`)은 문서가 보일 첫 아이콘을 가진 파일이다.

사용자가 `.hello` 에 이미 다른 프로그램을 골라 두었다면 Windows 는 그 선택을 지킨다.

## 링크: `[protocol.ID]`

`[protocol.HelloLink]` 는 `hello:` 방식을 등록한다. 브라우저나 실행 상자의 `hello:world` 링크가 링크 전체를
인자로 Hello 를 시작한다.

## 해 보기

설치한 뒤:

- `regedit`: `HKEY_LOCAL_MACHINE\SOFTWARE\Example Software\Hello` 에 값들이 있고,
  `...\WOW6432Node\Example Software\Hello` 에 `Mode`(32비트 쪽)가 있다.
- 새 터미널: `echo %HELLO_HOME%` 과 `echo %PATH%`.
- `C:\Program Files\Hello\settings.ini`: 새 두 줄.
- 바탕화면의 `test.hello` 파일이 Hello 로 열리고, Win+R 에 `hello:world`, Enter 로 Hello 가 시작된다.

Hello 를 지우면 `FirstRun` 말고는 모두 사라진다.

## 안에서 무슨 일이 일어났나

```text
C:\work\hello> rubrapack inspect hello.msi Registry
...
Colors	2	SOFTWARE\Example Software\Hello	Colors	[~]red[~]green[~]blue[~]	C_f023...
Greeting	2	SOFTWARE\Example Software\Hello	Greeting	Hello, world	C_185f...
HelloDoc.Ext	0	.hello		Example.HelloDocument	C_185f...
LogPath	2	SOFTWARE\Example Software\Hello	LogPath	#%%TEMP%\hello.log	C_2e80...
Runs	2	SOFTWARE\Example Software\Hello	MaxRuns	#100	C_97ef...
...
```

둘째 열은 루트다(0 HKCR, 1 HKCU, 2 HKLM, -1 HKMU). 값 열은 Windows Installer 가 원하는 대로 첫 글자에 형식을 싣는다:
`#100` 은 DWORD, `#%` 는 펼치는 문자열, `[~]` 는 여러 문자열을 가르고, `#x` 는 이진 데이터를 시작한다. 파일 형식과
링크는 프로그램 컴포넌트 안의 HKCR 아래 레지스트리 값일 뿐이다. QWORD 값은 이 표에 없다: 도우미 DLL 을 위한 속성
`RP_QWORDS` 에 실려 간다. 환경 변수는 `Environment` 표에 있다:

```text
HelloHome	=-*HELLO_HOME	[INSTALLDIR]	C_0fa9...
Path	=-*PATH	[~];[INSTALLDIR]	C_82df...
```

이름에서 `=` 는 "정하기", `-` 는 "제거 때 지우기", `*` 는 "시스템 변수"다. 값의 `[~]` 는 원래 있던 것을 뜻한다.
