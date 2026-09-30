# 캐비닛, 압축, 큰 패키지, 여러 아키텍처

목표: 파일을 어떻게 담을지 - 압축, 패키지 파일 하나 또는 패키지와 캐비닛들 - 를 정하고, 원본 하나로 Hello 를
x64, x86, Arm64 용으로 빌드하고, 빌드가 바이트 하나까지 되풀이되게 한다.

## 폴더

이제 프로그램은 처리기(프로세서) 종류마다 하나씩, 세 벌로 빌드되어 있다:

```text
dist\
    x64\hello.exe
    x86\hello.exe
    arm64\hello.exe
    docs\guide.txt
```

## 원본

```toml
# tutorial 15: hello.toml
[package]
name = "Hello"
manufacturer = "Example Software"
version = "$(VERSION)"
arch = "x64"
upgrade-code = "{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}"
upgrade-code-x86 = "{7D1C2B3A-4E5F-4061-9728-3A4B5C6D7E8F}"
upgrade-code-arm64 = "{0E9F8D7C-6B5A-4948-8372-6150F4E3D2C1}"
compress = "mszip:9"
cab = "external"
cab-max-size = 500

[define]
VERSION = "2.0.0"

[dir.INSTALLDIR]
path = "ProgramFiles/Hello"

[file.Hello]
dir = "INSTALLDIR"
source = "dist/$(ARCH)/hello.exe"

[files.DocFiles]
dir = "INSTALLDIR"
glob = "dist/docs/**"
```

## 캐비닛과 압축

MSI 안의 파일은 하나하나 따로 저장되지 않는다: *캐비닛*(`.cab`)에 담긴다. 캐비닛은 Windows 가 1990년대부터 써 온
압축 묶음 형식이다. `[package]` 의 키:

| 키 | 값 | 효과 |
|---|---|---|
| `compress` | `mszip:6`(기본값), `mszip:0` .. `mszip:9`, `mszip`(= 6), `none` | 얼마나 세게 압축할지. 9 가 가장 작고 느리며, 1 이 가장 빠르다. `none` 은 파일을 그대로 저장한다 |
| `cab` | `embed`(기본값), `external` | 캐비닛을 `.msi` 안에 둘지, 옆에 `hello-x64.cab` 으로 둘지 |
| `cab-max-size` | MiB 수 | 데이터가 그만큼 차면 새 캐비닛을 시작한다(`-1.cab`, `-2.cab`, ...) |

대부분의 프로그램에는 기본값이 맞다: 모든 것을 담은 `.msi` 하나. 외부 캐비닛은 아주 큰 제품을 위한 것이다:
**2 GiB 이상인 MSI 는 Windows Installer 가 열지 못한다.** 그래서 그만큼 큰 제품은 `cab = "external"` 을 써야
하고(rubrapack 은 넣어 두는 형태를 `RP1516` 으로 거부한다), 그때는 `.msi` 와 `.cab` 파일들을 늘 함께 - 같은
폴더, 같은 공유 폴더에 - 두어야 한다. `cab-max-size` 가 없으면 외부 캐비닛은 하나가 2 GiB 아래가 되게 나뉜다.

MiB(메비바이트)는 1024 x 1024 바이트다. MB 와 GiB 같은 단위는 제3부에서 설명한다.

압축은 컴퓨터의 모든 처리기에서 동시에 돈다. `--jobs 2` 는 많아야 둘만 쓴다. 처리기 16개로 200 MB 프로그램을
`mszip:6` 으로 압축하면 5초쯤 걸리고 105 MB 가 된다.

## 여러 아키텍처: `--arch`

`arch = "x64"` 는 원본 자신의 아키텍처다. `--arch x86` 은 같은 원본을 다른 아키텍처로 빌드한다:

```text
C:\work\hello> rubrapack build hello.toml -o out-x64\hello-x64.msi --arch x64
C:\work\hello> rubrapack build hello.toml -o out-x86\hello-x86.msi --arch x86
C:\work\hello> rubrapack build hello.toml -o out-arm64\hello-arm64.msi --arch arm64
```

