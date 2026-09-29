# `.rpk` 원본과 `rubrapack` 명령

`.rpk` 파일 하나가 패키지 하나를 기술한다. 형식은 [TOML 1.0](https://toml.io/ko/v1.0.0)의 엄격한
부분집합이다. 모든 `.rpk` 파일은 올바른 TOML 이지만, 부분집합 밖의 TOML 기능은 무시하지 않고
오류로 거부한다.

## 예

```toml
[define]
VERSION = "1.4.0"

[package]
name = "Example App"
manufacturer = "Example"
version = "$(VERSION)"
arch = "x64"                                        # x64, arm64, x86 가운데 하나 - 기본값 없음
upgrade-code = "{0B9A6C1E-3D2F-4A5B-8C7D-6E5F4A3B2C1D}" # 한 번 만들어 영원히 쓴다
language = "en-US"                                  # 또는 "ko-KR"

[dir.INSTALLDIR]
path = "ProgramFiles/Example App"

[dir.Docs]
path = "INSTALLDIR/docs"

[file.MainExe]
dir = "INSTALLDIR"
source = "dist/app.txt"

[file.Guide]
dir = "Docs"
source = "dist/guide.txt"
name = "User guide.txt"
```

```sh
rubrapack build example.rpk -o example.msi -D VERSION=1.4.1
rubrapack inspect example.msi File
```

## TOML 부분집합

받는 것: `[kind]`·`[kind.ID]` 표, 맨 키(`A-Z a-z 0-9 _ -`), TOML 이스케이프가 되는 기본 문자열
`"..."`, 리터럴 문자열 `'...'`(이스케이프 없음 - 백슬래시와 따옴표를 쓸 때 편하다:
`'SOFTWARE\Example'`), 십진수와 `0x` 정수, `true`/`false`, 한 가지 형만 담은 배열, `#` 주석.
BOM 이 있거나 없는 UTF-8, 또는 BOM 이 있는 UTF-16LE. 줄 끝은 LF 나 CRLF.

오류로 거부하는 것: 여러 줄 문자열, 인라인 표, 표 배열, 점으로 이은 키와 따옴표 키, 세 부분 이상의
표 이름, 부동소수, 날짜, 숫자 속 `_`, 8진·2진 수, 빈 배열이나 섞인 배열, 첫 표보다 앞의 키. 표와
키의 순서는 아무 의미가 없다.

## 표

| 표 | 키(굵은 것은 반드시) |
|---|---|
| `[package]` | **name**, **manufacturer**, **version**(`a.b.c` 또는 `a.b.c.d`), **arch**, **upgrade-code**, upgrade-code-x64 / -arm64 / -x86, product-code, summary-name(ASCII), language, scope(`machine`, `user`, `dual`), ui(`none`, `basic`, `minimal`, `installdir`, `features`), license(`.txt`, `.md`, `.rtf`), reboot(`suppress`/`allow`), downgrade-message, compress(`none`, `mszip`, `mszip:0`..`mszip:9`; 기본 `mszip:6`), cab(`embed` 또는 `external`), cab-max-size(MiB), refuse-upgrade-below, refuse-upgrade-message |
| `[define]` | 변수: `NAME = "value"` |
| `[feature.ID]` | **title**, description, level(1-32767), hidden, parent, required, follow-parent, when |
| `[dir.ID]` | **path** = `기준/상대/경로`, feature, guard(`true`: [설치 폴더 지키기](#설치-폴더-지키기) 참고) |
| `[file.ID]` | **dir**, **source**, name, vital(기본 true), any-arch, feature, component-guid, keep, when |
| `[files.ID]` | **dir**, **glob**, vital, any-arch, feature, keep, when |
| `[folder.ID]` | **dir**, **name**, keep, feature |
| `[arp]` | no-modify, no-repair, help(URL), about(URL), icon(`.ico`) - "설치된 앱"에 제품이 어떻게 보이는가 |
| `[property.ID]` | **value**, secure, hidden - 대문자 이름의 공개 속성 |
| `[action.ID]` | **run**(이 패키지의 `.exe` 를 가리키는 `file:ID`), **do**, **undo**, check |
| `[registry.ID]` | **root**(`HKLM`, `HKCU`, `HKCR`, `HKMU`), **key**, name, value, type, remove, keep, view, with, feature, when |
| `[remove.ID]` | **dir**, name(`*` 와 `?`; 없으면 폴더 자체), **on**(`install`, `uninstall`, `both`), feature |
| `[ini.ID]` | **dir**, **file**, **section**, **key**, value, mode(`set`, `add`, `remove`), feature, when |
| `[require.ID]` | **condition**, **message** |
| `[search.ID]` | **property**(또는 dir ID), **kind**(`registry`: root, key, name, view; `file`: path, file, min-version; `dir`: path; `component`: component-guid) |
| `[service.ID]` | **file**(`.exe` 를 가리키는 `file:ID`), **name**, display-name, description, start(`auto`, `demand`, `disabled`), account(`LocalSystem`, `LocalService`, `NetworkService`), args, start-on-install |
| `[assoc.ID]` | **extension**(`.ext`, 소문자), **prog-id**, **target**(`.exe` 를 가리키는 `file:ID`), description, icon(`file:ID`), args(기본 `"%1"`) |
| `[protocol.ID]` | **name**(스킴, 소문자), **target**(`.exe` 를 가리키는 `file:ID`), description, args(기본 `"%1"`) |
| `[font.ID]` | **file**(`path = "Fonts"` 인 dir 에 드는 파일의 `file:ID`), title |
| `[permission.ID]` | **target**(`dir:ID`, `file:ID`, `registry:ID`), **sddl** |
| `[env.ID]` | **name**, **value**, mode(`set`, `append`, `prepend`), keep, feature, when |
| `[copy.ID]` | **source**(`file:ID`), **dir**, name(기본: 원본 파일 이름) |
| `[ui]` | install-dir(dir ID; 기본 `INSTALLDIR`), banner(`.bmp`), launch(`file:ID`), launch-args, launch-checked, languages(영어에 덧붙일 언어, 예 `["ko"]`), license-xx, name-xx, font-xx, langid-xx - [여러 언어](#여러-언어) 참고 |
| `[ui-text.ID]` | **text** 또는 text-xx - 내장 대화창 문구 하나를 바꾼다 |
| `[dialog.ID]` | **after**(내장 페이지 또는 다른 `[dialog.*]`), title, description, title-xx, description-xx |
| `[dialog-control.ID]` | **dialog**, **type**(`text`, `checkbox`, `edit`, `radio`, `combo`), **x**, **y**, **width**, **height**, text, property, values, labels, text-xx, labels-xx |
| `[shortcut.ID]` | **dir**(dir ID, 또는 `Programs`, `Desktop`, `StartMenu`, `Startup`), **name**, **target**(`file:ID`), args, description, working-dir(dir ID), icon(`.ico`), when |
| `[msix]` | **identity-name**, **publisher**, publisher-display-name, min-version - [MSIX 패키지](#msix-패키지) 참고 |
| `[msix-app.ID]` | **executable**(`[file.*]` ID), display-name, description, logo-150, logo-44, store-logo |
| `[msix-extension.ID]` | **kind**(`alias`: **alias**; `startup-task`: task-id, display-name, enabled), app(`[msix-app.*]` ID; 기본은 첫째) - MSIX 전용 |

dir 경로의 `기준`은 다른 dir ID 이거나 다음 가운데 하나다: `ProgramFiles`(x64/arm64 는 64비트,
x86 은 32비트), `ProgramFiles32`, `CommonFiles`, `AppData`, `LocalAppData`, `CommonAppData`,
`StartMenu`, `Programs`, `Desktop`, `Startup`, `Windows`, `System`, `Fonts`, `Temp`. 알려진 폴더
하나만 쓴 경로(`path = "Fonts"`)는 그 폴더 바로 안에 드는 파일에 쓴다.

ID 는 `[A-Za-z_][A-Za-z0-9_]*` 꼴이고(최대 72자, feature 는 38자) dir·파일·feature 를 통틀어
서로 달라야 한다.

### MSIX 패키지

출력 이름이 `.msix` 로 끝나면 같은 원본이 MSIX 패키지가 된다: 완전 신뢰로 도는 데스크톱 앱 하나,
아키텍처 하나. MSIX 에만 필요한 것은 표 두 개가 더 말한다:

```toml
[msix]
identity-name = "Example.App"               # 3~50자: A-Z a-z 0-9 . -
publisher = "C=KR, O=Example, CN=Example"   # 서명 인증서의 주체, 마지막 부분부터
publisher-display-name = "Example"          # 기본: [package] manufacturer
min-version = "10.0.17763.0"                # 설치되는 가장 오래된 Windows(이것이 기본값)

[msix-app.Main]
executable = "MainExe"                      # 앱을 시작하는 [file.*]
display-name = "Example App"                # 기본: [package] name
description = "An example"                  # 기본: 표시 이름
logo-150 = "assets/Square150x150.png"       # PNG, 150x150
logo-44 = "assets/Square44x44.png"          # PNG, 44x44
store-logo = "assets/StoreLogo.png"         # PNG, 50x50
```

- 판은 `[package] version` 을 네 부분으로 늘린 것(`1.2.3` 은 `1.2.3.0`), 아키텍처는
  `[package] arch`, 언어는 `[package] language` 다.
- 패키지 자신의 폴더는 첫 `[msix-app.*]` 실행 파일이 드는 폴더 - `ProgramFiles/Example App` 처럼
  알려진 자리에 붙은 dir - 다. 다른 알려진 자리의 파일은 패키지의 가상 파일 시스템(VFS)에 들어가고,
  앱은 그것을 늘 보던 자리에서 본다: Program Files(`VFS\ProgramFilesX64`, x86 패키지와
  `ProgramFiles32` 는 `X86`), `CommonFiles`, `System`, `Windows`, `CommonAppData`(ProgramData).
  사용자의 `AppData`·`LocalAppData` 와 `Temp` 에는 가상 폴더가 없어 거기 드는 파일은 오류다
  (`RP1609`). `Fonts` 의 글꼴은 그 `[font.*]` 로 들어가고, 시작 메뉴·Programs·바탕화면 폴더는
  바로가기(`[shortcut.*]`)로, Startup 은 시작 작업(`[msix-extension.*]`)으로 쓴다.
- `[msix-app.*]` 표가 여럿이면 한 패키지에 항목이 여럿 생긴다(최대 100개). 패키지 로고는 첫째 것이다.
- `[registry.*]` 값은 패키지의 가상 레지스트리에 들어간다. 앱은 그것을 실제 레지스트리와 합쳐 보고,
  컴퓨터의 레지스트리는 건드리지 않는다: `Software` 아래의 `HKLM`(과 `HKMU`)은 `Registry.dat`
  (`view = "32"` 면 32비트 보기)에, `Software` 아래의 `HKCU` 는 `User.dat` 에 든다. MSIX 가 담을 수
  없는 것은 오류다(`RP1612`): `HKCR`(파일 형식과 프로토콜은 `[assoc.*]`·`[protocol.*]` 로 쓴다),
  `Software` 밖의 키, `remove` 와 `keep`, 그리고 Windows Installer 가 설치 때 채우는 부분이 든 값
  (`[INSTALLDIR]`, `[#File]` 등. 이스케이프 `[\[]` 와 `[\]]` 는 괜찮다).
- 로고는 셋 다 주거나 하나도 주지 않는다. 주지 않으면 한 가지 색의 로고가 들어간다. 로고는 크기가
  정확해야 한다(`RP1608`).
- MSIX 가 할 수 없는 것은 조용히 빼지 않고 오류로 알린다(`RP1605`): 사용자 지정 동작, 서비스,
  환경 변수, INI 파일, 권한, 설치 조건과 검색, 설치 때 지우거나 복사하는 파일, 빈 폴더.
  그런 표(또는 `[file.*]`/`[files.*]`)에 `msi-only = true` 를 달면 MSI 에는 들어가고 MSIX 는 그것
  없이 만들어진다. feature·속성·대화창·`[arp]` 은 Windows Installer 에만 해당하므로 MSIX 에는 쓰지
  않는다.
- `--unsigned-test` 는 서명 없는 패키지를 시험 삼아 설치할 때 Windows 가 요구하는 속성을 더한다
  (`Add-AppxPackage -AllowUnsigned`, 프로그램이 들어 있으면 관리자로). 그런 패키지는 배포용이 아니고
  정체(identity)도 서명판과 다르다. 대신 `--key` 를 주면 패키지(또는 묶음과 그 안의 패키지)에
  서명한다. 아래 `sign` 참고.
- 파일은 압축한다(`--msix-compress store` 로 끈다). 그림, 압축 파일처럼 이미 압축된 파일은 저장만
  한다. MSIX 에는 시각이 들어가지 않아, 같은 원본은 Linux 와 Windows 에서 같은 바이트가 된다.
- 출력 이름이 `.msixbundle` 로 끝나면 묶음이다: `--arch` 의 아키텍처마다 원본을 한 번씩 짓고
  (목록, `--arch x64,x86,arm64`; 없으면 원본의 아키텍처), 각 패키지는 안에서
  `<identity-name>_<version>_<arch>.msix` 라는 이름을 갖는다. Windows 는 그 가운데 자기 아키텍처의
  패키지를 설치한다(x64 컴퓨터는 x86 보다 x64 를 고른다). `$(ARCH)` 로 빌드마다 제 프로그램을 준다:
  `source = "bin/$(ARCH)/app.exe"`. 묶음의 판은 패키지들의 판이다.

### 설치한 프로그램으로 등록하기: `[action.ID]`

```toml
[action.Tip]
run = "file:MainExe"      # 이 패키지가 설치하는 .exe
do = "--register"         # 파일을 설치한 뒤, 그리고 복구할 때 다시 실행
undo = "--unregister"     # 제거할 때, 파일을 지우기 전에 실행
```

rubrapack 은 이 한 쌍을 관리자 권한의 지연(deferred) 동작과 그 되돌림(rollback) 짝으로 바꾼다.
그래서 제거와 업그레이드는 전부 되거나 전혀 되지 않는다: 뒤에서 무엇이 실패하면(또는 `do`/`undo`
자체가 0 아닌 값으로 끝나면) 파일이 되돌아오고 다른 명령이 등록을 되살린다. 두 명령은 두 번 실행해도
안전해야 하고, 아무것도 묻지 않고 끝나야 한다 - 창 없이 실행되고 사용자를 기다리는 것은 없다.
`check` 는 등록이 되어 있으면 0 으로 끝나는 명령을 (필요하면) 적는다.

### 레지스트리 값: `[registry.ID]`

```toml
[registry.InstallDir]
root = "HKLM"
key = 'SOFTWARE\Example'          # 리터럴 문자열은 백슬래시를 그대로 둔다
name = "InstallDir"               # 없으면 키의 기본값
value = "[INSTALLDIR]"            # MSI 서식 문자열: [PROPERTY], [#FileID], "[" 는 [\[]
```

`type` 은 `string`(기본), `expand`, `dword`(0~0xFFFFFFFF 정수), `qword`(정수, 또는 `"0x"` 와 16자
이하의 16진수), `binary`(16진수), `multi`(문자열 배열)이다. Windows Installer 는 REG_QWORD 를 스스로
쓰지 못한다. 그래서 rubrapack 이 작은 도우미 DLL 을 패키지에 넣어 쓰게 하고, 설치가 실패하면 그
DLL 이 이전 값을 되살린다. 값 하나가 구성 요소(component) 하나이고 제거할 때 지워진다(`keep = true`
면 남는다). `with = "file:ID"` 는 대신 그 파일의 구성 요소에 넣는다. 64비트 패키지에서 값은 64비트
레지스트리 보기로 가고, `view = "32"` 는 32비트 보기에 쓴다. `remove = true`(`value` 없이)는 설치
중에 이름 붙은 값을 - `name` 이 없으면 키 전체를 - 지운다.

### 바로가기: `[shortcut.ID]`

```toml
[dir.Menu]
path = "Programs/Example"         # 시작 메뉴 > Example

[shortcut.Settings]
dir = "Menu"
name = "Example settings"         # ".lnk" 는 붙여 준다
target = "file:MainExe"
args = "--settings \"[INSTALLDIR]\""
```

바로가기는 대상 파일(과 그 feature)에 딸린다. 바로가기 때문에 만든 폴더는 제거할 때 지운다. 컴퓨터
전체 패키지에서 `Programs` 와 `Desktop` 은 모든 사용자의 시작 메뉴와 공용 바탕화면이다.

MSIX 에서 바로가기는 앱을 시작한다(대상은 `[msix-app.*]` 의 실행 파일이어야 한다): `Programs` 나
`StartMenu` 의 것은 그 앱 자신의 시작 메뉴 항목이고(`args` 없음), `Desktop` 의 것은 매니페스트에
적히며 `min-version = "10.0.19645.0"` 이상이 필요하다(`RP1614`). 다른 폴더, `working-dir`,
`args` 속 `[...]` 는 거기서 오류다(`RP1613`). `Startup` 은 시작 작업으로 쓴다.

### 파일 형식과 링크: `[assoc.ID]`, `[protocol.ID]`

```toml
[assoc.Doc]
extension = ".exdoc"
prog-id = "Example.Document"      # 같은 prog-id 의 표들은 한 종류의 문서를 뜻한다
description = "Example document"
target = "file:MainExe"
args = "--open \"%1\""            # 기본값은 "%1"

[protocol.Link]
name = "example"                  # example:... 가 프로그램을 연다
target = "file:MainExe"
```

MSI 에서는 프로그램의 구성 요소에 든 `HKEY_CLASSES_ROOT` 아래 레지스트리 값이다: 확장자의 기본값은
prog-id, prog-id 에는 설명과 `DefaultIcon`(`icon`, 없으면 프로그램의 첫 아이콘), `shell\open\command`
가 있고, 스킴에는 `URL Protocol` 이 붙는다. `HKEY_CLASSES_ROOT` 는 설치 방식을 따른다: 컴퓨터
전체면 `HKLM\Software\Classes`, 사용자별이면 `HKCU\Software\Classes` 에 들고, 제거하면 없어진다.
이 프로그램만 맡겠다는 형식은 곧바로 이 프로그램으로 열리고, 사용자가 다른 프로그램을 골라 두었으면
Windows 는 그 선택을 지킨다.

MSIX 에서는 대상 실행 파일을 가진 앱의 매니페스트에 들어가고(파일 형식 연결, 프로토콜) `args` 는
평문이어야 한다. `icon` 은 쓰지 않는다: 앱의 로고가 파일 형식을 나타낸다.

### MSIX 전용: `[msix-extension.ID]`

```toml
[msix-extension.Cli]
kind = "alias"
alias = "example.exe"             # 콘솔에서 이 이름을 치면 앱이 시작된다

[msix-extension.Boot]
kind = "startup-task"             # 앱을 한 번 실행한 뒤로 Windows 와 함께 시작된다
display-name = "Example"          # 작업 관리자에 보이는 이름; task-id 기본값은 표 ID
enabled = true
```

MSI 빌드는 이것들을 뺀다. MSIX 에서 글꼴(`[font.*]`)은 패키지의 `Fonts` 폴더에서 다른 앱과 나눠
쓰며(`uap4:SharedFonts`) `title` 은 쓰지 않는다.

### 지우기와 복사: `[remove.ID]`, `[copy.ID]`

`[remove.ID]` 는 `dir` 에서 `name` 에 맞는 파일을 지운다 - 예를 들어 옛 판이 남긴 `*.log` 를
`on = "install"` 로, 프로그램이 실행 중에 만드는 파일을 `on = "uninstall"` 로. `name` 이 없으면 비어
있는 폴더 자체를 지운다. 설치가 실패하면 지운 파일은 되돌아온다. `[copy.ID]` 는 이 패키지의 파일을
다른 폴더에 한 벌 더 설치하고, 원본과 함께 들어오고 함께 나간다. 설치 전부터 있던 폴더는 절대 지우지
않는다.

### 환경 변수: `[env.ID]`

시스템(컴퓨터 전체) 변수다. `mode = "set"`(기본)은 값을 바꾸고, `append`/`prepend` 는 기존 값 끝에
`;value` 를, 앞에 `value;` 를 붙인다. 제거는 정확히 그만큼 되돌린다: set 한 변수는 지우고, 붙인
부분은 떼어 내고 나머지는 둔다. `keep = true` 면 남긴다.

### INI 파일: `[ini.ID]`

`mode = "set"`(기본)은 파일의 `[section]` 에 `key=value` 를 쓰고, `add` 는 쉼표 목록에 값을 덧붙이며
(`a` 가 `a,b` 로), `remove` 는 설치 중에 키를 지운다. 제거하면 `set` 과 `add` 가 쓴 것을 뗀다.
`remove` 는 파일 설치보다 먼저 실행되므로 옛 판이 남긴 INI 파일을 위한 것이다.

### 검색과 요구: `[search.ID]`, `[require.ID]`

검색은 무엇보다 먼저 실행되고 찾은 것을 공개 속성에 넣는다(못 찾으면 비어 있다): 레지스트리 값의 데이터,
파일의 전체 경로(`path` = 알려진 폴더와 상대 경로, 예 `System` 이나 `ProgramFiles/Example`; 프로그램
파일은 `min-version`), 폴더, 또는 다른 제품 구성 요소의 키 파일. 요구 사항은 `condition` 이 거짓이면
그 메시지로 첫 설치를 멈춘다(복구와 제거는 막지 않는다). 조건은 Windows Installer 의 문법을 쓰고
(`VersionNT >= 603`, `FOUND_TOOL`, `NOT OLDSETTING`) 검색 결과를 볼 수 있다.

```toml
[search.Tool]
property = "FOUND_TOOL"
kind = "file"
path = "System"
file = "tool.exe"

[require.Tool]
condition = "FOUND_TOOL"
message = "[ProductName] needs tool.exe."
```

`registry` 나 `dir` 검색은 속성 대신 dir 을 가리킬 수 있다. 그러면 찾은 폴더가 그 dir 의 기본값이 된다.
사용자가 고른 폴더를 패키지가 기억하는 방법이 이것이다: 폴더를 적어 두고, 다음 판에서 찾아본다.
레지스트리 값은 그 폴더가 아직 있을 때만 쓰이고, 명령줄에 준 폴더(`msiexec /i app.msi
INSTALLDIR=D:\Apps\Example\`)가 여전히 이긴다.

```toml
[registry.RememberDir]
root = "HKLM"
key = "Software\\Example"
name = "InstallDir"
value = "[INSTALLDIR]"

[search.PreviousDir]
property = "INSTALLDIR"           # dir: 레지스트리에 있는 폴더가 실제로 있으면 그것이 기본값
kind = "registry"
root = "HKLM"
key = "Software\\Example"
name = "InstallDir"
```

### 서비스, 글꼴, 권한

`[service.ID]` 는 패키지의 `.exe` 가 돌리는 서비스를 설치한다: 파일이 바뀌기 전과 제거할 때 멈추고,
제거할 때 지우며, `start-on-install = true` 면 설치 뒤에 시작한다. `[font.ID]` 는 패키지가 Fonts
폴더에 설치하는 글꼴 파일을 등록한다. `title` 이 없으면 Windows 가 TrueType/OpenType 파일에서 이름을
읽는다. `[permission.ID]` 는 패키지가 만드는 폴더, 그 파일, 또는 그 레지스트리 값에 SDDL 보안 서술자를
건다(이것이 있으면 패키지는 Windows Installer 5.0 을 요구한다).

### 사용자별 패키지와 겸용 패키지: `scope`

`scope = "machine"`(기본)은 모든 사용자에게 설치하고 관리자 권한이 필요하다. `scope = "user"` 는
권한 상승 없이 현재 사용자에게 설치한다: `ProgramFiles` 는 `%LOCALAPPDATA%\Programs` 가 되고,
`Programs` 와 `Desktop` 은 그 사용자의 것, 레지스트리 값은 `HKCU`(또는 `HKMU`), 환경 변수는 사용자의
것이며 do/undo 동작은 그 사용자로 실행된다. 컴퓨터 전체 설치를 요청하면 거부한다. `scope = "dual"`
은 기본으로 사용자별로 설치하고, 관리자 명령창에서
`msiexec /i x.msi ALLUSERS=1 MSIINSTALLPERUSER=""` 로 하면 컴퓨터 전체로 설치한다. 그 레지스트리
값은 설치된 방식에 따라 `HKLM` 이나 `HKCU` 가 되는 `HKMU` 를 쓴다. 서비스, 글꼴, 권한과 컴퓨터
폴더(`Windows`, `System`, `Fonts`, `CommonAppData`)는 `scope = "machine"` 이 필요하다.

### 캐비닛

파일은 패키지에 넣는 캐비닛 하나로 압축한다. `cab-max-size = N` 은 파일이 N MiB 를 넘을 때마다 새
캐비닛을 시작하고, `cab = "external"` 은 캐비닛을 패키지 옆에 `<name>.cab`(또는 `<name>-1.cab`,
`<name>-2.cab`, ...)으로 쓴다. 그 캐비닛은 패키지와 함께 다녀야 하고, `cab-max-size` 가 없으면 캐비닛마다
2 GiB 전에 나눈다. Windows Installer 는 2 GiB 이상인 패키지를 열지 못하므로, 그만큼 큰 내장 캐비닛은
`cab = "external"` 을 권하는 오류(`RP1516`)다. rubrapack 은 있는 캐비닛을 덮어쓰지 않고 패키지를 맨 나중에
쓴다: 패키지는 `<out>.rp-map` 에서 만들어지고(바이트가 메모리가 아니라 그 파일로 바로 간다) 모든 일이
성공했을 때만 제 이름으로 바뀌므로, 실패한 빌드는 패키지를 남기지 않는다. 압축은 모든 프로세서에서 돈다(덜 쓰려면 `--jobs N`); 어느 쪽이든 바이트는 같다. 모든 패키지에는 관리 설치(`msiexec /a`, 압축을 푼 네트워크
이미지)와 광고(`msiexec /jm`) 순서도 들어 있다.

MSI 서식 문자열로 해석되는 곳은 이것뿐이다: 레지스트리 `value`(와 multi 항목), 바로가기 `args`,
환경 변수 `value`, INI `value`, 요구 사항 `message`, 서비스 `args`. 나머지는 적은 그대로 쓴다.

### 설치된 앱 항목과 속성

`[arp]` 는 제품이 "설정 > 설치된 앱"에 어떻게 보이는지 정한다(`no-modify`, `no-repair`, `help`,
`about`). `[property.NAME]` 은 공개 속성을 더한다. `secure = true` 는 그 값이 설치의 관리자 권한
부분까지 가게 하고, `hidden = true` 는 로그에 값이 남지 않게 한다. 설치 엔진이나 rubrapack 이
스스로 쓰는 이름(`ARP*`, `MSI*`, `RP_*`, `ALLUSERS`, `REBOOT` 등)은 거부한다.

### 명령줄로 설치하기

대화창이 고르는 것은 모두 `msiexec` 에 줄 수 있다.

| 속성 | 하는 일 |
|---|---|
| `INSTALLDIR=D:\Apps\Example\` | 설치 폴더(대문자 ID 의 dir 모두) |
| `ADDLOCAL=Core,Extra` | 이 기능들을 설치(`ADDLOCAL=ALL`: 모든 기능) |
| `REMOVE=Extra` | 설치된 제품에서 이 기능들을 제거(`REMOVE=ALL`: 전부) |
| `INSTALLLEVEL=3` | `level` 이 3 이하인 기능을 모두 설치 |
| `RPLANGUAGE=ko` | 대화창 언어(`[ui] languages` 가 있을 때) |
| `ALLUSERS=1 MSIINSTALLPERUSER=""` | 겸용 패키지를 모든 사용자에게(기본: 현재 사용자만) |
| `DESK=1`, `APP_MODE=server` | 원본이 정한 속성: `when` 조건, 대화창 값 |

### 창 없이 설치하기

rubrapack 이 만드는 패키지는 모두 명령줄에서 사용자 화면 없이 설치·복구·업그레이드·제거된다:

```text
msiexec /i example.msi /qn /l*v install.log
msiexec /x {ProductCode} /qn
```

종료 코드 0 은 성공, 3010 은 다시 시작이 필요한 성공(rubrapack 은 컴퓨터를 스스로 다시 시작하지
않는다), 그 밖의 값은 실패이며 그 뒤 컴퓨터는 설치 전과 같다.

### 대화창: `ui`

`[package]` 의 `ui` 가 내장 대화창 세트 하나를 고른다. 없으면(`none`) Windows Installer 자신의 진행
막대만 보인다.

| `ui` | 설치할 때 | 이미 설치되어 있을 때 |
|---|---|---|
| `basic` | 진행, 끝(또는 오류) | 진행, 끝 |
| `minimal` | 환영, 사용권(있으면), 진행, 끝 | 복구 또는 제거 |
| `installdir` | 환영, 사용권, 설치 폴더(폴더 찾아보기 포함), 준비, 진행, 끝 | 복구 또는 제거 |
| `features` | `installdir` 에 더해 기능 트리와 필요한 디스크 공간 | 복구 또는 제거 |

모든 세트에는 취소 확인, 오류 대화창, 사용 중인 파일 목록, 디스크 공간 부족 경고도 있다. 대화창은
모두 기본값이 있는 값만 모으므로 `/qn` 은 여전히 창 없이 설치한다.

```toml
[package]
ui = "installdir"
license = "LICENSE.txt"           # 동의할 때까지 설치/다음 단추가 꺼져 있다

[ui]
install-dir = "APPDIR"            # 사용자가 바꿀 수 있는 dir(기본 INSTALLDIR)
banner = "banner.bmp"             # 위쪽 띠; 약 493 x 58 화소

[ui-text.WelcomeText]
text = "This will install [ProductName]. Close other programs first."
```

`.txt` 나 `.md` 사용권은 평문으로 보여 준다(한글, 이모지 같은 어떤 글자도 그대로 둔다). `.rtf` 사용권은
그대로 쓴다. `banner` 가 없으면 띠는 흰색이다. `[ui-text.ID]` 는 문구 하나를 바꾼다. 문구는 MSI 서식
문자열이라 `[ProductName]` 은 치환되고 `[` 자체는 `[\[]` 로 쓴다. ID 는 다음과 같다: `Back`, `Next`,
`Cancel`, `Install`, `Finish`, `OK`, `Yes`, `No`, `Retry`, `Ignore`, `Abort`, `Exit`, `Browse`,
`WelcomeTitle`, `WelcomeText`, `LicenseTitle`, `LicenseText`, `LicenseAccept`, `DirTitle`, `DirText`,
`DirLabel`, `BrowseTitle`, `BrowseText`, `BrowseLookIn`, `BrowseFolder`, `BrowseUp`, `BrowseNew`,
`CustomizeTitle`, `CustomizeText`, `Reset`, `DiskCost`, `DiskCostTitle`, `DiskCostText`, `ReadyTitle`,
`ReadyText`, `ProgressTitle`, `ProgressText`, `ProgressStatus`, `ExitTitle`, `ExitText`,
`UserExitTitle`, `UserExitText`, `FatalTitle`, `FatalText`, `CancelText`, `FilesInUseTitle`,
`FilesInUseText`, `OutOfDiskTitle`, `OutOfDiskText`, `MaintTitle`, `MaintText`, `Repair`,
`RepairText`, `Remove`, `RemoveText`, `LanguageTitle`, `LanguageText`, `DirGuardText`(아래 가드의 메시지).
단추 문구에서 `&` 는 바로 가기 키를
표시한다(`&Next` 는 Alt+N).

### 여러 언어

대화창은 영어다. `[ui] languages` 는 같은 패키지에 다른 언어를 덧붙인다. 그러면 첫 페이지가 언어를 묻고,
그 뒤의 모든 페이지 - 환영, 사용권, 폴더, 사용자 페이지, 준비, 진행, 완료, 그리고 취소·오류·사용 중
파일·디스크 공간·유지보수 페이지 - 가 고른 언어로 나온다. 미리 골라 두는 언어는 사용자의 지역 형식
(`UserLanguageID`), 그다음 시스템 로캘(`SystemLanguageID`)이 LANGID 에 드는 첫 덧붙인 언어이고, 없으면
영어다. 명령줄의 `RPLANGUAGE=ko` 는 바로 고른다. 무인 설치(`/qn`)는 아무것도 보여 주지 않으니 고를 것도
없다. 영어만 있으면 언어 페이지도 없다.

```toml
[ui]
languages = ["ko"]                # 영어는 늘 있고 기본이다
license-ko = "LICENSE-ko.txt"     # 언어별 사용권(기본: [package] license)

[ui-text.WelcomeText]
text-ko = "[ProductName]을(를) 설치합니다."     # text = 모든 언어, text-xx = 한 언어

[dialog.Options]
after = "RpInstallDirDlg"
title = "Options"
title-ko = "선택 사항"

[dialog-control.Mode]
# ...
labels = ["&Typical", "&Portable"]
labels-ko = ["표준(&T)", "휴대용(&P)"]
```

- 내장 문구는 영어와 한국어(`ko`)로 있다. 다른 언어(`ja`, `de`, ...)는 내장 문구를 모두 `text-xx` 로 주고
  (빠진 것은 lint 가 이름을 댄다), `name-xx`(언어 페이지에 보일 이름), `font-xx`(그 언어 대화창의 글꼴),
  `langid-xx`(그 언어를 고르게 하는 LANGID: 숫자 하나나 배열)를 줄 수 있다. 흔한 언어는 이것들이 내장되어
  있다: `ja`, `zh`, `de`, `fr`, `es`, `it`, `pt`, `nl`, `pl`, `ru`, `uk`, `tr`, `vi`, `th`.
- 한국어 대화창은 맑은 고딕, 영어는 Segoe UI 를 쓴다.
- `[ProductName]`, `[Manufacturer]`, `[ProductVersion]` 밖의 속성을 쓰는 문구는 언어를 고를 때 서식이
  풀리므로 255자 안이어야 한다.
- Windows Installer 자신의 문구 - 기능 트리 메뉴, 크기, 남은 시간, 오류 메시지 - 는 패키지 것이 아니다:
  앞의 것들은 영어로 남고, 오류 메시지는 Windows 를 따른다.
- `[package] language` 는 패키지의 언어(`ProductLanguage`, 요약 정보)만 정한다. 0.2 부터는 대화창을
  한국어로 만들지 않는다: `languages = ["ko"]` 를 넣는다(lint 가 `RP1317` 로 경고한다).

### 아이콘, 완료 페이지, 설치 범위 페이지

- `[arp] icon = "app.ico"` 는 "설치된 앱"에 보일 제품 아이콘이다. `[shortcut] icon` 은 바로가기에 따로
  `.ico` 를 준다(없으면 프로그램 자신의 아이콘). 둘 다 빌드할 때 읽어 패키지 안에 넣는다.
- `[ui] launch = "file:App"` 은 완료 페이지에 "[ProductName] 실행"을 둔다(`launch-checked = false` 가 아니면
  체크된 채로; `launch-args` 는 그 인자). "마침"을 누르면 첫 설치나 업그레이드 뒤에 설치를 실행한 사용자의
  권한으로(설치 엔진의 권한이 아니라) 프로그램을 띄운다. 복구·제거 뒤에는 띄우지 않고, `/qn` 에서는 결코
  띄우지 않는다.
- 대화창이 있는 `scope = "dual"` 패키지는 사용권 다음에 "나만"(기본) / "이 컴퓨터의 모든 사용자"(관리자
  권한 필요) 페이지를 둔다. 설치 폴더도 그에 따라 `%LOCALAPPDATA%\Programs` 나 Program Files 로 옮긴다.

### 나만의 대화창 페이지: `[dialog.ID]`, `[dialog-control.ID]`

`minimal`, `installdir`, `features` 에서는 내장 흐름에 페이지를 더할 수 있다. 더한 페이지도 다른
페이지와 같은 띠와 뒤로/다음/취소 단추를 갖고, 본문에 컨트롤을 놓는다.

```toml
[dialog.Options]
title = "Options"                 # 띠의 제목; 없으면 [ProductName]
description = "Choose how to install."
after = "RpInstallDirDlg"         # 이 페이지 바로 다음에 나온다

[dialog-control.ModeLabel]
dialog = "Options"
type = "text"
x = 20
y = 55
width = 330
height = 12
text = "Installation &mode:"

[dialog-control.Mode]
dialog = "Options"
type = "radio"
x = 20
y = 70
width = 200
height = 42                       # 값 하나에 12 이상
property = "APP_MODE"
values = ["typical", "portable", "server"]
labels = ["&Typical", "&Portable", "&Server"]

[property.APP_MODE]
value = "typical"                 # 기본값, 창 없는 설치에서도 쓰인다
```

- `after` 는 고른 세트의 페이지 - `RpWelcomeDlg`, `RpLicenseDlg`(사용권이 있을 때),
  `RpInstallDirDlg`(installdir, features), `RpCustomizeDlg`(features) - 또는 직접 만든 다른
  페이지다. 같은 페이지 뒤에 여러 페이지가 오면 ID 순서를 따른다. `minimal` 에서는 마지막 페이지의
  단추가 설치이고, 다른 세트에서는 준비 페이지가 늘 마지막이다.
- 좌표는 370 x 270 페이지 위의 대화창 단위다. 컨트롤은 본문, y = 45 에서 y = 234 사이에 놓는다. Tab
  키는 컨트롤을 위에서 아래로, 그다음 왼쪽에서 오른쪽으로 옮겨 다닌다.
- `text` 는 글씨를 보인다(`&` 는 바로 가기 키를 표시하고, 글씨 컨트롤에서는 다음 컨트롤로 옮긴다).
  `checkbox` 는 체크하면 속성을 `1` 로, 풀면 속성을 없앤다. `edit` 는 사용자가 속성 값을 입력하게
  한다. `radio` 와 `combo` 는 `values`(64바이트 이하 문자열 1~32개) 가운데 하나를 고르게 하고, 적은
  순서대로 `labels`(기본: 값 자체)로 보인다.
- `radio` 와 `combo` 에는 그 값 가운데 하나를 값으로 가진 `[property.*]` 가 있어야 한다: 대화창은 값을
  모을 뿐이고 창 없는 설치(`/qn`)는 기본값을 쓴다. 어떤 속성이든 명령줄에서 줄 수도 있다:
  `msiexec /i example.msi /qn APP_MODE=server`.
- 속성은 직접 정한 공개 이름(대문자)이고 컨트롤 하나에 하나이며, 설치의 관리자 권한 부분까지 가므로
  `[registry.*]`, `[ini.*]`, `[env.*]`, 조건 등에서 `[APP_MODE]` 로 쓸 수 있다.
- `Rp` 로 시작하는 페이지 ID 와 틀의 컨트롤 ID(`Banner`, `Title`, `Description`, `BannerLine`,
  `BottomLine`, `Back`, `Next`, `Cancel`)는 예약되어 있다. 페이지 하나에는 컨트롤이 64개까지 든다.

### 아키텍처와 업그레이드 계열

`arch` 는 원본 자신의 아키텍처, `upgrade-code` 는 그 업그레이드 계열이다. 같은 원본을 다른 아키텍처로
지으려면(`--arch`) 그 아키텍처에 `upgrade-code-x86`, `upgrade-code-arm64`, `upgrade-code-x64` 로 제
계열을 주어야 하고, 없으면 빌드를 거부한다. Windows Installer 의 업그레이드 감지는 아키텍처를 가리지
못해서, 코드를 같이 쓰면 한 아키텍처를 설치할 때 다른 아키텍처가 지워진다. MSIX 에는 업그레이드
코드가 없으므로 `.msix`·`.msixbundle` 빌드에는 필요 없다.

### 먼저 지워야 하는 판

`refuse-upgrade-below = "1.0.0"` 은 설치된 1.0.0 미만 판의 업그레이드를 거부하고 먼저 지우라고
알린다. 메시지(`refuse-upgrade-message`, 또는 기본 문구)는 늘 그 일을 하는 명령으로 끝난다:
`msiexec /x {ProductCode} /qn MSIRESTARTMANAGERCONTROL=Disable`. 옛 판을 다른 도구가
`MSIRESTARTMANAGERCONTROL=Disable` 없이 만들었을 때 쓴다: 그런 판을 업그레이드 도중에 지우면 그 파일을
불러 쓰는 프로그램을 모두 닫으려 한다(`formats/msi-package.md` 의 "사용 중인 파일" 참고).

### 와일드카드: `[files.ID]`

`glob` 은 `*`(한 폴더 안의 아무 글자들), `?`(한 글자), `**`(폴더 여러 단계)가 든 원본 경로다. 맞는
것은 이름순으로 정렬하므로 결과가 파일 시스템에 따라 달라지지 않는다. 첫 와일드카드 아래의 폴더는
`dir` 아래에 다시 만든다: `glob = "dist/layouts/**/*.jmt"` 는 `dist/layouts/de/x.jmt` 를
`<dir>/de/x.jmt` 로 설치한다. 맞는 것이 없거나, 심볼릭 링크이거나, 출력 파일 자신이 맞으면 오류다.

### 빈 폴더: `[folder.ID]`

파일이 하나도 들지 않아도 `dir` 안에 `name` 폴더를 만든다. `keep = true` 면 제거한 뒤에도 폴더가
남는다(프로그램이 쓰는 데이터를 위해).

### 기능(feature)

`[feature.*]` 표가 없으면 모든 것이 숨은 기능 하나에 든다. 기능을 하나라도 선언하면 모든 파일에
기능이 있어야 한다: 파일 자신의 `feature` 키, 또는 그 dir 의 `feature`. `level` 이 1 보다 큰 기능은
기본으로 설치하지 않는다. `ui = "features"` 면 사용자가 트리에서 기능을 고르고, 나중에 "설치된 앱"에서
바꿀 수 있다(유지보수 페이지의 "변경").

- `required = true`: 트리에서 "설치하지 않음"을 고를 수 없다.
- `follow-parent = true`: 부모가 설치되는 대로 따라간다.
- `when = "<조건>"`: 조건이 참이 아니면 그 기능은 꺼진다 - 설치하지도 보여 주지도 않는다(아래).

### 조건: `when`

`when` 은 Windows Installer 조건(`VersionNT64`, `DESK = "1"`, `NOT OLDVERSION`)을 받는다. 기능, 파일과
파일 묶음, 레지스트리 값(파일과 `with` 로 묶인 값은 안 된다: 파일 쪽에 둔다), 바로가기, 환경 변수, INI 값에
쓸 수 있다. 조건이 참일 때만 그것을 설치한다. 나만의 대화창 페이지에서 정한 속성, 명령줄에 준 속성, 검색
결과를 볼 수 있다. 조건은 그것을 처음 설치할 때(메이저 업그레이드 포함) 따진다 - 복구는 있는 그대로 둔다.
MSIX 는 모두 설치하므로 `when` 을 거부한다.

```toml
[dialog-control.Desk]             # 나만의 페이지의 체크박스
dialog = "Options"
type = "checkbox"
x = 20
y = 60
width = 300
height = 16
text = "바탕화면 바로 가기 만들기(&D)"
property = "DESK"

[shortcut.Desk]
dir = "Desktop"
name = "Example"
target = "file:App"
when = "DESK"
```

### 제거할 때 파일 남기기: `keep`

파일(또는 파일 묶음)에 `keep = true` 를 두면 제품을 제거해도 그 파일은 남는다 - 사용자가 바꿨을 수 있는
설정 파일용이다. 한 번 바뀐 파일은 복구나 다음 판이 덮어쓰지 않는다(판 없는 파일에 대한 Windows Installer
의 규칙). MSIX 는 파일을 모두 지우므로 `keep` 을 거부한다.

### 변수

문자열 값 속 `$(NAME)` 은 한 번 치환된다. 명령줄의 `-D NAME=value` 가 먼저, 그다음 `[define]` 이다.
치환 결과는 다시 읽지 않는다(`$(X)` 가 든 값은 그대로 남는다). `$$` 는 `$` 한 글자다. 정의되지 않은
이름은 오류다. `$(ARCH)` 는 `-D` 나 `[define]` 이 정의하지 않는 한 내장이다: 짓고 있는
아키텍처(`x64`, `arm64`, `x86`) - 그래서 원본 하나가 `source = "bin/$(ARCH)/app.exe"` 처럼
아키텍처마다 제 프로그램을 가리킬 수 있다.

### 프로그램 파일

PE 파일(`.exe`, `.dll` 등)은 검사한다: 머신 형식이 `arch` 와 맞아야 하고(x64 패키지의 x86 도우미는
`any-arch = true` 가 필요하다), 판 정보 리소스가 패키지 안 파일의 판이 된다. Windows Installer 는 그것을
보고 설치된 파일을 바꿀지 정한다.

### 설치 폴더 지키기

다른 프로그램이 그 파일을 불러 쓰는 프로그램 - 입력기, 셸 확장, 서비스 - 은 남이 미리 만들어 둔 폴더에
설치되면 안 된다: 거기 있던 파일은 그대로 남고, 프로그램 옆에 심어 둔 DLL 이 그 프로그램의 권한으로 실리기
때문이다. dir 에 `guard = true` 를 두면, 그 폴더가 다음과 같을 때 첫 설치가 파일을 하나도 놓기 전에 멈춘다.

- 이미 있고 주인이 SYSTEM, Administrators, TrustedInstaller 가 아니다.
- 연결 지점(junction)이나 다른 리파스 포인트를 거쳐 닿는다(폴더 자신이나 그 위의 어느 폴더든).

아직 없는 폴더는 통과한다(설치 엔진이 만들고, `[permission.*]` 로 잠글 수 있다). 복구, 제자리 업그레이드,
제거는 검사하지 않는다. 메이저 업그레이드는 새 판의 첫 설치라 똑같이 검사한다(이전 판이 만든 폴더는
통과한다). 설치는 `DirGuardText` 메시지(`[1]` 은 그 폴더; 대화창에서 고른 언어) 뒤에 1603 으로 끝나고,
`/qn` 에서도 같으며, 로그에 찾은 주인이 적힌다. 검사는 rubrapack 의 도우미 DLL 이 하고, 패키지가 그 DLL 을
싣는다(`REG_QWORD` 값과 같다). 보는 것은 주인이다: 관리자 소유라도 권한이 누구나 쓸 수 있게 되어 있는
폴더는 거부하지 않는다(`[permission.*]` 로 잠근다).

```toml
[dir.INSTALLDIR]
path = "ProgramFiles/Example"
guard = true
```

### 경로

원본 경로는 `.rpk` 파일 기준의 상대 경로이고 `/` 를 쓴다. 절대 경로, `\`, 심볼릭 링크, 없는 파일은
오류다. 설치될 이름에는 Windows 가 금하는 것(`< > : " / \ | ? *`, 제어 문자, 끝의 점이나 공백,
`CON` 같은 장치 이름)만 빼고 어떤 유니코드 글자든 쓸 수 있고, 한 폴더 안의 두 이름이 대소문자만
달라서는 안 된다. Windows Installer 는 이름을 8.3 짧은 이름과 함께 UTF-16 255단위에 저장하므로, 긴
이름은 짧은 이름이 남긴 만큼만 쓸 수 있다: 세 글자 확장자의 `name.ext` 는 242단위, 확장자 없는 이름은
246단위(NTFS 는 255 를 받는다). 더 긴 이름은 RP1514 로 거부한다.

## 명령줄

```text
rubrapack build <src.rpk> -o <out.msi|out.msix|out.msixbundle> [-D NAME=VALUE]...
                [--arch x64|arm64|x86 | --arch <목록> (.msixbundle)]
                [--compress none|mszip|mszip:N] [--jobs N] [--nfc] [--reproducible]
                [<키> [--cert <chain.pem>]
                 [--timestamp <URL> [--tsa-trust <인증서>] [--tls-trust <인증서>] [--system-roots]
                  [--proxy <URL>]] [--allow-unsigned-cabs]]
                [--unsigned-test] [--msix-compress deflate|store]      (.msix, .msixbundle)
rubrapack inspect <file.msi> [table | --summary | --files | --streams]
rubrapack inspect <file.msix|file.msixbundle> [--files | --manifest]
rubrapack inspect <file.cab>
rubrapack new [msi] <name>
rubrapack guid [--from <text>]
rubrapack lint <src.rpk> [-D NAME=VALUE]... [--arch x64|arm64|x86] [--target msi|msix] [--nfc] [--strict]
rubrapack lint <file.msi> [--previous <old.msi>] [--strict]
rubrapack lint <file.msix|file.msixbundle> [--strict]
rubrapack extract <file.msi|file.msix|file.msixbundle|file.cab> -d <새 폴더> [--limit-entries N] [--limit-bytes N]
rubrapack sign <file.exe|.dll|.msi|.msix|.msixbundle> <키> [--cert <chain.pem>]
               [--timestamp <URL> [--tsa-trust <인증서>] [--tls-trust <인증서>] [--system-roots]
                [--proxy <URL>]] [--allow-unsigned-cabs] [-o <out>]
rubrapack keys list [--pkcs11 <모듈> [--token-label <이름>] [--pin-env VAR | --pin-file FILE]]
rubrapack verify <file.exe|.dll|.msi|.msix|.msixbundle> [--trust <인증서>]... [--system-roots] [--tsa-trust <인증서>]...
rubrapack version | help [command]

<키> 는 다음 가운데 하나:
  --key <key.pfx|.pem> [--pass-env VAR | --pass-file FILE]                   키 파일
  --pkcs11 <모듈> --key-label <이름> [--token-label <이름>]
           [--pin-env VAR | --pin-file FILE]                                  PKCS#11 토큰 안의 키
  --key-store <SHA-1 지문> [--machine-store]                                  Windows 저장소의 키
```

- 명령줄 옵션은 원본보다 앞선다.
- `--reproducible` 은 패키지 코드를 내용에서 끌어낸다: 같은 원본은 Linux 에서든 Windows 에서든, 어느
  폴더에서든, 옵션 순서가 어떻든 같은 바이트가 된다. 없으면 패키지 코드는 무작위다 - Windows Installer
  가 서로 다른 패키지 파일에 기대하는 대로. MSI 에는 시각이 전혀 들지 않으므로(요약 정보에 만든·저장한
  날짜가 없고 캐비닛 항목은 1980-01-01) `SOURCE_DATE_EPOCH` 는 결과를 바꾸지 않는다. 원본의 경로나
  지은 컴퓨터·사용자의 이름도 들지 않는다. 원본 파일은 한 번만 읽고, 그 바이트를 해시하고 판을 읽고
  담는다. 짓는 동안 바뀐 파일은 빌드를 멈춘다(`RP1515`).
- `--nfc` 는 패키지가 폴더·파일·바로가기에 붙이는 이름을 유니코드 NFC(조합형)로 바꾼다. macOS 에서
  온 파일은 흔히 이름이 분해형이다 - 한글 음절 U+D55C 가 자모 셋 U+1112 U+1161 U+11AB 로 - 그리고
  Windows 는 이름을 있는 그대로 설치하므로 같은 낱말이 서로 다른 두 파일이 될 수 있다. 원본 파일의
  이름은 그대로 두고, 문구와 레지스트리 값도 적은 대로 둔다. `lint` 는 NFC 가 아닌 문구를
  경고하고(`RP2105`), 다른 이름과 같아지는 이름은 거부한다(`RP1511`). 정규화는 Unicode 17.0 자료로
  rubrapack 이 직접 한다.
- `inspect <file.msi> <table>` 은 표를 Windows Installer 의 IDT 형식으로 찍는다. `--files` 는 파일마다
  탭으로 나눈 한 줄을 찍는다: 설치 경로, 크기, File 키, 구성 요소, 판, 언어, MD5(`MsiFileHash` 에서;
  없으면 비어 있다). `--streams` 는 스트림과 그 크기를 늘어놓는다. `inspect <file.cab>` 은 캐비닛 속
  파일을 늘어놓는다. `inspect <file.msix>` 는 (블록 해시를 모두 확인한 뒤) 정체, 실행 파일, 파일을
  보인다. `--files` 는 경로, 크기, 압축 여부를, `--manifest` 는 `AppxManifest.xml` 을 찍는다. 묶음은
  정체와 패키지(하나하나 패키지로 열어 확인한 것)와 `AppxBundleManifest.xml` 을 보이고, `extract` 는
  패키지를 꺼내 쓴다.
- `new <name>` 은 `<name>.rpk` 를 쓴다. 프로그램 파일을 `dist/` 에 넣기만 하면 빌드되는 원본이고
  `upgrade-code` 는 새로 만든다. 있는 파일은 절대 바꾸지 않는다.
- `guid` 는 무작위 GUID(4판)를 찍는다. `guid --from <text>` 는 rubrapack 이 글에서 끌어내는 GUID 를
  어느 컴퓨터에서나 같게 찍는다: `guid` 의 32비트 리틀엔디언 길이와 그 4바이트, 32비트 리틀엔디언 수 1,
  글의 32비트 리틀엔디언 길이와 UTF-8 바이트에 대한 SHA-256. 해시의 앞 16바이트에 판 니블을 8 로,
  변종 비트를 `10` 으로 넣어(RFC 9562 UUIDv8) 대문자 `{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}` 로
  쓴다. 예를 들어 `guid --from hello` 는 `{B52FE8EF-B68D-84B5-91AF-E6E01BEC2773}` 이다.
- 진단은 `example.rpk:12:3: error[RP1201]: unknown key 'nmae' in [package] (did you mean 'name'?)`
  꼴이다.
- 쓰기 전에 `build` 는 완성된 표를 검사한다(`RP20xx` 진단, 종료 코드 5). 이 검사는 rubrapack 자신을
  지키는 것이라, `RP1xxx` 검사를 통과한 원본은 여기에 걸리지 않아야 한다.
- `lint <src.rpk>` 는 `build` 가 하는 검사를 모두 하고 아무것도 쓰지 않는다. 원본을 MSI 기준으로
  검사하며, `--target msix` 를 주면 MSIX 기준으로 검사한다(`[msix]` 표, 그리고 `keep` 이나 `when`
  처럼 MSIX 가 담을 수 없는 것). `lint <file.msi>` 는 어떤
  도구가 만든 패키지든 같은 표 규칙으로 검사한다: 설치를 멈추게 하는 것은 오류(종료 코드 5) - 열에 맞지
  않는 값, 가리키는 행이 없음, 중복 키, 깨진 대화창 Tab 순서(`RP2101`), 대화창의 첫/기본/취소 컨트롤이
  없음(`RP2102`), `ListBox` 표 없는 사용 중 파일 대화창(`RP2103`), `ErrorText`/`ErrorIcon` 없는 오류
  대화창(`RP2104`) - 이고 나머지는 경고다. `--strict` 는 경고도 실패로 친다. 65001 이 아닌 코드 페이지의
  패키지에는 알림(`RP2100`)이 붙는다: 그 글자는 UTF-8 로 검사할 수 없다. 표준 출력의 마지막 줄은 오류와
  경고의 수다. `lint <file.msix>` 는 Windows 가 읽는 방식대로 - ZIP, 블록 맵, 블록마다의 해시 - 검사하고,
  매니페스트에 정체가 있는지, 매니페스트가 가리키는 파일이 패키지에 있는지 본다(`RP2201`, `RP2202`).
  묶음은 매니페스트의 블록 맵, 그리고 패키지마다 매니페스트가 말하는 자리·크기·정체와 패키지 자체를
  본다. 아키텍처 하나에 패키지 하나다.
- `lint new.msi --previous old.msi` 는 `new.msi` 가 `old.msi` 를 깨끗하게 업그레이드하는지도 본다.
  오류: 다른 UpgradeCode(`RP2301`: 새 패키지가 옛것을 대체하지 않는다), 앞의 세 자리가 높지 않은
  버전(`RP2302`: Windows 는 그 세 자리만 비교한다). 경고: 같은 ProductCode(`RP2303`: 업그레이드에는
  새 것이 필요하다), GUID 는 그대로인데 키 경로(파일, 폴더, 레지스트리 값)가 바뀐 컴포넌트(`RP2304`),
  64비트 표시가 바뀐 컴포넌트(`RP2305`), 없어진 컴포넌트(`RP2306`: 그 자원은 옛 판과 함께 지워진다),
  없어진 기능(`RP2307`: 패치나 설치된 기능 바꾸기에서 사라진다). 마지막 줄은 이전 패키지를 밝힌다.
- `extract` 는 패키지를 설치되는 모습대로 푼다: 폴더는 Directory 트리의 긴 이름으로(`ProgramFiles64Folder`
  같은 표준 폴더는 그 이름 그대로), 파일은 안팎의 캐비닛에서, 또는 압축하지 않은 패키지라면 옆의 원본
  폴더에서 가져온다. 파일마다 크기와 `MsiFileHash` 를 확인한다. 대상은 새 폴더거나 비어 있어야 하고,
  패키지 전체가 통과할 때까지 아무것도 쓰지 않는다: `..`, `/`, `\`, `:`, 드라이브, 예약 장치 이름(`CON`,
  `COM1` 등), 끝의 점이나 공백이 든 이름, 대소문자만 다른 두 경로는 거부한다. 기본 한도는 항목
  100,000개와 16 GiB 다. `.cab` 은 안의 이름대로 푼다. `.msix` 는 블록이 모두 해시와 맞은 뒤 파일(ZIP
  자신의 `[Content_Types].xml` 과 `AppxBlockMap.xml` 은 빼고)을 푼다.
- `sign` 은 PE 파일, MSI 패키지, MSIX 패키지나 묶음(안의 패키지를 먼저 서명한다)에 Authenticode
  서명(SHA-256; RSA 또는 ECDSA P-256/P-384)을 제자리에서, 또는 `-o` 로 붙인다. `build --key` 는
  짓는 김에 (같은 코드로) 서명하고, 서명한 `--reproducible` 빌드도 매번 같은 바이트다(서명에 시각이
  없다) - 타임스탬프가 붙지 않는 한: 타임스탬프에는 서버의 시각과 일련번호가 들어 매번 다르고, 서명
  안의 패키지만 같다. 캐비닛이 밖에 있는 MSI 는 거부한다: 서명이 그것을 덮지 못한다 - 캐비닛을 안에
  넣거나, `--allow-unsigned-cabs` 로 경고와 함께 `.msi` 만 서명한다. 키 파일은 PKCS#12 파일
  (`.pfx`/`.p12`: PBKDF2-HMAC-SHA256 의 AES, SHA-256 MAC - 요즘 Windows 와 OpenSSL 이 내보내는
  것)이거나, 평문 또는 PBES2 로 암호화한 PKCS#8 PEM/DER 키다. `--cert` 는 키 파일에 없는 인증서를
  더한다. 인증서는 코드 서명용이어야 하고, CA 가 아니어야 하며, 지금 유효해야 한다. 암호는 환경 변수나
  파일에서 받고 명령줄에서는 절대 받지 않는다. 이미 서명된 파일은 거부한다. MSIX 의 `[msix] publisher`
  는 Windows 가 쓰는 방식의 인증서 주체 - 부분을 마지막부터 처음으로, `O=Example Ltd, CN=Example`
  처럼 - 와 같아야 하고, 다르면 서명을 멈추고 써야 할 주체를 알려 준다. `--unsigned-test` 와 키는
  함께 쓰지 않는다.
- 하드웨어 밖으로 나가서는 안 되는 키 - 2023년부터 공개 신뢰 코드 서명 인증서에 요구되는 것 - 는 거기
  머문다: rubrapack 이 해시, CMS 구조, 타임스탬프 요청을 스스로 계산하고 서명 연산 하나만 맡긴다.
  `--pkcs11 <모듈>` 은 토큰의 PKCS#11 라이브러리를 불러오고(rubrapack 이 불러오는 유일한 라이브러리이고,
  이름 준 것만 불러온다), `--pin-env` 나 `--pin-file` 의 PIN 으로(명령줄은 절대 아님) 로그인해,
  `--key-label` 이라는 이름표의 개인 키와 토큰에 같은 ID 로 저장된 인증서를 쓴다(토큰에 인증서가 없으면
  `--cert` 가 그것이나 사슬의 나머지를 준다). 토큰이 여럿이면 `--token-label` 로 고른다. Windows 에서
  `--key-store <지문>` 은 현재 사용자의 개인 저장소(`--machine-store`: 로컬 컴퓨터의 것)에 있고 개인
  키에 NCrypt 로 닿을 수 있는 인증서 - 키 저장소 공급자를 거친 스마트카드·토큰·TPM, 또는 소프트웨어
  키 - 를 쓰고, Windows 가 만들어 주는 사슬을 가져온다. RSA 키와 ECDSA 키 모두 된다. 서명은 모두
  파일에 쓰기 전에 인증서로 확인한다.
- `keys list` 는 서명에 쓸 수 있는 키를 보인다: `--pkcs11` 이면 토큰의 개인 키마다 이름표, ID, 인증서를,
  Windows 에서 그것 없이면 개인 저장소에서 개인 키가 있는 인증서와 그 지문을 보인다. 인증서마다 그것이
  서명하는 MSIX publisher 와 만료일을 찍고, 비밀은 아무것도 찍지 않는다.
- `--timestamp <URL>` 은 RFC 3161 타임스탬프 서버에 서명의 연서명을 청해, 인증서가 만료된 뒤에도
  서명이 유효하게 한다. 없으면 `sign` 과 `build --key` 는 그렇지 않을 것이라고 경고한다. 기본 서버는
  없고, 서버를 적지 않으면 네트워크에 아무것도 나가지 않는다. 공개 서버로는 `http://timestamp.digicert.com`,
  `http://timestamp.sectigo.com`, `http://time.certum.pl` 이 있다. `http` 여도 괜찮다: 답 자체가
  서명되어 있고 확인하기 때문이다(nonce, 서명의 해시, 서버의 서명과 그 타임스탬프 인증서).
  `--tsa-trust` 는 서버 인증서의 경로가 준 인증서 가운데 하나에서 끝나야 한다는 조건을 더한다. `https`
  서버와는 rubrapack 이 TLS 1.3 을 직접 말하고(AES-GCM, x25519 또는 P-256, RSA-PSS 또는 ECDSA 서버 키;
  TLS 1.2 는 아님) 서버 인증서를 확인한다 - `--tls-trust` 로 준 루트나 `--system-roots` 로 운영 체제
  저장소에서 찾은 루트까지의 경로, 날짜, 서버 이름을 담았는지(subjectAltName; 점을 넘는 와일드카드
  없음), TLS 서버용인지. 이 검사를 끄는 방법은 없다. 세 가지 신뢰는 섞이지 않는다: `--tls-trust` 와
  `--system-roots` 는 연결을 누구 말로 믿을지, `--tsa-trust` 는 시각을 누구 말로 믿을지를 정하고,
  어느 것도 다른 쪽에 쓰이지 않는다. Linux 에서 `--system-roots` 는 `$SSL_CERT_FILE` 이나 배포판의 CA
  묶음을 읽고, Windows 에서는 ROOT 저장소를 읽는다. Windows 는 어떤 루트를 무언가가 처음 필요로 할
  때에야 채우므로 그때까지 없을 수 있다. `--proxy http://host:port` 는 요청을 프록시로 보낸다(`https`
  서버는 `CONNECT` 로, 그래서 프록시는 서버 이름만 본다). 없으면 `https_proxy` 나 `HTTPS_PROXY`
  (`https` 서버), `http_proxy`(`http`; 대문자 `HTTP_PROXY` 는 읽지 않는다 - CGI 환경에서는 요청 머리글이
  그것을 정할 수 있다)를 쓰되, `no_proxy` 나 `NO_PROXY` - 이름들, 각각 그 아래 이름까지 포함, 또는
  `*` - 에 서버가 있으면 쓰지 않는다. 암호를 묻는 프록시와 `https://` 프록시는 지원하지 않는다(분명한
  오류). 타임스탬프가 실패하면 - 답 없음, 거절, 잘못된 답, 서명 인증서의 유효 기간 밖의 시각 - 아무것도
  서명하지 않고 아무것도 쓰지 않는다(종료 코드 6). 서명이 타임스탬프 없이 슬그머니 나가는 일은 없다.
  한도: 연결 10초, 전체 60초, 답 4 MiB, 리디렉션 3번, 네트워크 오류나 5xx 답 뒤 재시도 2번.
- `verify` 는 무엇을 확인했는지 찍는다 - 구조, 다이제스트, 서명, `--trust` 로 준 인증서까지의 경로,
  폐기 여부(찾아보지 않는다: `not-checked`), 타임스탬프 - 그리고 모두 맞고 경로가 신뢰하는 인증서에서
  끝날 때만 성공한다. `--trust` 가 없으면 아무것도 신뢰한다고 하지 않는다(종료 코드 4).
  `--system-roots` 는 서명자의 경로에 대해(타임스탬프 쪽은 아님) `--trust` 에 운영 체제의 루트를 더한다.
  Windows 자신의 평판 검사에 대해서는 아무것도 말하지 않는다. 타임스탬프 줄은 `not-present`,
  `<시각>, TSA not-checked (give --tsa-trust)`, `<시각>, TSA trusted`, `<시각>, TSA untrusted`,
  `invalid`, `unsupported`(`Set-AuthenticodeSignature -TimestampServer` 가 쓰는 옛 Authenticode
  타임스탬프로, rubrapack 은 확인하지 않는다) 가운데 하나이고 뒤의 셋은 실패다. 서버가 `--tsa-trust`
  로 신뢰되는 타임스탬프만 - `--trust` 는 여기에 치지 않는다 - 서명자 인증서가 유효해야 하는 시각을
  지금에서 찍힌 시각으로 옮긴다. (시험용으로, 환경 변수 `RUBRAPACK_TEST_NOW` - 1970년부터의 초 - 가
  `verify` 검사의 "지금"을 대신한다. 그래서 시계를 바꾸지 않고 만료된 인증서를 확인할 수 있다. 그
  시각으로 맞춘 시계가 허용하지 않을 것은 아무것도 넓히지 않는다.)
- 종료 코드: 0 성공, 1 원본 오류, 2 사용법, 3 입출력, 4 서명, 5 lint, 6 네트워크.
