# 첫 설치 파일: 프로그램 하나

목표: `hello.exe` 를 `C:\Program Files\Hello` 에 복사하고, "설치된 앱"에 나타나고, 깨끗하게 지워지는 설치 파일.

## 원본

`C:\work\hello` 폴더(앞 장 참고)에 아무 편집기로 - 메모장이면 된다 - `hello.toml` 이라는 글 파일을 만들고
이렇게 쓴다:

```toml
# tutorial 02: hello.toml
[package]
name = "Hello"
manufacturer = "Example Software"
version = "1.0.0"
arch = "x64"
upgrade-code = "{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}"

[dir.INSTALLDIR]
path = "ProgramFiles/Hello"

[file.Hello]
dir = "INSTALLDIR"
source = "dist/hello.exe"
```

한 줄씩 보면:

- `#` 으로 시작하는 줄은 주석이다. rubrapack 은 무시한다.
- `[package]` 는 *표*를 시작한다: 그 뒤의 줄은 다음 `[...]` 까지 이 표에 속한다. `package` 표는 제품이 무엇인지
  말한다.
  - `name` 은 사용자가 보는 이름이다: "설치된 앱"에서, 설치 창의 제목에서.
  - `manufacturer` 는 나나 회사다. "설치된 앱"이 이름 아래에 보인다.
  - `version` 은 수 셋(또는 넷)이다: 주판, 부판, 빌드. 3장이 판 바꾸기를 다룬다.
  - `arch` 는 프로그램이 만들어진 프로세서다: `x64`(요즘 거의 모든 PC), `x86`(32비트 프로그램), `arm64`.
    rubrapack 은 `hello.exe` 가 정말 x64 프로그램인지 확인한다.
  - `upgrade-code` 는 GUID - 16진수 32자리를 중괄호로 싼 128비트 수 - 로, 내 제품에만 있는 것이다.
    `rubrapack guid` 로 자기 것을 만들어(아래) 여기에 한 번 쓰고 **절대 바꾸지 않는다**. 이후 모든 판이 같은
    코드를 지녀야 한다. 그러지 않으면 Windows 가 다른 제품으로 보고 옛것 옆에 따로 설치한다.
- `[dir.INSTALLDIR]` 는 ID 가 `INSTALLDIR` 인 폴더를 선언한다. ID 는 다른 표에서 그것을 가리키려고 내가
  정하는 이름이다(영문자, 숫자, `_`). `path` 는 폴더가 어디인지 말한다: `ProgramFiles/Hello` 는 영어판이든
  한국어판이든 Windows 에서 `C:\Program Files\Hello` 다 - 알려진 폴더 `ProgramFiles` 는 사용자 컴퓨터에서
  정해진다.
- `[file.Hello]` 는 파일 하나를 ID `Hello` 로 설치한다. `dir` 은 들어갈 폴더를 ID 로 가리키고, `source` 는
  지금 파일이 있는 곳으로, `hello.toml` 기준의 상대 경로를 `/` 로 쓴다.

자기 업그레이드 코드를 받는다 - 칠 때마다 새로 나온다:

```text
C:\work\hello> rubrapack guid
{F1B1ED21-3061-4A65-8D77-FBB5ACCB1642}
```

예제의 코드 자리에 붙여 넣는다. (예제의 코드를 그대로 쓰면 Windows 가 내 제품과 이 책을 읽은 다른 사람의
제품을 "같은 제품"으로 본다.)

## 빌드

```text
C:\work\hello> rubrapack build hello.toml -o hello.msi
```

잘되면 아무것도 찍지 않고, 원본 옆에 `hello.msi`(약 28 KB)가 생긴다. 무언가 틀리면 rubrapack 은 어디가 왜
틀렸는지 말하고 아무것도 쓰지 않는다. 예를 들어 `source` 에 오타가 있으면:

```text
hello.toml:14:1: error[RP1507]: source file 'dist/hello.ex' not found
```

