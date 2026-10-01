# 더 많은 파일과 폴더

목표: 프로그램 폴더 전체를 설치한다 - 프로그램, 다른 이름으로 둘 읽어보기, 사용자가 고칠 수 있는 설정 파일,
문서 폴더, 하위 폴더에 든 예제, 데이터를 위한 빈 폴더 - 그리고 프로그램이 쓰는 기록 파일을 치운다.

## 폴더

```text
C:\work\hello\
    dist\
        hello.exe
        readme.txt
        settings.ini
        docs\
            guide.txt
        samples\
            sample1.txt
            sub\
                sample2.txt
    hello.toml
```

## 원본

```toml
# tutorial 04: hello.toml
format = 1

[package]
name = "Hello"
manufacturer = "Example Software"
version = "$(VERSION)"
arch = "x64"
upgrade-code = "{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}"

[define]
VERSION = "1.2.0"

[dir.INSTALLDIR]
path = "$(ProgramFiles)/Hello"

[dir.DocsDir]
path = "$(INSTALLDIR)/docs"

[dir.SamplesDir]
path = "$(INSTALLDIR)/samples"

[file.Hello]
dir = "INSTALLDIR"
source = "dist/hello.exe"

[file.Readme]
dir = "INSTALLDIR"
source = "dist/readme.txt"
name = "Read me.txt"
vital = false

[file.Settings]
dir = "INSTALLDIR"
source = "dist/settings.ini"
keep = true

[files.DocFiles]
dir = "DocsDir"
glob = "dist/docs/**"

[files.SampleFiles]
dir = "SamplesDir"
glob = "dist/samples/**/*.txt"

[folder.Data]
dir = "INSTALLDIR"
name = "data"
keep = true

[remove.Logs]
dir = "INSTALLDIR"
name = "*.log"
on = "uninstall"

[copy.ReadmeInDocs]
source = "file:Readme"
dir = "DocsDir"
name = "readme.txt"
```

빌드하고 파일을 본다:

```text
C:\work\hello> rubrapack build hello.toml -o hello.msi
C:\work\hello> rubrapack inspect hello.msi --files
[ProgramFiles64Folder]\Hello\Read me.txt	8	Readme
[ProgramFiles64Folder]\Hello\docs\guide.txt	6	F_6d1508f52f6615459567
[ProgramFiles64Folder]\Hello\hello.exe	17920	Hello
[ProgramFiles64Folder]\Hello\samples\sample1.txt	7	F_1dc2b5e85973ee91d955
[ProgramFiles64Folder]\Hello\samples\sub\sample2.txt	7	F_c9c5c4da73214a6abf37
[ProgramFiles64Folder]\Hello\settings.ini	23	Settings
```

(실제 출력에는 열이 더 있다. 여기서는 경로, 크기, ID 만 보였다.)

## 폴더: `[dir.ID]`

dir 의 `path` 는 Windows 폴더나 다른 dir 을 `$(...)` 로 써서 시작하고, 그 아래 폴더 이름을 잇는다:

| `path` | 사용자 컴퓨터에서(흔한 경우) |
|---|---|
| `$(ProgramFiles)/Hello` | `C:\Program Files\Hello` (`x86` 패키지는 `C:\Program Files (x86)\Hello`) |
| `$(INSTALLDIR)/docs` | `C:\Program Files\Hello\docs` - 사용자가 어디에 두든 dir `INSTALLDIR` 아래 |
| `$(ProgramData)/Hello` | `C:\ProgramData\Hello` - 모든 사용자가 함께 쓰는 데이터 |
| `$(LOCALAPPDATA)/Hello` | `C:\Users\<이름>\AppData\Local\Hello` |

