# 설치·복구·업그레이드·제거가 되는 가장 작은 패키지

표의 뜻은 Microsoft Learn("Database Tables", "Standard Actions", "Suggested InstallExecuteSequence")에
있다. 이 문서는 시험을 거친 조리법이다: 정확히 이대로 지은 패키지는 Windows 11 에서 컴퓨터 전체로
설치되고, 복구를 견디고, 옛 판을 업그레이드하고, 다운그레이드를 거부하고, 깨끗이 제거된다. [observed]

## 표와 열

형식은 [msi-database.md](msi-database.md)의 표기를 쓴다(`s72` = `CHAR(72)`, 대문자 = null 허용,
`l` = 지역화 가능, `i2`/`i4` 정수). 키는 굵게 쓴다.

| 표 | 열 |
|---|---|
| Property | **Property** s72, Value l0 |
| Directory | **Directory** s72, Directory_Parent S72, DefaultDir l255 |
| Component | **Component** s72, ComponentId S38, Directory_ s72, Attributes i2, Condition S255, KeyPath S72 |
| Feature | **Feature** s38, Feature_Parent S38, Title L64, Description L255, Display I2, Level i2, Directory_ S72, Attributes i2 |
| FeatureComponents | **Feature_** s38, **Component_** s72 |
| File | **File** s72, Component_ s72, FileName l255, FileSize i4, Version S72, Language S20, Attributes I2, Sequence i4 |
| MsiFileHash | **File_** s72, Options i2, HashPart1..HashPart4 i4 |
| CreateFolder | **Directory_** s72, **Component_** s72 |
| Media | **DiskId** i2, LastSequence i4, DiskPrompt L64, Cabinet S255, VolumeLabel S32, Source S72 |
| Upgrade | **UpgradeCode** s38, **VersionMin** S20, **VersionMax** S20, **Language** S255, **Attributes** i4, Remove S255, ActionProperty s72 |
| CustomAction | **Action** s72, Type i2, Source S72, Target S255 |
| InstallExecuteSequence, InstallUISequence | **Action** s72, Condition S255, Sequence I2 |

`File.Sequence` 와 `Media.LastSequence` 를 32비트 정수로 두면 패키지에 파일을 32767개 넘게 담을 수 있다.

## Property

