# 검사하고 들여다보기

목표: 사용자보다 먼저 실수를 잡는다 - 원본이나 완성된 패키지를 검사하고, 그 안의 모든 표와 파일과 스트림을
보고, 설치하지 않고 풀어 보고, 서명을 확인한다. 이 명령들은 다른 도구로 만든 패키지에도 쓸 수 있다.

이 장은 17장의 `hello.toml` 과 그것으로 빌드한 `hello.msi` 를 쓴다.

## 원본 검사: `lint`

`lint` 는 `build` 가 하는 모든 검사를 하고, 아무것도 쓰지 않는다:

```text
C:\work\hello> rubrapack lint hello.toml
C:\work\hello> rubrapack lint hello.toml --target msix
C:\work\hello> rubrapack lint hello.toml --arch x86 -D VERSION=2.2.0
```

아무 출력 없이 종료 코드 0 이면 문제가 없다는 뜻이다. `--target msix` 는 원본을 MSI 대신 MSIX 기준으로
검사하고, `--arch` 와 `-D` 는 같은 옵션을 준 `build` 가 빌드할 모습 그대로 검사한다.

경고는 빌드를 멈추지 않는다: 아마 뜻한 바가 아닌 것이 있지만 패키지는 동작한다. `--strict` 는 경고도 실패로
만든다 - 자동 빌드가 원하는 것이 이것이다. 제품 이름이 한국어인데 `summary-name` 이 없다고 하자(5장):

```text
C:\work\hello> rubrapack lint ko.toml
ko.toml:1:1: warning[RP1203]: name is not ASCII and summary-name is missing; the summary Subject will read 'rubrapack package'
C:\work\hello> echo %errorlevel%
0
C:\work\hello> rubrapack lint ko.toml --strict
ko.toml:1:1: warning[RP1203]: name is not ASCII and summary-name is missing; the summary Subject will read 'rubrapack package'
C:\work\hello> echo %errorlevel%
5
```

`%errorlevel%` 은 `cmd` 에서 마지막 명령의 종료 코드를 보는 방법이다. PowerShell 에서는 `$LASTEXITCODE` 다.
종료 코드:

| 코드 | 뜻 |
|---|---|
| 0 | 성공 |
| 1 | 원본의 오류 |
| 2 | rubrapack 이 알아듣지 못하는 명령줄 |
| 3 | 파일을 읽거나 쓰지 못함 |
| 4 | 서명 또는 서명 확인 |
| 5 | `lint` 가 문제를 찾음(`--strict` 면 경고도) |
| 6 | 네트워크(타임스탬프 서버) |

## 패키지 검사: `lint <파일>`

```text
C:\work\hello> rubrapack lint hello.msi
hello.msi: 0 errors, 0 warnings
C:\work\hello> rubrapack lint hello.msix
hello.msix: 7 files, 0 errors, 0 warnings
C:\work\hello> rubrapack lint hello.msixbundle
hello.msixbundle: 4 files, 0 errors, 0 warnings
```