Windows 폴더는 환경 변수의 이름을 그대로 쓴다(`%ProgramData%`, `%LOCALAPPDATA%`). 기초 지식
[9](../basics/09-folders-and-environment-variables.md)장이 이것들을 설명한다. `$(ProgramFiles)/Hello` 를 되풀이하지
않고 `INSTALLDIR` 위에 쌓는 것은 사용자가 설치 폴더를 고를 수 있게 되면(6장) 중요해진다: 그러면 `docs` 가
사용자가 고른 폴더를 따라간다. 전체 목록은 참조 부의 [Windows 이름](../rpk.md#windows-이름)에 있다.

## 파일 하나: `[file.ID]`

- `name` 은 파일을 다른 이름으로 설치한다 - 여기서는 `readme.txt` 가 `Read me.txt` 가 된다. 이름에는 어느
  언어의 글자든 쓸 수 있다. Windows 가 파일 이름에 금지하는 것(`< > : " / \ | ? *`)은 거부한다.
- `vital = false`: 이 파일을 쓰지 못해도 설치는 그 파일 없이 이어진다. 기본은 모든 파일이 필수다.
- `keep = true`: 제품을 지워도 파일이 남고, 사용자가 한 번 고친 뒤에는 복구나 다음 판이 덮어쓰지 않는다.
  사용자가 고치는 설정에 쓴다.

## 여러 파일: `[files.ID]`

`glob` 은 와일드카드가 든 경로다: `*` 는 폴더 이름 하나 안의 아무 글자들, `?` 는 글자 하나, `**` 는 몇 단계든
폴더다. 맞는 파일이 모두 설치되고, **첫 와일드카드 아래의 폴더는 `dir` 아래에 다시 만들어진다**:

| `glob` | `dist\samples\sub\sample2.txt` 가 가는 곳 |
|---|---|
| `SamplesDir` 로 `dist/samples/**/*.txt` | `...\Hello\samples\sub\sample2.txt` |
| `SamplesDir` 로 `dist/samples/*.txt` | 설치되지 않음: `*` 는 `sub` 안으로 들어가지 않는다 |

맞은 파일은 이름 순으로 정렬되므로, 파일 시스템이 어떤 순서로 보이든 패키지는 같다. 아무것도 맞지 않는 글롭은
빌드를 멈춘다(`RP1503`) - 대개 경로의 오타다. rubrapack 은 맞은 파일마다 ID 를 따로 준다(`F_` 와 경로에서 끌어낸
수).

## 빈 폴더: `[folder.ID]`

`[folder.Data]` 는 파일이 하나도 들어가지 않아도 `INSTALLDIR` 안에 `data` 를 만든다 - 프로그램이 쓸 자리다.
`keep = true` 는 제품을 지울 때 그 폴더(와 프로그램이 거기 쓴 것)를 남긴다.

## 치우기: `[remove.ID]`

프로그램이 스스로 만드는 파일 - 기록, 캐시 - 은 설치 파일의 것이 아니어서 Windows Installer 가 남겨 두고, 그
때문에 폴더도 남는다. `[remove.Logs]` 는 제품을 지울 때(`on = "uninstall"`) `INSTALLDIR` 의 `*.log` 를 지운다.
`on = "install"` 은 설치할 때(옛 판이 남긴 파일), `on = "both"` 는 둘 다에서 지운다. `name` 이 없으면 폴더가
비었을 때 폴더 자체를 지운다. 설치 전부터 있던 폴더는 절대 지우지 않고, 설치가 실패하면 지운 파일을 되살린다.

## 두 번째 사본: `[copy.ID]`

`[copy.ReadmeInDocs]` 는 파일 `Readme` 의 사본을 하나 더 `DocsDir` 에 `readme.txt` 로 설치한다. 사본은 원래 파일과
함께 오고 함께 간다.

## 만날 수 있는 키 두 개 더

- 파일의 `any-arch = true`: rubrapack 은 모든 프로그램 파일(`.exe`, `.dll`)이 패키지의 `arch` 로 만들어졌는지
  확인한다. 실수로 32비트 DLL 을 복사하는 x64 설치 파일은 망가진 프로그램을 설치하기 때문이다. x64 제품 안의
  32비트 도우미처럼 일부러 그런 것이면 `any-arch = true` 로 밝힌다.
- 파일의 `component-guid = "{...}"`: rubrapack 은 모든 컴포넌트의 GUID 를 제품과 파일 자리에서 끌어내므로, 판이
  바뀌어도 같게 유지되고 업그레이드는 그것에 기댄다. 앞선 패키지가 다른 GUID 로 만든 컴포넌트를 이어 갈 때만
  손으로 준다.

## 안에서 무슨 일이 일어났나

Windows Installer 는 *컴포넌트*를 설치한다: 함께 설치되고 함께 지워지는 작은 자원 묶음으로, 저마다 GUID 와 *키
경로*(그것이 있으면 "설치됨"을 뜻하는 것)를 가진다. rubrapack 은 파일마다 컴포넌트 하나를 만드는데 - 안전한
규칙이다 - 필요한 폴더, 지우기, 사본에도 하나씩 더 만든다:

```text
C:\work\hello> rubrapack inspect hello.msi Component
...
C_74a883a037bc227f9189	{51A68DD4-96FA-83C0-86AA-F20D51339238}	INSTALLDIR	272		Settings
C_cec3a9b89b2e391393d0	{615660C7-986E-8FB8-87AF-0616B544A6F9}	Data	272
...
```

속성 열의 `272` 는 `256 + 16` 이다: 256 은 64비트 컴포넌트, 16 은 "영구" - `keep = true` 다. 제4부가 이 표들을
설명한다. `RemoveFile`, `CreateFolder`, `DuplicateFile` 에 지우기, 빈 폴더, 사본이 들어 있다.