- `$(ARCH)` 는 내장 변수로, 지금 빌드하는 아키텍처인 `x64`, `x86`, `arm64` 중 하나다. 여기서는 x86 패키지에
  `dist/x86/hello.exe` 를 고른다. rubrapack 은 프로그램마다 이것과 맞는지 확인한다.
- `ProgramFiles` 도 따라간다: x64 와 Arm64 는 `C:\Program Files`, x86 은 `C:\Program Files (x86)`.
- **아키텍처마다 업그레이드 코드가 따로 있어야 한다** - `upgrade-code-x86`, `upgrade-code-arm64`(원본 자신의
  `arch` 가 다른 것이라면 `upgrade-code-x64` 도). Windows 의 업그레이드 논리는 아키텍처를 구별하지 못한다: 코드
  하나를 함께 쓰면 x86 패키지를 설치할 때 x64 패키지가 "옛 판"으로 지워진다. 그것들이 없으면 `--arch` 는
  거부된다.

외부 캐비닛은 패키지 이름을 따서 이름이 붙고 서로 부딪치면 안 되므로, 여기서는 출력마다 폴더를 따로 두었다.

## 매번 같은 바이트: `--reproducible`

보통의 빌드는 패키지마다 새 무작위 *패키지 코드*(파일 요약 정보에 든 GUID)를 주므로, 같은 원본을 두 번 빌드해도
결과가 다르다. `--reproducible` 은 그것을 내용에서 끌어낸다: 같은 원본과 파일은 어느 컴퓨터에서든, 처리기가
몇 개든 바이트 하나까지 같은 `.msi` 를 낸다:

```text
C:\work\hello> rubrapack build hello.toml -o r1.msi --reproducible --jobs 1
C:\work\hello> rubrapack build hello.toml -o r2\r1.msi --reproducible
C:\work\hello> fc /b r1.msi r2\r1.msi
FC: 다른 점이 없습니다.
```

(`fc /b` 는 두 파일을 바이트 단위로 비교하는 Windows 명령이다.) 그러면 누구나 공개된 패키지가 공개된 원본에서
빌드되었는지 확인할 수 있다. 패키지 코드는 `rubrapack inspect hello.msi --summary` 가 보여 준다(속성 `9`).

## Mac 에서 온 이름: `--nfc`

유니코드는 `한` 이나 `é` 같은 글자를 두 가지로 쓸 수 있다 - 코드 포인트 하나로, 또는 글자와 결합 표시들로. macOS
파일 시스템은 이름을 두 번째 형태로 저장한다. `--nfc` 는 파일과 폴더 이름을 Windows 가 쓰는 첫 번째 형태(NFC)로
바꾼다. `lint` 는 NFC 가 아닌 이름을 경고한다(`RP2105`). 유니코드 정규화는 제3부가 설명한다.

## 명령줄의 변수: `-D`

`-D NAME=VALUE` 는 어느 `[define]` 변수든 정하고, 몇 번이든 줄 수 있다: `-D VERSION=2.0.1 -D EDITION=Pro`.
`$$(` 는 글자 그대로의 `$(` 를 쓴다.

## 안에서 무슨 일이 일어났나

```text
C:\work\hello> rubrapack inspect out-x64\hello-x64.msi Media
1	2		hello-x64.cab
C:\work\hello> rubrapack inspect out-x86\hello-x86.msi --summary
...
7	Intel;1033
```

`Media` 표는 캐비닛을 나열한다: 디스크 1 이 파일 1~2 를 `hello-x64.cab` 에 담는다(패키지 안에 넣은 캐비닛은
`#cab1.cab` 으로 쓴다. `#` 은 "이 파일 안에"라는 뜻이다). 요약 정보의 *템플릿*(속성 7)은 플랫폼(`x64`, `Arm64`,
x86 은 `Intel`)과 언어를 적는다. 제4부가 캐비닛 형식 - CFHEADER, CFFOLDER, CFDATA - 과 MSZIP 압축을 바이트
단위로 보여 준다.
