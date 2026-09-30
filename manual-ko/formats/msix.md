# MSIX 패키지

Windows 가 MSIX 패키지를 읽고, 설치하고, 실행하게 하려면 프로그램이 무엇을 써야 하는가. 출처 표시는
[README.md](README.md)와 같다: **[spec]** 은 Microsoft Learn(패키지 매니페스트와 블록 맵 스키마)과 ECMA-376
Part 2(Open Packaging Conventions), **[observed]** 는 Windows 자신의 패키징 API(Windows 의 일부인
AppxPackaging.dll 의 `IAppxFactory`/`IAppxPackageWriter`)가 쓴 패키지, 그리고 `IAppxPackageReader` 로 다시
읽고 Windows 11 에서 `Add-AppxPackage` 로 설치한 rubrapack 의 패키지다. rubrapack 은 여기 적은 것을 쓴다.

## ZIP 압축 파일

- 페이로드 파일이 먼저, 그다음 `AppxManifest.xml`, `AppxBlockMap.xml`, `[Content_Types].xml` 이 그 순서로
  온다. [observed]
- 모든 항목은 크기와 상관없이 ZIP64 다: 로컬 머리는 필요 판 4.5, 플래그 0x0008(데이터 서술자), CRC 와
  크기 0, extra 필드 없음. 데이터 뒤에는 ZIP64 데이터 서술자(`PK\7\8`, CRC-32, 압축 크기와 원 크기 각
  8바이트)가 온다. 중앙 디렉터리 항목은 만든 판과 필요 판이 4.5(MS-DOS), 크기와 오프셋이 0xFFFFFFFF 이고
  ZIP64 extra 필드(0x0001, 24바이트: 원 크기, 압축 크기, 로컬 머리 오프셋)를 가진다. 압축 파일은 ZIP64 끝
  레코드(크기 44), 그 로케이터, 그리고 개수와 오프셋이 모두 0xFFFF / 0xFFFFFFFF 인 끝 레코드로 끝난다.
  [observed]
- 방식: 저장(0)과 deflate(8). `AppxManifest.xml`, `AppxBlockMap.xml`, `[Content_Types].xml` 은 페이로드가
  저장이어도 deflate 한다. [observed]
- 항목 이름은 OPC 부분 이름이다: 폴더 사이는 `/`, `A-Z a-z 0-9 - . _ ~` 밖의 모든 바이트는 UTF-8 바이트까지
  퍼센트 인코딩한다 - `data\<U+C790> %#(1).txt`(한글 음절, 공백, `%`, `#`, 괄호)는
  `data/%EC%9E%90%20%25%23%281%29.txt` 로 저장한다 - 그리고 UTF-8 이름 플래그는 켜지 않는다.
  `[Content_Types].xml` 은 대괄호를 그대로 둔다. [observed]
- 머리의 날짜는 읽지 않는다. Windows 는 쓰는 시각을, rubrapack 은 1980-01-01 00:00 을 써서 빌드마다
  패키지가 바뀌지 않게 한다. [observed]

## 블록 맵(`AppxBlockMap.xml`)

```xml
<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<BlockMap xmlns="http://schemas.microsoft.com/appx/2010/blockmap"
          xmlns:b4="http://schemas.microsoft.com/appx/2021/blockmap" IgnorableNamespaces="b4"
          HashMethod="http://www.w3.org/2001/04/xmlenc#sha256">
  <File Name="data\text.txt" Size="218890" LfhSize="43">
    <Block Hash="(원 바이트 65536개의 base64 SHA-256)" Size="(이 블록의 압축 바이트 수)"/>
    ...
    <b4:FileHash Hash="(파일 전체의 base64 SHA-256)"/>
  </File>
  ...
</BlockMap>
```

