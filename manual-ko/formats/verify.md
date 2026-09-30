# 출력을 Windows 와 대조하기

Microsoft 는 MSI 표 인코딩을 공개하지 않으므로 기준은 Windows 자신이다. 이 확인에는 Windows 컴퓨터와
Windows Installer API(`msi.dll`)만 있으면 된다. SDK 도구는 필요 없다.

## 데이터베이스가 열리고 풀린다

1. 파일을 `MsiOpenDatabase` 로 읽기 전용으로 연다 - 복합 파일과 문자열 풀이 받아들여진다는 뜻이다.
2. 모든 표(와 `_ForceCodepage`, `_SummaryInformation`)를 `MsiDatabaseExport` 로 IDT 파일로 내보낸다.
3. 자기 판독기가 내보낸 것과, 또는 `msi.dll` 로 만든 기준 데이터베이스(`MsiOpenDatabase` 만들기 모드,
   `65001` 이 든 `_ForceCodepage.idt` 의 `MsiDatabaseImport`, 뷰를 통한 SQL `CREATE TABLE`/`INSERT`)의
   IDT 와 비교한다. 바이트까지 같은 IDT 는 인코딩이 맞다는 뜻이다.

## 패키지가 설치된다

설치 엔진은 `msiexec` 보다 프로그램에서 부른다. 그래야 단계마다 동기로 돌고 오류 코드를 돌려준다:

| 단계 | API | 기대 |
|---|---|---|
| 조용한 UI | `MsiSetInternalUI(INSTALLUILEVEL_NONE)` | |
| 자세한 로그 | `MsiEnableLog`(모든 모드) | 무엇이 실패하면 읽는다 |
| 설치 | `MsiInstallProduct(path, "")` | 0 |
| 이름 | 설치된 경로마다 `FindFirstFile`, 항목 자신의 UTF-16 이름을 정확히 비교 | 같음 |
| 내용 | 설치된 파일마다 `MsiGetFileHash` 와 자기 `MsiFileHash` 행 | 같음 |
| 등록 | `MsiGetProductInfo(ProductCode, InstalledProductName)` | 자기 ProductName |
| 복구 | 설치된 파일 하나를 지우고 `MsiReinstallProduct(ProductCode, ...)` | 파일이 같은 이름으로 돌아옴 |
| 업그레이드 | 판 N 위에 판 N+1 설치 | 옛 ProductCode 없어지고 새것이 설치됨 |
| 다운그레이드 | 판 N 을 다시 설치 | 거부(1603, 로그에 거부한 동작의 이름) |
| 기능 | `MsiQueryFeatureState`; `MsiConfigureFeature(..., INSTALLSTATE_LOCAL)` | `INSTALLLEVEL` 보다 높은 수준은 켤 때까지 없음 |
| 되돌림 | `InstallFiles` 뒤에 실패하는 지연 사용자 지정 동작(형식 34 + 1024, `"[SystemFolder]cmd.exe" /c exit 1`)과 새 패키지 코드를 가진 판 N+1 의 사본을 N 위에 설치 | 1603(로그: 오류 1722); N 은 그대로 등록되어 있고 파일도 그대로 |
| 제거 | `MsiConfigureProduct(ProductCode, ..., INSTALLSTATE_ABSENT)` | 0; 폴더, 키, 등록이 사라짐 |

컴퓨터 전체 설치에는 관리자 권한 프로세스가 필요하다. 시험 제품은 제 UpgradeCode 를 쓰고 같은 실행
안에서 지운다.

## 쓰기 전에 하는 검사

`msi.dll` 은 깨진 데이터베이스를 많이 받아들이고, 실패하더라도 설치 때에야 실패한다(열 너비조차 강제하지
않는다). 작성기는 제 표를 먼저 검사해야 한다. rubrapack 의 `build` 는 다음 가운데 하나라도 어기는
패키지는 쓰지 않는다(진단 `RP2001`-`RP2015`):

- 칸마다 열의 종류가 맞고, null 을 허용하지 않는 열에 null 이 없고, 문자열은 UTF-16 단위로 센 열
  너비보다 길지 않으며(BMP 밖 글자는 둘로 센다) 올바른 UTF-8 이다. 16비트 정수는 -32767..32767, 32비트
  정수는 -2147483648 이 아니다(그 값들은 null 을 뜻한다);
- 기본 키가 겹치지 않고, 참조(`Component.Directory_`, `File.Component_`, `FeatureComponents`,
  `CreateFolder`, `MsiFileHash.File_`, `Directory_Parent`, `Feature_Parent`)가 있는 행을 가리키며, 순서
  표의 동작은 모두 표준 동작, `CustomAction`, `Dialog` 가운데 하나다;
