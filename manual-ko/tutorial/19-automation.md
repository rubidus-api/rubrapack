# 빨리 시작하기와 자동화

목표: rubrapack 이 첫 원본을 대신 쓰게 하고, 편집기를 열지 않고 그것을 고친다. 그리고 이 튜토리얼의 모든 단계를
스스로 릴리스를 빌드하고 검사하고 서명하는 스크립트로 만든다 - 내 컴퓨터에서, 빌드 서버에서, 또는 AI 도우미를
통해.

## 답으로 원본 만들기: `new`

프로그램 파일이 `dist\` 에 있는 폴더에서:

```text
C:\work\hello2> rubrapack new hello.toml
rubrapack new: a few questions make the source (Enter takes the value in [ ]).
Product name [hello]: Hello
Manufacturer (shown in Installed apps) [Hello authors]: Example Software
Version [1.0.0]:
Folder with the files to install [dist]:
  2 files and 2 folders there
Main program, for shortcuts (- for none) [hello.exe]:
Architecture (x64, x86, arm64) [x64]:
Folder name under Program Files [Hello]:
Install for (machine, user, dual) [machine]:
  sub folders: docs samples
Optional parts the user may tick (sub folders, commas; - for none) [-]: samples
Dialogs (none, basic, minimal, installdir, features) [features]:
License to accept (.txt, .md, .rtf; - for none) [LICENSE.txt]:
Korean dialogs as well as English? [y/N]: y
Start menu shortcut? [Y/n]:
Desktop shortcut? [y/N]: y
wrote hello.toml
the same without questions:
  rubrapack new "hello.toml" --name "Hello" --manufacturer "Example Software" --version "1.0.0" --arch "x64" --dist "dist" --main "hello.exe" --install-dir "Hello" --scope "machine" --ui "features" --license "LICENSE.txt" --languages "ko" --optional "samples" --shortcuts "start,desktop"
lint: no problems
```

질문 하나하나가 이 튜토리얼의 어느 장에서 설명한 것이고, `[ ]` 안의 값은 Enter 가 받는 값이다. 제안은 앞의 답을
따라간다: 이름을 `Hello` 로 하면 제조사로 `Hello authors` 가 제안되고, 선택 구성요소가 생기면 대화창으로
`features` 가 제안된다.

만들어진 원본은 보통의 원본이다 - 각 줄이 무엇을 하는지 적은 주석, 새 업그레이드 코드, 폴더 맨 위의 파일마다
`[file.*]`, 하위 폴더마다 글롭, 선택 구성요소마다 `[feature.*]` 가 들어 있다:

```text
[feature.Main]
title = "Hello"
required = true                  # always installed

[feature.samples]
title = "samples"
level = 2                        # offered, not ticked by default
...
[files.samples_files]
dir = "samples_dir"
glob = "dist/samples/**"
```

여기서부터는 2~18장처럼 손으로 고치거나 `edit` 로 고친다. `new` 는 있는 파일을 절대 바꾸지 않는다: `new hello.toml`
을 한 번 더 하면 `edit` 을 쓰라고 한다.

다른 형태 셋:

- `new` 가 마지막에 출력하는 줄 - `rubrapack new hello.toml --name ... --shortcuts ...` - 은 묻지 않고 같은 답을
  준다. 스크립트용이다. 빠진 옵션은 질문이 제안했을 기본값을 받는다.
- `rubrapack new hello`(`.toml` 없는 이름, 옵션 없음)는 아무것도 묻지 않고 짧은 시작용 원본을 쓴다. 손으로 채울
  때 쓴다.
- `rubrapack new hello -i`(또는 `--interactive`)는 질문을 한다. 질문은 오류 출력으로 가고 답은 한 줄에 하나씩 읽으므로, 답을 적은 파일을
  흘려 넣을 수 있다: `rubrapack new hello.toml < answers.txt`. 마지막 질문 전에 답이 떨어지면 아무것도 쓰지 않는다.

## 원본 고치기: `edit`

옵션 없는 `edit` 은 메뉴를 보인다:

```text
C:\work\hello2> rubrapack edit hello.toml
hello.toml: "Hello" 1.0.0, x64, machine, dialogs features
  1 name, manufacturer, version, architecture   2 install folder, who it installs for
  3 dialogs, license, languages                  4 optional parts
  5 shortcuts                                    6 files: match the program folder
  7 any key                                      v view   l lint   s save   q quit
