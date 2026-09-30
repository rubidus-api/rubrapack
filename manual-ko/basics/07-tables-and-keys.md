# 표, 키, 참조

`.msi` 안(6장)의 스트림은 대부분 **표**다: 패키지는 작은 **데이터베이스**다. Windows Installer 는 내가 적은 단계별
각본을 따르는 것이 아니라, 표를 읽고 무엇을 할지 스스로 알아낸다. 이 장은 표를 읽는 데 필요한 데이터베이스 개념 몇
가지를 설명한다.

## 표, 행, 열

**표**는 격자다. **열**마다 이름과 형식이 있고, **행** 하나가 하나의 대상 - 파일 하나, 바로가기 하나, 레지스트리 값
하나 - 이다. 튜토리얼 첫 패키지(튜토리얼 2장)의 `File` 표를 `rubrapack inspect hello.msi File` 이 출력한 대로 보면:

```text
File	Component_	FileName	FileSize	Version	Language	Attributes	Sequence
s72	s72	l255	i4	S72	S20	I2	i4
File	File
Hello	C_185f8db32271fe25f561	hello.exe	17920	1.2.3.4	1033	512	1
```

격자로 펼치면:

| File | Component_ | FileName | FileSize | Version | Language | Attributes | Sequence |
|---|---|---|---|---|---|---|---|
| `Hello` | `C_185f8db32271fe25f561` | `hello.exe` | 17920 | `1.2.3.4` | `1033` | 512 | 1 |

글의 둘째 줄이 열마다 **형식**을 준다:

| 형식 | 뜻 |
|---|---|
| `s72` | 72자까지의 문자열 |
| `l255` | 255자까지의 *지역화할 수 있는*(번역이 바꿀 수 있는) 문자열 |
| `i2`, `i4` | 2 또는 4 바이트 정수(2장) |
| 대문자 `S`, `L`, `I` | 같지만 칸이 비어 있어도("null") 된다 |

표준 표의 열 형식은 Windows Installer 가 정해 두었고, rubrapack 은 모든 값을 그것과 대조한다(`lint`, 튜토리얼
18장): `i2` 열에 100,000 은 들어가지 않는다.

## 기본 키: 행을 하나로 만드는 것

셋째 줄 `File	File` 은 표 이름과 **기본 키** - 행마다 값이 다른 열(또는 열들) - 를 적는다. `File` 표에서는 열
`File` 이다: `Hello` 가 이 행의 이름이고 다른 어떤 행도 그 이름을 쓸 수 없다. 그래서 rubrapack 의 ID 는 겹치면 안
된다(튜토리얼 2장): `[file.Hello]` 에 적은 ID 가 이 키가 된다.

어떤 표는 열 둘을 함께 써야 한다. `FeatureComponents` 는 어느 컴포넌트가 어느 기능에 속하는지 적는다. 기능 하나에
컴포넌트가 여럿이고 컴포넌트 하나가 여러 기능에 들 수 있으므로, 둘의 짝만이 하나뿐이다:

```text
Feature_	Component_
s38	s72
FeatureComponents	Feature_	Component_
Main	C_185f8db32271fe25f561
```

## 외래 키: 행을 가리키는 행

이름이 `_` 로 끝나는 열은 *다른* 표의 행의 키를 담는다 - **외래 키**, 곧 참조다. 참조를 따라가면 표들이 하나의 그림으로
이어진다. 파일에서 시작해 참조를 따라가 보면:

```text
File              Hello
  Component_  --> Component   C_185f8db32271fe25f561
                    ComponentId   {B80BEC59-F582-8F10-8EB1-638C6687B899}
                    Directory_  --> Directory   INSTALLDIR
                                      DefaultDir   Hello
                                      Directory_Parent --> Directory   ProgramFiles64Folder
                                                             DefaultDir  .
                                                             Directory_Parent --> TARGETDIR
                    KeyPath     --> File        Hello   (back to the start)
FeatureComponents Main + C_185f8db32271fe25f561
  Feature_    --> Feature     Main
                    Level  1   (installed by default)
```

소리 내어 읽으면: 파일 `hello.exe` 는 한 컴포넌트에 속하고, 그 컴포넌트는 `ProgramFiles64Folder`(Program Files)
안의 폴더 `Hello` 에 설치되며, 기능 `Main` 이 그 컴포넌트를 설치한다. 컴포넌트의 *키 경로*는 파일 자신이다:
`hello.exe` 가 있으면 그 컴포넌트는 설치된 것으로 친다(튜토리얼 4장).

없는 행을 가리키는 참조는 Windows Installer 가 멈출 오류다 - `rubrapack lint` 가 어느 도구로 만든 `.msi` 에서든
검사하는 것 중 하나다.

## 내가 주는 키, rubrapack 이 만드는 키

표에는 원본의 ID 가 그대로 나온다 - `Hello`, `INSTALLDIR`, `Main`. Windows Installer 에는 필요하지만 원본에 자기 표가
없는 행에는 rubrapack 이 키를 만든다: 파일마다 만드는 컴포넌트는 `C_` 와 16진수 20자리, 글롭으로 찾은 파일은 `F_` 와
16진수 20자리. 이 숫자는 그 대상의 자리의 해시(4장)에서 오므로 빌드마다 같다.

## 문자열은 한 번만: 문자열 풀

표에는 되풀이되는 문자열 - `INSTALLDIR`, 컴포넌트 키, `Hello` - 이 가득하고, MSI 는 서로 다른 문자열을 한 번씩만
보관한다. 패키지의 모든 문자열이 **문자열 풀**이라는 목록 하나(스트림 둘, `_StringPool` 과 `_StringData`)에 있고,
표의 문자열 칸은 그 목록에서의 문자열 *번호*만 2 바이트로(아주 큰 패키지에서는 3 바이트로) 담는다. 튜토리얼 첫
패키지는 표 16개 전체에 서로 다른 문자열이 111개다:

```text
C:\work\hello> rubrapack inspect hello.msi
code page: 65001
strings: 111
tables: 16
```

풀은 문자열마다 몇 칸이 쓰는지도 센다. 그 바이트는 제4부가 보인다.

## 표에 대한 표

데이터베이스는 자기를 설명한다. 두 표가 다른 표들을 나열한다: `_Tables` 는 표마다 한 행, `_Columns` 는 모든 표의 열마다
형식과 함께 한 행이다. `inspect` 가 처음 두 줄에 출력하는 열 이름과 형식을 거기서 찾는다. *순서* 표들
(`InstallExecuteSequence`, `InstallUISequence`, 그리고 관리용·광고용 설치를 위한 셋 더)은 설치의 단계를 순서대로,
단계마다 조건과 함께 나열한다(튜토리얼 9장과 13장) - 절차마저 표다.

## IDT 텍스트 형식

`inspect` 가 출력하는 글은 Windows Installer 자신의 **IDT** 형식이다: 머리 세 줄, 그다음 행마다 한 줄, 열은 탭으로
나눈다. Microsoft 의 Windows SDK 도구들이 표를 이 형식으로 들이고 내보내므로, `rubrapack inspect` 의 표를 다른 도구가
내보낸 것과 비교할 수 있다.

## 쓰이는 곳

- 튜토리얼 [2](../tutorial/02-a-first-installer.md)장과 [4](../tutorial/04-files-and-folders.md)장: ID, 컴포넌트,
  키 경로.
- 튜토리얼 [18](../tutorial/18-checking-and-looking-inside.md)장: `inspect` 와 `lint`.
- 제4부: [MSI 데이터베이스](../formats/msi-database.md)(문자열 풀과 표 인코딩),
  [설치하는 표들](../formats/msi-package.md).