- `ProductCode`, `ProductName`, `ProductVersion`, `Manufacturer`, `ProductLanguage` 가 있다. GUID 는
  중괄호 속 대문자, 구성 요소 GUID 는 서로 다르며, ProductCode 는 UpgradeCode 와 다르다;
- `File.Sequence` 는 저마다 다르고 어떤 `Media.LastSequence` 가 덮는다;
- 표준 동작의 순서를 지킨다(비용 계산은 `InstallValidate` 전, 파일 동작은 `InstallInitialize` 와
  `InstallFinalize` 사이);
- 64비트 구성 요소 속성(256)이 요약 Template 의 플랫폼(`x64`/`Arm64` 대 `Intel`)과 맞는다;
- 파일 키 경로는 같은 구성 요소의 파일이다;
- `Upgrade.ActionProperty` 는 저마다 대문자(공개)이고 `SecureCustomProperties` 에 있다 - 그렇지 않으면
  컴퓨터 전체 설치가 그것을 서버 쪽에 넘기지 않는다.

## Microsoft 의 ICE 규칙 [관찰]

Windows SDK 의 "MSI Tools" 에는 MsiVal2 와 `darice.cub` 가 들어 있다. Microsoft 의 작성 도구들이 돌리는 내부
일관성 검사기(ICE)다: `MsiVal2 package.msi darice.cub -f` 가 실패한 것을 찍는다. Windows 에서만 돌고, 위의
검사보다 한 걸음 더 간다 - 표의 모양만이 아니라 뜻을 안다. rubrapack 의 시험 패키지(시험에 쓰는 모든 원본,
39개)에서 나오는 것과 그 까닭:

- **ICE03, "Missing specifications in _Validation"** 이 모든 열에: `_Validation` 표는 검사기만을 위해 열을
  설명한다. Windows Installer 는 읽지 않고, rubrapack 은 쓰지 않는다.
- **ICE82**, 겹친 순서 번호: 대화창 언어가 여럿인 패키지의 언어 동작들이 번호 하나를 함께 쓴다. 서로 무관한
  속성 설정 동작이라 순서가 상관없다.
- **ICE34**, 언어 페이지 라디오 속성에 `Property` 행이 없다: 일부러다. 명령줄(`RPLANGUAGE=ko`)이 감지보다
  이겨야 하고, 감지는 `NOT RPLANGUAGE` 일 때만 돈다.
- **ICE43, ICE57**, 파일을 키 경로로 쓰는 컴포넌트의 광고되지 않은 바로가기: 이 규칙들은 바로가기 폴더를
  사용자별로 가정한다. 컴퓨터 전체 패키지(`ALLUSERS=1`)에서 시작 메뉴와 바탕화면은 모든 사용자 폴더이고,
  사용자별·겸용 패키지는 사용자마다 따로 설치되므로, 이 규칙이 막으려는 "첫 사용자만" 문제는 생기지 않는다.
- **ICE52**, `AppSearch` 의 비공개 속성(`RpFound_<ID>`, 기억하는 폴더): 명령줄이 정하지 못하게 일부러
  비공개다. `AppSearch` 는 두 순서에서 모두 돌므로 서버 쪽에 넘길 필요가 없다.

셋은 진짜 결함이었고 고쳤다(0.4.3): 완료·취소·실패 페이지가 `AdminUISequence` 에 없어 전체 UI 의 관리
설치(`msiexec /a`)가 그 페이지 없이 끝났다(ICE20). 폴더를 키 경로로 쓰는 컴포넌트(INI 항목, `[remove]`)가
`CreateFolder` 에 없었다(ICE18). 대화창이 있는 패키지에 `ControlCondition` 표가 없었는데, ICE17 은 대화창마다
이 표를 읽는다(검사 대신 오류 2228 로 멈췄다).

## 멀쩡해 보이지만 아닌 것

- `msi.dll` 이 만든 데이터베이스가 완전한 명세는 아니다: `msi.dll` 은 65001 요약 문자열을 제대로 저장하고도
  다시 읽지 못한다([msi-summary.md](msi-summary.md)).
- `MsiDatabaseExport` 는 요약 날짜를 지역 시각으로 찍는다. 날짜는 UTC 로 비교한다.
- `MsiOpenDatabase` 가 성공했다고 행 순서나 참조 수가 맞다는 뜻은 아니다 - 그것은 설치만이 말해 준다.
  열어 보기만 하지 말고 설치해서 시험한다.
