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

## 멀쩡해 보이지만 아닌 것

- `msi.dll` 이 만든 데이터베이스가 완전한 명세는 아니다: `msi.dll` 은 65001 요약 문자열을 제대로 저장하고도
  다시 읽지 못한다([msi-summary.md](msi-summary.md)).
- `MsiDatabaseExport` 는 요약 날짜를 지역 시각으로 찍는다. 날짜는 UTC 로 비교한다.
- `MsiOpenDatabase` 가 성공했다고 행 순서나 참조 수가 맞다는 뜻은 아니다 - 그것은 설치만이 말해 준다.
  열어 보기만 하지 말고 설치해서 시험한다.
