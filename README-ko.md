**한국어** | [English](README.md) — **rubrapack v0.9.0** — [Linux(x64)](https://github.com/rubidus-api/rubrapack/releases/download/v0.9.0/rubrapack-0.9.0-linux-x86_64) · [EXE(x64)](https://github.com/rubidus-api/rubrapack/releases/download/v0.9.0/rubrapack-0.9.0-windows-x64.exe) · [PDF(ko)](https://github.com/rubidus-api/rubrapack/releases/download/v0.9.0/rubrapack-manual-0.9.0-ko.pdf) · [PDF(en)](https://github.com/rubidus-api/rubrapack/releases/download/v0.9.0/rubrapack-manual-0.9.0-en.pdf)

# rubrapack

**짧은 글 파일 하나로 Windows 설치 파일을.** rubrapack 은 프로그램을 몇 줄로 적은 설명을 Windows Installer
패키지(`.msi`)나 MSIX 패키지(`.msix`, `.msixbundle`)로 만들고, 서명하고, 검사합니다. Windows 에서도 Linux 에서도
프로그램 파일 하나로 돌아가며, 따로 설치할 것이 없습니다.

```sh
rubrapack new app.toml                    # 몇 가지를 묻고 app.toml 을 써서 검사합니다
rubrapack build app.toml -o app.msi       # 제대로 된 설치 파일: 제거 항목, 업그레이드, 복구
```

## 왜 rubrapack 인가

- **MSI 패키징이 쉬워집니다.** 설치할 것 - 파일, 폴더, 바로가기, 레지스트리 값, 서비스 - 을 짧은 `.toml`
  파일(평범한 TOML)에 적으면, Windows Installer 가 요구하는 데이터베이스 표, 컴포넌트 GUID, 캐비닛, 업그레이드
  규칙, 대화창은 rubrapack 이 씁니다 - 동의해야 넘어가는 약관 페이지, 설치 폴더 고르기, 골라서 설치하는 선택
  구성요소 트리까지, 영어와 한국어 등 여러 언어로. XML 도, 표 편집기도, GUID 관리도 필요 없습니다. 직접 간직할
  것은 업그레이드 코드 하나뿐입니다.
- **자동화하기 쉽고, AI 에게 맡기기는 더 쉽습니다.** 원본은 평범한 글이고 도구는 명령 하나이며, 모든 문제는
  `파일:줄:열: error[RPnnnn]: 설명` 꼴로, 흔히 고칠 방법까지 붙여 알려 줍니다(`unknown key 'glb' in [files.App]
  (did you mean 'glob'?)`). 종료 코드는 정해져 있고, `lint` 는 아무것도 쓰지 않고 검사만 하며, `inspect` 는 완성된
  표를 글로 보여 주고, 같은 원본은 언제나 같은 바이트가 됩니다. 그래서 스크립트나 CI 작업, AI 코딩 도우미가
  원본을 쓰고, 검사하고, 오류를 읽고, 고치고, 빌드하고, 그 결과를 스스로 확인할 수 있습니다. AI 도우미에게
  "이 프로그램을 rubrapack 으로 패키징해 줘"라고 맡기면 그 과정을 릴리스 스크립트로 만드는 일까지 시킬 수 있습니다.
- **설치할 것이 없습니다.** 파일 하나 - Windows 용 약 1 MB, Linux 용 0.9 MB - 를 내려받아 실행하면 끝입니다.
  .NET 도, Python 같은 인터프리터도, SDK 도, 라이브러리도 필요 없습니다. Linux 판은 C 라이브러리만, Windows 판은
  Windows 에 들어 있는 DLL 만 씁니다(`msi.dll` 조차 쓰지 않습니다). Windows 컴퓨터 없이 Linux 서버나 CI 에서
  Windows 패키지를 만들 수 있습니다.
- **순수 C 라 작고 빌드하기 쉽습니다.** C23 약 33,000 줄에 작은 기반 라이브러리를 함께 넣었고, 제3자 의존성은
  없습니다. 빌드에는 C 컴파일러 하나면 됩니다: `cc -std=c23 -o nob nob.c && ./nob` (보통 PC 에서 1분 안쪽, GCC 14,
  Clang 19, MinGW-w64 GCC 16 으로 확인). 복합 파일과 MSI 데이터베이스, 캐비닛과 deflate, ZIP 과 MSIX, 레지스트리
  하이브, 암호, PKCS#12, HTTP 와 TLS 까지 모두 이 저장소 안에서 공개 명세를 보고 구현했고 Windows 와 대조해
  확인했습니다.
- **결과를 믿을 수 있습니다.** 패키지는 Linux 에서도 Windows 에서도 바이트까지 똑같이 재현됩니다. 모든 기능은
  Windows 11 에서 패키지를 설치·실행·복구·업그레이드·제거하는 시험으로 확인합니다. `lint` 는 다른 도구가 만든
  MSI 도 검사하고, `lint new.msi --previous old.msi` 는 업그레이드를 망가뜨리는 실수를 사용자보다 먼저 잡아
  냅니다.

## 이럴 때 씁니다

- C, C++, Rust, Go, .NET, Python, Electron 등 무엇으로 만들었든 데스크톱 프로그램을 배포하면서, Program Files,
  시작 메뉴, "설치된 앱"의 항목, 깨끗한 제거와 업그레이드를 갖춘 제대로 된 Windows 설치 파일이 필요할 때.
- 릴리스를 Linux(CI, 컨테이너, 빌드 서버)에서 만들면서 서명된 Windows 설치 파일도 같은 과정에서 뽑고 싶을 때.
- 하드웨어 토큰(PKCS#11)이나 Windows 인증서 저장소에 있는 키로 서명해야 할 때.
- MSI 와 같은 원본에서 MSIX 패키지나 여러 아키텍처를 담은 묶음도 만들고 싶을 때.
- 누가 만들었든 MSI·MSIX 패키지를 들여다보고, 검사하고, 풀어 봐야 할 때.

## 한눈에 보기

```sh
rubrapack new app.toml                               # 물어본 뒤 app.toml 을 씁니다 (또는: new app.toml --dist dist ...)
rubrapack edit app.toml                              # 나중에 고치기: 메뉴, 또는 --set define.VERSION=1.1.0 --sync
rubrapack build app.toml -o app.msi                  # Windows Installer 패키지
rubrapack build app.toml -o app.msix --key signer.pfx --pass-env PW --timestamp http://timestamp.digicert.com
rubrapack build app.toml -o app.msixbundle --arch x64,x86,arm64
rubrapack lint app.msi && rubrapack verify app.msix --trust root.pem
```

첫 패키지를 한 단계씩 만드는 방법은 매뉴얼의 첫 장에 있습니다: [시작하기](manual-ko/rpk.md#시작하기).

## 자세한 기능

- **원본 하나로 두 형식을.** 선언형 `.toml` 파일(TOML 의 엄격한 부분집합; 예전 이름 `.rpk` 도 됩니다) 하나에 제품을 한 번 기술하면, 같은
  원본에서 MSI 와 MSIX 가 모두 나옵니다. 한쪽 형식이 담을 수 없는 것은 조용히 빼지 않고 오류로 알려 드립니다.
- **MSI:** 파일과 폴더(와일드카드, 남기거나 지우는 폴더), 기능, 레지스트리 값(REG_QWORD 와 32비트 보기 포함),
  바로가기, 파일 형식과 URL 스킴, 환경 변수, INI 파일, 서비스, 글꼴, 권한, 설치 조건과 검색, 되돌림이 되는
  등록·해제 프로그램 실행, 다운그레이드를 거부하는 메이저 업그레이드, 컴퓨터 전체·사용자별·겸용 패키지, 안이나
  밖에 두는 MSZIP 캐비닛, 내장 대화창 세트(영어에 한국어나 다른 언어를 같은 패키지 안에 덧붙일 수 있고,
  첫 페이지에서 언어를 고르며, 한국어 시스템에서는 한국어가 미리 골라집니다), 직접 만드는 대화창 페이지,
  기억하는 설치 폴더, 관리자 소유가 아닌 미리 만들어진 설치 폴더를 거부하는 가드를 지원합니다. 사용자가
  고르는 것도 지원합니다: 필수 기능과 설치 뒤 "변경"이 있는 기능 트리, 기능·파일·바로가기·설정에 거는
  조건(`when`), 겸용 패키지의 "나만 / 모든 사용자", 완료 페이지에서 프로그램 실행. "설치된 앱"과 바로가기의
  아이콘, 제거할 때 남기는 파일도 지원합니다. x64, x86, Arm64 패키지를 만듭니다.
- **MSIX:** 가상 레지스트리(`Registry.dat`, `User.dat`)와 가상 파일 시스템을 쓰는 완전 신뢰 데스크톱 앱,
  패키지 하나에 여러 앱, 파일 형식, 프로토콜, 실행 별칭, 시작 작업, 바탕화면 바로가기, 공유 글꼴, 여러
  아키텍처를 담는 묶음, 서명 없는 시험 패키지를 지원합니다.
- **서명:** PE 파일, MSI 와 MSIX 패키지와 묶음에 Authenticode 서명을 합니다. RSA 와 ECDSA 를 쓰고,
  RFC 3161 타임스탬프(HTTP 또는 자체 TLS 1.3)를 붙입니다. 키는 PFX/PEM 파일, PKCS#11 토큰(`--pkcs11`), 또는
  NCrypt 를 거친 Windows 인증서 저장소(`--key-store`)에서 가져옵니다. 그래서 하드웨어 밖으로 나가면 안 되는
  코드 서명 키도 그 자리에 둔 채 서명할 수 있습니다.
- **확인:** `lint`(원본과 어떤 도구로 만든 MSI·MSIX 든, 그리고 MSI 를 이전 판과 대조), `inspect`, `extract`, `verify`, `keys list`.
- **재현 가능하고 빠릅니다:** 같은 원본은 Linux 에서도 Windows 에서도, 압축하는 스레드 수와 상관없이 같은
  바이트가 됩니다(기본은 모든 프로세서).

## 하지 않는 일

- WiX 의 앞단이 아니고 WiX 와 호환되지도 않습니다. `.wxs` 파일은 읽지 않습니다.
- 패치(`.msp`), 변환(`.mst`), 병합 모듈 작성, 부트스트래퍼 묶음은 없습니다.
- 도구 자체는 64비트 전용입니다(x86 패키지도 만듭니다).

## 상태

0.7.0 입니다. 위의 모든 기능은 Linux 의 시험과, Windows 11(x64)에서 패키지를 설치·실행·복구·
업그레이드·제거하는 시험으로 확인했습니다. 아직 실제 하드웨어에서 시험하지 않은 것: Arm64 패키지(구조만 확인 -
Arm64 컴퓨터가 없습니다), 하드웨어 PKCS#11 토큰(소프트웨어 토큰으로 대신했습니다), Windows 에서의 네이티브
빌드(Windows 실행 파일은 MinGW-w64 로 교차 빌드해 Windows 에서 돌렸습니다).

## 문서

- [`manual-ko/rpk.md`](manual-ko/rpk.md) - 사용자 매뉴얼: 원본(`.toml`) 쓰기와 명령줄.
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
# Linux (GCC 14, Clang 19 로 확인): build/native/rubrapack 을 만듭니다
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
