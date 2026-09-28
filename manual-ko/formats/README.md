# 구현하는 사람을 위한 패키지 형식

English: [`../../manual/formats/`](../../manual/formats/README.md).

이 문서들은 Windows 가 받아들이는 Windows Installer 패키지(`.msi`)와 MSIX 패키지·묶음을 만들려면
프로그램이 무엇을 써야 하는지, 그리고 그것들에 어떻게 서명하는지를 설명한다. 공개 명세와 Windows
자체에 대한 실험으로 썼으므로, rubrapack 의 코드를 읽지 않고도 이것만으로 자기 도구를 만들 수 있다 -
또는 rubrapack 의 코드를 이것과 대조해 볼 수 있다.

| 문서 | 다루는 것 |
|---|---|
| [`cfb.md`](cfb.md) | 복합 파일(Compound File Binary): 모든 `.msi` 의 그릇 |
| [`msi-database.md`](msi-database.md) | 스트림, 문자열 풀, 시스템 표, 표 인코딩, IDT 내보내기 |
| [`msi-summary.md`](msi-summary.md) | 요약 정보 스트림과 그 코드 페이지 함정 |
| [`msi-package.md`](msi-package.md) | 설치·업그레이드·복구·제거를 하는 표들: 파일, 레지스트리, 바로가기, 서비스, 글꼴, 대화창, 사용자별 패키지, 순서 |
| [`pe.md`](pe.md) | 프로그램 파일: 머신 형식과 판 정보 리소스 |
| [`cab-mszip.md`](cab-mszip.md) | 캐비닛 파일, MSZIP 블록, deflate 인코더 |
| [`identity.md`](identity.md) | 결정적인 GUID 와 키, 재현 가능한 빌드 |
| [`authenticode.md`](authenticode.md) | PE 파일과 MSI 패키지의 Authenticode 서명: 다이제스트, Windows 가 받는 CMS 구조, ECDSA, RFC 3161 타임스탬프 |
| [`verify.md`](verify.md) | Windows 자신의 구성 요소로 결과를 확인하는 방법 |
| [`msix.md`](msix.md) | MSIX: ZIP 배치, 블록 맵, 매니페스트, 가상 레지스트리와 파일 시스템, 확장, 묶음, 서명 |
| [`registry.md`](registry.md) | MSIX `Registry.dat`·`User.dat` 가 담는 레지스트리 하이브 파일(REGF) |

## 사실을 읽는 법

모든 서술에는 출처 표시가 붙는다:

- **[spec]** - 공개 명세: [MS-CFB], [MS-OLEPS], [MS-CAB], [MS-MCI], RFC 1951, RFC 1321, RFC 3161,
  RFC 5652, RFC 9562, PKWARE ZIP APPNOTE, ECMA-376 Part 2(OPC), Authenticode PE 명세, 또는
  Microsoft Learn 의 Windows Installer·MSIX 참고 문서.
- **[observed]** - Windows 11 에서 잰 것: MSI 문서는 25H2(빌드 26200)와 Windows Installer 5.0.10011 에서
  `msi.dll` 로 데이터베이스를 만들어 바이트 단위로 다시 읽고 패키지를 설치해서, MSIX·레지스트리
  하이브·서명 문서는 빌드 26100 에서 Windows 자신의 패키징 API, 오프라인 레지스트리 라이브러리,
  서명기와 대조하고 패키지를 설치·실행해서 얻었다. 관찰은 그 빌드들에 대한 사실이다. Microsoft 가
  형식을 문서로 내지 않은 곳(MSI 표 인코딩, 레지스트리 하이브, MSIX 서명의 해시 기록은 공개되지
  않았다)에서는 관찰이 유일한 출처다.

여기의 어떤 글이나 코드도 다른 MSI·MSIX·캐비닛·레지스트리 구현에서 오지 않았다.

## 표기

- 따로 말하지 않으면 모든 정수는 리틀엔디언이다.
- `u16`, `u32`, `u64` 는 부호 없는 수, `i16`, `i32` 는 2의 보수 부호 있는 수다.
- 오프셋은 이름 붙은 구조의 처음부터 센 바이트 수다.
- 16진 바이트열은 파일에 놓인 순서로 쓴다: `D0 CF 11 E0`.

## 흐름을 한 그림으로

```text
원본 기술 --> 표(문자열/정수/스트림의 행)
         --> MSI 데이터베이스 인코딩(문자열 풀 + 표마다 스트림 하나)
         --> 캐비닛에 담은 파일(저장 또는 MSZIP) --> 스트림 하나 더
         --> 요약 정보 스트림
         --> 모든 스트림을 복합 파일 하나에 쓴다  = .msi
```

MSIX 패키지는 더 단순하다: 페이로드 파일(각각 독립된 64 KiB 조각으로 deflate), `AppxManifest.xml`,
`AppxBlockMap.xml`(조각마다 해시), `[Content_Types].xml` 이 ZIP64 압축 파일 하나에 들고, 레지스트리
값은 `Registry.dat`/`User.dat` 하이브에 든다. 묶음은 패키지를 나란히 담고, 서명은
`AppxSignature.p7x` 를 더한다([msix.md](msix.md), [registry.md](registry.md)).

화살표 하나가 위의 문서 하나다. 어느 것도 만드는 데 Windows 가 필요하지 않다: rubrapack 은 Linux 와
Windows 에서 같은 바이트를 만든다.
