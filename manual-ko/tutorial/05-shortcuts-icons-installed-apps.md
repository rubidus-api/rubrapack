# 바로가기, 아이콘, "설치된 앱" 항목

목표: 시작 메뉴에 "Example Software" 폴더를 만들어 Hello 와 그 읽어보기를 넣고, 자기 아이콘을 가진 바탕화면
바로가기를 두고, "설치된 앱"의 Hello 에 아이콘과 링크를 단다.

원본 옆에 아이콘 파일 `hello.ico` 가 있어야 한다(아무 `.ico` 나 된다. 많은 그림 편집기가 저장할 수 있다).

## 원본

```toml
# tutorial 05: hello.toml
format = 1

[package]
name = "Hello"
manufacturer = "Example Software"
version = "$(VERSION)"
arch = "x64"
upgrade-code = "{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}"

[define]
VERSION = "1.3.0"

[arp]
icon = "hello.ico"
help = "https://example.com/hello/help"
about = "https://example.com/hello"
no-repair = true

[dir.INSTALLDIR]
path = "$(ProgramFiles)/Hello"

[dir.MenuFolder]
path = "$(Programs)/Example Software"

[file.Hello]
dir = "INSTALLDIR"
source = "dist/hello.exe"

[file.Readme]
dir = "INSTALLDIR"
source = "dist/readme.txt"

[shortcut.StartMenu]
dir = "MenuFolder"
name = "Hello"
target = "file:Hello"
description = "Says hello"
working-dir = "INSTALLDIR"

[shortcut.ReadmeLink]
dir = "MenuFolder"
name = "Hello - read me"
target = "file:Readme"

[shortcut.Desktop]
dir = "Desktop"
name = "Hello"
target = "file:Hello"
args = "--greet \"[ProductName] user\""
icon = "hello.ico"
```

## 바로가기: `[shortcut.ID]`

- `dir` 은 바로가기가 들어갈 곳이다: 내 dir, 또는 바로 `Programs`(시작 메뉴의 프로그램 목록), `StartMenu`,
  `Desktop`, `Startup`(로그인할 때마다 실행). 여기서 dir `MenuFolder` 는 시작 메뉴 프로그램 안의 "Example
  Software" 폴더라서, 시작 메뉴에 "Example Software" 와 그 안의 항목 둘이 보인다.
- `name` 은 바로가기 아래의 글이다.
- `target` 은 바로가기가 여는 파일이다: `file:` 과 이 패키지의 `[file.*]` ID.
- `description` 은 도움말 풍선, `working-dir`(dir ID)은 프로그램이 시작하는 폴더다.
- `args` 는 프로그램의 인자다. MSI *서식 문자열*이어서 바로가기를 만들 때 `[ProductName]` 이 "Hello" 가 된다.
  TOML 문자열 안의 큰따옴표는 `\"` 로 쓴다.
- `icon` 은 바로가기에 자기 `.ico` 를 준다. 없으면 Windows 는 프로그램 자체의 아이콘을 보인다.

rubrapack 이 바로가기를 위해 만든 시작 메뉴 폴더는 바로가기와 함께 제품을 지울 때 사라진다.

## "설치된 앱" 항목: `[arp]`

("ARP" 는 이 목록의 옛 이름 Add or Remove Programs(프로그램 추가/제거)다.)

| 키 | 효과 |
|---|---|
| `icon = "hello.ico"` | 목록의 아이콘 |
| `help = "https://..."` | Windows 가 제품과 함께 보이는 지원 링크 |
| `about = "https://..."` | 제품의 웹 페이지 |
| `no-repair = true` | 이 제품에 복구가 없다 |
| `no-modify = true` | 수정/변경이 없다(패키지에 바꿀 것이 없을 때 쓸모 있다. 8장이 바꿀 것을 더한다) |

## 다른 글자로 된 이름

`name` 은 어느 언어로든 쓸 수 있다(`name = "안녕"`). 그런데 `.msi` 파일 자체의 속성(자세히 탭)은 ASCII 글자만
보일 수 있어서, 그런 이름에는 ASCII 형태도 준다: `[package]` 의 `summary-name = "Annyeong"`. 없으면 rubrapack 이
경고하고(`RP1203`) 속성에는 "rubrapack package" 가 보인다.

## 해 보기

빌드하고 설치한 뒤 시작 메뉴를 연다: "Example Software" 에 "Hello" 와 "Hello - read me" 가 있고, 바탕화면에는
준 아이콘을 단 "Hello" 가 있다. "설치된 앱"에는 아이콘과 함께 Hello 가 보인다. 지우면 바로가기와 시작 메뉴
폴더도 사라진다.

## 안에서 무슨 일이 일어났나

```text
C:\work\hello> rubrapack inspect hello.msi Shortcut
Shortcut	Directory_	Name	Component_	Target	Arguments	Description	Hotkey
...
Desktop	DesktopFolder	Hello	C_2ea6d5bfd9b4a9073b30	[INSTALLDIR]hello.exe	--greet "[ProductName] user"
ReadmeLink	MenuFolder	HELLO-~1|Hello - read me	C_1643d000d3751c809248	[INSTALLDIR]readme.txt
StartMenu	MenuFolder	Hello	C_a7452f86f3da3f7d2a6c	[INSTALLDIR]hello.exe		Says hello
```

- 바로가기마다 자기 컴포넌트가 있고, 대상 파일의 기능에 든다: 파일과 함께 설치되고 지워진다. 그 키 경로는
  `HKEY_CURRENT_USER\Software\Example Software\Hello\Shortcuts` 아래 값이다. Windows 의 규칙이 시작 메뉴와
  바탕화면을 사용자의 자리로 보기 때문이다(제4부 [Windows 와 대조하기](../formats/verify.md#microsoft-의-ice-규칙-관찰)).
- `[INSTALLDIR]hello.exe` 가 대상이다: 어디에 설치됐든 폴더 `INSTALLDIR` 와 파일 이름.
- `HELLO-~1|Hello - read me` 에는 이름이 둘 들어 있다: 옛 방식의 짧은 이름(8자, 점, 3자 - MS-DOS 시절부터)과 긴
  이름. Windows Installer 는 모든 이름에 둘 다를 원하고, 짧은 이름은 rubrapack 이 지어낸다.

아이콘은 패키지 안(`Icon` 표)에 저장되고, "설치된 앱" 설정은 `Property` 표에 들어 있다:

```text
ARPHELPLINK	https://example.com/hello/help
ARPNOREPAIR	1
ARPPRODUCTICON	RpIcon1.ico
ARPURLINFOABOUT	https://example.com/hello
```
