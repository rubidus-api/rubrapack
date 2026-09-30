# GUID 와 해시

패키지 곳곳에 16~32 바이트짜리 수 두 종류가 나온다: 이름이 절대 겹치지 않게 무언가에 이름을 붙이는 **GUID**, 그리고
파일 내용을 요약해 어떤 변화든 드러나게 하는 **해시**. rubrapack 은 둘을 잇기도 한다: 해시*로부터* GUID 를 만든다.

## GUID

**GUID**(globally unique identifier, 표준에서는 UUID 라 부른다)는 128비트 - 16 바이트 - 수로, 16진수 32자리를
다섯 묶음으로 나누어, 보통 중괄호 안에 쓴다:

```text
{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}
 8 digits  4    4    4    12 digits
```

Windows 는 어디서도 다른 것과 헷갈리면 안 되는 것에 GUID 로 이름을 붙인다: 제품(제품 코드), 판을 넘어 이어지는 제품
가족(업그레이드 코드), 컴포넌트, 패키지 빌드 하나(패키지 코드), COM 개체의 종류. GUID 를 등록하는 곳은 없다. 아무도
나눠 주지 않는다. 너무 많아서 겹치지 않는 것이다: 무작위 GUID 는 2^122 가지, 약 5 x 10^36 개다. 컴퓨터 한 대가
초마다 무작위 GUID 10억 개를 만들어도, 그중 둘이 같을 확률이 절반에 이르려면 약 86년이 걸린다.

### 판과 변형

128비트가 다 자유롭지는 않다. 글 속의 두 자리가 어떤 GUID 인지 말한다:

```text
{98AE4FED-BF0B-4C77-B712-E0649EB47178}      rubrapack guid          (random)
               ^    ^
               |    variant: 8, 9, A or B (the bits 10xx)
               version: 4 = random

{B52FE8EF-B68D-84B5-91AF-E6E01BEC2773}      rubrapack guid --from hello   (derived)
               ^    ^
               |    variant: 9
               version: 8 = made by a method of the maker's own choosing
```

**판 4** GUID 는 무작위 비트 122개다: `rubrapack guid` 가 업그레이드 코드용으로 하나 만든다. **판 8** GUID 는 만든
쪽이 정한 방법으로 계산한 122비트다 - rubrapack 은 아래처럼 이름에서 계산한다.

### 글과 바이트

글로 쓴 GUID 는 32자리를 순서대로 적은 것이다. 이진 구조(복합 파일의 디렉터리, 프로그램 파일) 안에서 Windows 는
앞의 세 묶음을 little-endian 수(2장)로, 뒤의 두 묶음을 바이트 그대로 저장한다:

```text
  text:   B52FE8EF - B68D - 84B5 - 91AF - E6E01BEC2773
  bytes:  ef e8 2f b5  8d b6  b5 84  91 af  e6 e0 1b ec 27 73
          (reversed)  (rev.) (rev.) (as written)
```

MSI 표는 GUID 를 대문자, 중괄호 붙은 글로 둔다. 섞인 순서는 제4부에서만 중요하다.

## 해시

**해시 함수**는 얼마든지 많은 데이터를 읽어 짧고 크기가 정해진 수, **해시**(또는 *다이제스트*)를 만든다. 좋은
해시 함수에는 세 성질이 있다:

1. **같은 입력, 같은 해시** - 어느 컴퓨터에서든, 언제든.
2. **조금만 바뀌어도 전부 바뀐다.** 입력의 비트 하나를 바꾸면 해시 비트의 절반쯤이 눈에 띄는 무늬 없이 바뀐다.
3. **한 방향.** 해시로부터 그 해시를 내는 입력을 찾을 수 없다 - 끝없이 입력을 시험해 보는 것 말고는.

튜토리얼의 `guide.txt`, 여섯 바이트 `47 75 69 64 65 0a`("Guide" 와 줄 바꿈)와, `g` 를 소문자로 바꿔 비트 하나만
다른 것(`47` = `0100 0111`, `67` = `0110 0111`)을 보자:

| 입력 | MD5 (16 바이트) |
|---|---|
| `Guide\n` | `2b 67 39 28 1f 93 72 44 2d f3 0e 78 4b 7d bd c7` |
| `guide\n` | `e5 24 11 d5 57 3e b8 9b 5c 31 26 97 79 50 d3 ef` |

| 입력 | SHA-256 (32 바이트) |
|---|---|
| `Guide\n` | `32 74 fc ad 88 6c de 4e 2c a8 6b 11 d3 0f d7 c4 48 58 ea df 1c 43 7a 95 83 f3 1e 78 15 db 1a f6` |
| `guide\n` | `90 c3 90 ec 1d e8 06 bf 94 58 85 cd 0a f5 1e 90 c3 cd 8c da 0d 0f f6 76 05 1a 56 c2 08 48 c9 0f` |

PowerShell 에서 해 본다: `Get-FileHash -Algorithm MD5 dist\docs\guide.txt`.

흔한 해시 함수:

