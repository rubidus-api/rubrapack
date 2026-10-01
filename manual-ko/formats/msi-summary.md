# 요약 정보

모든 패키지에는 `\x05SummaryInformation` 스트림이 있다(첫 글자는 U+0005, 이름은 압축하지 않는다).
[MS-OLEPS]가 정한 OLE 속성 집합이고, Microsoft Learn("Summary Information Stream Property Set")이 설치
패키지가 어떤 속성을 쓰는지 알려 준다.

## 배치 [spec] [observed]

```text
u16  바이트 순서     FFFE
u16  판              0 (msi.dll 은 0 을 쓴다)
u32  OS 필드         msi.dll 은 0x00020206 을 쓴다
16   CLSID           0
u32  구역 수         1
16   FMTID           F29F85E0-4FF9-1068-AB91-08002B27B3D9, 저장은 E0 85 9F F2 F9 4F 68 10 AB 91 08 00 2B 27 B3 D9
u32  구역 오프셋     48

구역:
u32  구역 크기
u32  속성 수
수만큼 (u32 속성 id, u32 구역 처음부터의 오프셋)
값들, 각각 4바이트 배수로 채움:
  VT_I2 (2)        u32 형식, i16 값, 채움 2바이트
  VT_I4 (3)        u32 형식, i32 값
  VT_LPSTR (30)    u32 형식, NUL 포함 u32 바이트 수, 바이트, NUL, 채움
  VT_FILETIME (64) u32 형식, 1601-01-01 UTC 부터의 100ns 간격 수 u64
```

## 설치 패키지가 정하는 속성

| Id | 이름 | 형식 | 값 |
|---|---|---|---|
| 2 | Title | LPSTR | 예 `Installation Database` |
| 3 | Subject | LPSTR | 제품 이름 |
| 4 | Author | LPSTR | 제조사 |
| 5 | Keywords | LPSTR | 예 `Installer` |
| 7 | Template | LPSTR | `<플랫폼>;<언어 id>`: `x64;1033`, `Arm64;1042`, `Intel;1033` |
| 9 | Revision Number | LPSTR | **패키지 코드**: GUID, 서로 다른 패키지 파일마다 새로 |
| 12, 13 | Create / Last Save time | FILETIME | 선택 |
| 14 | Page Count | I4 | 최소 설치 엔진 판: x64 와 x86 은 200, Arm64 는 500 |
| 15 | Word Count | I4 | 비트 1 = 파일이 (캐비닛에) 압축되어 있음; 보통 패키지는 2 |
| 18 | Creating Application | LPSTR | 아무것 |

## 코드 페이지 함정 - 요약 문자열은 ASCII 로 [observed]

속성 1(`CodePage`, VT_I2)은 LPSTR 바이트의 인코딩을 말한다. Windows 11 25H2 에서:

- **65001:** `msi.dll` 은 문자열을 UTF-8 로 제대로 쓰지만, 코드 페이지를 부호 있는 값 -535 로 다시 읽어
  모든 문자열 변환에 실패하고(설치 로그: "Failed to retrieve # 9 summary info property. GetLastError
  returned 87, the code page is -535") 오류 1620 으로 패키지를 거부한다.
- **1200(UTF-16):** 똑같이 거부한다(로그: "... the code page is 1200"). [MS-OLEPS]대로 완전한 UTF-16
  문자열과 바이트 수를 써도 그렇다.
- **코드 페이지 속성 없음, ASCII 문자열:** 설치된다.

그러니 속성 1 은 빼고 요약 문자열은 모두 ASCII 로 쓴다. 지역화한 제품 이름은 데이터베이스 코드 페이지를
쓰는 `Property` 표(`ProductName`)에 둔다 - 거기서는 65001 이 통한다([msi-database.md](msi-database.md)
참고).

## 실제 예: 튜토리얼의 hello.msi

튜토리얼의 첫 패키지: 스트림은 336 바이트다. 머리는:

| 오프셋 | 바이트 | 필드 | 값 |
|---|---|---|---|
| `0x00` | `fe ff` | 바이트 순서 | 0xFFFE |
| `0x04` | `06 02 02 00` | OS | 0x00020206 |
| `0x18` | `01 00 00 00` | 구역 수 | 1 |
| `0x1C` | `e0 85 9f f2 ...` | FMTID | `F29F85E0-...` |
| `0x2C` | `30 00 00 00` | 구역 오프셋 | 48 |
| `0x30` | `20 01 00 00` | 구역 크기 | 288 |
| `0x34` | `09 00 00 00` | 속성 수 | 9 |

그 뒤에 (속성 번호, 오프셋) 짝 9개와 값들:

| 속성 | 오프셋 | 종류 | 원래 바이트 | 값 |
|---|---|---|---|---|
| 2 | `0x80` | VT_LPSTR | `1e 00 00 00 16 00 00 00 49 6e 73 74 61 6c 6c 61 ...` | `Installation Database` |
| 3 | `0xA0` | VT_LPSTR | `1e 00 00 00 06 00 00 00 48 65 6c 6c 6f 00` | `Hello` |
| 4 | `0xB0` | VT_LPSTR | `1e 00 00 00 11 00 00 00 45 78 61 6d 70 6c 65 20 ...` | `Example Software` |
| 5 | `0xCC` | VT_LPSTR | `1e 00 00 00 0a 00 00 00 49 6e 73 74 61 6c 6c 65 ...` | `Installer` |
| 7 | `0xE0` | VT_LPSTR | `1e 00 00 00 09 00 00 00 78 36 34 3b 31 30 33 33 ...` | `x64;1033` |
| 9 | `0xF4` | VT_LPSTR | `1e 00 00 00 27 00 00 00 7b 30 37 36 46 44 35 38 ...` | `{076FD588-FECC-8FF1-938F-04D68A1CA9EA}` |
| 14 | `0x124` | VT_I4 | `03 00 00 00 c8 00 00 00` | 200 |
| 15 | `0x12C` | VT_I4 | `03 00 00 00 02 00 00 00` | 2 |
| 18 | `0x134` | VT_LPSTR | `1e 00 00 00 11 00 00 00 72 75 62 72 61 70 61 63 ...` | `rubrapack 0.23.0` |

문자열 값은 형식(30), NUL 을 포함한 바이트 수, 바이트, NUL, 그리고 4 의 배수까지 채움이다. 속성 9 인 패키지 코드는 `--reproducible` 이면 내용에서
끌어내고, 없으면 빌드마다 새것이다.
