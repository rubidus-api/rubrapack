# rubrapack

Windows 설치 패키지 - Windows Installer(`.msi`)와 MSIX(`.msix`, `.msixbundle`) - 를 만들고 서명하는 명령줄
도구입니다. Windows 와 Linux 에서 모두 돌아갑니다.

English: [README.md](README.md).

```sh
rubrapack new app                                   # 채워 넣을 원본 app.rpk 를 만듭니다
rubrapack build app.rpk -o app.msi                  # Windows Installer 패키지
rubrapack build app.rpk -o app.msix --key signer.pfx --pass-env PW --timestamp http://timestamp.digicert.com
rubrapack build app.rpk -o app.msixbundle --arch x64,x86,arm64
rubrapack lint app.msi && rubrapack verify app.msix --trust root.pem
```

## 하는 일

- **원본 하나로 두 형식을.** 선언형 `.rpk` 파일(TOML 의 엄격한 부분집합) 하나에 제품을 한 번 기술하면, 같은
  원본에서 MSI 와 MSIX 가 모두 나옵니다. 한쪽 형식이 담을 수 없는 것은 조용히 빼지 않고 오류로 알려 드립니다.
- **MSI:** 파일과 폴더(와일드카드, 남기거나 지우는 폴더), 기능, 레지스트리 값(REG_QWORD 와 32비트 보기 포함),
  바로가기, 파일 형식과 URL 스킴, 환경 변수, INI 파일, 서비스, 글꼴, 권한, 설치 조건과 검색, 되돌림이 되는
  등록·해제 프로그램 실행, 다운그레이드를 거부하는 메이저 업그레이드, 컴퓨터 전체·사용자별·겸용 패키지, 안이나
  밖에 두는 MSZIP 캐비닛, 내장 대화창 세트(한국어와 영어)와 직접 만드는 대화창 페이지를 지원합니다. x64,
  x86, Arm64 패키지를 만듭니다.
- **MSIX:** 가상 레지스트리(`Registry.dat`, `User.dat`)와 가상 파일 시스템을 쓰는 완전 신뢰 데스크톱 앱,
  패키지 하나에 여러 앱, 파일 형식, 프로토콜, 실행 별칭, 시작 작업, 바탕화면 바로가기, 공유 글꼴, 여러
  아키텍처를 담는 묶음, 서명 없는 시험 패키지를 지원합니다.
- **서명:** PE 파일, MSI 와 MSIX 패키지와 묶음에 Authenticode 서명을 합니다. RSA 와 ECDSA 를 쓰고,
  RFC 3161 타임스탬프(HTTP 또는 자체 TLS 1.3)를 붙입니다. 키는 PFX/PEM 파일, PKCS#11 토큰(`--pkcs11`), 또는
  NCrypt 를 거친 Windows 인증서 저장소(`--key-store`)에서 가져옵니다. 그래서 하드웨어 밖으로 나가면 안 되는
  코드 서명 키도 그 자리에 둔 채 서명할 수 있습니다.
- **확인:** `lint`(원본과 어떤 도구로 만든 MSI·MSIX 든), `inspect`, `extract`, `verify`, `keys list`.
- **재현 가능:** 같은 원본은 Linux 에서도 Windows 에서도 같은 바이트가 됩니다.
- **운영 체제 말고는 실행 의존성이 없습니다:** 복합 파일과 MSI 데이터베이스, 캐비닛과 deflate, ZIP/OPC 와
  블록 맵, 레지스트리 하이브, 암호, PKCS#12, HTTP 와 TLS 를 모두 이 저장소 안에서 공개 명세를 보고 구현했고,
  Windows 와 대조해 확인했습니다.

## 하지 않는 일

- WiX 의 앞단이 아니고 WiX 와 호환되지도 않습니다. `.wxs` 파일은 읽지 않습니다.
- 패치(`.msp`), 변환(`.mst`), 병합 모듈 작성, 부트스트래퍼 묶음은 없습니다.
- 도구 자체는 64비트 전용입니다(x86 패키지도 만듭니다).

## 상태

0.1.0, 첫 공개판입니다. 위의 모든 기능은 Linux 의 시험과, Windows 11(x64)에서 패키지를 설치·실행·복구·
업그레이드·제거하는 시험으로 확인했습니다. 아직 실제 하드웨어에서 시험하지 않은 것: Arm64 패키지(구조만 확인 -
Arm64 컴퓨터가 없습니다), 하드웨어 PKCS#11 토큰(소프트웨어 토큰으로 대신했습니다), Windows 에서의 네이티브
빌드(Windows 실행 파일은 MinGW-w64 로 교차 빌드해 Windows 에서 돌렸습니다).

## 문서

- [`manual-ko/rpk.md`](manual-ko/rpk.md) - 사용자 매뉴얼: `.rpk` 원본 쓰기와 명령줄.
- [`manual-ko/formats/`](manual-ko/formats/README.md) - 구현하는 분을 위한 파일 형식 매뉴얼: 복합 파일, MSI
  데이터베이스 인코딩, 요약 정보, 설치하는 표들, 캐비닛/MSZIP/deflate, 결정적 정체, Authenticode 와
  타임스탬프, MSIX 패키지·묶음·서명, 레지스트리 하이브, 그리고 자기 출력을 Windows 와 대조하는 방법.
- 두 매뉴얼을 묶은 책(PDF, 웹판): <https://rubidus-api.github.io/rubrapack/>
- 영어판이 원본입니다: [`manual/`](manual/README.md).

## 글자 인코딩

패키지 글자는 처음부터 끝까지 유니코드입니다. MSI 데이터베이스는 코드 페이지 65001(UTF-8)로 쓰고, Windows
Installer 가 설치할 때 UTF-16 으로 바꿉니다. 파일 이름, 폴더, 레지스트리 값, 바로가기에 어떤 문자든 쓸 수
있습니다.

## 빌드

```sh
# Linux (gcc 14+ 또는 clang 18+): build/native/rubrapack 을 만듭니다
cc -std=c23 -o nob nob.c && ./nob

# Windows (MinGW-w64 UCRT): build/native/rubrapack.exe 를 만듭니다
gcc -std=c23 -o nob.exe nob.c && nob.exe

# Linux -> Windows x64 교차 빌드: build/win64/rubrapack.exe 를 만듭니다
./nob --target=win64
```

`nob.c` 는 C23 컴파일러 하나만 있으면 됩니다. 옵션: `--sanitize`(AddressSanitizer 와 UBSan), `--debug`,
`clean`. `CC` 와 `RUBRAPACK_WIN64_CC` 로 컴파일러를 고릅니다. MSI 패키지가 REG_QWORD 값을 위해 싣는 도우미
DLL 은 `resources/bin/` 에 SHA-256 합과 함께 있고, `src/` 에서 바이트까지 똑같이 다시 빌드됩니다.

## 라이선스

MIT 입니다. `LICENSE` 와 `THIRD_PARTY_NOTICES.md` 를 보세요.