수는 줄과 열이고, 대괄호 안의 코드(`RP1507`: RP15xx 는 "설치할 파일")는 참조 부의
[진단 코드](../rpk.md#진단-코드)가 설명한다. 그 줄을 고치고 다시 빌드한다.

## 설치하고 지우기

`hello.msi` 를 Windows 컴퓨터에 복사해 두 번 누른다. 모든 사용자의 것인 Program Files 에 설치하므로 Windows 가
관리자 허락을 묻는다(사용자 계정 컨트롤 창). 그다음 진행 막대가 있는 작은 창이 잠깐 나타난다 - 이 패키지에는
아직 자기 대화창이 없다(6장에서 더한다). 끝나면:

- `C:\Program Files\Hello\hello.exe` 가 있다.
- 설정 > 앱 > 설치된 앱에 **Hello**, 판 1.0.0, Example Software 가 보인다.

"설치된 앱"에서(`...` 메뉴, 제거) 지우거나 터미널에서 지운다:

```text
msiexec /x hello.msi
```

파일과 폴더가 다시 사라진다. 같은 패키지를 창 없이 설치할 수도 있는데, 회사가 소프트웨어를 배포하는
방식이다(`/qn` 은 "조용히, 사용자 화면 없이"라는 뜻; 터미널을 관리자로 연다):

```text
msiexec /i hello.msi /qn
msiexec /x hello.msi /qn
```

Windows 에서 무언가 잘못되면 기록이 이유를 알려 준다: `msiexec /i hello.msi /l*v install.log` 는 Windows
Installer 가 한 일을 모두 `install.log` 에 쓴다.

## 안에서 무슨 일이 일어났나

`rubrapack inspect` 는 설치하지 않고 패키지 안을 보여 준다. `--files` 는 파일과 그 자리를 보인다:

```text
C:\work\hello> rubrapack inspect hello.msi --files
[ProgramFiles64Folder]\Hello\hello.exe	17920	Hello	C_185f8db32271fe25f561	1.2.3.4	1033
```

`ProgramFiles64Folder` 는 64비트 Program Files 의 Windows Installer 이름이고, `17920` 은 바이트 단위 크기다.
`1.2.3.4` 와 `1033`(미국 영어)은 `hello.exe` 안에 적힌 판과 언어다(Windows 는 파일을 바꿔 넣을 때 이것을
비교한다). MSI 는 데이터베이스이고 `inspect` 는 어떤 표든 찍을 수 있다. `File` 표에는 파일 하나에 행 하나가 있다:

```text
C:\work\hello> rubrapack inspect hello.msi File
File	Component_	FileName	FileSize	Version	Language	Attributes	Sequence
s72	s72	l255	i4	S72	S20	I2	i4
File	File
Hello	C_185f8db32271fe25f561	hello.exe	17920	1.2.3.4	1033	512	1
```

첫 줄은 열 이름, 둘째 줄은 열의 형식(`s72` 는 72자까지의 문자열, `i4` 는 4바이트 정수, 대문자는 비어 있어도
되는 열), 셋째 줄은 표 이름과 키 열이다. `Hello` 는 내 파일 ID, `C_185f...` 는 rubrapack 이 이 파일을 위해 만든
*컴포넌트*, `512` 는 "필수"(이 파일을 쓰지 못하면 설치가 실패한다)라는 뜻이다. 제품의 정체는 `Property` 표에 있다:

```text
C:\work\hello> rubrapack inspect hello.msi Property
...
ProductCode	{98BFA9A2-2741-81F9-A1CA-2C6626AE1304}
ProductName	Hello
ProductVersion	1.0.0
UpgradeCode	{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}
...
```

`ProductCode` 는 *이 판*을 가리키는 또 하나의 GUID 다. rubrapack 이 업그레이드 코드, 아키텍처, 판에서
끌어내므로 판이 바뀌면 저절로 바뀐다. 직접 쓰지 않는다. 이 표들이 파일에 어떻게 저장되는지는 제4부가 설명한다.

## 빌드하지 않고 검사하기

`rubrapack lint hello.toml` 은 빌드가 하는 검사를 모두 하되 아무것도 쓰지 않는다 - 고친 뒤 확인하기 좋다.
`rubrapack lint hello.msi` 는 다 만든 패키지를 검사하며, 다른 도구로 만든 것도 된다:

```text
C:\work\hello> rubrapack lint hello.msi
hello.msi: 0 errors, 0 warnings
```

## 편집기 대신 질문으로

`rubrapack new hello.toml` 은 이름, 판, 파일이 든 폴더 등을 물은 뒤 위와 같은 원본을 쓴다(업그레이드 코드도
만든다). 19장이 이 명령과 짝인 `rubrapack edit` 를 설명한다. 여기서처럼 원본을 한 번 손으로 써 보는 것이 각
줄이 하는 일을 익히는 가장 좋은 방법이다.
