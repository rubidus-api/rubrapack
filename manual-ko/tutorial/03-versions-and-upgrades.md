# 판과 업그레이드

목표: Hello 1.1.0 을 내서, 설치하면 1.0.0 을 바꿔 넣고, 그 뒤에 1.0.0 을 다시 설치하려 하면 거부되게 한다.

## 모든 판에 원본 하나

릴리스 전마다 `version` 을 손으로 고쳐도 되지만 잊기 쉽다. 대신 판을 기본값이 있는 *변수*로 두고, 빌드할 때
진짜 번호를 준다:

```toml
# tutorial 03: hello.toml
format = 1

[package]
name = "Hello"
manufacturer = "Example Software"
version = "$(VERSION)"
arch = "x64"
upgrade-code = "{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}"
downgrade-message = "더 새 Hello 가 이미 설치되어 있습니다. 이 판을 원하면 먼저 그것을 지우십시오."

[define]
VERSION = "1.0.0"

[dir.INSTALLDIR]
path = "$(ProgramFiles)/Hello"

[file.Hello]
dir = "INSTALLDIR"
source = "dist/hello.exe"
```

- `[define]` 에는 변수를 둔다. 문자열 안 어디서든 `$(VERSION)` 은 변수 값으로 바뀐다. 여기서 기본값은
  `1.0.0` 이다.
- 명령줄의 `-D 이름=값` 은 그 빌드 한 번에서 변수를 덮어쓴다:

```text
C:\work\hello> rubrapack build hello.toml -o hello-1.0.0.msi
C:\work\hello> rubrapack build hello.toml -o hello-1.1.0.msi -D VERSION=1.1.0
```

- `downgrade-message` 는 사용자가 더 새 판 위에 옛(또는 같은) 판을 설치하려 할 때 보는 안내문이다. 없으면
  rubrapack 은 "The same or a newer version of [ProductName] is already installed." 를 쓴다(`[ProductName]` 은
  이름으로 바뀐다).

## Windows 는 두 판을 어떻게 다루나