- 페이로드 파일마다 `File` 하나, `AppxManifest.xml` 에도 하나. 블록 맵과 `[Content_Types].xml` 은 싣지
  않는다. `Name` 은 적은 그대로의 경로로 `\` 를 쓰고 퍼센트 인코딩하지 않는다. `Size` 는 원 크기,
  `LfhSize` 는 로컬 머리의 크기(30 + ZIP 이름 길이)다. 빈 파일에는 `Block` 이 없다. [spec] [observed]
- 원 데이터 65536바이트마다 `Block` 하나. `Hash` 는 그 원 블록의 base64 SHA-256 이다. [spec]
- deflate 한 파일은 블록마다 독립된 원시 deflate 조각 하나다: 조각마다 새 디코더로 풀리고(앞 블록을 되돌아
  가리키는 것이 없다) 빈 저장 블록(`00 00 FF FF`)으로 끝나며, 마지막 조각 뒤에는 빈 마지막 블록(`03 00`)이
  온다. 블록의 `Size` 는 그 조각의 바이트 수라서, 파일의 압축 크기는 `Size` 합 더하기 2 다(빈 파일은
  2바이트, 블록 없음). 저장 파일의 블록에는 `Size` 가 없다. 압축되지 않는 데이터도 (저장 블록으로)
  deflate 한다. [observed]
- `b4:FileHash` 는 블록이 둘 이상인 파일에만 나온다. [observed]
- Windows 의 판독기는 파일을 읽으면서 블록마다 해시를 확인하고, 파일이 블록 맵과 다른 패키지는 읽을 때도
  설치할 때도 거부한다. [observed]

## `[Content_Types].xml`

한 줄: 처음 쓰인 순서대로의 파일 확장자(소문자)마다 `Default` 하나, `xml` 은
`application/vnd.ms-appx.manifest+xml`, 그리고 `application/vnd.ms-appx.blockmap+xml` 인
`Override PartName="/AppxBlockMap.xml"`. rubrapack 은 확장자 없는 파일마다 `Override` 를, 그리고 페이로드의
`.xml` 파일이 `xml` 기본을 차지했으면 `/AppxManifest.xml` 에 `Override` 를 더한다. [observed]

## 매니페스트(`AppxManifest.xml`)

Windows 가 설치하고 시작하는 가장 작은 데스크톱 앱(네임스페이스 `foundation/windows10`, `uap/windows10`,
`restrictedcapabilities`): [spec] [observed]

- `Identity`: `Name`(`A-Z a-z 0-9 . -` 로 3~50자), `Publisher`(서명 인증서의 주체), `Version`(네 부분, 각
  65535 이하), `ProcessorArchitecture`(`x64`, `arm64`, `x86`).
- `Properties`: `DisplayName`, `PublisherDisplayName`, `Logo`(패키지 속 PNG).
- `MinVersion` 과 `MaxVersionTested` 를 가진 `Dependencies/TargetDeviceFamily Name="Windows.Desktop"`.
- `Resources/Resource Language`.
- `uap:VisualElements`(`DisplayName`, `Description`, `BackgroundColor`, `Square150x150Logo`,
  `Square44x44Logo`)를 가진 `Application Id Executable EntryPoint="Windows.FullTrustApplication"`, 그리고
  제한된 기능 `runFullTrust`.
- 매니페스트 속 경로는 `\` 를 쓰고 패키지의 파일을 가리킨다.

## 가상 레지스트리(`Registry.dat`, `User.dat`)

패키지 뿌리의 레지스트리 하이브(REGF 형식: [registry.md](registry.md)). Windows 11(26100)에서 패키지된 앱으로
잰 것: [observed]

- `Registry.dat`: 그 `REGISTRY\MACHINE\SOFTWARE` 키가 `HKLM\Software` 를 나타낸다(하이브의 뿌리 자체는
  아니다). `REGISTRY\MACHINE\SOFTWARE\WOW6432Node\...` 는 앱이 32비트 보기에서 보는 것이다. Microsoft 의
  설명("registry.dat serves as the logical equivalent of HKLM\Software")은 앱이 보는 것에 대한 말이지
  하이브의 배치에 대한 말이 아니다.
- `User.dat`: 그 뿌리가 `HKCU` 를 나타낸다(그 아래 `Software\...` 가 `HKCU\Software\...`).
- 앱은 패키지의 키를 실제 레지스트리와 합쳐 읽는다. 컴퓨터의 레지스트리에는 아무것도 쓰지 않고, 제거한
  뒤에는 아무것도 남지 않는다.
- Windows 의 패키징 도구는 이 하이브를 오프라인 레지스트리 라이브러리로 쓴다(그 표시 "OfRg" 가 기본 블록에
  있다).

## 가상 파일 시스템(`VFS\...`)

`VFS\<폴더>` 아래의 파일은 앱에게 실제 자리에 있는 것으로 보이고, 실제 폴더는 바뀌지 않는다. 잰 것:
`ProgramFilesX64`(%ProgramFiles%), `SystemX64`(System32), `Common AppData`(%ProgramData%) - 그리고 Microsoft
Learn 이 말하는 대로 `AppData` 나 `Local AppData` 에는 없다. rubrapack 이 쓰는 다른 이름(`ProgramFilesX86`,
`ProgramFilesCommonX64`/`X86`, `SystemX86`, `Windows`)은 Microsoft Learn 이 늘어놓은 것이다. [spec] [observed]

## 확장

rubrapack 이 앱의 `<Extensions>`(`uap:VisualElements` 뒤)에 쓰는 것, 그리고 이것뿐이다: 여기 행이 없는 원본
기능은 오류이고, 필요한 Windows 빌드가 패키지의 `MinVersion` 보다 높은 것은 `min-version` 을 올리라고
요구한다. [spec] Microsoft Learn, "Integrate your desktop app with Windows using packaging extensions" 와 요소
문서들. 마지막 열은 Windows 11(26100)에 패키지를 설치해 확인한 것이다. [observed]

| 원본 | 요소(범주) | 네임스페이스 | 최소 빌드 | 기능 | Windows 11 에서 확인한 것 |
|---|---|---|---|---|---|
| `[assoc]` | `uap:Extension` `windows.fileTypeAssociation` > `uap3:FileTypeAssociation`(Name, Parameters) > `uap:DisplayName`, `uap:SupportedFileTypes` > `uap:FileType` | uap, uap3 | 14393 | runFullTrust | `.rpx`/`.rpy` 파일을 열면 프로그램이 그 파일과 함께 시작된다 |
| `[protocol]` | `uap3:Extension` `windows.protocol` > `uap3:Protocol`(Name, Parameters) | uap3 | 14393 | runFullTrust | `scheme:` URI 를 시작하면 프로그램이 그것과 함께 시작된다 |
| `[msix-extension]` 별칭 | `uap3:Extension` `windows.appExecutionAlias`(Executable, EntryPoint) > `uap3:AppExecutionAlias` > `desktop:ExecutionAlias`(Alias) | uap3, desktop | 14393 | runFullTrust | 별칭이 `%LOCALAPPDATA%\Microsoft\WindowsApps` 에 생기고 프로그램을 시작한다 |
| `[msix-extension]` 시작 작업 | `desktop:Extension` `windows.startupTask`(Executable, EntryPoint) > `desktop:StartupTask`(TaskId, Enabled, DisplayName) | desktop | 14393 | runFullTrust | 첫 시작 뒤에 등록된다 |
| `[shortcut]` Desktop | `desktop7:Extension` `windows.shortcut` > `desktop7:Shortcut`(File `$(Desktop)\<이름>.lnk`, Icon, Arguments, Description) | desktop7 | 19645 | runFullTrust | 사용자의 바탕화면에 바로가기가 생기고 프로그램을 시작한다 |
| `[shortcut]` Programs, StartMenu | 없음: 앱 자신의 시작 메뉴 항목 | - | - | - | 시작 메뉴에 앱이 있다 |
| `[font]` | `uap4:Extension` `windows.sharedFonts` > `uap4:SharedFonts` > `uap4:Font`(File `Fonts\<이름>`), 첫 앱에 | uap4 | 15063 | - | 패키지가 설치되어 있는 동안 다른 프로그램이 글꼴을 보고, 제거하면 보지 못한다 |

- 네임스페이스는 쓸 때만 `Package` 에 선언하고 무시 가능(ignorable)으로 둔다. 그래서 확장 없는 패키지는 위의
  매니페스트를 그대로 지킨다.
- `Parameters`, `Arguments`: 리터럴 글(MSI 의 `[...]` 는 거부). Windows 는 `%1` 자리에 파일이나 URI 를 넣는다.
- `FileTypeAssociation` 의 `Name`: prog-id 를 소문자로. prog-id 를 같이 쓰는 `[assoc]` 표들은 `FileType` 이
  여럿인 연결 하나가 된다.
- 모든 것은 원본이 대상으로 적은 실행 파일을 가진 앱에 들어간다. 앱의 실행 파일이 아닌 대상은 오류다.

## 묶음(`.msixbundle`)

Windows 의 묶음 작성기(`IAppxBundleWriter`)가 만드는 대로, 패키지처럼 놓인 ZIP 압축 파일(데이터 서술자를 가진
ZIP64 항목): [observed]

- 패키지들이 먼저, 저장(방식 0)으로, 그 파일 이름으로, 더한 순서대로. 그다음
  `AppxMetadata/AppxBundleManifest.xml`, `AppxBlockMap.xml`, `[Content_Types].xml` 을 deflate 로.
- 블록 맵은 묶음 매니페스트(`AppxMetadata\AppxBundleManifest.xml`)만 싣는다. 패키지들은 제 블록 맵을 가진다.
- `[Content_Types].xml`: `Default` `msix` = `application/vnd.ms-appx`, `Default` `xml` =
  `application/vnd.ms-appx.bundlemanifest+xml`, 그리고 `/AppxBlockMap.xml` 의 `Override`.
- 묶음 매니페스트, 줄 끝은 CRLF, 들여쓰기는 탭, 마지막 꼬리표 뒤에는 줄 끝이 없다:

```xml
<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<Bundle xmlns="http://schemas.microsoft.com/appx/2013/bundle" SchemaVersion="5.0" xmlns:b4="http://schemas.microsoft.com/appx/2018/bundle" xmlns:b5="http://schemas.microsoft.com/appx/2019/bundle" IgnorableNamespaces="b4 b5">
	<Identity Name="Example.App" Publisher="CN=Example" Version="1.0.0.0"/>
	<Packages>
		<Package Type="application" Version="1.0.0.0" Architecture="x64" FileName="Example.App_1.0.0.0_x64.msix" Offset="66" Size="61340">
			<Resources>
				<Resource Language="en-US"/>
			</Resources>
			<b4:Dependencies>
				<b4:TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.17763.0" MaxVersionTested="10.0.26100.0"/>
			</b4:Dependencies>
		</Package>
	</Packages>
