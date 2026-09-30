# 파일 속의 파일

패키지는 여러 파일을 담은 파일 하나다: `.msi` 는 표, 요약 정보, 캐비닛을 담고, `.msix` 는 프로그램 파일, 로고,
매니페스트를 담는다. 파일 안에 파일을 넣는 고전적인 방법은 두 가지 - 작은 **파일 시스템**과 **묶음 파일**(archive) -
이고, 두 패키지 형식이 하나씩 쓴다. 이 장은 튜토리얼의 패키지로 둘을 설명한다.

## 디스크가 파일을 저장하는 방법

디스크는 같은 크기의 조각, **섹터**(또는 *클러스터*)로 나뉜다 - 이를테면 하나에 4096 바이트 - 그리고 0, 1, 2, ...
로 번호가 붙는다. 파일이 한 조각에 다 들어가는 일은 드물고, 받은 섹터들이 나란히 있을 필요도 없다. 그래서 파일
시스템은 데이터 말고 두 가지를 더 둔다:

- **디렉터리**: 파일마다 이름, 크기, 그리고 *첫* 섹터.
- **할당 표**: 섹터마다 같은 파일의 *다음* 섹터 번호, 또는 "마지막"이나 "비어 있음" 표시.

파일을 읽으려면 디렉터리에서 첫 섹터를 찾고, 표를 따라 섹터에서 섹터로 - **사슬**을 따라 - "마지막" 표시까지 간다:

```text
  directory:  report.txt   size 10,000   first sector 4

  allocation table:
    sector:  0     1     2     3     4     5     6     7
    next:    -     -     -     -     5     7     free  end

  report.txt = sector 4 -> sector 5 -> sector 7 -> end    (3 x 4096 bytes, the last one partly used)
```

USB 메모리의 FAT 파일 시스템이 이렇게 동작한다(FAT: *file allocation table*, 파일 할당 표). 파일을 지우면 섹터를
비었다고 표시할 뿐이고, 파일이 커지면 사슬에 섹터를 더 이을 뿐이다.

## MSI 는 파일 속의 파일 시스템이다

`.msi` 는 **복합 파일**(Compound File, Microsoft 의 "구조화된 저장소")이다: 위의 방식 그대로를 파일 하나 안에 둔
것이다. 튜토리얼의 첫 `hello.msi`, 28,672 바이트를 뜯어 보면:

```text
  offset 0x0000  header           (the 512-byte header, padded to one 4096-byte sector)
  offset 0x1000  sector 0         the allocation table (FAT)
  offset 0x2000  sector 1         the directory
  offset 0x3000  sector 2         the mini FAT (see below)
  offset 0x4000  sector 3         the mini stream (see below)
  offset 0x5000  sector 4         cab1.cab, first part
  offset 0x6000  sector 5         cab1.cab, second part
```

머리가 처음 4096 바이트를 차지하므로 섹터 *n* 은 바이트 (*n* + 1) x 4096 에서 시작한다. 머리(1장)가 나머지가 어디
있는지 말한다: 오프셋 `0x30` 의 `01 00 00 00` - 디렉터리는 섹터 1 에서 시작한다. 섹터 0 의 할당 표를 4바이트
little-endian 수(2장)로 읽으면:

| 섹터 | 값 | 뜻 |
|---|---|---|
| 0 | `0xFFFFFFFD` | 이 섹터는 할당 표 자신을 담는다 |
| 1 | `0xFFFFFFFE` | 사슬의 끝: 디렉터리는 섹터 하나 |
| 2 | `0xFFFFFFFE` | 사슬의 끝: 미니 FAT 은 섹터 하나 |
| 3 | `0xFFFFFFFE` | 사슬의 끝: 미니 스트림은 섹터 하나 |
| 4 | `5` | 다음은 섹터 5 |
| 5 | `0xFFFFFFFE` | 사슬의 끝: `cab1.cab` 은 섹터 4 와 5 |
| 6 ... | `0xFFFFFFFF` | 비어 있음(섹터가 더 없다) |

안에 든 파일을 **스트림**이라 부른다. 디렉터리에는 스트림마다 128 바이트 항목이 하나씩 있다: UTF-16(3장)으로 쓴 이름,
종류, 첫 섹터, 크기. 첫 항목은 언제나 뿌리(root)다:

```text
  52 00 6f 00 6f 00 74 00 20 00 45 00 6e 00 74 00 72 00 79 00 00 00 ...
   R     o     o     t           E     n     t     r     y   (end)
```

