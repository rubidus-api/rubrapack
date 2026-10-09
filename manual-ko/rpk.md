# 원본 파일과 `rubrapack` 명령

원본 파일 하나가 패키지 하나를 기술한다. 형식은 [TOML 1.0](https://toml.io/ko/v1.0.0)의 엄격한
부분집합이고 이름은 `*.toml` 이라서, 편집기와 GitHub, AI 도우미가 TOML 로 알아본다(예전 이름 `*.rpk` 도 똑같이
읽는다). 모든 원본은 올바른 TOML 이지만, 부분집합 밖의 TOML 기능은 무시하지 않고
오류로 거부한다.

## 시작하기

rubrapack 은 프로그램 파일 하나다. 릴리스 페이지에서 `rubrapack-<판>-windows-x64.exe`(원하면
`rubrapack.exe` 로 이름을 바꾼다)나 `rubrapack-<판>-linux-x86_64`(`chmod +x` 한다)를 내려받아 그 자리에서
실행하거나 `PATH` 에 둔다. 따로 설치할 것은 없다: 런타임도, SDK 도, 라이브러리도 필요 없다. 같은 프로그램이
Windows 에서도 Linux 에서도 같은 패키지를 만든다.

이 절은 설치 파일을 아는 독자를 위한 짧은 판이다. **제1부 튜토리얼**은 같은 것을 처음부터 한 걸음씩 가르치고,
모든 표와 옵션까지 나아간다: [시작하기 전에](tutorial/01-before-you-start.md)부터 읽는다.

### 첫 패키지

배포할 것을 `dist/` 폴더에 둔다: 프로그램, 그리고 그것이 필요로 하는 파일은 `dist/files/` 에(하위 폴더도
그대로 간다). 그다음 rubrapack 에게 시작할 원본을 쓰게 한다:

```sh
rubrapack new app.toml      # 묻고 나서 app.toml 을 쓰고 검사한다
```

제품 이름, 판, 파일이 든 폴더, 주 프로그램(아키텍처는 파일에서 읽는다), Program Files 아래 폴더, 누구를 위해
설치하는지, 어떤 하위 폴더가 선택 구성요소인지, 대화창, 약관(옆에 `LICENSE.txt`, `.md`, `.rtf` 가 있으면 권한다),
한국어 대화창, 바로가기를 묻는다. Enter 는 대괄호 안의 값을 쓴다. 끝에는 같은 답을 한 줄 명령(`rubrapack new app.toml
--dist dist --ui features ...`)으로 보여 주는데, 스크립트나 AI 도우미는 묻지 않고 이것을 돌리면 된다.
이름 없이 `rubrapack new` 만 주면 파일 이름까지 묻고, `rubrapack new app`(확장자 없이)은 대신 고정된 틀을 쓴다.

어느 쪽이든 `app.toml` 은 읽고 고칠 수 있는 평문이다 - 아무 편집기로나, 또는 `rubrapack edit app.toml` 으로. `edit` 는
같은 질문을 지금 값을 기본으로 보여 주는 메뉴다(판, 설치 폴더, 대화창과 약관, 선택 구성요소, 바로가기, 그리고
프로그램 폴더가 바뀐 뒤 파일 목록 맞추기). 그 값만 바꾸고 다른 줄, 주석, 표는 그대로 둔다. 스크립트에서는:
`rubrapack edit app.toml --set define.VERSION=1.1.0 --sync`. 아래 원본은 프로그램과 파일을 `Program Files\My App` 에 설치하고,
시작 메뉴에 넣고, 사용자가 폴더를 바꿀 수 있는 대화창을 보인다:

```toml
format = 1

[package]
name = "My App"
manufacturer = "My Company"            # 설정 > 설치된 앱에 보인다
version = "$(VERSION)"
arch = "x64"                           # x64, arm64, x86
upgrade-code = "{E8C1815C-CCD7-4F3F-B914-92A4E3F3A317}"   # `new` 가 준 것; 영원히 간직한다
ui = "installdir"

[define]
VERSION = "1.0.0"

[dir.INSTALLDIR]
path = "$(ProgramFiles)/My App"

[file.App]
dir = "INSTALLDIR"
source = "dist/app.exe"

[files.Rest]
dir = "INSTALLDIR"
glob = "dist/files/**"

[shortcut.StartMenu]
dir = "Programs"
name = "My App"
target = "file:App"
```

```sh
rubrapack build app.toml -o app-1.0.0.msi
rubrapack build app.toml -o app-1.0.1.msi -D VERSION=1.0.1    # 다음 판
```

이것으로 완전한 설치 파일이다. "설치된 앱"에 나타나고, 스스로 복구하고, 설치한 것을 모두 지운다. 더 높은
판은 설치된 판을 대체하고, 같거나 낮은 판은 안내문과 함께 거부된다. 컴포넌트 GUID, 파일 키, 캐비닛과 표는
원본에서 끌어내므로, 판에서 판으로 기억해야 할 것은 업그레이드 코드뿐이다.

### 약관 페이지와 선택 구성요소

설치 파일이라면 흔히 바라는 두 가지: 설치 전에 사용자가 약관에 동의하는 것, 그리고 일부 구성요소를 고를 수
있는 것. `license` 는 약관 페이지를 더하고("동의합니다"에 표시하기 전에는 다음 단추가 꺼져 있다),
`ui = "features"` 는 사용자가 설치할 기능을 고르는 트리를 더한다. 아래 원본은 프로그램을 늘 설치하고, 예제는
보여 주되 고르지 않은 상태로 둔다:

```toml
format = 1

[package]
name = "My App"
manufacturer = "My Company"
version = "$(VERSION)"
arch = "x64"
upgrade-code = "{E8C1815C-CCD7-4F3F-B914-92A4E3F3A317}"
ui = "features"                        # welcome, license, folder, feature tree, ready
license = "LICENSE.txt"                # Next stays off until "I accept" is ticked

[define]
VERSION = "1.0.0"

[feature.Main]
title = "My App"
description = "The program itself."
required = true                        # always installed: the tree does not offer to leave it out

[feature.Samples]
title = "Samples"
description = "Example documents to try the program with."
level = 2                              # offered in the tree, not selected by default

[dir.INSTALLDIR]
path = "$(ProgramFiles)/My App"
feature = "Main"

[dir.SamplesDir]
path = "$(INSTALLDIR)/samples"
feature = "Samples"

[file.App]
dir = "INSTALLDIR"
source = "dist/app.exe"

[files.Rest]
dir = "INSTALLDIR"
glob = "dist/files/**"

[files.Samples]
dir = "SamplesDir"
glob = "dist/samples/**"

[shortcut.StartMenu]
dir = "Programs"
name = "My App"
target = "file:App"
```

약관 글은 `LICENSE.txt`(`.txt`, `.md`, `.rtf`)에, 예제 파일은 `dist/samples/` 에 둔다. `level = 2` 인 기능은
사용자가 표시하지 않으면 설치되지 않고, `required` 는 트리가 그 기능을 빼자고 제안하지 못하게 한다. 나중에
사용자는 "설치된 앱"의 "변경"에서 고른 것을 바꿀 수 있고, 명령줄에서도 대화창 없이 같은 일을 한다:
`msiexec /i app.msi /qn ADDLOCAL=Samples` 는 예제를 더하고, `REMOVE=Samples` 는 뺀다(`required` 는 트리에만
적용되고 명령줄은 묶지 않는다). 나머지는 [기능(feature)](#기능feature)과 [조건: when](#조건-when)에 있다.

### 나머지를 설명하는 곳

| 하려는 일 | 읽을 곳 |
|---|---|
| `msiexec` 로 설치, 업그레이드, 제거, 기록 남기기 | [첫 설치 파일](tutorial/02-a-first-installer.md), [판과 업그레이드](tutorial/03-versions-and-upgrades.md) |
| 오류를 이해하고 패키지 안 보기(`lint`, `inspect`, `extract`, 종료 코드) | [검사하고 들여다보기](tutorial/18-checking-and-looking-inside.md), [진단 코드](#진단-코드) |
| 패키지에 서명하기 | [서명과 타임스탬프](tutorial/16-signing.md) |
| 스크립트나 CI 작업, 또는 AI 도우미로 빌드하기 | [빨리 시작하기와 자동화](tutorial/19-automation.md) |

## 예

```toml
format = 1

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
path = "$(ProgramFiles)/Example App"

[dir.Docs]
path = "$(INSTALLDIR)/docs"

[file.MainExe]
dir = "INSTALLDIR"
source = "dist/app.txt"

[file.Guide]
dir = "Docs"
source = "dist/guide.txt"
name = "User guide.txt"
```

```sh
rubrapack build example.toml -o example.msi -D VERSION=1.4.1
rubrapack inspect example.msi File
```

## 원본 형식

원본의 첫 키는 어떤 표보다도 앞에 오는 `format = 1` 이다: 원본 형식의 판을 정수 하나로 적는다. 원본을 다르게
써야 할 때만 올라가고, 표나 키가 새로 생겨도 그대로다. 이것이 없는 원본은 rubrapack 0.18 이하의 것으로 보고
경고(`RP1108`)를 낸다. 그 뒤에 바뀐 것은 쓰인 자리에서 거부한다 - 형식 1 은 경로를 `$(...)` 로 시작한다
([경로](#경로)). 더 큰 수가 적힌 원본에는 더 새 rubrapack 이 필요하다.

## TOML 부분집합

받는 것: `[kind]`·`[kind.ID]` 표, 맨 키(`A-Z a-z 0-9 _ -`), TOML 이스케이프가 되는 기본 문자열
`"..."`, 리터럴 문자열 `'...'`(이스케이프 없음 - 백슬래시와 따옴표를 쓸 때 편하다:
`'SOFTWARE\Example'`), 십진수와 `0x` 정수, `true`/`false`, 한 가지 형만 담은 배열, `#` 주석.
BOM 이 있거나 없는 UTF-8, 또는 BOM 이 있는 UTF-16LE. 줄 끝은 LF 나 CRLF.

오류로 거부하는 것: 여러 줄 문자열, 인라인 표, 표 배열, 점으로 이은 키와 따옴표 키, 세 부분 이상의
표 이름, 부동소수, 날짜, 숫자 속 `_`, 8진·2진 수, 빈 배열이나 섞인 배열, 첫 표보다 앞의 키(`format` 은 빼고). 표와
키의 순서는 아무 의미가 없다.

## 표

| 표 | 키(굵은 것은 반드시) |
|---|---|
| `[package]` | **name**, **manufacturer**, **version**(`a.b.c` 또는 `a.b.c.d`), **arch**, **upgrade-code**, upgrade-code-x64 / -arm64 / -x86, product-code, summary-name(ASCII), language, scope(`machine`, `user`, `dual`), ui(`none`, `basic`, `minimal`, `installdir`, `features`), license(`.txt`, `.md`, `.rtf`), reboot(`suppress`/`allow`), cleanup(`false`: 정리 작업 없음), preflight(`false`: 진행 전 점검 없음), close-programs(`ask`, `always`, `never`), parent(추가 기능: 본체 제품의 upgrade-code), remove-addons(`true`: 이 제품을 지우면 추가 기능도 지운다), replaces(이 패키지가 대신하는 제품들의 업그레이드 코드), downgrade-message, compress(`none`, `mszip`, `mszip:0`..`mszip:9`, `lzx`, `lzx:15`..`lzx:21`; 기본 `mszip:6`), cab(`embed` 또는 `external`), cab-max-size(MiB), refuse-upgrade-below, refuse-upgrade-message |
| `[define]` | 변수: `NAME = "value"` |
| `[feature.ID]` | **title**, description, level(1-32767), hidden, parent, required, follow-parent, when, default-when |
| `[dir.ID]` | **path** = `기준/상대/경로`, feature, guard(`true`: [설치 폴더 지키기](#설치-폴더-지키기) 참고) |
| `[file.ID]` | **dir**, **source**, name, vital(기본 true), any-arch, feature, component-guid, keep, when |
| `[files.ID]` | **dir**, **glob**, vital, any-arch, feature, keep, when |
| `[folder.ID]` | **dir**, **name**, keep, feature |
| `[arp]` | no-modify, no-repair, no-remove, help(URL), about(URL), icon(`.ico`) - "설치된 앱"에 제품이 어떻게 보이는가 |
| `[property.ID]` | **value**, secure, hidden - 대문자 이름의 공개 속성 |
| `[action.ID]` | **run**(이 패키지의 `.exe` 를 가리키는 `file:ID`), **do**, **undo**, check |
| `[registry.ID]` | **root**(`HKLM`, `HKCU`, `HKCR`, `HKMU`), **key**, name, value, type, remove, keep, view, with, feature, when |
| `[remove.ID]` | **dir**, name(`*` 와 `?`; 없으면 폴더 자체), **on**(`install`, `uninstall`, `both`), upgrade(`false`: 업그레이드가 이 판을 지울 때는 하지 않음), feature |
| `[ini.ID]` | **dir**, **file**, **section**, **key**, value, mode(`set`, `add`, `remove`), feature, when |
| `[require.ID]` | **condition**, **message** |
| `[search.ID]` | **property**(또는 dir ID), **kind**(`registry`: root, key, name, view; `file`: path, file, min-version; `dir`: path; `component`: component-guid) |
| `[service.ID]` | **file**(`.exe` 를 가리키는 `file:ID`), **name**, display-name, description, start(`auto`, `demand`, `disabled`), account(`LocalSystem`, `LocalService`, `NetworkService`), args, start-on-install |
| `[assoc.ID]` | **extension**(`.ext`, 소문자), **prog-id**, **target**(`.exe` 를 가리키는 `file:ID`), description, icon(`file:ID`), args(기본 `"%1"`), content-type, perceived-type, default(`false`: "연결 프로그램"에만) |
| `[menu.ID]` | **on**(파일 형식, `"*"`, `"folder"`, `"background"`, `"drive"`, `"assoc:ID"`. 하위 메뉴의 항목에는 없다), **text** 또는 text-xx, target(`.exe` 를 가리키는 `file:ID`. 없으면 하위 메뉴), args(기본 `"%1"`), icon(`file:ID`), parent(하위 메뉴의 ID), multi(`each`, `one`, `single`), extended, windows11(기본 `true`) - 탐색기 우클릭 메뉴의 항목: [탐색기의 우클릭 메뉴](#탐색기의-우클릭-메뉴-menuid) 참조 |
| `[protocol.ID]` | **name**(스킴, 소문자), **target**(`.exe` 를 가리키는 `file:ID`), description, args(기본 `"%1"`) |
| `[com.ID]` | **file**(`.exe` 나 `.dll` 의 `file:ID`), **class**(`{GUID}`), description, threading(`sta` 기본, `mta`, `both`, `neutral`; DLL), args(프로그램), prog-id, app-id(`{GUID}`), surrogate(dllhost 에서 도는 DLL), typelib(`{LIBID}`), typelib-version(기본 `"1.0"`), typelib-file(기본: 서버), msi-only - COM 클래스, [COM 클래스](#com-클래스-comid) 참고 |
| `[handler.ID]` | **kind**(`thumbnail`, `preview`, `property`), **class**(`[com.*]` 의 DLL 클래스), **types**(`[".ext", ...]`), description(미리 보기 처리기의 이름), msi-only - 탐색기 처리기, [탐색기 처리기](#탐색기-처리기-handlerid) 참고 |
| `[font.ID]` | **file**(`path = "$(Fonts)"` 인 dir 에 드는 파일의 `file:ID`), title |
| `[permission.ID]` | **target**(`dir:ID`, `file:ID`, `registry:ID`), **sddl** |
| `[env.ID]` | **name**, **value**, mode(`set`, `append`, `prepend`), keep, feature, when |
| `[copy.ID]` | **source**(`file:ID`), **dir**, name(기본: 원본 파일 이름) |
| `[merge.ID]` | **source**(`.msm` 병합 모듈), **dir**(모듈의 뿌리 폴더가 갈 곳), feature, config(설정할 수 있는 모듈에 `["이름=값", ...]`). MSIX 에서는 파일과 레지스트리 값만 |
| `[module]` | **name**(모듈의 ID: 영문자·숫자·`_`, 35자 이내), **manufacturer**, **version**, **arch**, **id**(모듈의 GUID, 모든 판에서 그대로), language(기본 `neutral`, `en-US`, `ko-KR`), compress(`none`, `mszip`, `mszip:0`..`mszip:9`) - `[package]` 대신: 병합 모듈, [병합 모듈 만들기](#병합-모듈-만들기-module) 참고 |
| `[ui]` | install-dir(dir ID; 기본 `INSTALLDIR`), banner(`.bmp`), launch(`file:ID`), launch-args, launch-checked, save-log(기본 `true`), languages(영어에 덧붙일 언어, 예 `["ko"]`), license-xx, name-xx, font-xx, langid-xx - [여러 언어](#여러-언어) 참고 |
| `[ui-text.ID]` | **text** 또는 text-xx - 내장 대화창 문구 하나를 바꾼다 |
| `[dialog.ID]` | **after**(내장 페이지 또는 다른 `[dialog.*]`), title, description, title-xx, description-xx |
| `[dialog-control.ID]` | **dialog**, **type**(`text`, `checkbox`, `edit`, `radio`, `combo`), **x**, **y**, **width**, **height**, text, property, values, labels, text-xx, labels-xx |
| `[shortcut.ID]` | **dir**(dir ID, 또는 `Programs`, `Desktop`, `StartMenu`, `Startup`), **name**, **target**(`file:ID`), args, description, working-dir(dir ID), icon(`.ico`), when |
| `[msix]` | **identity-name**, **publisher**, display-name, display-name-xx, publisher-display-name, publisher-display-name-xx, min-version, capabilities(이름들), file-system-virtualization, registry-virtualization(기본 `true`), main-package, main-publisher, modification, language-packs(기본 `true`), appinstaller-uri, package-uri, update-hours(0-255, 기본 24), update-prompt, update-blocks, update-background - [MSIX 패키지](#msix-패키지) 참고 |
| `[msix-app.ID]` | **executable**(`[file.*]` ID), display-name, display-name-xx, description, description-xx, logo-150, logo-44, store-logo(저마다 `.scale-NNN` 변형을 둘 수 있다), background-color(기본 `transparent`, `#RRGGBB`), hidden(시작 메뉴 항목 없음) |
| `[msix-dependency.ID]` | **name**, **publisher**, **min-version** - MSIX 가 필요로 하는 프레임워크 패키지 - MSIX 전용 |
| `[msix-extension.ID]` | **kind**(`alias`: **alias**; `startup-task`: task-id, display-name, enabled; `firewall`: **direction**(`in`, `out`), **protocol**(`tcp`, `udp`), ports(`8080` 또는 `8000-8100`), profile(`all`, `domain`, `private`, `public`), file(기본은 앱의 프로그램); `com-server`: **file**(`.exe` 나 `.dll`), **class**(`{GUID}`), display-name, args(`.exe`), threading(`sta` 기본, `mta`, `both`, `neutral`; `.dll`); `toast`: **class**, file(기본은 앱의 프로그램), args(기본 `-ToastActivated`); `context-menu`: **file**(`.dll`), **class**, **types**(`[".txt", "*"]`), verb(기본은 표 ID), threading), app(`[msix-app.*]` ID; 기본은 첫째) - MSIX 전용 |
| `[chain]` | **name**, **manufacturer**, **version**, arch(설치 프로그램 자신의 것: `x64`, `x86`, `arm64`), elevate(기본 `true`) - 체인 원본: [설치 프로그램 하나에 여러 패키지](#설치-프로그램-하나에-여러-패키지-chain) 참고 |
| `[chain-package.ID]` | **source**(`.msi`), properties(msiexec 속성), vital(기본 `true`) |

dir 경로의 `$(기준)`은 다른 dir ID 이거나 Windows 폴더다: `$(ProgramFiles)`(x64/arm64 는 64비트,
x86 은 32비트), `$(ProgramFiles(x86))`, `$(CommonProgramFiles)`, `$(APPDATA)`, `$(LOCALAPPDATA)`,
`$(ProgramData)`, `$(TEMP)`, `$(SystemRoot)`, 그리고 환경 변수가 없는 폴더 `$(StartMenu)`, `$(Programs)`,
`$(Desktop)`, `$(Startup)`, `$(System)`, `$(Fonts)` - [Windows 이름](#windows-이름) 참고. 폴더 하나만 쓴
경로(`path = "$(Fonts)"`)는 그 폴더 바로 안에 드는 파일에 쓴다.

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
- 패키지 자신의 폴더는 첫 `[msix-app.*]` 실행 파일이 드는 폴더 - `$(ProgramFiles)/Example App` 처럼
  알려진 자리에 붙은 dir - 다. 다른 알려진 자리의 파일은 패키지의 가상 파일 시스템(VFS)에 들어가고,
  앱은 그것을 늘 보던 자리에서 본다: Program Files(`VFS\ProgramFilesX64`, x86 패키지와
  `ProgramFiles(x86)` 는 `X86`), `CommonProgramFiles`, `System`, `SystemRoot`, `ProgramData`.
  사용자의 `APPDATA`·`LOCALAPPDATA` 와 `TEMP` 에는 가상 폴더가 없어 거기 드는 파일은 오류다
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
- 로고와 이름이 같고 확장자 앞에 `.scale-100`, `.scale-125`, `.scale-150`, `.scale-200`, `.scale-400` 이 붙은
  파일은 그 배율의 화면을 위한 같은 로고이고, 크기는 로고 크기에 배율을 곱한 것이다(`Square44x44.scale-150.png`
  는 66x66). 로고 자신이 있으면 배율 100 으로 치므로 `.scale-100` 과 함께 있을 수 없다.
- `display-name-xx`, `description-xx`(`[msix-app.*]`)와 `display-name-xx`, `publisher-display-name-xx`(`[msix]`;
  `display-name` 의 기본값은 패키지 이름)는 그 글을 다른 언어로 준다(`xx` 는 `[ui]` 처럼 `ko`, `ja`, `de`, ...).
  접미사 없는 글이 패키지 자신의 언어의 것이므로, 그 언어의 접미사는 오류다(`RP1606`). 그러면 매니페스트는 그
  글들을 `ms-resource:` 이름으로 가리키고 언어들을 적는다.
- 여러 배율의 로고와 여러 언어의 글은 `resources.pri`, 곧 Windows 가 그것들을 찾는 패키지 자원 색인에 들어간다.
  그런 것이 없는 패키지에는 없다.
- MSIX 가 할 수 없는 것은 조용히 빼지 않고 오류로 알린다(`RP1605`): 사용자 지정 동작, 권한, 설치 조건과 검색, 설치 때 지우는 파일, 빈 폴더. `[copy.*]` 는 파일을 복사본의 자리에 한 번 더 패키지에 넣는다.
- `[ini.*]` 항목은 패키지 안의 INI 파일(프로그램 옆, 또는 가상 파일 시스템 안)이 되고, 패키지를 빌드할 때 쓴다:
  ASCII, 또는 ASCII 밖의 글이 있으면 BOM 이 있는 UTF-16. `add` 는 값을 쉼표로 잇고, `remove` 는 새 패키지에서
  지울 것이 없다. 설치 때 채우는 부분(`[...]`)이 든 값과 `when` 은 거절한다(`RP1612`).
- `[env.*]` 는 MSIX 에서 컴퓨터의 환경을 바꾸지 못한다. 대신 애플리케이션 자신의 프로세스에 변수를 준다: 모든
  애플리케이션이 rubrapack 의 실행기(`rubrapack\<AppId>.exe`, 콘솔 애플리케이션이면 콘솔 프로그램)를 거쳐 시작하고,
  실행기는 `rubrapack\launch.txt` 를 읽어 변수를 정한 뒤(`append`/`prepend` 는 있던 값에 `;` 로 잇는다) 프로그램을
  자기 폴더에서 받은 명령줄로 시작하고 그 종료 코드를 돌려준다. 값 안에서 패키지의 dir 은 실제 폴더가, Windows 폴더나
  이름은 그 환경 변수가 되고, `%NAME%` 은 프로그램이 시작할 때 읽는다. 패키지 밖의 dir, 설치 때 채우는 부분, `when` 은
  거절한다(`RP1612`). 실행 별칭과 시작 작업도 실행기로 시작하고, 서비스·COM 서버·방화벽 규칙은 프로그램 자체를 가리킨다.
- `capabilities` 는 모든 패키지에 있는 `runFullTrust` 말고 애플리케이션이 Windows 에 요구하는 것을 선언한다.
  rubrapack 은 이름마다 매니페스트 스키마가 원하는 요소로 쓴다(모르는 이름은 `RP1616`):
  - `Capability`: internetClient, internetClientServer, privateNetworkClientServer, allJoyn, codeGeneration
  - `uap:Capability`: documentsLibrary, picturesLibrary, videosLibrary, musicLibrary, removableStorage,
    enterpriseAuthentication, sharedUserCertificates, appointments, contacts, userAccountInformation,
    objects3D, phoneCall, voipCall, chat, blockedChatMessages
  - `uap3:Capability`: backgroundMediaPlayback, remoteSystem, userNotificationListener; `uap6`:
    graphicsCapture; `uap7`: globalMediaControl
  - `rescap:Capability` (제한된 능력: Microsoft Store 가 까닭을 묻는다): allowElevation, unvirtualizedResources,
    broadFileSystemAccess, packageManagement, packageQuery, confirmAppClose, appDiagnostics,
    appLicensing, localSystemServices, packagedServices, extendedExecutionUnconstrained,
    extendedBackgroundTaskTime, inputForegroundObservation, inputObservation, inputSuppression,
    inputInjectionBrokered, uiAccess, interopServices, customInstallActions, modifiableApp,
    appCaptureSettings
  - `DeviceCapability`: webcam, microphone, location, bluetooth, proximity, radios, wiFiControl,
    lowLevel, gazeInput
  완전 신뢰로 도는 데스크톱 애플리케이션에는 몇 개만 필요하다: `allowElevation` 은 관리자 권한으로 시작하게 하고,
  라이브러리와 장치 이름은 그것을 확인하는 API 에서 쓰인다.
- `file-system-virtualization = false` 와 `registry-virtualization = false` 는 애플리케이션이 패키지의 개인 사본 대신
  실제 자리(설치 폴더, `HKCU`)에 쓰게 한다. `unvirtualizedResources` 를 더하고 `min-version = "10.0.18362.0"` 이상이
  필요하다(`RP1614`). Microsoft 는 Store 에서 이것을 일부 게임에만 허락한다. 직접 설치하는 패키지는 쓸 수 있다.
- `[msix-dependency.ID]` 는 애플리케이션이 필요로 하는 프레임워크 패키지 - C++ 런타임
  `Microsoft.VCLibs.140.00.UWPDesktop`(게시자 `CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond,
  S=Washington, C=US`), Windows App SDK - 를 쓸 수 있는 가장 오래된 판과 함께 적는다. 그것이 없으면 Windows 가
  패키지를 설치하지 않는다. MSI 빌드는 이 표를 뺀다.
- `background-color` 는 애플리케이션 타일의 색을 정하고, `hidden = true` 는 애플리케이션(예를 들어 도우미)을 시작
  메뉴에서 뺀다.
- `main-package = "Contoso.Main"`(주 패키지의 정체 이름)은 이 패키지를 그 패키지의 *선택적 패키지*로 만든다: 주
  패키지의 컨테이너 안에서 도는 콘텐츠나 애플리케이션이고, 주 패키지와 함께여야만 설치된다
  (`uap3:MainPackageDependency`; Windows 10 1703). 주 패키지의 게시자가 다르면 `main-publisher` 로 적는다(Store
  밖에서만). `modification = true` 를 더하면 대신 *수정 패키지*가 된다: 주 애플리케이션의 설정을 바꾸는 파일(가상 파일
  시스템 안)과 레지스트리 값 - 예를 들어 기업의 설정 - 이고, 자기 애플리케이션은 없다(`rescap6:ModificationPackage`;
  Windows 10 1903, `min-version = "10.0.18362.0"`). 두 가지 모두 `[msix-app.*]` 표가 꼭 있지는 않고, 능력과 가상화
  설정은 두지 않는다: 주 패키지의 것이 적용된다(`RP1617`). 애플리케이션이 없으면 dir `INSTALLDIR` 가 패키지 자신의
  폴더다(선택적 패키지. 수정 패키지는 모두 가상 파일 시스템에 둔다).
- 번들에서는 패키지 자신의 언어가 아닌 언어의 글(`display-name-xx`, `description-xx`,
  `publisher-display-name-xx`)이 그 언어의 리소스 패키지(`<정체>_<판>_language-xx.msix`, `ResourcePackage`, 실행
  없음)에 `resources.pri` 의 제 몫과 함께 들어간다. Windows 는 사용자 언어의 것만 설치한다. `language-packs = false`
  는 모든 언어를 아키텍처마다의 패키지에 둔다.
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
인자는 쓴 그대로 넘어간다(서식 문자열이 아니다). `check` 는 아직 효과가 없어 경고(`RP1318`)만 낸다.

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
path = "$(Programs)/Example"         # 시작 메뉴 > Example

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

파일 형식에는 무엇이 들었는지, 그리고 프로그램이 그 형식을 가져갈지도 적을 수 있다:

```toml
[assoc.Picture]
extension = ".png"
prog-id = "Example.Picture"
target = "file:MainExe"
content-type = "image/png"        # 그 형식의 미디어 형식
perceived-type = "image"          # image, text, audio, video, compressed, document, system, application
default = false                   # "연결 프로그램"에만 올리고 형식을 가져가지 않는다
```

모든 `[assoc.*]` 는 자기 prog-id 를 확장자의 `OpenWithProgids` 에 올려서 "연결 프로그램"에 나오게 한다.
`default = false` 면 그것만 한다(확장자의 기본값은 건드리지 않는다). MSIX 에서는 `content-type` 이
매니페스트에 들어가고, `perceived-type` 과 `default` 는 MSI 의 것이다.

### 탐색기의 우클릭 메뉴: `[menu.ID]`

```toml
[menu.Convert]
on = [".png", ".jpg", "folder"]   # 어디에 나오는가
text = "Convert with Example"
text-ko = "Example 로 변환"        # 언어별 문구
target = "file:MainExe"
args = "--convert \"%1\""         # "%1" 은 고른 것의 경로. 기본값은 "%1" 하나
icon = "file:MainExe"             # 기본: 프로그램 자신의 아이콘

[menu.Tools]                      # target 이 없으면 하위 메뉴
on = "*"
text = "Example tools"

[menu.Checksum]
parent = "Tools"                  # 그 하위 메뉴의 항목. 하위 메뉴가 나오는 곳에 나온다
text = "Checksum"
target = "file:MainExe"
args = "--sum \"%1\""
multi = "single"                  # 하나만 골랐을 때만
```

항목은 우클릭한 것을 넘겨 패키지의 프로그램을 시작한다. `on` 은 자리 하나 또는 목록이다: 파일 형식
(`".png"`), `"*"`(모든 파일), `"folder"`, `"background"`(열린 폴더의 빈 곳: 이때 `%1` 은 그 폴더),
`"drive"`, `"assoc:ID"`(어느 `[assoc.*]` 의 파일 형식, 그 prog-id 아래). `multi` 는 여러 개를 골랐을 때의
뜻이다: `"each"`(기본: 항목마다 한 번 실행), `"one"`(전부를 넘겨 한 번 실행: `args` 에 `%*` 를 쓰면 모든
경로), `"single"`(하나를 골랐을 때만 항목이 나온다). `extended = true` 면 Shift 를 누른 채일 때만 나온다.
하위 메뉴는 한 단계까지다.

같은 표가 Windows 의 두 메뉴에 다 쓰인다:

- **옛 메뉴**(Windows 10, Windows 11 의 "추가 옵션 표시")는 레지스트리의 동사를 읽는다. 패키지는 자리마다
  동사를 적는다(`SystemFileAssociations\.png\shell\<제품>.<ID>`, `*`, `Directory`, `Directory\Background`,
  `Drive`). 패키지의 코드는 탐색기 안에서 돌지 않는다. 동사의 문구는 하나다: `text-xx` 가 있으면 설치
  언어의 문구가 들어간다(대화 상자의 언어, 없으면 `[ui] languages` 가운데 사용자의 표시 언어). `"one"` 은
  여기서 `"each"` 처럼 항목마다 실행된다.
- **Windows 11 메뉴**는 신원이 있는 패키지가 선언한 항목만 보여 주고, 항목 하나하나가 COM 클래스여야 한다.
  그 클래스는 rubrapack 이 제공한다: 메뉴 부품 `rubrapack_menu.dll` 이 모든 항목을 맡아, 사용자의 표시
  언어로 문구를 보이고 프로그램을 시작한다. 이 DLL 은 탐색기가 아니라 `dllhost.exe` 에서 돌고, 설치가 그
  항목에 대해 적어 둔 일만 한다.
  - **MSIX** 는 신원이 있다: DLL 이 패키지에 들어가고 매니페스트가 항목을 선언한다.
  - **MSI** 는 항목의 프로그램 옆에 파일 둘을 더 설치한다 - 그 DLL 과, 매니페스트와 로고만 든 작은 패키지
    `rubrapack_menu.msix`. 설치할 때 이 패키지를 프로그램 폴더에 대해 등록하고(컴퓨터의 모든 사용자용,
    다른 사용자는 다음 로그인 때) 제거할 때 지운다. **인증서가 필요 없다**: 관리자 권한의 설치에서
    Windows 가 받아 주는 무서명 형식으로 만든다. `--key` 와 `[msix] publisher`(인증서의 주체)가 있으면
    서명한다. 등록할 수 없는 곳 - Windows 10 버전 2004 이전, 또는 관리자 권한이 없는 사용자의 사용자별 설치 - 에서는 설치가
    그대로 진행되고 항목은 옛 메뉴에만 나온다. 로그에 그렇게 적힌다(`rubrapack: menu:`).
- 패키지가 등록된 곳에서는 Windows 가 그 항목을 옛 메뉴에도 보여 주므로, 거기서는 레지스트리 동사를
  끈다(`LegacyDisable`): 항목은 한 번씩만 나온다.

Windows 11 에서 알아 둘 것: 항목은 설치가 끝나고 몇 초 뒤에 나타난다. 한 패키지의 항목이 같은 종류의
대상에 둘 이상이면 Windows 가 제품 이름을 단 항목 하나 아래로 묶는다. `"drive"` 와 `windows11 = false` 인
항목은 옛 메뉴에만 나온다. 메뉴 부품이 무엇을 하는지 보려면 `HKCU\Software\rubrapack` 아래에 `MenuLog`
값(파일 경로)을 둔다: 고른 항목과 시작한 명령이 그 파일에 덧붙는다.

### COM 클래스: `[com.ID]`

```toml
[com.Widget]
file = "file:WidgetDll"           # 이 패키지의 .exe 나 .dll 이 클래스를 제공한다
class = "{8D1E2F30-4A5B-4C6D-8E7F-901A2B3C4D35}"
description = "Example widget"
prog-id = "Example.Widget"        # 스크립트가 부르는 이름
threading = "both"                # DLL 의 아파트: sta(기본), mta, both, neutral
app-id = "{8D1E2F30-4A5B-4C6D-8E7F-901A2B3C4D36}"
surrogate = true                  # 호출한 프로세스 밖 dllhost 에서 돌 수 있다
typelib = "{8D1E2F30-4A5B-4C6D-8E7F-901A2B3C4D37}"
typelib-version = "1.2"           # 16진 숫자의 major.minor; typelib-file 로 다른 파일을 가리킨다

[com.Server]
file = "file:MainExe"             # 프로그램: LocalServer32, args 를 붙여 시작한다
class = "{8D1E2F30-4A5B-4C6D-8E7F-901A2B3C4D38}"
args = "-Embedding"
```

MSI 에서는 파일 형식처럼 서버의 구성 요소에 든 `HKEY_CLASSES_ROOT` 아래 레지스트리 값이 된다: 설명을 단
`CLSID\{class}`, `InprocServer32`(DLL 과 `ThreadingModel`) 또는 `LocalServer32`(따옴표 친 프로그램 뒤에 `args`),
prog-id 가 있으면 `CLSID\{class}\ProgID` 와 `<prog-id>\CLSID`, app-id(또는 app-id 가 없을 때 클래스 ID 를 쓰는
`surrogate`)가 있으면 클래스의 `AppID` 값과 `AppID\{app-id}`(서로게이트면 `DllSurrogate` 도), typelib 이 있으면
`CLSID\{class}\TypeLib` 와 `TypeLib\{typelib}\<version>`(패키지 아키텍처에 따라 `0\win64` 또는 `0\win32`,
`FLAGS`, `HELPDIR`). 제거하면 함께 사라진다. 값은 Class·ProgId·TypeLib·AppId 표가 아니라 Registry 행으로 쓴다 -
Microsoft 도구가 광고하지 않는 클래스를 쓰는 방식이다. 그 표들의 클래스는 Windows Installer 가 광고된 것(처음 쓸 때
설치)으로 등록하기 때문이다. 사용자별 설치면 `HKCU\Software\Classes` 에 들어간다. Microsoft 검증(ICE33)은 표를 쓰라며 이런 행에 경고를
내는데, `[com]` 에서는 예상된 경고다. 클래스 ID, prog-id, ID 는 각각
한 번만 쓴다(`RP1301`). `threading` 과 `surrogate` 는 DLL 의, `args` 는 프로그램의 것이다(`RP1316`).

MSIX 에서 `[com]` 은 패키지 COM 카탈로그의 클래스(`com:ComServer`: 프로그램은 `ExeServer`, DLL 은
`SurrogateServer`)가 되고 prog-id 는 `com:ProgId` 가 된다. `app-id` 와 `typelib` 은 MSI 에만 들어가며 경고가
그렇게 알린다(`RP1612`).

### 탐색기 처리기: `[handler.ID]`

```toml
[handler.Thumbs]
kind = "thumbnail"                # 탐색기가 파일에 보이는 그림
class = "{8D1E2F30-4A5B-4C6D-8E7F-901A2B3C4D41}"   # 패키지 DLL 의 [com.*] 클래스
types = [".exdoc"]

[handler.Preview]
kind = "preview"                  # 미리 보기 창에 보이는 것
class = "{8D1E2F30-4A5B-4C6D-8E7F-901A2B3C4D43}"
types = [".exdoc"]
description = "Example document preview"
msi-only = true

[handler.Props]
kind = "property"                 # 탐색기와 검색이 쓰는 파일 속성(제목, 작성자 ...)
class = "{8D1E2F30-4A5B-4C6D-8E7F-901A2B3C4D42}"
types = [".exdoc"]
msi-only = true
```

처리기는 패키지 DLL 안의 COM 클래스로 `[com.*]`(`threading` 포함)에 적고, `[handler]` 는 그것이 어떤 파일 형식을
맡는지 Windows 에 알린다. MSI 에서: 썸네일과 미리 보기 처리기는 `HKEY_CLASSES_ROOT` 아래 형식마다의 `ShellEx`
값이다(썸네일 `{E357FCCD-...}`, 미리 보기 `{8895B1C6-...}`). 미리 보기 처리기는 `PreviewHandlers` 목록에도 오르고,
그 클래스는 Windows 미리 보기 호스트(`prevhost.exe`, 패키지 아키텍처에 따라 64비트나 32비트)의 `AppID` 를 받으므로
그 `[com]` 에는 `app-id` 와 `surrogate` 를 쓰지 않는다. 속성 처리기는 `HKLM` 의 `PropertySystem\PropertyHandlers`
아래이며 Windows 가 컴퓨터 단위로만 읽으므로 패키지는 `scope = "machine"` 이어야 한다. 한 형식에는 종류마다 처리기
하나다(`RP1301`).

MSIX 에서 썸네일·미리 보기 처리기는 패키지의 파일 형식 연결(`desktop2:ThumbnailHandler`,
`desktop2:DesktopPreviewHandler`, 클래스는 패키지 COM 카탈로그)에 들어간다. 형식 묶음마다 연결 하나이며, 그
형식을 여는 `[assoc]` 가 있으면 거기에 들어간다. 패키지가 설치되어 있는 동안 탐색기가 쓴다. 패키지는 클래스를
대리 프로세스(`dllhost`)에서 돌리므로 미리 보기 처리기의 클래스에는 `threading = "sta"` 를 준다. 다른 모델이면
그 창이 메시지 루프 없는 스레드에 놓여 빈 채로 남을 수 있다(경고, `RP1612`). 패키지에 든 속성
처리기는 Windows 11 에서 탐색기에 값을 주지 못했다(같은 클래스를 MSI 로 설치하면 준다). 그래서 MSIX 는 그
종류를 거절하고(`RP1612`) `msi-only = true` 가 MSI 에만 남긴다.

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

```toml
[msix-extension.Web]
kind = "firewall"                 # 패키지가 설치돼 있는 동안의 Windows 방화벽 규칙
direction = "in"
protocol = "tcp"
ports = "8080"
profile = "private"               # 기본값 "all". 다른 프로그램이면 file = "file:ID"
```

MSI 빌드는 이것들을 뺀다. MSIX 에서 글꼴(`[font.*]`)은 패키지의 `Fonts` 폴더에서 다른 앱과 나눠
쓰며(`uap4:SharedFonts`) `title` 은 쓰지 않는다.

```toml
[msix-extension.Server]
kind = "com-server"               # 프로그램(또는 DLL)이 제공하는 COM 클래스
file = "file:MainExe"
class = "{7A1B2C3D-4E5F-4A6B-8C7D-9E0F1A2B3C41}"
args = "-Embedding"

[msix-extension.Toast]
kind = "toast"                    # 앱의 알림을 누르면 이 클래스로 앱이 시작된다
class = "{7A1B2C3D-4E5F-4A6B-8C7D-9E0F1A2B3C43}"

[msix-extension.Menu]
kind = "context-menu"             # DLL(IExplorerCommand)이 처리하는 탐색기 오른쪽 메뉴 항목
file = "file:MenuDll"
class = "{7A1B2C3D-4E5F-4A6B-8C7D-9E0F1A2B3C44}"
types = [".txt", "*"]             # 파일 형식. "*" 는 모든 파일
```

이것들은 앱의 `com:ComServer`(프로그램이면 `ExeServer`, DLL 이면 `SurrogateServer`), 프로그램을 그 클래스와
`args` 로 등록하는 `desktop:ToastNotificationActivation`, 그리고 DLL 을 클래스로 쓰는
`desktop4:FileExplorerContextMenus` 가 된다. 패키지가 설치돼 있는 동안 Windows 는 그 클래스들을 패키지 COM 목록에
올린다. 오른쪽 메뉴는 DLL 이 그 항목을 구현해야(IExplorerCommand) 보인다. rubrapack 은 등록할 뿐 구현하지 않는다.

`[service.*]` 는 MSIX 에 패키지 서비스(`desktop6:Service`, 첫째 앱 안, 권한 `packagedServices` 와
`account = "LocalSystem"` 이면 `localSystemServices`)로 들어간다: 이름·시작·계정은 MSI 와 같고 `args` 는
평문이어야 한다. `display-name` 과 `description` 은 쓰지 않고, 서비스는 `start-on-install` 이 아니라
`start` 대로 시작한다. `min-version = "10.0.19041.0"` 이상이 필요하다(`RP1614`). 패키지를 지우면 서비스가
멈추고 지워지며, 방화벽 규칙도 함께 사라진다.

### 지우기와 복사: `[remove.ID]`, `[copy.ID]`

`[remove.ID]` 는 `dir` 에서 `name` 에 맞는 파일을 지운다 - 예를 들어 옛 판이 남긴 `*.log` 를
`on = "install"` 로, 프로그램이 실행 중에 만드는 파일을 `on = "uninstall"` 로. `name` 이 없으면 비어
있는 폴더 자체를 지운다. 설치가 실패하면 지운 파일은 되돌아온다. `[copy.ID]` 는 이 패키지의 파일을
다른 폴더에 한 벌 더 설치하고, 원본과 함께 들어오고 함께 나간다. 설치 전부터 있던 폴더는 절대 지우지
않는다.

업그레이드는 옛 판도 지우고, 그때 옛 판의 `on = "uninstall"` 행도 돈다. `upgrade = false` 는 그 행을
거기서 뺀다: 파일은 제품(또는 그 기능)을 정말 지울 때만 지워진다 - 새 판이 그대로 쓰는 파일이나, 옛 판을
아직 돌리는 프로그램이 다시 읽을 수 있는 파일에 쓴다. RemoveFile 표 대신 rubrapack 의 도우미 DLL 이
한다: 파일을 백업 폴더(그 볼륨의 `Config.Msi`, 사용자별 패키지는 임시 폴더)로 옮기고, 제거가 실패하면
되돌리고, 성공하면 지운다. 프로그램이 쥐고 있는 파일은 다음 재시작 때 지워진다. 업그레이드는 옛 판 자신의
행을 돌리므로, 이 키를 넣고 만든 첫 판부터 효과가 있다.

### 환경 변수: `[env.ID]`

시스템(컴퓨터 전체) 변수다. `mode = "set"`(기본)은 값을 바꾸고, `append`/`prepend` 는 기존 값 끝에
`;value` 를, 앞에 `value;` 를 붙인다. 제거는 정확히 그만큼 되돌린다: set 한 변수는 지우고, 붙인
부분은 떼어 내고 나머지는 둔다. `keep = true` 면 남긴다.

### INI 파일: `[ini.ID]`

`mode = "set"`(기본)은 파일의 `[section]` 에 `key=value` 를 쓰고, `add` 는 쉼표 목록에 값을 덧붙이며
(`a` 가 `a,b` 로), `remove` 는 설치 중에 키를 지운다. 제거하면 `set` 과 `add` 가 쓴 것을 뗀다.
`remove` 는 파일 설치보다 먼저 실행되므로 옛 판이 남긴 INI 파일을 위한 것이다.

### 설치 프로그램 하나에 여러 패키지: `[chain]`

`[chain]` 이 있는 원본은 패키지 대신 설치 프로그램을 만든다: `rubrapack build suite.toml -o setup.exe`.
설치 프로그램은 `[chain-package.*]` 패키지들을 원본에 적힌 차례로, 저마다의 SHA-256 과 함께 담는다. 실행하면
모두 확인한 뒤 Windows Installer 로 하나씩 차례로 설치한다. 이미 설치된 패키지(ProductCode 로 본다)는
건너뛴다. `vital` 패키지(기본)가 실패하면 체인이 멈추고 그 패키지의 오류를 돌려준다. 그 패키지는 스스로
되돌리고, 앞의 패키지들은 남는다. `properties` 는 msiexec 명령줄에서처럼 그 패키지에 건넨다.

- `setup.exe` 는 Windows Installer 의 진행 창과 마지막 안내를 보인다. `/passive` 는 진행만, `/quiet` 는 아무것도
  보이지 않는다. `/uninstall` 은 패키지들을 뒤에서부터 지운다. `/log <파일>` 은 자세한 기록을 쓴다. 종료 코드:
  0, 3010(다시 시작 필요), 실패한 패키지의 오류, 설치 프로그램이 손상되면 1620, 사용자가 권한 올리기를 거절하면 1602.
- `elevate = true`(기본)면 설치 프로그램이 모든 패키지를 위해 관리자 권한을 한 번 묻는다. 사용자별 체인은
  `elevate = false` 로 한다.
- 체인에는 `[chain]`, `[chain-package.ID]`, `[define]` 만 들어간다. 패키지마다 먼저 자기 원본으로 빌드한다.
  `--key` 는 여느 프로그램처럼 설치 프로그램에 서명한다. `inspect setup.exe` 는 패키지 목록을, `extract setup.exe
  -d 폴더` 는 패키지들을 꺼내 준다.

### 병합 모듈: `[merge.ID]`

병합 모듈(`.msm`)은 다른 회사가 자기 런타임이나 라이브러리를 위해 내놓는 설치 프로그램 조각이다.
`[merge.ID]` 는 그 표들을 패키지에 옮겨 담는다: 모듈의 파일, 구성 요소, 레지스트리 값, 모듈 자신의
동작. 모듈의 뿌리 폴더는 `dir` 이 되고, 구성 요소는 `feature`(기본: 그 폴더의 기능, 없으면 `Main`)에
들어가며, 모듈의 캐비닛은 따로 두 번째 캐비닛으로 들어간다. 모듈의 표가 필요로 하는데 패키지에
없는 표준 동작(예를 들어 `WriteRegistryValues`)은 늘 쓰는 자리에 더한다. UTF-8 이 아닌 코드 페이지에
ASCII 밖의 글자가 있는 모듈은 합치지 않는다(`RP1517`). MSIX 에서 모듈은 파일(`dir` 아래, 또는 시스템 폴더 같은
표준 폴더로 가는 것은 가상 파일 시스템 안)과 레지스트리 값(패키지의 하이브 안)을 준다. 그 이상 - 사용자 지정 동작,
구성 요소의 조건, 환경 변수나 INI 항목, 서비스, 글꼴, COM 등록 - 이 필요한 모듈이나 설치 때 채우는 값은 거기서
거절한다(`RP1517`. `msi-only = true` 는 MSI 에만 둔다).

합친 모듈의 `ModuleSignature` 와 `ModuleComponents` 행은 Microsoft 의 병합 도구처럼 패키지에 남는다. 모듈이
`ModuleIgnoreTable` 에 적은 표는 들어가지 않는다.

*설정할 수 있는* 모듈(`ModuleConfiguration` 표가 있는 것)은 값 - 글, 수, 비트 묶음, 또는 모듈의 폴더 같은
키 - 을 묻고, `config` 가 항목마다 `"이름=값"` 문자열 하나로 준다:

```toml
[merge.Runtime]
source = "runtime.msm"
dir = "INSTALLDIR"
config = ["ServerName=example.com", "Port=8080", "DataDir=DataFolder.6E0A1C52_8F3B_4B7D_9A21_3C4D5E6F7A99"]
```

주지 않은 항목은 병합 도구가 답하지 않을 때처럼 모듈의 기본값을 쓴다. 값은 모듈의 `ModuleSubstitution` 표가
말하는 자리에, Microsoft 의 병합 도구가 넣는 방식대로 들어간다: 비트 묶음 항목은 마스크의 비트만 바꾸고, 키
항목은 `[=Item;2]` 로 키의 한 부분만 쓸 수 있고, 널 GUID 는 기능의 이름이 되며, 답을 받은 `KeyNoOrphan` 항목이
모두 대신한 기본 행은 빠진다. 키 값은 모듈 자신의 키(GUID 포함)로, 모듈의 이스케이프 꼴(세미콜론은 `\;`)로
쓴다. 모르는 항목, 수가 아닌 수 항목, 모듈이 값을 요구하는데 빈 값은 거절하고(`RP1517`), 설정을 받지 않는
모듈에 준 `config` 도 그렇다. 항목의 이름, 종류, 기본값은 모듈의 `ModuleConfiguration` 표에 있다:
`rubrapack inspect runtime.msm ModuleConfiguration`.

### 병합 모듈 만들기: `[module]`

`[package]` 대신 `[module]` 이 있는 원본은 남이 합칠 병합 모듈을 만든다:
`rubrapack build runtime.toml -o runtime.msm`.

```toml
format = 1

[module]
name = "ExampleRuntime"
manufacturer = "Example"
version = "2.1.0"
arch = "x64"
id = "{6E0A1C52-8F3B-4B7D-9A21-3C4D5E6F7A99}"     # 모듈 자신의 GUID, 모든 판에서 그대로

[dir.RuntimeDir]
path = "$(TARGETDIR)/Example Runtime"             # TARGETDIR: 패키지가 모듈을 두는 곳

[files.Runtime]
dir = "RuntimeDir"
glob = "runtime/*"

[registry.Home]
root = "HKLM"
key = 'SOFTWARE\Example\Runtime'
name = "Home"
value = '$(RuntimeDir)'                           # 패키지가 정해 준 폴더
```

- 모듈에는 `[dir.*]`, `[file.*]`, `[files.*]`, `[folder.*]`, `[registry.*]`, `[env.*]`, `[ini.*]`,
  `[remove.*]`, `[copy.*]`, `[define]` 이 든다. 기능, 대화창, 동작, 서비스 등은 모듈을 합치는 패키지의 몫이다
  (`RP1201`). qword 레지스트리 값과 `guard` 는 rubrapack 의 도우미 DLL 이 필요한데 모듈은 그것을 담지 않는다
  (`RP1316`).
- 경로는 모듈의 뿌리 `$(TARGETDIR)` - 합치는 패키지가 옮긴다(`[merge.ID] dir`) - 이나 Windows 폴더(`$(System)`
  등)에서 시작한다.
- 모듈이 정한 키는 모두 `.<GUID>`(`id`, `-` 는 `_`)로 끝나 패키지의 키와 부딪히지 않는다 - `[dir.RuntimeDir]` 는
  `RuntimeDir.6E0A1C52_8F3B_...` 가 된다 - 그 키를 가리키는 것도 그렇고, 값 안의 `$(RuntimeDir)` 도 그렇다. 그래서
  모듈 안의 ID 는 35자 이내다(`RP1301`). 구성 요소 GUID 는 패키지가 업그레이드 코드에서 끌어내듯 `id` 에서
  끌어낸다.
- 모듈에는 `ModuleSignature`(`name.<GUID>`, 언어, 판), `ModuleComponents`, 표가 필요로 하는 표준 동작을 담은
  `ModuleInstallExecuteSequence`, 빈 `FeatureComponents` 와 `InstallExecuteSequence` 표(ICEM04)가 있고, 파일은
  키 이름으로 `MergeModule.CABinet` 에 든다.
- Microsoft 의 도구로 확인했다: 모듈 ICE(`mergemod.cub`)는 아무것도 보고하지 않고, 병합 도구(`mergemod.dll`)는
  오류 없이 `[merge.ID]` 와 같은 표로 합치며, 합친 패키지가 설치된다.

### 검색과 요구: `[search.ID]`, `[require.ID]`

검색은 무엇보다 먼저 실행되고 찾은 것을 공개 속성에 넣는다(못 찾으면 비어 있다): 레지스트리 값의 데이터,
파일의 전체 경로(`path` = 알려진 폴더와 상대 경로, 예 `$(System)` 이나 `$(ProgramFiles)/Example`; 프로그램
파일은 `min-version`), 폴더, 또는 다른 제품 구성 요소의 키 파일. 요구 사항은 `condition` 이 거짓이면
그 메시지로 첫 설치를 멈춘다(복구와 제거는 막지 않는다). 조건은 Windows Installer 의 문법을 쓰고
(`VersionNT >= 603`, `FOUND_TOOL`, `NOT OLDSETTING`) 검색 결과를 볼 수 있다.

```toml
[search.Tool]
property = "FOUND_TOOL"
kind = "file"
path = "$(System)"
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
폴더(`SystemRoot`, `System`, `Fonts`, `ProgramData`)는 `scope = "machine"` 이 필요하다.

### 캐비닛

파일은 패키지에 넣는 캐비닛 하나로 MSZIP(deflate)이나 LZX(`compress = "lzx"`: 대개 5~20% 더 작고,
MSZIP 이 처리기를 모두 쓰는 데 비해 처리기 하나로 초당 4 MB 쯤)로 압축한다. `cab-max-size = N` 은 파일이 N MiB 를 넘을 때마다 새
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

"설치된 앱"의 제거는 `msiexec /qb /x {ProductCode}` 를 돌린다: 패키지의 대화창이 아니라 Windows Installer 자신의
축소 창이고, 창이 있는 프로그램이 파일을 쥐고 있으면 자신의 사용 중인 파일 상자 - 영어이고 '취소'가 기본이다('무시'면
계속된다) - 를 띄운다. `no-remove = true` 는 그 제품의 제거를 끈다. 남는 수정은 패키지의 대화창을 돌리고(`msiexec /i`),
거기서 제거를 고르면 패키지 자신의 사용 중인 파일 창('계속'이 기본)으로 간다. 거의 모든 프로그램이 쥐는 입력기나 셸
확장에 쓸 만하다. `[package] ui` 가 `minimal`, `installdir`, `features`(제거 페이지가 있는 세트)여야 하고 `no-modify`
는 없어야 한다.

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

모든 세트에는 취소 확인, 오류 대화창, 사용 중인 파일 목록, 디스크 공간 부족 경고도 있다. 사용 중인 파일
목록은 창이 있는 프로그램이 바꾸거나 지울 파일을 쥐고 있을 때 나온다 - 입력기나 셸 확장이면 거의 모든
프로그램이다. 기본 단추는 '계속'이다: 파일은 곧바로 바뀌고, 이미 열린 프로그램은 다시 열 때까지 이전 것을
쓰며, `reboot = "suppress"`(기본)이면 다시 시작을 묻지 않는다(`FilesInUseText`; `reboot = "allow"` 면
Windows 가 다시 시작을 물을 수 있다고 적는 `FilesInUseTextRestart`). 대화창은
모두 기본값이 있는 값만 모으므로 `/qn` 은 여전히 창 없이 설치한다.

```toml
format = 1

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
`FilesInUseText`, `FilesInUseTextRestart`, `Continue`, `OutOfDiskTitle`, `OutOfDiskText`, `MaintTitle`, `MaintText`, `Repair`,
`RepairText`, `Remove`, `RemoveText`, `LanguageTitle`, `LanguageText`, `DirGuardText`(아래 가드의 메시지),
`PreflightAsk`, `PreflightSilent`, `PreflightFolder`, `PreflightCache`(사전 점검의 메시지), 그리고
제거할 때 - 유지보수 페이지의 제거, 또는 `REMOVE=ALL` - 쓰는 `RemovalProgressTitle`, `RemovalExitTitle`,
`RemovalExitText`, `RemovalUserExitTitle`, `RemovalUserExitText`, `RemovalFatalTitle`, `RemovalFatalText`(이것을
주지 않는 다른 언어는 설치 문구를 쓴다).
단추 문구에서 `&` 는 바로 가기 키를
표시한다(`&Next` 는 Alt+N).

### 여러 언어

대화창은 영어다. `[ui] languages` 는 같은 패키지에 다른 언어를 덧붙인다. 그러면 첫 페이지가 언어를 묻고,
그 뒤의 모든 페이지 - 환영, 사용권, 폴더, 사용자 페이지, 준비, 진행, 완료, 그리고 취소·오류·사용 중
파일·디스크 공간·유지보수 페이지 - 가 고른 언어로 나온다. 미리 골라 두는 언어는 사용자의 지역 형식
(`UserLanguageID`), 그다음 시스템 로캘(`SystemLanguageID`)이 LANGID 에 드는 첫 덧붙인 언어이고, 없으면
영어다. 명령줄의 `RPLANGUAGE=ko` 는 그 선택을 대신 정한다(언어 페이지는 그대로 나오고 그것이 골라져 있다). 무인 설치(`/qn`)는 아무것도 보여 주지 않으니 고를 것도
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
- 한 언어의 대화창 글꼴은 `font-xx` 가 있으면 그것이다(영어는 `font-en`). 없으면 그 언어에 흔한 글꼴 -
  `ko` 는 맑은 고딕, `ja` 는 Yu Gothic UI, `zh` 는 Microsoft YaHei UI, `th` 는 Leelawadee UI - 이고, 그 밖의
  언어는 모두 Segoe UI 다. 그 언어의 라이선스 글(`.txt`·`.md` 인 `license-xx`)도 같은 글꼴로 보인다. 라이선스의
  RTF 에는 글꼴의 영어 이름이 필요한데, rubrapack 은 한국어·일본어·중국어 Windows 에 딸린 글꼴과 나눔 글꼴의 영어
  이름을 안다(`나눔고딕` 은 NanumGothic). 그 밖의 현지 이름이면 라이선스 글은 그 언어에 흔한 글꼴로 보이고 경고
  (`RP1319`)가 난다: 영어 이름으로 쓰면 된다. 글꼴은 Windows 가 늘어놓는 이름으로 1~31자로 쓴다(`RP1308`). 사용자
  컴퓨터에 없는 글꼴은 Windows 가 비슷한 것으로 바꾼다.
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
- 완료·취소·실패 페이지에는 **로그 저장...** 이 있어, 이번 실행의 로그 사본을 사용자가 고른 곳에 저장한다
  (rubrapack 도우미 DLL 의 `RpSaveLog`). 패키지는 `MsiLogging` 을 두어 `/l` 이 없어도 Windows Installer 가 사용자의
  임시 폴더에 로그를 남기게 하고, 로그가 없으면 단추를 숨긴다. `[ui] save-log = false` 는 단추와 로그 남기기와
  도우미를 모두 뺀다.
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

### 진행하기 전에: 사전 점검

설치·업그레이드·제거가 무엇이든 바꾸기 전에 패키지가 살핀다:

- **제품의 파일을 쓰고 있는 프로그램** - 실행 중인 제품 자신의 프로그램과, 제품의 DLL 을 싣고 있는 다른 프로그램(입력기나
  셸 확장이면 열려 있는 거의 모든 프로그램). 몇 개인지와 이름을 보이고 모두 종료할지 묻는다. **예**는 창들에 닫기를 청하고
  몇 초 뒤 남은 것을 끝낸다(그 안의 저장하지 않은 작업은 잃을 수 있다). **아니요**는 종료하지 않고 계속한다 - 파일은
  어차피 바뀌고, 그 프로그램들은 다시 열 때까지 이전 것을 쓴다. **취소**는 아무것도 바꾸지 않고 그만둔다.
- **물어볼 창이 없으면**(`/qn`) 오류(1603; 로그에 프로그램 이름)로 멈춘다 - 명령줄이 할 일을 말해 주지 않는 한:
  `RPCLOSE=yes` 는 모두 종료하고 계속하고, `RPCLOSE=no` 는 종료하지 않고 계속한다.
- 설치와 업그레이드에서: **이미 있는 패키지 폴더**가 설치 프로그램이 그 안의 파일을 지울 수 있는 폴더이고, **옛 판의 캐시된
  패키지**가 남아 있는지(없으면 업그레이드가 그 판을 지울 수 없다). 아니면 무엇이 잘못됐는지와 제품을 먼저 제거하라는 말을
  띄우고 아무것도 설치하지 않는다. 제거는 이 때문에 거절되지 않는다.

`close-programs = "always"` 는 묻지 않고 종료하고, `"never"` 는 종료하지도 묻지도 않는다(`/qn` 에서도). 기본은 `"ask"`
다. 입력기나 셸 확장에는 보통 `"never"` 를 쓴다: 열려 있는 거의 모든 프로그램이 그 DLL 을 싣고 있어 `"ask"` 면 업그레이드와
제거 때마다 질문이 나오고 조용한 실행은 매번 멈추는데, 파일은 아무것도 종료하지 않고도 안전하게 바뀐다(옛 사본은 정리 작업이
치운다). 명령줄의 `RPCLOSE=yes` 나 `RPCLOSE=no` 는 패키지의 설정보다 앞선다. `preflight = false` 는 이 단계를 통째로 뺀다. 문구는 `PreflightAsk`, `PreflightSilent`, `PreflightFolder`,
`PreflightCache` 다(`[ui-text.*]`; 영어와 한국어는 내장이고, 이것을 주지 않는 다른 언어는 영어를 쓴다).

Windows Installer 가 스스로 돌보는 것이 둘 있다: 다른 설치가 도는 동안 두 번째 설치는 곧바로 1618 로 끝나고, 중간에 끊긴
설치(충돌, 정전) 뒤의 다음 설치는 끝나지 않은 것을 먼저 되돌린다. 끊긴 제거 뒤에는 그 되돌리기가 제품을 도로 놓고 제거는
1605 로 끝난다: 한 번 더 제거하면 된다.

### 나중에 정리하기: 정리 작업

실행 중인 프로그램이 쥐고 있는 파일은 곧바로 지울 수 없을 때가 있다. Windows Installer 는 옮길 수 있는 것은
옆으로 옮기고 나머지는 다음 재시작 때 지우도록 걸어 두는데, 입력기나 셸 확장이 있으면 컴퓨터가 몇 주씩 재시작하지
않을 수 있다. 그래서 파일을 지우거나 바꾸는 설치(제거, 업그레이드, 복구)는 예약 작업
`rubrapack cleanup {ProductCode}`(사용자별이면 뒤에 사용자의 SID)를 건다. 작업은 2분 뒤, 그다음엔 로그온 때마다와
15분마다 돌아서, 이 설치가 재시작 때로 미룬 것 - 패키지 폴더의 파일과 그 파일의 `Config.Msi` 백업 사본 - 을 아무도
쥐지 않게 되는 대로 지우고, 그 때문에만 남았던 패키지 폴더를 지운 뒤 자신도 지운다. 할 일이 없으면 첫 실행에서
사라지고, 30일이 지나면 그만두고 나머지는 재시작에 맡긴다. 컴퓨터별 패키지는 SYSTEM 으로, 사용자별 패키지는 그
사용자로(권한 상승 없이) 돌고, 그 계정만 쓸 수 있는 폴더(`%ProgramData%\rubrapack\cleanup\{ProductCode}`, 또는
사용자의 `%LOCALAPPDATA%`)에서 돈다. 컴퓨터별의 경우 그 자리의 `rubrapack` 폴더를 다른 누가 먼저 만들어 두었으면 쓰지
않고 작업도 걸지 않는다. 다른 것은 지우지 않는다: 이 설치가 걸어 둔 것만, 같은 파일이 아직 그 자리에 있을 때만,
그리고 설치된 제품이 지금 그 자리에 가진 파일은 지우지 않는다. `[package] cleanup = false` 면 넣지 않는다.

컴퓨터별 패키지는 앞선 제거로부터 자기 파일도 지킨다: 다른 제품을 지울 때 프로그램이 그 파일을 쥐고 있었으면 그
파일의 삭제가 재시작 때로 걸려 남는데, 이 패키지가 같은 자리에 똑같은 파일을 설치하면 Windows Installer 는 이미 있는
파일을 그대로 둔다 - 그러면 재시작이 그 파일을 지운다. 패키지는 설치하면서 그런 삭제를 거둬들인다(설치가 실패하면
다시 걸어 둔다).

따로 제거 프로그램은 없다: Windows 의 "설치된 앱"은 MSI 를 Windows Installer 로 지우고, 제거가 남긴 것은
정리 작업이 맡는다.

### 제품과 함께 지워지는 추가 기능: `parent`, `remove-addons`

추가 기능은 다른 제품을 넓히는 따로 된 패키지다 - 언어 팩, 플러그인 - 그 제품 없이는 쓸모가 없다. 추가 기능은
본체 제품을 업그레이드 코드로 적는다: `[package] parent = "{...}"`. 설치하면 자기 제품 코드를
`SOFTWARE\rubrapack\Addons\<그 업그레이드 코드>` 에 적고(컴퓨터별은 HKLM, 사용자별은 HKCU), 지우면 그 값도
지운다. 본체는 `remove-addons = true` 로 둔다: 본체를 정말로 지울 때(업그레이드가 바꿔 넣을 때가 아니라) 곧바로
도는 정리 작업이 제거가 끝나기를 기다렸다가, 거기 적힌 추가 기능 가운데 아직 설치된 것을
`msiexec /x {ProductCode} /qn` 으로 지운다 - 본체를 어느 길로 지웠든(설치된 앱, 관리 화면, `msiexec /x`).
그 순간 지울 수 없는 추가 기능(다른 설치가 도는 중)은 작업의 다음 실행에서 다시 해 본다. Windows Installer 는 한
번에 두 패키지를 지울 수 없으므로 추가 기능은 본체보다 먼저가 아니라 몇 초 뒤에 지워진다 - 본체 없이도 지워지게
만든다. 이 키로 구운 본체의 첫 판부터 효과가 있고, 정리 작업이 있어야 한다(`cleanup = false` 와 함께면 오류).
두 키 모두 MSI 패키지에만 쓴다.

### 다른 제품을 대신하기: `replaces`

`replaces = ["{UpgradeCode}", ...]`(16개까지)는 이 패키지가 대신하는 제품들이다 - 이를테면 따로 된 패키지였다가 이제
이 패키지의 기능이 된 추가 기능. 이 패키지를 설치하면 그 제품들의 설치된 판을 모두 같은 실행에서 지운다 - 자기
옛 판을 지우듯이(RemoveExistingProducts; 그들 자신의 제거가 `UPGRADINGPRODUCTCODE` 와 함께 돈다). 그래서 같은
파일의 주인이 둘이 되지 않는다. 이 패키지의 업그레이드 코드나 `parent` 는 쓸 수 없다. MSI 에만 쓴다.

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
- `default-when = "<조건>"`(`level` 이 1보다 클 때): 기본으로는 꺼져 있지만, 처음 설치할 때 조건이 맞으면 기본으로
  켜진다 - 이를테면 검색으로 이 부분이 전에 따로 된 패키지로 설치되어 있었음을 알았을 때(`replaces` 참고). 그러면
  창 없는 업그레이드도 그것을 남긴다.
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

문자열에 이름을 넣는 방법은 `$(NAME)` 하나뿐이고, `$$` 는 `$` 한 글자다. 이름은 한 번만 바뀌고 그 결과는
다시 읽지 않는다(`$(X)` 가 든 값은 그대로 남는다). 이름은 다음 가운데 하나다.

- **빌드 변수**, 어디서나: 명령줄의 `-D NAME=value` 가 먼저, 그다음 `[define]`. `$(ARCH)` 는 `-D` 나
  `[define]` 이 정의하지 않는 한 내장이다: 짓고 있는 아키텍처(`x64`, `arm64`, `x86`) - 그래서 원본 하나가
  `source = "bin/$(ARCH)/app.exe"` 처럼 아키텍처마다 제 프로그램을 가리킬 수 있다. 정의되지 않은 이름은
  오류다(`RP1403`).
- **dir ID** 나 **Windows 이름**, 경로의 맨 앞([경로](#경로))과 Windows Installer 가 설치하는 동안 채우는
  값에서: `[registry.*]`·`[env.*]`·`[ini.*]` 의 값, 바로 가기·서비스·파일 형식·링크의 `args`,
  `[action.*]` 의 `do`/`undo`/`check`, `[require.*]` 의 `message`, `[ui]` 의 `launch-args`. 그 밖의 자리에서는
  오류다(`RP1404`).

빌드 변수에는 Windows 이름이나 dir ID 를 붙일 수 없다(`RP1404`).

### Windows 이름

[Windows 폴더와 환경 변수](basics/09-folders-and-environment-variables.md)의 이름이고, Windows 의 철자로
쓴다. 대소문자는 가리지 않는다(`$(appdata)` 는 `$(APPDATA)`). 값 안에서는 Windows Installer 가 설치할 때
설치하는 사용자의 것으로 채우는 것이 된다 - `type = "expand"` 레지스트리 값에서는, 읽는 프로그램이 실행할 때
실행하는 사용자의 것으로 푸는 환경 변수가 된다:

| 이름 | 경로 기준 | 설치할 때(64비트 / 32비트 패키지) | 실행할 때(`type = "expand"`) |
|---|---|---|---|
| `ProgramFiles` | 예 | `[ProgramFiles64Folder]` / `[ProgramFilesFolder]` | `%ProgramFiles%` |
| `ProgramFiles(x86)` | 예 | `[ProgramFilesFolder]` | `%ProgramFiles(x86)%` |
| `ProgramW6432` | - | `[ProgramFiles64Folder]` / `[%ProgramW6432]` | `%ProgramW6432%` |
| `CommonProgramFiles` | 예 | `[CommonFiles64Folder]` / `[CommonFilesFolder]` | `%CommonProgramFiles%` |
| `CommonProgramFiles(x86)` | - | `[CommonFilesFolder]` | `%CommonProgramFiles(x86)%` |
| `CommonProgramW6432` | - | `[CommonFiles64Folder]` / `[%CommonProgramW6432]` | `%CommonProgramW6432%` |
| `ProgramData`, `ALLUSERSPROFILE` | 예 | `[CommonAppDataFolder]` | `%ProgramData%` ... |
| `APPDATA` | 예 | `[AppDataFolder]` | `%APPDATA%` |
| `LOCALAPPDATA` | 예 | `[LocalAppDataFolder]` | `%LOCALAPPDATA%` |
| `TEMP`, `TMP` | 예 | `[TempFolder]` | `%TEMP%` ... |
| `SystemRoot`, `windir` | 예 | `[WindowsFolder]` | `%SystemRoot%` ... |
| `System` | 예 | `[System64Folder]` / `[SystemFolder]` | - |
| `Fonts`, `Desktop`, `StartMenu`, `Programs`, `Startup` | 예 | `[FontsFolder]`, `[DesktopFolder]`, `[StartMenuFolder]`, `[ProgramMenuFolder]`, `[StartupFolder]` | - |
| `USERNAME` | - | `[LogonUser]` | `%USERNAME%` |
| `COMPUTERNAME` | - | `[ComputerName]` | `%COMPUTERNAME%` |
| `SystemDrive`, `USERPROFILE`, `PUBLIC`, `HOMEDRIVE`, `HOMEPATH`, `USERDOMAIN`, `LOGONSERVER`, `ComSpec`, `Path`, `PATHEXT`, `OS`, `PROCESSOR_ARCHITECTURE`, `NUMBER_OF_PROCESSORS` | - | `[%NAME]` | `%NAME%` |

값 안의 dir ID 는 `[ID]`, 곧 설치할 때의 그 폴더가 된다. 폴더의 Windows Installer 값은 `\` 로 끝나므로 폴더
이름 바로 뒤의 `\` 하나는 빠진다: `'$(INSTALLDIR)\app.exe'` 는 `[INSTALLDIR]app.exe` 가 된다. 환경 변수가 없는
폴더는 `type = "expand"` 값에 쓸 수 없다(`RP1404`). MSIX 에는 설치할 때가 없다: 그것이 필요한 값은 거기서
거부하고(`RP1612`, `RP1613`), 펼칠 수 있는 값 안의 `%NAME%` 은 그대로 쓸 수 있다.

있는 그대로 쓴 `[NAME]` 과 `%NAME%` 은 바뀌지 않고 지나간다. `$(...)` 이름이 없는 것을 위해서다:
`[#FileID]`, `[ProductVersion]`, 글자 `[` 를 뜻하는 `[\[]`, 파일 형식 인수의 `%1`.

### 프로그램 파일

PE 파일(`.exe`, `.dll` 등)은 검사한다: 머신 형식이 `arch` 와 맞아야 하고(x64 패키지의 x86 도우미는
`any-arch = true` 가 필요하다), 판 정보 리소스가 패키지 안 파일의 판이 된다. Windows Installer 는 그것을
보고 설치된 파일을 바꿀지 정한다.

### 설치 폴더 지키기

다른 프로그램이 그 파일을 불러 쓰는 프로그램 - 입력기, 셸 확장, 서비스 - 은 남이 미리 만들어 둔 폴더에
설치되면 안 된다: 거기 있던 파일은 그대로 남고, 프로그램 옆에 심어 둔 DLL 이 그 프로그램의 권한으로 실리기
때문이다. dir 에 `guard = true` 를 두면, 그 폴더가 다음과 같을 때 첫 설치가 파일을 하나도 놓기 전에 멈춘다.

- 이미 있고 주인이 SYSTEM, Administrators, TrustedInstaller 가 아니다.
- 아직 없는데, 컴퓨터별 설치에서 그 위에 실제로 있는 가장 깊은 폴더의 주인이 그 셋이 아니다(남의 폴더 안에 만든 폴더는
  그 폴더의 권한을 물려받고, 그 주인은 다른 폴더를 그 자리에 놓을 수 있다).
- 연결 지점(junction)이나 다른 리파스 포인트를 거쳐 닿는다(폴더 자신이나 그 위의 어느 폴더든).

믿을 수 있는 주인의 폴더(예: `Program Files`) 아래의 아직 없는 폴더는 통과한다(설치 엔진이 만들고, `[permission.*]` 로
잠글 수 있다). 복구, 제자리 업그레이드,
제거는 검사하지 않는다. 메이저 업그레이드는 새 판의 첫 설치라 똑같이 검사한다(이전 판이 만든 폴더는
통과한다). 설치는 `DirGuardText` 메시지(`[1]` 은 그 폴더; 대화창에서 고른 언어) 뒤에 1603 으로 끝나고,
`/qn` 에서도 같으며, 로그에 찾은 주인이 적힌다. 검사는 rubrapack 의 도우미 DLL 이 하고, 패키지가 그 DLL 을
싣는다(`REG_QWORD` 값과 같다). 보는 것은 주인이다: 관리자 소유라도 권한이 누구나 쓸 수 있게 되어 있는
폴더는 거부하지 않는다(`[permission.*]` 로 잠근다).

```toml
[dir.INSTALLDIR]
path = "$(ProgramFiles)/Example"
guard = true
```

### 경로

설치될 경로 - `[dir.*]` 와 `[search.*]` 의 `path` - 는 폴더로 시작한다: dir ID 나 경로 기준이 되는 Windows
이름을 `"$(ProgramFiles)/Example"`, `"$(INSTALLDIR)/docs"` 처럼 쓰고, 그다음 `/` 와 그 아래 폴더들을 쓴다. 뒤에는
빌드 변수가 와도 된다(`"$(INSTALLDIR)/v$(VERSION)"`).

원본 경로는 원본 파일 기준의 상대 경로이고 `/` 를 쓴다. 절대 경로, `\`, 심볼릭 링크, 없는 파일은
오류다. 설치될 이름에는 Windows 가 금하는 것(`< > : " / \ | ? *`, 제어 문자, 끝의 점이나 공백,
`CON` 같은 장치 이름)만 빼고 어떤 유니코드 글자든 쓸 수 있고, 한 폴더 안의 두 이름이 대소문자만
달라서는 안 된다. Windows Installer 는 이름을 8.3 짧은 이름과 함께 UTF-16 255단위에 저장하므로, 긴
이름은 짧은 이름이 남긴 만큼만 쓸 수 있다: 세 글자 확장자의 `name.ext` 는 242단위, 확장자 없는 이름은
246단위(NTFS 는 255 를 받는다). 더 긴 이름은 RP1514 로 거부한다.

## 명령줄

```text
rubrapack build <src.toml> -o <out.msi|out.msm|out.msix|out.msixbundle> [-D NAME=VALUE]...
                [--arch x64|arm64|x86 | --arch <목록> (.msixbundle)]
                [--compress none|mszip|mszip:N|lzx|lzx:N] [--jobs N] [--nfc] [--reproducible]
                [<키> [--cert <chain.pem>]
                 [--timestamp <URL> [--tsa-trust <인증서>] [--tls-trust <인증서>] [--system-roots]
                  [--proxy <URL>]] [--allow-unsigned-cabs]]
                [--unsigned-test] [--msix-compress deflate|store]      (.msix, .msixbundle)
rubrapack inspect <file.msi> [table | --summary | --files | --streams]
rubrapack inspect <file.msi> [table | --summary] --json
rubrapack inspect <file.msix|file.msixbundle> [--files | --manifest]
rubrapack inspect <file.cab>
rubrapack new [msi] <name>
rubrapack new [<file>.toml] [-i]
rubrapack new <name> [--dist <folder>] [--name ...] [--ui ...] [--optional ...] ...
rubrapack edit <file>.toml [--set <table>.<key>=<value>]... [--unset <table>.<key>]... [--sync]
rubrapack guid [--from <text>]
rubrapack lint <src.toml> [-D NAME=VALUE]... [--arch x64|arm64|x86] [--target msi|msix] [--nfc] [--strict] [--json]
rubrapack lint <file.msi> [--previous <old.msi>] [--strict] [--json]
rubrapack explain [RPnnnn]
rubrapack schema
rubrapack lint <file.msix|file.msixbundle> [--strict]
rubrapack extract <file.msi|file.msix|file.msixbundle|file.cab> -d <새 폴더> [--limit-entries N] [--limit-bytes N]
rubrapack sign <file.exe|.dll|.msi|.msp|.msix|.msixbundle> <키> [--cert <chain.pem>]
               [--timestamp <URL> [--tsa-trust <인증서>] [--tls-trust <인증서>] [--system-roots]
                [--proxy <URL>]] [--allow-unsigned-cabs] [-o <out>]
rubrapack keys list [--pkcs11 <모듈> [--token-label <이름>] [--pin-env VAR | --pin-file FILE]]
rubrapack verify <file.exe|.dll|.msi|.msp|.msix|.msixbundle> [--trust <인증서>]... [--system-roots] [--tsa-trust <인증서>]...
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
- `new <name>` 은 `<name>.toml` 을 쓴다. 프로그램 파일을 `dist/` 에 넣기만 하면 빌드되는 원본이고
  `upgrade-code` 는 새로 만든다. `new <file>.toml`, 이름 없는 `new`(파일 이름도 묻는다), `new <name> -i` 는 대신 [첫 패키지](#첫-패키지)의 질문을 한다(질문은
  표준 오류로, 답은 표준 입력에서 한 줄씩 받으므로 파이프로 줄 수 있다; 마지막 답 전에 입력이 끝나면 아무것도
  쓰지 않는다). `new <name>` 에 옵션을 주면 묻지 않고 같은 답을 받는다: `--name`, `--manufacturer`, `--version`,
  `--dist`(기본 `dist`), `--main <파일>|-`, `--arch`, `--install-dir`, `--scope`, `--optional <폴더,...>|-`, `--ui`,
  `--license <파일>|-`, `--languages ko|-`, `--shortcuts start,desktop|none`; 빠진 것은 질문이 권했을 기본값을 쓴다.
  원본은 폴더 최상위 파일을 하나씩 적고(바로가기는 파일을 가리키고, 글롭은 파일을 뺄 수 없다) 파일이 든 하위
  폴더는 글롭으로 적는다. 선택 하위 폴더는 필수 `Main` 옆에 `level = 2` 기능이 된다. 쓴 뒤에는 `lint` 처럼
  검사한다(문제가 있으면 종료 코드 1). 있는 파일은 절대 바꾸지 않는다. 이미 있는 `app.toml` 에 `new app.toml` 을
  주면 `edit` 를 쓰라고 알린다.
- `rubrapack new <파일>.toml --from <패키지.msi>` 는 이미 있는 패키지로 원본을 만든다(RFC-0023). 파일은 원본 옆
  새 폴더(`<이름>-files`, 또는 `--dist <이름>`)에 `extract` 와 같은 배치로 풀리고, 원본은 그 파일로 패키지를
  설명한다 - 이름, 판, 아키텍처, 업그레이드 코드, 폴더, 파일(파일이 키 경로인 구성 요소의 코드까지 옮겨 결과물이
  원래 패키지를 업그레이드한다. 제품 코드는 새 판마다 달라야 하므로 옮기지 않고 주석에 적는다), 기능, 레지스트리 값, 바로가기, 환경 변수, INI 값, 서비스, 공개 속성,
  실행 조건, 복사, 제거, 글꼴, 권한, "설치된 앱" 설정, 그리고 rubrapack 이 만든 패키지라면 그 대화창 세트,
  실행 확인란, 동작, qword 값, 폴더 보호까지. 원본이 말할 수 없는 것 - 사용자 정의 동작, 원래 패키지의 대화창,
  검색, COM 과 그 밖의 표 - 은 원본 끝에 표마다 적고, 바꾼 것(드라이브 뿌리에 있던 폴더를 Program Files 아래로,
  컴퓨터별 패키지에서 사용자별 값 제외 등)도 함께 적는다. `--publisher "CN=..."` 를 주면 `[msix]` 와
  `[msix-app.*]`(시작 메뉴 바로가기의 프로그램)도 써서 같은 원본으로 `.msix` 를 빌드할 수 있다. MSIX 가 담지
  못하는 것은 빌드가 거절하며 `msi-only = true` 를 붙이면 된다. 병합 모듈은 거절한다(`[merge.ID]` 로 병합한다).
- `edit <file>.toml` 은 원본을 그 자리에서 고친다. 옵션이 없으면 메뉴를 보인다: 1 이름, 제조사, 판(`version =
  "$(VERSION)"` 이면 `[define] VERSION`), 아키텍처; 2 Program Files 아래 폴더와 설치 범위; 3 대화창, 약관, 한국어
  대화창; 4 선택 구성요소로 할 하위 폴더(처음 켜면 필수 기능 `Main` 을 더하고, 그때부터 기능이 필요한 모든 것에
  준다); 5 시작 메뉴·바탕화면 바로가기와 그것이 여는 프로그램; 6 프로그램 폴더와 파일 목록 맞추기 - 사라진 파일과
  하위 폴더는 빼고 새것은 더한다; 7 아무 `표.키`; `v` 는 글을 보이고, `l` 은 `lint` 처럼 검사하고, `s` 는 저장하고
  검사하며, `q` 는 나간다(바뀐 것이 있으면 먼저 묻는다). 질문마다 지금 값을 권한다. 바꾼 값만 다시 쓰고 주석,
  순서, 손으로 쓴 표는 그대로다. 구문이 깨질 변경은 하지 않는다. 옵션을 주면 묻지 않고 차례로 적용해 저장한다:
  `--set package.version=1.2.0`(TOML 값 - `"글"`, 수, `true`, `["ko"]` - 이면 그대로, 아니면 문자열로 쓰고, 없는
  표는 더한다), `--unset package.license`, `--sync`(묻지 않는 6). UTF-16 원본은 거부한다: 먼저 UTF-8 로 저장한다.
- `explain [RPnnnn]` 은 진단 코드가 무엇에 관한 것인지, 그 코드를 단 메시지, 할 일, 매뉴얼에서 더 볼 곳을
  알려 준다. 코드가 없으면 범위를 나열한다(RFC-0025).
- `lint`(원본, 패키지, `--previous`)에 `--json` 을 주면 표준 출력에 JSON 객체 하나를 낸다 - `file`, `exit`,
  `errors`, `warnings`, `not_shown`, 그리고 `file`, `line`, `column`, `severity`, `code`, `message` 를 가진
  `diagnostics`(파일 전체에 관한 것은 줄과 칸이 0) - 종료 코드는 같다. `inspect <file.msi>` 에서는 모든 표, 한
  표, `--summary` 를 JSON 으로 준다.
- `schema` 는 원본의 JSON Schema(표, 키, 필수 키, 참거짓과 숫자)를 파서의 키 목록에서 만들어 출력한다. 편집기용이며,
  Taplo 기반 편집기에서는 원본 첫 줄에 `#:schema ./rubrapack.schema.json` 을 쓴다.
- `guid` 는 무작위 GUID(4판)를 찍는다. `guid --from <text>` 는 rubrapack 이 글에서 끌어내는 GUID 를
  어느 컴퓨터에서나 같게 찍는다: `guid` 의 32비트 리틀엔디언 길이와 그 4바이트, 32비트 리틀엔디언 수 1,
  글의 32비트 리틀엔디언 길이와 UTF-8 바이트에 대한 SHA-256. 해시의 앞 16바이트에 판 니블을 8 로,
  변종 비트를 `10` 으로 넣어(RFC 9562 UUIDv8) 대문자 `{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}` 로
  쓴다. 예를 들어 `guid --from hello` 는 `{B52FE8EF-B68D-84B5-91AF-E6E01BEC2773}` 이다.
- 진단은 `example.toml:12:3: error[RP1201]: unknown key 'nmae' in [package] (did you mean 'name'?)`
  꼴이다.
- 쓰기 전에 `build` 는 완성된 표를 검사한다(`RP20xx` 진단, 종료 코드 5). 이 검사는 rubrapack 자신을
  지키는 것이라, `RP1xxx` 검사를 통과한 원본은 여기에 걸리지 않아야 한다.
- `lint <src.toml>` 는 `build` 가 하는 검사를 모두 하고 아무것도 쓰지 않는다. 원본을 MSI 기준으로
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
- `transform <base.msi> <target.msi> -o <out.mst>` 는 변환을 쓴다: 바탕 패키지를 대상 패키지로 바꾸는
  행들(더한 것, 바뀐 것, 지운 것, 더하거나 뺀 표)을 msi.dll 의 `MsiDatabaseGenerateTransform` 이 쓰는
  모양대로, 두 패키지를 밝히는 요약 정보와 함께. `--validate` 는 Windows 가 변환을 적용하기 전에 확인할
  것을 정한다: `product-code` 와 `upgrade-code`(기본), `language`, `platform`, 또는 `none`. 거부하는
  것(`RP0013`): 열이 다른 표, 바뀐 `File` 이나 `Media` 표(변환은 파일을 나르지 않는다), 표의 16번째 뒤의
  열이 바뀐 것. `inspect <파일.mst> --base <base.msi>` 는 행을, `inspect <파일.mst> --summary` 는
  요약 정보를 보여 준다.
- `patch <base.msi> <target.msi> -o <out.msp>` 는 설치된 바탕 판을 그 자리에서 대상 판으로 고치는 패치를
  쓴다: 제품 변환(달라진 행; 있던 파일은 바탕 패키지의 캐비닛 속 자리를 지킨다), 패치 변환(새 파일과 바뀐
  파일의 새 자리, 패치의 캐비닛을 가리키는 Media 행, PatchPackage, PatchFiles 동작), 그리고 그 파일들을
  통째로 담은 캐비닛을 Microsoft 의 `MsiMsp.exe` 와 같은 모양으로. 두 패키지는 제품 코드와 업그레이드
  코드가 같고, 판의 앞 두 수가 같고(*작은 업데이트* 나 *마이너 업그레이드*), 캐비닛이 내장이고, 바탕의 파일과
  구성 요소를 모두 가져야 한다. `--patch-code {GUID}`(기본: 두 패키지 코드에서 끌어냄), `--family <이름>`
  (MsiPatchSequence; 기본은 제품 이름), `--no-removal`(패치를 따로 제거할 수 없게). 거부는 `RP0013` 이다.
  `inspect <파일.msp> --base <base.msi>` 는 패치의 표와 두 변환을 보여 준다. `sign` 은 패치에도 패키지처럼
  서명하고(그 변환까지 서명에 든다), `verify` 가 그것을 확인한다.
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

## 진단 코드

rubrapack 이 알리는 모든 문제에는 파일, 줄, 열 다음에 코드가 붙는다: `error[RPnnnn]` 또는 `warning[RPnnnn]`.
앞의 두 자리가 문제의 종류를 말한다:

| 코드 | 무엇이 잘못됐나 | 볼 곳 |
|---|---|---|
| RP00xx | 명령줄: 모르는 명령이나 옵션, 읽거나 쓸 수 없는 파일, 변환이 나를 수 없는 차이(`RP0013`) | `rubrapack help <명령>` |
| RP10xx | 원본 파일의 인코딩: UTF-8(또는 BOM 있는 UTF-16)이 아님, 짝 없는 캐리지 리턴 | 파일을 UTF-8 로 저장한다 |
| RP11xx | rubrapack 이 읽는 부분집합 밖의 TOML: 여러 줄 문자열, 인라인 표, 두 번 정의한 표; `format` 이 없거나 너무 새 것(RP1108) | [TOML 부분집합](#toml-부분집합), [원본 형식](#원본-형식) |
| RP12xx | 표와 키: 모르는 표나 키(제안과 함께), 빠진 필수 키나 표, 기능이 생긴 뒤 기능 없는 항목 | [표](#표) |
| RP13xx | 값: ID(모든 표에 걸쳐 유일, 예약어 아님), GUID, 판, 범위를 벗어난 수, 없는 것을 가리키는 참조 | 그 표의 절 |
| RP14xx | 변수: 값 없는 `$(NAME)`, 닫히지 않은 `$(`, 쓸 수 없는 자리의 Windows 이름이나 dir ID | [변수](#변수) |
| RP15xx | 설치할 파일: 없음, 폴더임, 링크임, 맞는 것 없는 글롭, 다른 아키텍처의 프로그램, 너무 긴 이름, 빌드 중에 바뀐 파일, 너무 큰 패키지 | [경로](#경로), [프로그램 파일](#프로그램-파일) |
| RP16xx | MSIX: MSIX 패키지에 필요한 것, 담을 수 없는 것 | [MSIX 패키지](#msix-패키지) |
| RP19xx | 이 rubrapack 이 제공하지 않는 기능 | - |
| RP20xx | 완성된 표가 Windows Installer 규칙을 어김. RP1xxx 검사를 통과한 원본은 여기에 걸리지 않아야 한다: 알려 주기 바란다 | - |
| RP21xx | 패키지 `lint`: 대화창, 코드 페이지, 글자 정규화 | [명령줄](#명령줄)의 `lint` |
| RP22xx | MSIX 패키지나 묶음의 `lint` | [명령줄](#명령줄)의 `lint` |
| RP23xx | `lint --previous`: 이 패키지가 앞 판을 깨끗하게 업그레이드하지 못함 | [판과 업그레이드](tutorial/03-versions-and-upgrades.md) |

무엇을 고칠지는 메시지가 말한다. 매뉴얼에서 찾을 때는 위의 코드 범위를 쓴다.