MSI 는 Windows Installer 가 적용하는 규칙으로 표를 검사한다: 설치를 멈추게 할 것은 오류, 나머지는 경고다. 다른
도구가 만든 패키지에는 경고가 있을 수 있다. 여기서는 악센트 글자를 두 조각으로 저장하는 프로그램에서 제품 이름을
입력했다(제3부의 [유니코드 정규화](../basics/03-text.md#유니코드-정규화) 참고):

```text
C:\work\hello> rubrapack lint cafe.msi
cafe.msi: warning[RP2105]: lint: Feature row 'Main': column Title is not in NFC (1 row); build --nfc puts folder, file and shortcut names in NFC
cafe.msi: warning[RP2105]: lint: Property row 'ProductName': column Value is not in NFC (1 row); build --nfc puts folder, file and shortcut names in NFC
cafe.msi: 0 errors, 2 warnings
```

MSIX 는 Windows 가 설치 전에 검사하는 것을 검사한다: ZIP 압축 파일, 블록 맵, 모든 블록의 해시, 그리고 매니페스트가
가리키는 파일이 실제로 있는지. `lint new.msi --previous old.msi` 는 업그레이드를 검사한다(3장).

## 표 보기: `inspect`

MSI 는 작은 데이터베이스다. `inspect` 에 표 이름을 주면 그 표를 출력한다:

```text
C:\work\hello> rubrapack inspect hello.msi File
File	Component_	FileName	FileSize	Version	Language	Attributes	Sequence
s72	s72	l255	i4	S72	S20	I2	i4
File	File
F_6d1508f52f6615459567	C_a10be66379e69940a2e0	guide.txt	6			512	1
Hello	C_185f8db32271fe25f561	hello.exe	17920	1.2.3.4	1033	512	2
```

이것은 Windows Installer 자신의 표 텍스트 형식(*IDT*)으로, 그 도구들도 읽는다. 처음 세 줄이 머리다:

1. 열 이름들
2. 열마다 형식: `s72` 는 72자까지의 문자열, `l255` 는 *지역화할 수 있는* 문자열, `i4` 는 4바이트 정수, `i2` 는
   2바이트 정수. 대문자(`S72`, `I2`)는 그 열이 비어 있어도 된다는 뜻이다.
3. 표 이름, 그리고 키 열들 - 행을 하나로 구별하는 열.

그 뒤의 행이 파일 하나씩이다. `hello.exe` 는 판 정보 리소스가 있는 프로그램이라 판 번호와 언어(1033 은 영어(미국))가
있고, `guide.txt` 에는 없다. Attributes `512` 는 "필수" 비트다(4장: 파일을 쓰지 못하면 설치가 실패한다). 제4부가
모든 형식과 행이 저장되는 방식을 설명한다.

표 이름 없이 부르면 `inspect` 는 모든 표를 행과 열 수와 함께 나열하고, Windows 가 파일 속성에 보이는 몇 가지 사실인
*요약 정보*도 보인다:

```text
C:\work\hello> rubrapack inspect hello.msi
code page: 65001
strings: 181
tables: 20
  AdminExecuteSequence  rows=8 columns=3
  ...
  File  rows=2 columns=8
  ...
summary:
  2 = Installation Database
  3 = Hello
  4 = Example Software
  5 = Installer
  7 = x64;1033
  9 = {7FC806A2-F359-4941-A159-26A77B78F839}
  14 = 200
  15 = 2
  18 = rubrapack 0.32.0
```

요약 정보의 수는 속성 ID 다: 2 제목, 3 주제, 4 작성자, 5 키워드, 7 플랫폼과 언어, 9 *패키지 코드*(빌드마다 새
GUID), 14 필요한 Windows Installer 판(200 = 2.0), 15 단어 플래그(2 = 압축됨), 18 만든 프로그램. `--summary` 는
이것만 출력한다.

## 파일과 스트림

```text
C:\work\hello> rubrapack inspect hello.msi --files
[ProgramFiles64Folder]\Hello\guide.txt	6	F_6d1508f52f6615459567	C_a10be66379e69940a2e0			2b6739281f9372442df30e784b7dbdc7
[ProgramFiles64Folder]\Hello\hello.exe	17920	Hello	C_185f8db32271fe25f561	1.2.3.4	1033
```

파일마다 한 줄: 설치되는 곳, 크기, 파일 ID, 컴포넌트, 판, 언어, 그리고 판 번호 없는 파일을 바꿀지 Windows 가
정할 때 쓰는 MD5 해시(프로그램은 판 번호로 비교하므로 없다).

```text
C:\work\hello> rubrapack inspect hello.msi --streams
table  40	File
stream 6821	cab1.cab
table  14	Media
table  808	_Columns
...
stream 332	!SummaryInformation
```

*스트림*은 패키지 안의 파일이다(제4부: 패키지는 파일 안의 파일 시스템이다). 표 하나가 스트림 하나이고,
`cab1.cab` 에 압축된 파일들이, `!SummaryInformation` 에 요약 정보가 든다.

캐비닛도 따로 볼 수 있다 - 15장의 외부 캐비닛:

```text
C:\work\hello> rubrapack inspect out-x64\hello-x64.cab
6	F_6d1508f52f6615459567
17920	Hello
```

## 설치하지 않고 풀기: `extract`

```text
C:\work\hello> rubrapack extract hello.msi -d unpacked
unpacked: 2 files, 17926 bytes
```

파일은 설치될 모습 그대로, 표준 폴더 이름 아래에 나온다:

```text
unpacked\ProgramFiles64Folder\Hello\guide.txt
unpacked\ProgramFiles64Folder\Hello\hello.exe
```

`extract` 는 `.msi`, `.cab`, `.msix`, `.msixbundle` 에 쓸 수 있고, 어디서 온 패키지에도 안전하다: 대상 폴더는
새것이거나 비어 있어야 하고, 모든 파일을 크기와 해시로 확인하고, 폴더 밖에 쓰게 될 이름(`..`, 드라이브, `CON`)은
거부하며, 패키지 전체가 통과할 때까지 아무것도 쓰지 않는다. 디스크를 채우려고 만든 패키지에 대비한 한도도 있다:

```text
C:\work\hello> rubrapack extract hello.msi -d small --limit-entries 2
rubrapack: error[RP0008]: 'hello.msi': 4 entries, more than the limit 2 (--limit-entries); nothing was extracted
C:\work\hello> rubrapack extract hello.msi -d small --limit-bytes 1000
rubrapack: error[RP0008]: 'hello.msi': more than 1000 bytes to write (--limit-bytes); nothing was extracted
```

기본값은 항목 100,000개와 16 GiB 다. (항목 넷은 파일 둘과 폴더 둘, `ProgramFiles64Folder` 와 `Hello` 다.)

## 한 곳만을 위한 변경: `transform`

Hello 를 자기 컴퓨터들에 설치하는 IT 부서가 자기 패키지를 따로 만들지 않고 인사말만 바꾸고 싶을 수 있다.
`transform` 은 한 패키지를 다른 패키지로 바꾸는 차이를 변환(`.mst`)으로 쓰고, Windows 는 원래 패키지를 설치하면서
그것을 적용한다. 원본을 복사해 바꾼 패키지를 만들고(여기서는 `[registry.Greeting]` 에
`value = "Hello from the IT desk"`) 비교한다:

```text
C:\work\hello> rubrapack build custom.toml -o custom.msi
C:\work\hello> rubrapack transform hello.msi custom.msi -o custom.mst
C:\work\hello> rubrapack inspect custom.mst --base hello.msi
Registry	update	Greeting	Value=Hello from the IT desk
C:\work\hello> msiexec /i hello.msi TRANSFORMS=custom.mst
```

`inspect <파일.mst> --base <패키지>` 는 변환의 행을 보여 준다: `insert`, `update`(키, 그다음 바뀐 열),
`delete`. 기본으로 변환은 만든 바탕이 된 제품에만 적용된다(`--validate product-code,upgrade-code`).
`--validate none` 은 이 확인을 뺀다. 변환은 표를 나르고 파일은 나르지 않는다: 파일이 다른 패키지는
거부한다(`RP0013`).

## 서명: `verify`

16장에서 `verify` 를 보았다. 배포하기 직전에 돌릴 검사다:

```text
C:\work\hello> rubrapack verify hello.msi --system-roots
```

## MSIX 패키지

```text
C:\work\hello> rubrapack inspect hello.msix
C:\work\hello> rubrapack inspect hello.msix --files
C:\work\hello> rubrapack inspect hello.msix --manifest
```

첫 번째는 모든 블록의 해시를 확인한 뒤 정체, 실행 파일, 파일들을 보인다. 나머지 둘은 17장에서 보았다.

## 더 빠른 시험 빌드: `--compress none`

빌드 시간의 대부분은 압축이다. 이것저것 해 보는 동안에는 압축을 뺀다:

```text
C:\work\hello> rubrapack build hello.toml -o fast.msi --compress none
```

패키지는 커지지만(여기서는 32768 바이트 대신 45056 바이트) 똑같이 동작한다.

## GUID: `guid`

```text
C:\work\hello> rubrapack guid
{98AE4FED-BF0B-4C77-B712-E0649EB47178}
C:\work\hello> rubrapack guid --from hello
{B52FE8EF-B68D-84B5-91AF-E6E01BEC2773}
```

첫 번째는 무작위 - 매번 새것 - 로, 업그레이드 코드에 쓴다. 두 번째는 rubrapack 이 글에서 *끌어낸* GUID 로, 어느
컴퓨터에서든 언제나 같다. rubrapack 은 컴포넌트 GUID 를 이렇게 끌어내므로 판이 바뀌어도 같게 유지된다. 제3부가 [그 계산](../basics/04-guids-and-hashes.md#해시로-만드는-guid-guid---from)을 보여 준다.

## 오류의 뜻: `explain`

모든 메시지에는 `RP` 와 네 자리 숫자의 코드가 붙는다. `explain` 은 그 코드가 무엇에 관한 것인지, 그 코드를 단
메시지들, 할 일, 매뉴얼에서 더 볼 곳을 알려 준다:

```text
C:\work\hello> rubrapack explain RP1612
RP1612 - MSIX: what an MSIX package needs, and what it cannot carry

Messages with this code (* stands for a name or a value):
  - [registry.*]: an MSIX writes all its values (when); use msi-only = true
  ...
What to do: give [msix] and [msix-app.*] what they need, or keep the item for the MSI with msi-only = true.
Manual: Reference, "MSIX packages" - https://rubidus-api.github.io/rubrapack/en/rpk.html#msix-packages
```

코드 없이 `rubrapack explain` 을 주면 범위를 나열한다(RP13xx 값, RP16xx MSIX, ...).

## 스크립트용: `--json`

`lint` 와 `inspect` 는 빌드 스크립트나 편집기가 읽도록 JSON 으로도 답한다:

```text
C:\work\hello> rubrapack lint hello.toml --json
{"file": "hello.toml", "exit": 1, "errors": 1, "warnings": 0, "not_shown": 0, "diagnostics": [
  {"file": "hello.toml", "line": 7, "column": 1, "severity": "error", "code": "RP1201", "message": "unknown key 'sorce' in [file.App] (did you mean 'source'?)"}
]}
C:\work\hello> rubrapack inspect hello.msi File --json
C:\work\hello> rubrapack inspect hello.msi --summary --json
```

종료 코드는 `--json` 이 없을 때와 같고, 메시지는 오류 출력 대신 객체에 들어간다. `inspect <패키지> --json` 은 모든
표를(`{"codepage", "tables": [...]}`), 표 이름을 주면 그 표 하나를(`{"name", "columns", "rows"}`, 칸은 문자열, 숫자,
`null`, 또는 있는 스트림이면 `true`), `--summary --json` 은 요약 정보를 준다.

## 명령줄의 도움말

```text
C:\work\hello> rubrapack help
rubrapack 0.32.0 - build Windows Installer (.msi) and MSIX (.msix) packages

usage: rubrapack <command> [arguments]

commands:
  build    build a package from a source file
  sign     sign a PE file, an MSI package, an MSIX package or bundle
  ...
C:\work\hello> rubrapack help sign
usage: rubrapack sign <file.exe|.dll|.msi|.msp|.msix|.msixbundle> (--key <key.pfx|.pem> ...
C:\work\hello> rubrapack version
rubrapack 0.32.0 (proven_c_lib-v0.6.0)
```

`rubrapack --help` 는 `rubrapack help` 와 같다. `help <명령>` 은 그 명령의 옵션을 출력한다 - 모든 옵션과 뜻을 담은
제2부의 짧은 판이다.