### 작은 스트림: 미니 스트림

4096 바이트 섹터에 20 바이트짜리 표를 넣으면 공간 대부분이 버려진다. 그래서 4096 바이트(머리의 오프셋 `0x38` 에 있는
수)보다 작은 스트림은 **미니 스트림**이라는 스트림 하나에 함께 두고, 64 바이트 **미니 섹터**로 잘라, 자기 표인
**미니 FAT** 으로 관리한다. 이 패키지에서는 스트림 열아홉 개 - 4~1,392 바이트짜리 표 열여덟과 332 바이트 요약
정보 - 가 섹터 3 의 3,840 바이트 미니 스트림을 나눠 쓴다. 보통 섹터를 쓸 만큼 큰 것은 6,776 바이트 캐비닛뿐이다.
같은 방식을 한 단계 아래에서 한 번 더 쓴 것이다.

### 이름

MSI 의 스트림 이름은 이상하게 보인다 - `䡀㼿䕷䑬㹪䒲䠯`. 이름 하나에 허용되는 31 글자에 더 많은 글자를 넣으려고
Windows Installer 가 표 이름을 한중일 문자 영역의 글자로 눌러 담기 때문이다. `rubrapack inspect hello.msi --streams`
(튜토리얼 18장)는 그것을 풀어 보인다: `File`, `Component`, `cab1.cab`, ... 이름 하나는 평범하다:
`\005SummaryInformation`, 곧 요약 정보로, 복합 파일을 다루는 어느 도구든 읽을 수 있다.

## MSIX 는 묶음 파일이다

**묶음 파일**은 파일 시스템보다 단순하다: 파일을 하나씩, 저마다 작은 머리 뒤에 차례로 쓰고, 끝의 목록이 각각 어디
있는지 말한다. 파일을 제자리에서 키울 수는 없지만 패키지는 그럴 일이 없다. MSIX 가 쓰는 ZIP 형식은 이렇게 생겼다:

```text
  "PK 03 04"  local header: name, sizes, method   + the file's (compressed) bytes     hello.exe
  "PK 03 04"  local header                        + bytes                             guide.txt
  ...                                                                                  (9 files)
  "PK 01 02"  central directory: one entry per file, with where its local header is
  "PK 06 06"  end record: how many files, where the central directory starts
```

튜토리얼의 `hello.msix`(11,009 바이트)는 `50 4b 03 04` - ZIP 을 만든 Phil Katz 의 머리글자 "PK" - 로 시작하고 이름
`hello.exe` 가 뒤따른다. 중앙 디렉터리는 바이트 10,098 에서 시작해 항목 아홉을 나열한다. 읽는 쪽은 먼저 파일 끝으로
가서 끝 기록을 찾고, 거기서 목록으로 간다. (MSIX 는 4 GiB 넘는 묶음을 위해 만든 ZIP64 형태의 끝 기록 `PK 06 06` 을
쓴다.)

항목마다 방법(5장)을 적는다: deflate 는 8, 저장은 0 - 여기서는 PNG 로고 셋이 0, 나머지는 모두 8 이다.

## 왜 두 설계인가?

| | 복합 파일(MSI) | ZIP(MSIX) |
|---|---|---|
| 만든 목적 | 제자리에서 고치는 문서(1990년대의 Word, Excel) | 한 번 싸서 여러 번 읽는 파일 |
| 스트림 바꾸기 | 그 사슬을 다시 쓴다 | 묶음 전체를 다시 쓴다 |
| 작은 조각 | 미니 스트림에서 싸다 | 조각마다 30 바이트 넘는 머리가 붙는다 |
| 한 조각 읽기 | 사슬을 따라간다 | 중앙 디렉터리에서 찾는다 |

Windows Installer 는 1999년에 Office 가 쓰던 저장소 위에 만들어졌고, MSIX 는 2018년에 `.docx` 파일이 쓰는 포장
표준(OPC) 위에 만들어졌다 - 그것이 ZIP 이다. 둘 다 "파일 속의 파일"이다.

## 쓰이는 곳

- 튜토리얼 [18](../tutorial/18-checking-and-looking-inside.md)장: `inspect --streams`, `extract`.
- 제4부: [복합 파일](../formats/cfb.md), [MSI 데이터베이스](../formats/msi-database.md)(스트림 이름),
  [MSIX](../formats/msix.md)(ZIP 구조).