</Bundle>
```

- `Offset` 은 묶음에서 패키지의 바이트가 시작하는 곳(로컬 머리 다음), `Size` 는 그 길이다. `Resources` 와
  `Dependencies` 는 패키지 매니페스트의 `Resource` 와 `TargetDeviceFamily` 요소를 되풀이한다.
- 묶음의 `Version` 은 작성기의 인수(부분마다 16비트인 64비트 수)다. rubrapack 은 패키지들의 판을 준다. 모든
  패키지는 묶음의 `Name` 과 `Publisher`, 한 가지 판, 그리고 저마다 다른 아키텍처를 가져야 한다.
- 같은 패키지들로 만든 rubrapack 의 묶음은 같은 항목을 같은 오프셋에 가지고, 매니페스트·블록 맵·콘텐츠
  형식이 바이트까지 같다. ZIP 날짜만 다르다(rubrapack 은 1980). Windows 는 그 가운데 자기 아키텍처의 패키지를
  설치한다: x64 컴퓨터는 x86 과 arm64 가 있어도 x64 를, x86 만 든 묶음에서는 x86 을. [observed]

## 서명(`AppxSignature.p7x`)

Windows 의 서명기(`APPX_SIP_CLIENT_DATA` 를 준 `mssign32!SignerSignEx2`; PowerShell 명령은 패키지에 서명하지
못한다)와 대조해 알아내고, rubrapack 이 서명한 패키지를 설치해 확인했다. [observed]

- 매니페스트의 `Publisher` 는 Windows 가 보여 주는 방식으로 쓴 서명 인증서의 주체와 같아야 한다: RDN 을
  마지막부터 처음으로, `A=value` 를 `, ` 로 이어서 - `/CN=Example/O=Example Ltd` 로 만든 인증서는
  `O=Example Ltd, CN=Example` 이 필요하다. 그렇지 않으면 서명이 0x8007000B 로 실패한다.
- 서명은 서명기가 하는 대로 압축 파일을 다시 쓴다: 페이로드의 로컬 레코드, 매니페스트, 블록 맵은 그대로
  두고, `[Content_Types].xml` 에
  `<Override PartName="/AppxSignature.p7x" ContentType="application/vnd.ms-appx.signature"/>` 를 더하고,
  서명을 맨 끝에 deflate 로, 크기를 로컬 머리에 담아(필요 판 2.0, 데이터 서술자 없음)더한다. 중앙 디렉터리는
  크기와 오프셋이 허락하는 한 ZIP32 방식(ZIP64 extra 필드 없음)으로 쓰고, 끝 레코드의 디스크 번호는 0 이다.
- `AppxSignature.p7x` 는 `PKCX` 뒤에 [authenticode.md](authenticode.md)와 같은 CMS SignedData 가 오되, 다음이
  다르다: `data` 는 `SEQUENCE { SpcSipInfo (1.3.6.1.4.1.311.2.1.30), SEQUENCE { INTEGER 0x01010000,
  OCTET STRING <SIP GUID>, INTEGER 0, INTEGER 0, INTEGER 0, INTEGER 0, INTEGER 0 } }` 이고 패키지 SIP GUID
  바이트는 `4B DF C5 0A 07 CE E2 4D B7 6E 23 C8 39 A0 9F D1`, 묶음 SIP GUID 바이트는
  `B3 58 5F 0F DE AA 9A 4B A4 34 95 74 2D 92 EC EB` 다. 서명 속성은 `contentType` 과 `messageDigest` 뿐이다.
- DigestInfo 의 다이제스트는 해시 하나가 아니라 `APPX` 뒤에 4바이트 꼬리표와 SHA-256 의 기록들이 오는
  것이다:
  - `AXPC`: 압축 파일 처음부터 서명 항목의 로컬 머리까지;
  - `AXCD`: 서명 항목을 뺀 중앙 디렉터리, 그다음 ZIP64 끝 레코드, 그 로케이터, 끝 레코드 - 모두 서명 항목이
    없는 것처럼(중앙 디렉터리가 서명의 로컬 머리 자리에서 시작하는 것처럼). Windows 는 서명 항목의 자리를
    그 ZIP32 필드에서 읽는다: ZIP64 방식으로 기술한 서명 항목(거기 0xFFFFFFFF)은 `HashMismatch` 이고, 서명한
    뒤 중앙 디렉터리를 그렇게 다시 쓴 패키지도 그렇다;
  - `AXCT`: `[Content_Types].xml`(원 바이트); `AXBM`: `AppxBlockMap.xml`;
  - `AXCI`: `AppxMetadata/CodeIntegrity.cat`, 패키지에 그것이 있을 때만.
- Windows 의 서명기는 패키지에 프로그램 파일이 있으면 `AppxMetadata/CodeIntegrity.cat` - 같은 키로 서명한,
  패키지 프로그램 파일의 카탈로그 - 도 더한다. 그것은 선택이다: 그것 없이 서명한 패키지도 설치되고
  실행된다. rubrapack 은 그것을 쓰지 않고, 있으면 `AXCI` 를 확인한다.
- 묶음: 안의 모든 패키지를 먼저 서명하고(저마다 제 `AppxSignature.p7x`), 그 둘레에 묶음을 쓰고, 묶음 SIP
  GUID 로 `AXCI` 없이 서명한다.
- `-AllowUnsigned` 와 publisher OID(아래)가 필요한 서명 없는 패키지는 서명하지 않는다: 서명은 그 OID 를 가진
  publisher 를 거부한다.

## 서명 없는 패키지 설치하기

- `Publisher` 가 `OID.2.25.311729368913984317654407730594956997722=1` 로 끝나야 한다. 그러면
  `Add-AppxPackage -AllowUnsigned` 가 Windows 11 에 설치한다. [spec]
- 실행 파일이 든 패키지는 관리자 권한 프로세스가 필요하다(아니면 0x80073D2B: 서명 없는 패키지는 실행 파일
  활성화를 가질 수 없다). 개발자 모드는 필요 없다. [observed]
- 네트워크 로그온(SSH 세션)에서 `Add-AppxPackage` 는 "PLM initialization" 에서 0x80070005 로 실패하고,
  대화형 세션에서는 된다. [observed]
- 파일 형식이 든 패키지를 제거하면 `HKCU\Software\Classes\.<ext>` 아래에 빈 `OpenWithProgids` 키가 남는다 -
  패키지가 아니라 Windows 가 하는 일이다. [observed]

## 실제 예: 튜토리얼의 hello.msix

튜토리얼 17장의 MSIX(`--unsigned-test`, x64)는 11009 바이트다. 첫 파일 `hello.exe` 의 로컬 머리로 시작한다:

| 오프셋 | 바이트 | 필드 | 값 |
|---|---|---|---|
| `0x00` | `50 4b 03 04` | 서명 | `PK\3\4` |
| `0x04` | `2d 00` | 필요한 판 | 45 (4.5, ZIP64) |
| `0x06` | `08 00` | 플래그 | 0x0008: 크기는 데이터 뒤에 |
| `0x08` | `08 00` | 방법 | 8 (deflate) |
| `0x0A` | `00 00 21 00` | 시각, 날짜 | 1980-01-01 00:00 |
| `0x0E` | `00 00 00 00 00 00 00 00 00 00 00 00` | CRC, 크기 | 0 (데이터 기술자에) |
| `0x1A` | `09 00 00 00` | 이름, 추가 필드 길이 | 9, 0 |
| `0x1E` | `68 65 6c 6c 6f 2e 65 78 65` | 이름 | `hello.exe` |

`hello.exe` 의 deflate 된 6706 바이트가 뒤따르고, 이어서 데이터 기술자가 온다: `50 4b 07 08 ac a3 c6 2e 32 1a 00 00 00 00
00 00 00 46 00 00 00 00 00 00` - `PK\7\8`, CRC-32 `2EC6A3AC`, 그리고 압축된 크기와 원래 크기를 8 바이트씩(6706,
17920).

`AppxBlockMap.xml` 에 있는 그 항목:

```xml
<File Name="hello.exe" Size="17920" LfhSize="39">
  <Block Hash="K7HbHzXLkhbwTo3dcUr0vs35DlYX27qMOexDOTR8e2k=" Size="6704"/>
```

파일이 64 KiB 보다 작아 블록은 하나다. `Hash` 는 원래 17920 바이트의 SHA-256 을 base64 로 쓴 것(여기서 계산한 값
`K7HbHzXLkhbwTo3dcUr0vs35DlYX27qMOexDOTR8e2k=`), `Size` 는 그 deflate 부분의 6704 바이트 - 압축된 크기 6706 에서 2
바이트 마지막 블록을 뺀 것 - 이고, `LfhSize` 는 로컬 머리의 39 바이트(30 + 이름 9 바이트)다.
