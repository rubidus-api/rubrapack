# 설치하는 동안 내 프로그램 실행하기

목표: 파일이 자리 잡은 뒤 `hello.exe --register` 를 실행해 Hello 가 어딘가(다른 프로그램, 플러그인 호스트, 시스템)에
자신을 등록하게 하고, 제거하기 전에는 `hello.exe --unregister` 를 실행한다. 그리고 무언가 실패하면 모두 되돌린다.

## 왜 그냥 프로그램을 실행하면 안 되나?

설치는 트랜잭션이다: 40단계가 실패하면 Windows Installer 는 1~39단계를 되돌려 컴퓨터를 원래대로 한다. 내가 실행한
프로그램은 그 밖에 있다 - Windows 는 그 프로그램이 한 일을 어떻게 되돌릴지 모른다. 그래서 rubrapack 은 명령을
*둘* 받는다. 하는 것과 되돌리는 것. 그리고 일이 어떻게 흘러가든 알맞은 것이 돌게 꾸민다.

## 원본

```toml
# tutorial 13: hello.toml
[package]
name = "Hello"
manufacturer = "Example Software"
version = "$(VERSION)"
arch = "x64"
upgrade-code = "{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}"

[define]
VERSION = "1.11.0"

[dir.INSTALLDIR]
path = "ProgramFiles/Hello"

[file.Hello]
dir = "INSTALLDIR"
source = "dist/hello.exe"

[action.Register]
run = "file:Hello"
do = "--register"
undo = "--unregister"
```

## `[action.ID]`

| 키 | 뜻 |
|---|---|
| `run` | 프로그램: `file:` 과 이 패키지가 설치하는 `.exe` 의 ID |
| `do` | 설치 뒤의 인자 - 복구 때 다시 |
| `undo` | 제거 때, 파일을 지우기 전의 인자 |
| `check` | 아직 효과가 없다: rubrapack 이 경고한다(`RP1318`). 쓰지 않는다 |

인자는 쓴 그대로 넘어간다: 서식 문자열이 *아니어서* `[INSTALLDIR]` 은 글자 `[INSTALLDIR]` 그대로 도착한다.
프로그램은 자기 자리(자기 경로)를 알고, 대개 그것이면 충분하다.

두 명령 모두:

- 아무것도 묻지 않고 끝나야 한다: 창 없이 돌고, 대답해 줄 사람이 없다.
- 두 번 실행해도 안전해야 한다(이미 등록된 것을 등록: 괜찮음).
- 실패를 0 이 아닌 종료 코드로 알린다 - 그러면 설치가 실패하고 되돌려진다.

컴퓨터 전체 패키지에서는 설치 파일의 관리자 권한으로, 사용자별 패키지에서는 사용자로 돈다.

## 무엇이 언제 도나

| 상황 | 도는 것 |
|---|---|
| 첫 설치 | 파일 설치 뒤 `do` |
| 복구 | `do` 다시 |
| 제거 | `undo`, 그다음 파일이 사라짐 |
| 새 판으로 업그레이드 | 옛 판의 `undo`(옛 판 제거), 그다음 새 판의 `do` |
| `do` 가 돈 뒤 설치 실패 | 되돌리기의 일부로 `undo` |
| `undo` 가 돈 뒤 제거 실패 | `do`, 그리고 파일이 돌아옴 |

## 해 보기

이런 일을 하는 진짜 프로그램은 인자를 읽고, 예를 들어 레지스트리 값을 쓰거나 등록 인터페이스를 부른다. 순서를
보려면 `hello.exe` 가 인자를 기록 파일에 쓰게 하고, 설치, 복구, 제거를 한 뒤 기록을 읽는다.

## 안에서 무슨 일이 일어났나

```text
C:\work\hello> rubrapack inspect hello.msi CustomAction
RP_Register_Do	3090	Hello	--register
RP_Register_DoRollback	3410	Hello	--unregister
RP_Register_RedoRollback	3410	Hello	--register
RP_Register_Undo	3090	Hello	--unregister
RP_Register_UndoRollback	3410	Hello	--register
```

*사용자 지정 동작*이 다섯이다. 형식 수는 비트 플래그를 더한 것이다: 18("이 패키지가 설치한 `.exe` 실행") + 1024
("지연": 설치 스크립트 안에서 파일 복사와 차례를 맞춰 돈다) + 2048("가장 없음": 설치 파일의 권한으로) = 3090. 되돌리기
짝은 256("되돌릴 때만 실행")과 64("종료 코드 무시")를 더한다. `InstallExecuteSequence` 는 이것들을 파일 동작 둘레에
두고, 각각 `hello.exe` 컴포넌트의 상태에 대한 조건을 단다:

```text
RP_Register_UndoRollback	$C_185f...=2 AND ?C_185f...=3	3400
RP_Register_Undo	$C_185f...=2 AND ?C_185f...=3	3401
RemoveFiles		3500
InstallFiles		4000
RP_Register_DoRollback	$C_185f...>2 AND ?C_185f...<>3	4001
RP_Register_RedoRollback	$C_185f...>2 AND ?C_185f...=3	4002
RP_Register_Do	$C_185f...>2	4003
```

`$C` 는 컴포넌트가 갈 상태(2 없음, 3 설치됨), `?C` 는 지금 상태다. "설치되어 있다가 사라짐"이면 undo, "들어옴"이면
do 가 돈다.