Choose [q]:
```

고르면 그 질문들을 지금 값을 제안하며 다시 묻는다. `7` 은 어느 표의 어느 키든 바꾼다. `v` 는 원본을 보이고, `l` 은
검사하고, `s` 는 저장하고 검사하며, `q` 는 나간다(바뀐 것이 있으면 먼저 묻는다). 바꾼 값만 다시 쓴다: 내 주석과
순서, 손으로 쓴 표는 그대로 남는다.

옵션을 주면 `edit` 은 아무것도 묻지 않는다:

```text
C:\work\hello2> rubrapack edit hello.toml --set define.VERSION=1.1.0 --set package.summary-name=Hello --unset package.license
wrote hello.toml
lint: no problems
```

- `--set 표.키=값` 은 키를 정한다. TOML 로 쓴 값 - `"글"`, 수, `true`, `["ko"]` - 은 그대로 쓰고, 그 밖의 것은
  글로 받는다. 없는 표는 더한다.
- `--unset 표.키` 는 키를 지운다.

프로그램이 바뀐 뒤 - 여기서는 `readme.txt` 가 없어지고 `changes.txt` 가 새로 생겼다 - `--sync` 는 파일 목록을 폴더에
맞춘다:

```text
C:\work\hello2> rubrapack edit hello.toml --sync
  removed [file.readme_txt]
  added [file.changes_txt]
wrote hello.toml
lint: no problems
```

## 릴리스 스크립트

지금까지의 모든 것은 원본 밖에 상태가 없는 명령이므로, 릴리스는 짧은 스크립트가 된다. `hello.toml` 옆의
PowerShell 스크립트 `release.ps1`:

```text
param([Parameter(Mandatory)] [string] $Version)
$ErrorActionPreference = "Stop"
function Run { & rubrapack @args; if ($LASTEXITCODE -ne 0) { throw "rubrapack $args failed ($LASTEXITCODE)" } }