| 이름 | 크기 | 오늘날의 쓰임 |
|---|---|---|
| MD5 | 128비트 | 변화를 알아차리는 데만. 보안에는 깨졌다(MD5 가 같은 다른 입력을 일부러 만들 수 있다) |
| SHA-1 | 160비트 | 마찬가지. 인증서의 *지문*(8장)으로는 아직 쓰인다 |
| SHA-256 | 256비트 | 서명의 표준이고, rubrapack 이 GUID 를 끌어낼 때 쓴다 |

### 튜토리얼 패키지 속의 해시

**`MsiFileHash`**: 판 번호 없는 파일마다 Windows Installer 는 MD5 를 보관했다가, 복구나 업그레이드 때 디스크의
파일이 아직 패키지가 설치한 그 파일인지 판단한다. `inspect --files` 가 그것을 16진수로 보인다(튜토리얼
[18](../tutorial/18-checking-and-looking-inside.md)장). 표 자체는 그것을 4바이트 수 넷으로 저장한다 - 16 바이트를
넷씩, little-endian 부호 있는 수로 읽은 것이다(2장):

```text
C:\work\hello> rubrapack inspect hello.msi MsiFileHash
File_	Options	HashPart1	HashPart2	HashPart3	HashPart4
...
F_6d1508f52f6615459567	0	674850603	1148359455	2014245677	-943882933

2b 67 39 28 -> 0x2839672B =  674850603
1f 93 72 44 -> 0x4472931F = 1148359455
2d f3 0e 78 -> 0x780EF32D = 2014245677
4b 7d bd c7 -> 0xC7BD7D4B = 3351084363, top bit set, minus 2^32 = -943882933
```

- **MSIX 블록 맵**(튜토리얼 [17](../tutorial/17-msix-and-bundles.md)장): 모든 파일의 64 KiB 블록마다 SHA-256.
- **서명**(튜토리얼 [16](../tutorial/16-signing.md)장): 서명하는 것은 패키지가 아니라 패키지의 해시다.
- **`--reproducible`**(튜토리얼 [15](../tutorial/15-cabinets-and-architectures.md)장): 패키지 코드를 내용의
  해시에서 끌어낸다.

## 해시로 만드는 GUID: `guid --from`

컴포넌트의 GUID 는 판이 바뀌어도 같아야 하지만 - 업그레이드가 그것에 기댄다 - 누구도 수백 개를 적어 두고 싶지는
않다. 그래서 rubrapack 은 하나하나를 *끌어낸다*: 그것을 가리키는 글(제품과 파일의 자리)을 해시해 GUID 로 바꾼다.
같은 글은 언제나 같은 GUID 를, 다른 글은 다른 GUID 를 낸다.

`rubrapack guid --from <글>` 은 어떤 글이든 그 계산의 결과를 보인다. `hello` 로 한 단계씩:

**1. 입력 바이트를 만든다.** 부분마다 길이를 4바이트 little-endian 수로 쓰고 그 바이트를 잇는다: 낱말 `guid`,
수 1(방법의 판), 그리고 UTF-8 로 쓴 글.

```text
04 00 00 00  67 75 69 64  01 00 00 00  05 00 00 00  68 65 6c 6c 6f
length 4     "guid"       1            length 5     "hello"
```

길이를 적으면 부분이 모호하지 않다: `guid` + `hello` 가 `guidh` + `ello` 와 헷갈릴 수 없다.

**2. SHA-256 으로 해시한다:**

```text
b5 2f e8 ef b6 8d f4 b5 d1 af e6 e0 1b ec 27 73 a2 8a 56 80 0f 9f 53 1b cd dc ee 7c f5 c8 59 90
```

**3. 앞의 16 바이트를 남긴다:** `b5 2f e8 ef b6 8d f4 b5 d1 af e6 e0 1b ec 27 73`.

**4. 판과 변형을 표시한다.** 바이트 6 의 위 네 비트를 `1000`(판 8)으로: `f4` 가 `84` 가 된다. 바이트 8 의 위 두
비트를 `10`(변형)으로: `d1` = `1101 0001` 이 `1001 0001` = `91` 이 된다.

```text
b5 2f e8 ef b6 8d 84 b5 91 af e6 e0 1b ec 27 73
                  ^^    ^^
```

**5. GUID 로 쓴다.** 바이트를 순서대로, 대문자로:

```text
C:\work\hello> rubrapack guid --from hello
{B52FE8EF-B68D-84B5-91AF-E6E01BEC2773}
```

어느 프로그램이든 이것을 되풀이하면 같은 GUID 를 얻는다. 제4부 [결정적인 정체](../formats/identity.md)가
rubrapack 이 종류마다 해시하는 글을 정확히 적어 두었다.

## 쓰이는 곳

- 튜토리얼 [2](../tutorial/02-a-first-installer.md)장: `rubrapack guid` 로 만든 업그레이드 코드.
- 튜토리얼 [4](../tutorial/04-files-and-folders.md)장과 [15](../tutorial/15-cabinets-and-architectures.md)장:
  끌어낸 컴포넌트 GUID 와 패키지 코드.
- 튜토리얼 [16](../tutorial/16-signing.md), [17](../tutorial/17-msix-and-bundles.md),
  [18](../tutorial/18-checking-and-looking-inside.md)장: 서명, 블록 맵, 파일 해시.
- 제4부: [결정적인 정체](../formats/identity.md), [Authenticode](../formats/authenticode.md),
  [MSIX](../formats/msix.md).