`ProductCode`, `ProductName`, `ProductVersion`(`a.b.c`, a 와 b 는 255 까지, c 는 65535 까지; 넷째 부분도
되지만 판을 비교할 때는 무시된다), `Manufacturer`, `ProductLanguage`(1033, 1042, ...), `UpgradeCode`,
`ALLUSERS=1`(컴퓨터 전체), `REBOOT=ReallySuppress`, `MSIRESTARTMANAGERCONTROL=Disable`(아래 "사용 중인
파일" 참고), 그리고 아래 업그레이드 속성 둘을 나열한 `SecureCustomProperties`.

## Directory

- 부모가 없고 `DefaultDir = SourceDir` 인 `TARGETDIR`.
- 쓰는 표준 폴더(`ProgramFiles64Folder`, `ProgramFilesFolder`, ...)는 저마다 `DefaultDir = .` 인
  `TARGETDIR` 의 자식. 이 이름들은 Windows 가 스스로 푼다. x64·Arm64 패키지는 `...64Folder` 이름을,
  x86 은 64 없는 이름을 쓴다.
- 그 아래의 자기 폴더는 `DefaultDir = SHORT|Long name`. 긴 이름은 어떤 유니코드 글이든 된다(코드 페이지
  65001 일 때). 짧은 이름은 올바른 8.3 ASCII 이름이어야 하고, 같은 부모의 모든 파일·폴더 사이에서 달라야
  한다. 긴 이름이 이미 올바른 8.3 이름이면 그것만 써도 된다.

## 구성 요소, 기능, 파일

- **파일 하나에 구성 요소 하나**, 그 파일이 키 경로다. `ComponentId` 는 제품의 수명 동안 같은 자리의 같은
  자원에 대해 변하지 않아야 하는 GUID 다([identity.md](identity.md) 참고). `Attributes` 는 x64·Arm64
  패키지면 256(64비트), x86 이면 0.
- `Level` 1 인 기능. `Display` 0 은 그것을 숨긴다. 모든 구성 요소를 `FeatureComponents` 에 짝짓는다. 자식
  기능은 `Feature_Parent` 에 부모를 적는다. `Level` 이 `INSTALLLEVEL`(패키지가 정하지 않으면 1)보다 높은
  기능은 기본으로 설치하지 않는다. 뒤에 `MsiConfigureFeature` 로 더할 수 있고, 그러면 그 파일들이
  들어온다. [observed]
- `File.FileName` 은 DefaultDir 처럼 `SHORT|Long` 이다. `Attributes` 512 는 파일을 필수(vital)로 표시한다.
  `Sequence` 번호 1..n 은 캐비닛 속 파일의 순서다.
- 판 정보 리소스가 없는 파일에는 **MsiFileHash** 행이 필요하다: 파일의 MD5 를 리틀엔디언 32비트 정수
  넷으로 읽은 것(부호 있는 값으로 저장). 설치된 파일은 `MsiGetFileHash` 로 해시하면 정확히 이 값이
  나온다. 판 정보 리소스가 있는 파일은 대신 `File.Version` 에 판을 적는다(해시는 필요 없다).

## 빈 폴더

파일 없이 있어야 하는 폴더에는 제 Directory 행, `Directory_` 가 그 폴더이고 `KeyPath` 가 null 인 구성
요소(폴더 자체가 키 경로), 둘을 가리키는 `CreateFolder` 행, `FeatureComponents` 행을 준다.
`CreateFolders` 가 설치할 때 만들고, `RemoveFolders` 가 제거할 때 비어 있으면 지운다. [observed] 구성 요소
속성 16(영구)이면 다른 것은 다 지워져도 폴더는 남는다. [observed]

## Media 와 캐비닛

행 하나: `DiskId 1`, `LastSequence n`, `Cabinet #cab1.cab` - `#` 는 "이 패키지 속 `cab1.cab` 이라는
스트림"이라는 뜻이다. 캐비닛은 파일을 `File` 키 이름으로 `Sequence` 순서대로 담는다
([cab-mszip.md](cab-mszip.md)). 요약의 Word Count 2 는 파일이 캐비닛에 압축되어 있다는 뜻이다.

## 메이저 업그레이드와 다운그레이드 거부

패키지의 `UpgradeCode` 를 가진 Upgrade 행 둘:

| VersionMin | VersionMax | Attributes | ActionProperty | 뜻 |
|---|---|---|---|---|
| 이 판 | - | `0x102`(찾기만, 최솟값 포함) | `NEWER_FOUND` | 같거나 새로운 판이 설치됨 |
| - | 이 판 | `0x001`(기능 옮기기) | `OLDER_FOUND` | 옛 판이 설치됨: 지운다 |

Upgrade 표는 UpgradeCode, 판, 언어로만 맞춰 보고 아키텍처는 보지 않는다. x64 빌드와 x86 빌드가
UpgradeCode 를 같이 쓰면 하나를 설치할 때 다른 것이 "옛 판"으로 지워진다. 둘을 나란히 설치할 수 있어야
하면 아키텍처마다 제 UpgradeCode 를 준다(확인함: 코드가 다른 두 아키텍처는 서로 상관없이 설치·제거된다).
[observed]

`NEWER_FOUND` 를 조건으로 한 오류 사용자 지정 동작(형식 19, `Target` = 메시지; 서식 문자열이라
`[ProductName]` 이 된다)은 같거나 새로운 판이 있을 때 오류 1603 으로 설치를 멈춘다. 판마다 새
`ProductCode` 가 필요하다(그리고 패키지 파일마다 새 패키지 코드).

## 설치한 프로그램으로 등록·해제하기

어떤 제품(입력기, 셸 확장)은 표의 행이 아니라 자기 프로그램으로 스스로를 등록해야 한다. 설치·복구·
업그레이드·제거를 원자적으로 지키는 방법은 그 프로그램의 구성 요소 `C` 의 동작 상태(`$C` = 이번 설치가
그것에 하는 일, `?C` = 그 전의 상태; 2 없음, 3 로컬)를 쓰는 것이다 [spec]. 형식 18 동작(`Source` = 그
프로그램의 File 키, `Target` = 인수)을 모두 지연(0x400), 가장하지 않음(0x800)으로 둔다:

| 동작 | 형식 | 자리 | 조건 | 하는 일 |
|---|---|---|---|---|
| `…UndoRollback` | 18+0x100+0x400+0x800+0x40 | `RemoveFiles` 앞 | `$C=2 AND ?C=3` | 등록(다음 행의 되돌림) |
| `…Undo` | 18+0x400+0x800 | 그 뒤, `RemoveFiles` 앞 | `$C=2 AND ?C=3` | 해제 |
| `…DoRollback` | 18+0x100+0x400+0x800+0x40 | `InstallFiles` 뒤 | `$C>2 AND ?C<>3` | 해제 |
| `…RedoRollback` | 같음 | 그 뒤 | `$C>2 AND ?C=3` | 등록 |
| `…Do` | 18+0x400+0x800 | 둘 다 뒤 | `$C>2` | 등록 |

되돌림 동작은 그것이 되돌리는 동작보다 *앞에* 놓아야 한다: 되돌림은 스크립트를 거꾸로 실행하고, 이미
스크립트에 든 되돌림 동작만 실행한다. 되돌림 동작은 종료 코드를 무시하고(0x40) 앞으로 가는 동작은
그러지 않으므로, 등록이나 해제가 실패하면 설치가 실패하고 되돌려진다. Windows 11 에서 본 것: 복구는
`…Do` 를 다시 실행한다. 해제가 실패하면 제거가 되돌려지고 (아직 있는) 프로그램이 다시 등록된다.
업그레이드가 실패하면 새 패키지의 `…DoRollback` 이 실행되고, 옛 판의 파일이 돌아오고, 옛 패키지 자신의
`…UndoRollback` 이 옛 프로그램을 다시 등록한다. [observed]

**`InstallInitialize` 와 `RemoveExistingProducts` 사이에는 스크립트에 쓰는 것이 아무것도 있어서는 안 된다**:
거기 지연 동작이나 되돌림 동작이 하나라도 있으면 모든 업그레이드가 오류 2613("RemoveExistingProducts
action sequenced incorrectly")으로 멈춘다. [observed]

## 레지스트리 값

`Registry`(**Registry** s72, Root i2, Key l255, Name L255, Value L0, Component_ s72)는
`WriteRegistryValues`(5000)가 쓰고 제거할 때 되돌린다. `RemoveRegistry`(Value 없는 같은 열)는
`RemoveRegistryValues`(2600)가 처리한다. Root 0 = HKCR, 1 = HKCU, 2 = HKLM. 값의 첫 글자가 형식을
정한다: `#x` 이진(16진), `#%` 확장 문자열, `#` 뒤에 숫자는 DWORD(`#4294967295` 까지), 어디든 `[~]` 가
있으면 다중 문자열(`[~]a[~]b[~]`). `#` 으로 시작하는 보통 문자열은 `##` 로 쓴다. RemoveRegistry 의 Name
`-` 는 키 전체를 지운다. 설치가 실패하면 덮어쓴 값과 지운 키가 되살아난다. [observed]

키 경로가 레지스트리 값인 구성 요소는 속성 4 와 `KeyPath` = 그 Registry 행을 가진다. 구성 요소의 64비트
속성(256)이 레지스트리 보기를 고른다: 그것이 없으면 64비트 패키지도 32비트 보기(`WOW6432Node`)에 쓴다.
64비트 패키지 속 32비트 구성 요소는 허용되고, 32비트 패키지 속 64비트 구성 요소는 허용되지 않는다.

REG_QWORD 는 Registry 표로 나타낼 수 없다. rubrapack 은 그것을 `Binary` 표의 DLL 사용자 지정 동작(형식
1)으로 쓴다: 즉시 동작이 구성 요소 동작 상태와 현재 값을 읽어 목록 둘을 `CustomActionData` 로 넘긴다 -
하나는 쓰거나 지우는 지연 동작에게, 하나는 원래 있던 것을 되살리는 그 되돌림 짝에게. DLL 의 비트 수는
패키지와 맞아야 한다(엔진은 그것을 패키지 아키텍처의 사용자 지정 동작 서버에 싣는다).
[observed: x64 와 x86]

파일 형식과 URL 스킴은 `Extension`/`Verb`/`ProgId` 표(처음 쓸 때 광고와 복구를 끌고 온다)가 아니라
프로그램의 구성 요소에 든 Root 0(HKCR) 아래의 보통 `Registry` 행이다: `.ext`(기본값) = ProgId;
`ProgId`(기본값) = 그 설명, `ProgId\DefaultIcon` = `[#File],0`, `ProgId\shell\open\command` =
`"[#File]" "%1"`. 스킴은 `scheme`(기본값) = `URL:<설명>`, `URL Protocol` = 빈 값(Name 이 있고 Value 가
null 이면 빈 문자열을 쓴다), 그리고 같은 `DefaultIcon` 과 `shell\open\command`. Root 0 은 설치 방식을
따른다: 컴퓨터 전체면 HKLM\Software\Classes, 사용자별이면 HKCU\Software\Classes. 그러면 이 프로그램만
맡는 형식의 파일을 열 때 곧바로 이 프로그램이 시작된다. [observed]

## 바로가기

`Shortcut`(**Shortcut** s72, Directory_ s72, `.lnk` 없는 Name l128 `SHORT|Long`, Component_ s72, 광고 아닌 바로가기면 Target 은 서식 경로(`[#FileKey]` 나 `[DirKey]이름`), Arguments(서식), Description(평문), Hotkey, Icon_, IconIndex,
ShowCmd, WkDir = Directory 키)는 `CreateShortcuts`(4500)가 쓰고 `RemoveShortcuts`(3200)가 지운다.
바로가기를 대상 파일의 구성 요소에 두어도 되지만, Microsoft 의 ICE43/ICE57 은 광고 아닌 바로가기마다
`HKCU` 값을 키 경로로 쓰는 구성 요소를 바란다([verify.md](verify.md)). rubrapack 은 바로가기마다 그런 구성 요소를
주고 대상을 `[DirKey]이름` 으로 쓴다(ICE69). 바로가기만을 위해 만든 폴더는 저절로 지워지지 않는다: 그 폴더와
표준 폴더 아래의 부모마다 `RemoveFile` 행(FileName null, DirProperty = 그 폴더, InstallMode 2)을 더한다.
`ALLUSERS=1` 이면 `ProgramMenuFolder` 와 `DesktopFolder` 는 모든 사용자의 시작 메뉴와 공용 바탕화면이다.
설치가 실패하면 바로가기도 폴더도 남지 않고, 복구는 지운 바로가기를 다시 만든다. [observed]

## 파일 지우기와 복제

`RemoveFile`(**FileKey** s72, Component_ s72, `*`/`?` 가 든 FileName L255 또는 폴더면 null,
DirProperty s72, InstallMode i2: 1 설치, 2 제거, 3 둘 다)은 구성 요소가 설치될 때(1)나 제거될 때(2)
`RemoveFiles` 에서 실행된다. 키 경로 파일이 없고 `Directory_` 가 있는 구성 요소가 그 운반자 노릇을 한다.
`DuplicateFile`(**FileKey** s72, Component_ s72 = 원본의 구성 요소, File_ s72, DestName L255
`SHORT|Long`, DestFolder S72)은 `DuplicateFiles`(4210)와 `RemoveDuplicateFiles` 에서 실행된다. 설치 때
지운 파일은 설치가 실패하면 돌아온다. 설치 전부터 있던 폴더는 비어 있어도 제거할 때 그대로 둔다.
[observed]

## 환경 변수

`Environment`(**Environment** s72, Name l255, 서식 Value L255, Component_ s72)는
`WriteEnvironmentStrings`(5200)와 `RemoveEnvironmentStrings` 가 쓴다. Name 접두: `*` 시스템 변수(없으면
사용자 변수), `=` 만들거나 정함, `-` 구성 요소를 제거할 때 되돌림. Value `[~];x` 는 `;x` 를 뒤에 붙이고
`x;[~]` 는 앞에 붙인다. `-` 가 있으면 제거할 때 정한 변수는 지우고, 원래 있던 변수에서는 붙인 부분만
떼어 낸다. `-` 가 없으면 아무것도 되돌리지 않는다. 설치가 실패하면 이전 값이 되살아난다. [observed]

## INI 파일

`IniFile`(**IniFile** s72, FileName l255 `SHORT|Long`, DirProperty S72, Section l96, Key l128, 서식
Value l255, Action i2: 0 줄 더하기, 1 줄 만들기, 3 태그 더하기, Component_ s72)은 `WriteIniValues`(5100)가
적용하고 구성 요소를 제거할 때 되돌린다. `RemoveIniFile`(같은 열, Value null 허용, Action 2 줄 지우기,
4 태그 지우기)은 `InstallFiles` 전에 실행되는 `RemoveIniValues` 가 처리한다. 태그 더하기는 쉼표 목록에
`,value` 를 붙이고, 제거하면 그 항목만 뗀다. ASCII 가 아닌 파일·구역·키·값 이름도 된다. 설치가 실패하면
파일이 되살아난다. [observed]

## 검색과 설치 조건

두 순서 모두 50 에 있는 `AppSearch`(**Property**, **Signature_**)는 같은 서명을 가진 찾개(locator)로
속성을 정한다: `RegLocator`(Root, Key, Name, Type 2 = 원 값, +16 = 64비트 보기), 파일이면 `Signature`
행(FileName, MinVersion)을 곁들인 `DrLocator`(Path 는 `[System64Folder]` 같은 폴더 속성으로 시작해도
된다), 또는 `CompLocator`(ComponentId, Type 1 = 키 파일). 그 속성은 공개 이름이어야 하고, 서버 쪽까지
가려면 `SecureCustomProperties` 에 있어야 한다. 두 순서 모두 100 에 있는 `LaunchCondition`(**Condition**,
서식 Description)은 조건이 거짓이면 그 설명으로 설치를 멈춘다. [observed]

## 서비스, 글꼴, 권한

`ServiceInstall`(Name, 서식 DisplayName 과 Description, ServiceType 0x10, StartType 2/3/4, ErrorControl 1,
StartName null 또는 `NT AUTHORITY\LocalService`, 서식 Arguments, Component_ = exe 의 구성 요소)은
`InstallServices`(5800)와 함께 쓴다. `ServiceControl` 의 Event 플래그는 0x1 설치 때 시작, 0x2 설치 때
멈춤, 0x20 제거 때 멈춤, 0x80 제거 때 지움이고 Wait 1, `StopServices`(1900), `DeleteServices`(2000),
`StartServices`(5900)와 함께 쓴다. `Font`(File_, FontTitle null = 글꼴에서 읽음)는
`RegisterFonts`/`UnregisterFonts` 와 함께 쓰고, 파일은 `FontsFolder` 바로 안에 있어야 한다.
`MsiLockPermissionsEx`(**MsiLockPermissionsEx**, LockObject, Table = `CreateFolder`, `File`, `Registry`,
`ServiceInstall` 가운데 하나, **SDDLText**(열 이름), Condition)는 Windows Installer 5.0 이 필요하다(요약
page count 500). 폴더는 CreateFolder 행을 거쳐 그것을 받는다. 설치가 실패하면 서비스도 글꼴도 남지 않는다.
[observed]

## 캐비닛, 관리 이미지, 광고

캐비닛이 여럿이면 저마다 `Media` 행 하나(DiskId 1..n, LastSequence = 그 안 마지막 파일의 순번). 안에
넣은 것은 `Cabinet` 에 `#` 와 함께 이름 적은 스트림이고, 밖의 것은 패키지 옆에 있는 파일로 `Cabinet` 에
`#` 없이 이름을 적는다(긴 이름도 된다). `AdminExecuteSequence`(CostInitialize 800, FileCost 900,
CostFinalize 1000, InstallValidate 1400, InstallInitialize 1500, InstallAdminPackage 3900, InstallFiles
4000, InstallFinalize 6600)와 `AdminUISequence` 는 `msiexec /a` 가 원본처럼 설치되는 압축 없는 이미지를
쓰게 한다. `AdvtExecuteSequence`(CostInitialize, CostFinalize, InstallValidate, InstallInitialize,
PublishFeatures 6300, PublishProduct 6400, InstallFinalize)는 `msiexec /jm` 이 제품을 광고하게 한다.
[observed]

## 사용자별 패키지와 겸용 패키지

패키지 하나로 하는 방법: `ALLUSERS=2` 와 `MSIINSTALLPERUSER=1` 은 사용자별로 설치한다 - 엔진이
`ProgramFilesFolder`/`ProgramFiles64Folder` 를 `%LOCALAPPDATA%\Programs` 로, 메뉴와 바탕화면 폴더를 그
사용자의 것으로 돌린다 - 그리고 `ALLUSERS=1 MSIINSTALLPERUSER=""` 는 컴퓨터 전체로 설치한다. 요약 Word
Count 비트 8 은 권한 상승이 필요 없다는 뜻이다. Registry Root -1(HKMU)은 사용자별이면 HKCU, 컴퓨터
전체면 HKLM 이다. `*` 없는 Environment 이름은 사용자 변수다. 사용자별 제품이 설치되고 나면 엔진은
`MSIINSTALLPERUSER` 를 지우므로, 그것을 보는 설치 조건은 `Installed OR ...` 로 써야 한다. 그러지 않으면
제품을 더는 제거할 수 없다. 일반적으로 설치 조건은 모두 `Installed OR (...)` 로 쓴다. [observed]

## 사용 중인 파일

바꾸거나 지울 파일을 실행 중인 프로그램이 쥐고 있을 때(그 안에 DLL 이 실려 있을 때):

- 기본인 Restart Manager 를 쓰면 Windows Installer 는 **조용한 설치(`/qn`)에서도 그런 파일을 쥔 프로그램을
  모두 끄려 하고** 뒤에 다시 시작한다. 하나라도 닫히지 않으면 설치 전체가 실패한다(1601). [observed]
- Property 표에 `MSIRESTARTMANAGERCONTROL=Disable` 이 있으면 엔진은 쥐인 파일을 옆으로
  옮기고(`C:\Config.Msi\*.rbf`) 새 파일을 곧바로 제자리에 두고 0 을 돌려준다. 프로그램은 옛 사본으로 계속
  돌고, 그 뒤에 시작한 프로그램은 새 파일을 싣고, 옛 사본은 다음 재시작 때 지워진다. [observed]
- 명령줄에서 준 속성은 설치하고 있는 패키지에만 간다. 업그레이드 중 옛 판의 제거는 **옛 패키지의**
  Property 표를 따른다 - 그 속성은 첫 판부터 넣어 둔다. [observed]

## 순서

InstallExecuteSequence(대괄호 안은 조건):

```text
FindRelatedProducts 25, <다운그레이드 거부 동작> 30 [NEWER_FOUND], CostInitialize 800,
FileCost 900, CostFinalize 1000, MigrateFeatureStates 1200, InstallValidate 1400,
InstallInitialize 1500, RemoveExistingProducts 1501, ProcessComponents 1600,
UnpublishFeatures 1800, RemoveFiles 3500, RemoveFolders 3600, CreateFolders 3700,
InstallFiles 4000, RegisterUser 6000, RegisterProduct 6100, PublishFeatures 6300,
PublishProduct 6400, InstallFinalize 6600
```

InstallUISequence: FindRelatedProducts 25, 다운그레이드 거부 동작 30 [NEWER_FOUND], CostInitialize 800,
FileCost 900, CostFinalize 1000, MigrateFeatureStates 1200, ExecuteAction 1300(그리고 대화창, [대화창](#대화창)
참고).

`InstallInitialize` 바로 뒤의 `RemoveExistingProducts` 는 옛 판의 제거를 새 설치의 트랜잭션 안에 넣는다.
그래서 업그레이드가 실패하면 옛 판으로 되돌아간다. [observed: `InstallFiles` 바로 뒤에 실패하는 지연
사용자 지정 동작이 있으면 업그레이드는 1603 으로 끝나고 옛 판이 파일까지 바이트 그대로 다시 등록된다.
첫 설치가 실패하면 파일도 등록도 남지 않는다.]

## 대화창

대화창이 있는 패키지는 다음 표를 가진다(형식은 위와 같다):

| 표 | 열 |
|---|---|
| Dialog | **Dialog** s72, HCentering i2, VCentering i2, Width i2, Height i2, Attributes I4, Title L128, Control_First s50, Control_Default S50, Control_Cancel S50 |
| Control | **Dialog_** s72, **Control** s50, Type s20, X i2, Y i2, Width i2, Height i2, Attributes I4, Property S72, Text L0, Control_Next S50, Help L50 |
| ControlEvent | **Dialog_** s72, **Control_** s50, **Event** s50, **Argument** s255, **Condition** S255, Ordering I2 |
| ControlCondition | **Dialog_** s72, **Control_** s50, **Action** s50, **Condition** s255 |
| EventMapping | **Dialog_** s72, **Control_** s50, **Event** s50, Attribute s50 |
| TextStyle | **TextStyle** s72, FaceName s32, Size i2, Color I4, StyleBits I2 |
| UIText | **Key** s72, Text L255 |
| Binary | **Name** s72, Data v0(스트림) |
| RadioButton | **Property** s72, **Order** i2, Value s64, X i2, Y i2, Width i2, Height i2, Text L0, Help L50 |
| ComboBox | **Property** s72, **Order** i2, Value s64, Text L64 |

엔진이 대화창을 보일 때 검사하는 것(저마다 그 번호를 단 오류 대화창으로 보았다):

- **Tab 순서.** `Control_Next` 는 그것을 가진 모든 컨트롤을 `Control_First` 에서 시작해 한 바퀴 도는 고리를
  이루어야 한다. 컨트롤 하나를 빠뜨리거나 `Control_Next` 가 빈 컨트롤에서 끝나는 사슬은 2810 이나 2809
  로 설치를 멈춘다. 정적 글씨는 고리에 끼지 않는다. [observed] rubrapack 은 짓는 대화창마다 검사하고
  깨진 것은 쓰지 않는다(RP0010).
- **오류 대화창.** `ErrorDialog` 속성이 가리키는 대화창에는 `ErrorText` 라는 `Text` 컨트롤과 `ErrorIcon`
  이라는 `Icon` 컨트롤이 있어야 한다(없으면 2835). Attributes 에 오류 대화창 비트(0x10000)가 있고,
  `EndDialog` 인수 `ErrorAbort` .. `ErrorYes` 를 가진 `A`, `C`, `I`, `N`, `O`, `R`, `Y` 누름단추가
  메시지에 필요한 대로 보인다. [observed]
- **문구 속 대괄호.** `Text` 는 서식 문자열이다: `[Next]` 는 속성으로 읽혀 아무것도 보이지 않는다.
  [observed] 본문에서 단추 이름은 대신 따옴표로 적는다.
- **사용 중인 파일.** Restart Manager 가 꺼져 있으면 엔진은 `FilesInUse` 라는 대화창(`Retry`, `Ignore`,
  `Exit` 로 끝나는 단추)을 `FileInUseProcess` 에 묶인 `ListBox` 와 함께 보인다. **ListBox 표는 비어 있어도
  있어야 한다**: 없으면 엔진은 2205 를 로그에 남기고 대화창을 건너뛰며, 설치는 무시를 고른 것처럼 계속된다.
  메이저 업그레이드에서는 대화창이 두 번 나온다(새 파일, 그다음 옛 판의 제거). 무시한 뒤에는 쥐인 파일을
  옆으로 옮기고 설치는 0 으로 끝난다. [observed]
- **오류 아이콘.** 엔진은 컨트롤의 Binary 가 무엇을 담든 메시지 종류에 맞는 자기 아이콘(설치 조건이면
  경고 삼각형)을 `ErrorIcon` 에 넣는다. [observed]
- **목록 순서.** `ComboBox` 는 컨트롤에 Sorted 속성(0x10000)이 없으면 항목을 가나다순으로 늘어놓고, 있으면
  Order 열을 따른다. [observed] rubrapack 은 그것을 켠다.
- **권한 상승.** 권한 없는 사용자가 대화형으로 시작한 컴퓨터 전체 설치는 실행 순서가 시작될 때 보안
  데스크톱에서 동의(UAC)를 묻는다. [observed]
- **코드 페이지.** 데이터베이스 코드 페이지가 65001 이면 한국어 제목, 문구, RTF 사용권 문구가 한국어 시스템
  로캘에서도 영어(1252) 시스템 로캘에서도 제대로 보인다. [observed,
  [msi-database.md](msi-database.md#코드-페이지) 참고]

rubrapack 의 세트: 모든 대화창은 370 x 270 대화창 단위이고, 띠(그림이 `Binary.RpBanner` 인 `Bitmap`
컨트롤 0,0,370,44)에 굵은 제목(`{\RpTitle}`, TextStyle)과 설명 한 줄이 있고, 44 와 234 에 `Line`, y 243 에
뒤로/다음/취소가 있다. InstallUISequence 는 환영(1230, `NOT Installed`)이나 유지 관리 대화창(1240,
`Installed AND NOT RESUME AND NOT Preselected`), 진행 대화창(1280, 모덜리스, EventMapping 으로 `ActionText`
와 `SetProgress` 를 구독), 그리고 -1(성공), -2(취소), -3(치명적 오류)의 끝 대화창 셋을 더한다. 설치 폴더
대화창은 폴더의 Directory 키를 `PathEdit` 에 넣고, 그 다음 단추는 `NewDialog` 전에 경로를 검사하는
`SetTargetPath` 를 실행한다. 사용권 문구는 RTF 를 담은 `ScrollableText` 컨트롤이다: 평문은 `\uN?`
이스케이프(U+FFFF 너머의 글자는 대리 쌍)로, 줄마다 `\par` 하나로 바뀐다.

작성자가 만든 페이지는 같은 틀 속의 보통 Dialog 행이다. `Control_Next` 는 컨트롤들을 위치 순서대로 지나
뒤로, 다음, 취소로 간다. 라디오 묶음은 단추가 RadioButton 행(묶음 기준 위치)인 `RadioButtonGroup` 컨트롤이고,
드롭다운 목록은 ComboList(0x20000)와 Sorted 가 있는 `ComboBox` 컨트롤로 ComboBox 행에서 채운다. 그 속성들은
`SecureCustomProperties` 에 더해지므로, 대화창에서 고르거나 명령줄에서 준 값이 실행 순서까지 간다.
[observed]

## 이 조리법이 아직 다루지 않는 것

위의 오류 형식, 등록 쌍, REG_QWORD 도우미 말고 다른 사용자 지정 동작, 그리고 `_Validation`(검증 도구에는
필요하지만 설치 엔진에는 필요 없다). rubrapack 이 구현하는 대로 이 문서들도 자란다.