Run lint hello.toml --strict -D VERSION=$Version
foreach ($arch in "x64", "x86", "arm64") {
    Run build hello.toml -o "out\hello-$Version-$arch.msi" -D VERSION=$Version --arch $arch --reproducible `
        --key-store $env:SIGN_THUMBPRINT --timestamp http://timestamp.digicert.com
    Run lint "out\hello-$Version-$arch.msi" --strict --previous "released\hello-$arch.msi"
    Run verify "out\hello-$Version-$arch.msi" --system-roots
}
Run build hello.toml -o "out\hello-$Version.msixbundle" -D VERSION=$Version --arch x64,x86,arm64 `
    --key-store $env:SIGN_THUMBPRINT --timestamp http://timestamp.digicert.com
```

```text
PS C:\work\hello> .\release.ps1 -Version 2.2.0
```

`released\` 에는 바로 전 판의 패키지가 아키텍처마다 하나씩 있어서, `--previous` 가 각 업그레이드를 검사한다(3장).
스크립트는 실패한 첫 명령에서 멈춘다. rubrapack 의 종료 코드가 0 이 아니기 때문이다(18장). 지문은 환경 변수에서
오므로 스크립트에는 비밀이 없고, 원본과 함께 보관해도 된다.

## 빌드 서버에서

rubrapack 은 Linux 에서도 돌고, `--reproducible` 이면 거기서도 같은 바이트를 빌드하므로 빌드 서버(GitHub Actions,
GitLab CI, Jenkins)에 Windows 가 없어도 된다. Linux 러너에서:

```text
V=0.21.0
curl -sLo rubrapack "https://github.com/rubidus-api/rubrapack/releases/download/v$V/rubrapack-$V-linux-x86_64"
chmod +x rubrapack
./rubrapack lint hello.toml --strict
./rubrapack build hello.toml -o "hello-$VERSION.msi" -D VERSION="$VERSION" --reproducible \
    --key signer.pfx --pass-env SIGN_PASS --timestamp http://timestamp.digicert.com
```

키와 그 비밀번호는 서버의 비밀 저장소(예를 들어 GitHub Actions 저장소의 *secrets*)에서 이 단계 동안만 파일과 환경
변수로 온다. 절대 저장소에 넣지 않는다. 하드웨어 토큰이나 클라우드 서명 서비스의 키는 대신 `--pkcs11` 로 쓴다(16장).

## AI 도우미와 함께

원본은 짧은 일반 텍스트이고 모든 오류 메시지가 어디서 왜 틀렸는지 말하므로, AI 코딩 도우미가 위의 일을 모두 할 수
있다: 이 매뉴얼(책, <https://rubidus-api.github.io/rubrapack/>)을 주고 무엇을 설치할지 말한다. 도우미는 원본을 쓰고,
깨끗해질 때까지 `rubrapack lint` 를 돌리고, 빌드하고, `inspect` 와 `extract` 로 결과를 확인하고, 릴리스 스크립트를
쓸 수 있다. 둘은 내 몫으로 남는다:

- 첫 판의 업그레이드 코드를 이후 모든 판에 그대로 둔다(3장).
- 배포하기 전에 실제 Windows 에 패키지를 한 번 설치해 보고, 전 판에서의 업그레이드도 해 본다.

## 설치 프로그램 하나에 여러 패키지: `[chain]`

제품이 다른 패키지를 먼저 필요로 할 때가 있다 - 런타임, 드라이버, 두 번째 도구. *체인* 은 그것들을
차례로 설치하는 `setup.exe` 하나에 담는다. 패키지마다 자기 원본으로 빌드한 뒤, 그것들을 적는 작은 원본을 쓴다:

```toml
[chain]
name = "Hello Suite"
manufacturer = "Example Software"
version = "1.0.0"

[chain-package.Runtime]
source = "runtime.msi"

[chain-package.Hello]
source = "hello.msi"
properties = "INSTALLDIR=\"C:\\Tools\\Hello\""
vital = true
```

```text
C:\work\suite> rubrapack build suite.toml -o setup.exe
C:\work\suite> rubrapack inspect setup.exe
C:\work\suite> setup.exe /passive
```

설치 프로그램은 패키지마다 SHA-256 을 확인하고, 이미 설치된 것은 건너뛰며, 처음 실패한 `vital` 패키지에서
멈춘다. `setup.exe /uninstall` 은 뒤에서부터 다시 지운다. `[chain]` 의 `elevate = false` 는 관리자 권한을
묻지 않게 한다(사용자별 패키지용). 나머지는 [참고 문서](../rpk.md#설치-프로그램-하나에-여러-패키지-chain)에 있다.

## 여기서 어디로

rubrapack 의 모든 표와 모든 옵션을 써 보았다. 이제부터는:

- **제2부 참조**는 모든 표의 모든 키와 모든 옵션을 한곳에 모았다 - 원본을 쓰는 동안 열어 둘 페이지다.
- **제3부 기초 지식**은 이 모든 것 밑의 개념 - 비트와 바이트, 문자 인코딩, GUID, 해시, 압축, 서명 - 을
  처음부터 설명한다.
- **제4부 파일 형식**은 내가 빌드한 파일을 열어 모든 바이트를 보여 준다: MSI 의 파일 시스템, 표와 문자열, 캐비닛,
  MSIX 의 ZIP 과 블록 맵, 그리고 서명.