`hello-1.0.0.msi` 를 설치하고 이어서 `hello-1.1.0.msi` 를 설치한다. Windows Installer 는 새 패키지가 같은
업그레이드 코드에 더 높은 판임을 보고, 1.0.0 을 지운 뒤 1.1.0 을 설치한다 - "설치된 앱"에는 항목 하나가 남고
이제 1.1.0 을 보인다. 이것을 *메이저 업그레이드*라고 하며, rubrapack 의 패키지는 이렇게 업그레이드한다. 새 판은
늘 완전한 패키지이고, 새 판을 설치하는 데 옛 판이 필요하지 않다. (설치된 판을 그 자리에서 고치는 것은
[아래](#작은-수정을-패치로-patch)의 패치다.)

이제 `hello-1.0.0.msi` 를 다시 설치해 본다: Windows 가 다운그레이드 안내문을 보이고 아무것도 바꾸지 않는다.
`hello-1.1.0.msi` 를 한 번 더 실행하는 것도 오류가 아니다. 설치된 판과 제품 코드가 같으므로(아래 참고) Windows
는 제품이 이미 설치되어 있다고 알아보고 두 번 설치하지 않는다. 설치된 판을 고치려면 - 지워진 파일을 되살리는
등 - `msiexec /fa hello-1.1.0.msi` 를 쓴다.

판을 올리지 *않고* 바뀐 파일로 다시 빌드한 패키지는 설치된 것과 제품 코드는 같지만 같은 패키지가 아니어서,
Windows 가 자체 안내문으로 거부한다: "Another version of this product is already installed"(오류 1638). 그러니
파일이 바뀌면 판을 올린다 - 설치된 것을 바꿔 넣는 것은 더 높은 판뿐이다.

rubrapack 이 이를 어떻게 꾸미는지는 `Upgrade` 표에 보인다:

```text
C:\work\hello> rubrapack inspect hello-1.1.0.msi Upgrade
UpgradeCode	VersionMin	VersionMax	Language	Attributes	Remove	ActionProperty
s38	S20	S20	S255	i4	S255	s72
Upgrade	UpgradeCode	VersionMin	VersionMax	Language	Attributes
{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}		1.1.0		1		RP_OLDER_FOUND
{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}	1.1.0			258		RP_NEWER_FOUND
```

첫 행은 1.1.0 보다 낮은 설치된 판을 찾고(지우려고), 둘째 행은 1.1.0 이상을 찾아 다운그레이드 안내문과 함께
설치를 멈춘다. 속성 수는 비트 플래그다 - `258` 은 `256 + 2`, "최솟값 포함"에 "찾기만 하고 지우지 않음"을
더한 것이다. 비트 플래그는 제3부의 [비트 플래그](../basics/02-numbers-and-flags.md#비트-플래그), 이 표는 제4부의 [설치하는 표들](../formats/msi-package.md)이 설명한다.

## 판 번호

- 수 셋이나 넷: `주판.부판.빌드` 또는 `주판.부판.빌드.리비전`. Windows Installer 는 앞의 셋에 `255.255.65535`
  까지 허용한다.
- **Windows 는 앞의 셋만 비교한다.** `1.0.0.1` 과 `1.0.0.2` 는 같은 판이다: 둘째는 "같은 것"으로 거부된다.
  릴리스마다 셋째 수(또는 그 위의 수)를 올린다.
- 틀린 판은 빌드를 멈춘다:

```text
hello.toml:5:1: error[RP1308]: version '1.0' must be a.b.c or a.b.c.d with a, b <= 255 and c, d <= 65535
```

## 두 식별자

| | 업그레이드 코드 | 제품 코드 |
|---|---|---|
| 가리키는 것 | 모든 판에 걸친 제품 | 판 하나 |
| 직접 쓰나 | `upgrade-code` 에 한 번; 절대 바꾸지 않는다 | 아니다: rubrapack 이 끌어낸다 |
| 바뀌나 | 바뀌지 않는다 | 판(앞의 수 셋)이 바뀔 때마다 |

```text
1.0.0: ProductCode {98BFA9A2-2741-81F9-A1CA-2C6626AE1304}
1.1.0: ProductCode {9CC17977-2023-8B8F-8BB0-2FA82C5B248F}
```

끌어낸 제품 코드는 같은 판을 빌드할 때마다, 어느 컴퓨터에서나 같다. `[package]` 의 `product-code` 는 이것을
손으로 정한다 - 앞선 판을 다른 도구가 자기 제품 코드로 만든 제품을 이어 갈 때만 필요하다.

## 사용자보다 먼저 업그레이드를 확인하기

어떤 실수는 빌드를 멈추지 않지만 사용자 컴퓨터에서 업그레이드를 망가뜨린다: 바뀐 업그레이드 코드, 높지 않은 판,
Windows 가 따라갈 수 없게 자리를 옮긴 파일. 새 패키지를 그 앞의 것과 대조한다:

```text
C:\work\hello> rubrapack lint hello-1.1.0.msi --previous hello-1.0.0.msi
hello-1.1.0.msi: 0 errors, 0 warnings (against hello-1.0.0.msi)
```

실수가 어떻게 보이는지 보려고 같은 패키지를 자기 자신과 대조하면:

```text
C:\work\hello> rubrapack lint hello-1.0.0.msi --previous hello-1.0.0.msi
hello-1.0.0.msi: error[RP2302]: ProductVersion 1.0.0 is not higher than the previous 1.0.0 in its first three fields: Windows Installer does not upgrade
hello-1.0.0.msi: warning[RP2303]: the ProductCode is the previous version's: that is a minor upgrade (REINSTALLMODE), not the major upgrade rubrapack builds
hello-1.0.0.msi: 1 error, 1 warning (against hello-1.0.0.msi)
```

낸 `.msi` 는 모두(예: `releases` 폴더에) 보관해, 다음 판을 그것과 대조할 수 있게 한다.

## 작은 수정을 패치로: `patch`

패치(`.msp`)는 설치된 판을 그 자리에서 고친다: 바뀐 파일만 통째로, 그리고 달라진 표의 행을 나르고,
Windows 는 아무것도 지우지 않고 그것을 적용한다. 패치는 *같은 제품*의 두 빌드를 잇기 때문에 제품 코드가
같아야 한다 - rubrapack 은 보통 그것을 판에서 끌어낸다:

```text
C:\work\hello> rubrapack patch hello-1.0.0.msi hello-1.1.0.msi -o hello-1.1.0.msp
rubrapack: error[RP0013]: ProductCode differs: that is a major upgrade, which a patch does not carry
```

패치로 이을 판들에는 `[package]` 에 제품 코드를 고정하고(`rubrapack guid` 가 새것을 찍는다) 셋째 수만
올린다:

```toml
product-code = "{5B3E2A71-9C4D-4E8F-A1B2-C3D4E5F60718}"
```

```text
C:\work\hello> rubrapack build hello.toml -o hello-1.1.0.msi -D VERSION=1.1.0
C:\work\hello> rubrapack build hello.toml -o hello-1.1.1.msi -D VERSION=1.1.1
C:\work\hello> rubrapack patch hello-1.1.0.msi hello-1.1.1.msi -o hello-1.1.1.msp
C:\work\hello> msiexec /p hello-1.1.1.msp
```

1.1.0 이 설치된 컴퓨터는 패치를 적용하고, 새 컴퓨터는 `hello-1.1.1.msi` 를 설치한다. 패치는 따로 제거할
수 있고, 그러면 1.1.0 의 파일과 값이 돌아온다(`--no-removal` 로 만들면 따로 제거할 수 없다). 패치마다 패치
코드가 있고, `--patch-code` 로 주지 않으면 두 패키지에서 끌어낸다. `--family` 는 패치가 속한 줄기의
이름이다(기본은 제품 이름). `inspect hello-1.1.1.msp --base hello-1.1.0.msi` 는 패치가 바꾸는 것을
보여 준다. 패치는 파일이나 구성 요소를 지우거나, 제품 코드나 업그레이드 코드를 바꾸거나, 판의 앞 두 수를
바꿀 수 없다(`RP0013`). `sign` 은 아직 패치에 서명하지 않는다.

## 다른 도구로 만든 옛 판

내 제품이 전에 다른 도구의 패키지로 설치되었더라도, 업그레이드 코드가 같으면 rubrapack 의 업그레이드가 그것을
바꿔 넣는다. 그것이 잘 안 될 때 도움이 되는 키가 둘 있다:

- `refuse-upgrade-below = "0.60.0"`: 이 판보다 낮은 판은 업그레이드하지 않고 거부하며, 사용자에게 먼저 지우는
  방법을 알려 준다 - 업그레이드 도중에 지우면 망가지는 것으로 알려진 옛 패키지를 위한 것이다.
- `refuse-upgrade-message = "..."`: 그 안내문의 앞부분(지우는 명령은 뒤에 붙는다).

## 제품과 함께 가는 추가 기능

따로 된 패키지로 내는 언어 팩이나 플러그인은 자기 업그레이드 코드와 판이 있고, Windows 도 따로 된 앱으로 보인다.
제품과 함께 지우려면 추가 기능이 제품의 업그레이드 코드를 적고, 제품은 추가 기능도 함께 지우라고 한다:

```toml
# hello-pack.toml
[package]
parent = "{6F1B2C3D-4E5F-4A6B-8C7D-9E0F1A2B3C60}"   # hello 의 upgrade-code

# hello.toml
[package]
remove-addons = true
```

그러면 Hello 를 지우면 몇 초 뒤에 팩도 지워지고, Hello 를 업그레이드하면 팩은 남는다. 참조의 "제품과 함께
지워지는 추가 기능"을 보라.
