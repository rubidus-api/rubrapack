# Windows 폴더와 환경 변수

패키지는 만든 사람이 본 적 없는 PC 의 자리를 가리킨다: "프로그램 폴더", "이 사용자의 설정 폴더", "Windows 폴더".
어떤 PC 에서는 프로그램 폴더가 `C:\Program Files` 이고 다른 PC 에서는 `D:\Program Files` 다. 사용자 이름도 PC
마다 다르다. 이 장은 Windows 가 그런 자리를 어떻게 부르는지 - 환경 변수, 알려진 폴더(known folder), Windows
Installer 의 폴더 속성 - 그리고 각 이름이 *언제* 실제 경로로 바뀌는지 설명한다.

## 환경 변수란

실행 중인 프로그램은 저마다 **환경 변수**라는 작은 목록을 지닌다: `TEMP=C:\Users\<이름>\AppData\Local\Temp`
처럼 이름과 글 값의 짝이다. 프로그램은 자기를 실행한 프로그램에게서 이 목록의 사본을 받는다. 자기 사본을 바꿔도
다른 프로그램에게는 아무것도 바뀌지 않는다. 이름은 대소문자를 가리지 않는다: `%TEMP%`, `%Temp%`, `%temp%` 는
같은 변수다.

도구마다 쓰는 방식이 다르다:

| 어디서 | 변수를 쓰는 방식 |
|---|---|
| 명령 프롬프트(`cmd.exe`), 배치 파일 | `%TEMP%` |
| PowerShell | `$env:TEMP` |
| REG_EXPAND_SZ 형식의 레지스트리 값, 바로 가기의 대상 | `%TEMP%`, 값을 읽을 때 바뀐다 |
| Windows Installer 의 서식 문자열 | `[%TEMP]`, 설치하는 동안 바뀐다 |

목록이 어디서 오는가: 사용자가 로그온하면 Windows 가 다음으로 목록을 만든다.

- **시스템** 변수, 모든 사용자에게 같다(`HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment`).
- **사용자** 변수(`HKCU\Environment`). 사용자의 `Path` 는 시스템 `Path` 의 끝에 덧붙는다.
- Windows 가 로그온할 때 알아내는 값: 사용자 이름과 프로필 폴더, 도메인, 로그온 서버(`HKCU\Volatile Environment`),
  그리고 표준 폴더.

저장된 변수를 바꾸면 그 뒤에 시작한 프로그램에만 닿는다. 그래서 변수를 정하는 패키지(`[env.*]`, 튜토리얼 11장)를
설치한 뒤에는 명령 프롬프트를 새로 열어야 보인다.

## 흔히 쓰는 변수

Windows 10·11 에서 드라이브가 `C:` 일 때의 흔한 값이다. `<이름>` 은 사용자의 계정 이름이다.

### 폴더

| 변수 | 흔한 값 | 참고 |
|---|---|---|
| `SystemDrive` | `C:` | Windows 가 시작한 드라이브. 백슬래시가 없다 |
| `SystemRoot` | `C:\Windows` | Windows 폴더 |
| `windir` | `C:\Windows` | 같은 폴더의 옛 이름 |
| `ProgramFiles` | `C:\Program Files` | 64비트 Windows 의 32비트 프로그램에서는 `C:\Program Files (x86)` |
| `ProgramFiles(x86)` | `C:\Program Files (x86)` | 64비트 Windows 에만 있다 |
| `ProgramW6432` | `C:\Program Files` | 64비트 Windows 에만 있다. 32비트 프로그램에서도 64비트 폴더 |
| `CommonProgramFiles` | `C:\Program Files\Common Files` | 여러 프로그램이 함께 쓰는 부품. `(x86)`, `W6432` 형태는 위와 같다 |
| `ProgramData` | `C:\ProgramData` | 모든 사용자가 함께 쓰는 데이터 |
| `ALLUSERSPROFILE` | `C:\ProgramData` | 같은 폴더의 옛 이름 |
| `PUBLIC` | `C:\Users\Public` | 모든 사용자가 볼 수 있는 파일 |
| `USERPROFILE` | `C:\Users\<이름>` | 사용자의 프로필 폴더 |
| `HOMEDRIVE`, `HOMEPATH` | `C:`, `\Users\<이름>` | 사용자의 홈을 둘로 나눈 것. 도메인에서는 네트워크 공유(`HOMESHARE`)일 수 있다 |
| `APPDATA` | `C:\Users\<이름>\AppData\Roaming` | 로밍 프로필과 함께 옮겨 다니는 설정 |
| `LOCALAPPDATA` | `C:\Users\<이름>\AppData\Local` | 이 PC 에만 있는 설정과 캐시 |
| `TEMP`, `TMP` | `C:\Users\<이름>\AppData\Local\Temp` | 임시 파일. 서비스에서는 `C:\Windows\Temp` |

오래된 안내서에 나오는 `C:\Documents and Settings\<이름>\Application Data` 같은 경로는 Windows XP 의 것이다.
Windows Vista 부터 프로필은 `C:\Users` 아래에 있고, `ALLUSERSPROFILE` 은 `C:\ProgramData` 가 되었다.

Program Files 폴더가 둘이라서 32비트 프로그램이 `%ProgramFiles%` 를 물으면 `C:\Program Files (x86)` 을 받는다:
Windows 는 프로그램마다 자기 종류의 폴더를 준다.

### 폴더가 아닌 것

| 변수 | 흔한 값 | 참고 |
|---|---|---|
| `USERNAME` | `<이름>` | 계정 이름 |
| `USERDOMAIN` | `OFFICE`, 또는 PC 이름 | 계정의 도메인. 로컬 계정이면 컴퓨터 이름 |
| `LOGONSERVER` | `\\DC01`, 또는 `\\` 와 PC 이름 | 비밀번호를 확인한 컴퓨터 |
| `COMPUTERNAME` | `DESKTOP-1A2B3C` | 이 PC 의 이름 |
| `ComSpec` | `C:\Windows\system32\cmd.exe` | 명령 해석기 |
| `Path` | `C:\Windows\system32;C:\Windows;...` | 폴더 없이 입력한 프로그램을 찾는 폴더들, `;` 로 나눈다 |
| `PATHEXT` | `.COM;.EXE;.BAT;.CMD;...` | 확장자 없이 입력한 프로그램에 붙여 보는 확장자 |
| `OS` | `Windows_NT` | |
| `PROCESSOR_ARCHITECTURE` | `AMD64`, `ARM64` 또는 `x86` | 32비트 프로그램에서는 `x86` 이고, 실제 값은 `PROCESSOR_ARCHITEW6432` 에 있다 |
| `NUMBER_OF_PROCESSORS` | `8` | |

명령 프롬프트는 `%CD%`, `%DATE%`, `%TIME%`, `%RANDOM%`, `%ERRORLEVEL%` 에도 답하지만, 물을 때 지어내는
값이다. 환경에 들어 있지 않으므로 다른 프로그램은 보지 못한다.

## 변수가 없는 폴더

바탕 화면, 시작 메뉴와 그 안의 프로그램 폴더, 시작 프로그램 폴더, 글꼴 폴더, 문서 폴더에는 환경 변수가 없다.
Windows 는 이것들을 **알려진 폴더**로 부르고, 프로그램은 `SHGetKnownFolderPath` 로 묻는다. 변수가 있는 폴더도
이쪽이 믿을 만하다: 프로그램을 실행하는 쪽이 환경을 마음대로 줄 수 있고, 사용자가 문서나 바탕 화면을 다른 곳(예를
들어 OneDrive 안)으로 옮겼을 수 있는데, 그것은 알려진 폴더에만 반영된다.

## 세 시점

이름은 세 시점 가운데 하나에서 경로가 되고, 그 시점이 *누구의* 경로인지를 정한다.

1. **빌드할 때**, `rubrapack build` 를 실행하는 기계에서. 대상 PC 에 대해서는 아직 아무것도 모른다. rubrapack 의
   `$(NAME)` 변수가 여기서 바뀐다. 빌드 기계의 환경 변수는 읽지 않는다(값은 `-D NAME=value` 로 넘긴다).
2. **설치할 때**, 대상 PC 에서. Windows Installer 가 폴더 속성과 서식 문자열의 `[%NAME]` 부분을 설치하는 사용자의
   것으로 알아낸다. 관리자가 설치하는 컴퓨터 전체용 패키지에서 "로컬 응용 프로그램 데이터" 폴더는 관리자의 것이다.
3. **실행할 때**, 프로그램이 값을 읽을 때마다. `%LOCALAPPDATA%\Hello\log` 를 담은 REG_EXPAND_SZ 값은 사용자마다
   자기 폴더를 준다. 각 사용자의 프로그램이 그 사용자의 환경으로 풀기 때문이다.

그래서 사용자마다의 로그 폴더는 실행할 때를 위해 쓴다:

```toml
[registry.LogDir]
root = "HKLM"
key = 'SOFTWARE\Example Software\Hello'
name = "LogDir"
type = "expand"
value = '%LOCALAPPDATA%\Hello\log'        # 사용자마다 프로그램이 푼다
```

`value = '[LocalAppDataFolder]Hello\log'` 라고 쓰면 설치한 사용자의 폴더가 모든 사용자에게 저장된다.

## Windows Installer 의 이름

Windows Installer 에는 대부분의 폴더에 자기 속성이 있고, 설치할 때 값이 정해진다:

| 변수 | Windows Installer 속성 |
|---|---|
| `ProgramFiles` | 64비트 패키지에서 `ProgramFiles64Folder`, 32비트 패키지에서 `ProgramFilesFolder` |
| `ProgramFiles(x86)` | `ProgramFilesFolder` |
| `CommonProgramFiles` | `CommonFiles64Folder` / `CommonFilesFolder` |
| `ProgramData`, `ALLUSERSPROFILE` | `CommonAppDataFolder` |
| `APPDATA` | `AppDataFolder` |
| `LOCALAPPDATA` | `LocalAppDataFolder` |
| `TEMP` | `TempFolder` |
| `SystemRoot`, `windir` | `WindowsFolder` |
| `SystemDrive` | `WindowsVolume`(백슬래시까지: `C:\`) |
| (`System32`) | 64비트 패키지에서 `System64Folder`. `SystemFolder` 는 32비트 시스템 폴더다 |
| `USERNAME` | `LogonUser` |
| `COMPUTERNAME` | `ComputerName` |

폴더 속성은 모두 백슬래시로 끝나므로 `[ProgramFiles64Folder]Hello` 사이에 백슬래시를 넣지 않는다. `USERNAME` 을
조심한다: Windows Installer 의 같은 이름 속성은 등록할 때 입력한 이름이지 계정이 아니다. 계정은 `LogonUser` 다.
속성이 없는 변수는 `[%NAME]` 으로 쓴다.

## rubrapack 에서

rubrapack 은 이 이름들을 모두 한 가지로 쓴다: Windows 의 철자로 쓴 `$(NAME)`(대소문자는 가리지 않는다). `$$` 는
`$` 한 글자다. 이름이 언제 바뀌는지는 놓인 자리가 정한다:

- **경로의 맨 앞**에서는 폴더다: `path = "$(ProgramFiles)/Hello"`, `path = "$(LOCALAPPDATA)/Hello"`,
  `path = "$(Fonts)"`. rubrapack 이 알맞은 Windows Installer 폴더로, MSIX 에서는 알맞은 패키지 폴더로
  바꾼다(튜토리얼 4장).
- **Windows Installer 가 채우는 값** - 레지스트리·환경 변수·INI 값, 인수, 메시지 - 에서는 설치할 때의 형태다:
  `'$(USERPROFILE)\notes'` 는 `[%USERPROFILE]\notes` 가 되고, `'$(LOCALAPPDATA)\Hello'` 는
  `[LocalAppDataFolder]Hello` 가 된다(폴더 속성은 이미 `\` 로 끝나므로 이름 뒤의 `\` 하나는 빠진다).
- **`type = "expand"` 레지스트리 값**에서는 실행할 때의 형태다: `'$(LOCALAPPDATA)\Hello\log'` 는
  `%LOCALAPPDATA%\Hello\log` 로 저장되고, 사용자마다 프로그램이 그 사용자의 것으로 푼다.

그래서 위의 사용자마다의 로그 폴더는 이렇게 쓴다:

```toml
[registry.LogDir]
root = "HKLM"
key = 'SOFTWARE\Example Software\Hello'
name = "LogDir"
type = "expand"
value = '$(LOCALAPPDATA)\Hello\log'       # %LOCALAPPDATA%\Hello\log 로 저장된다
```

`type = "expand"` 가 없으면 같은 값이 설치한 사용자의 폴더를 저장한다. 모든 이름과 그것이 무엇이 되는지는 참조
부의 [Windows 이름](../rpk.md#windows-이름)에 표로 있다. MSIX 에는 설치할 때가 없다: 그것이 필요한 값은 거기서
거부하고(RP1612), 실행할 때의 형태는 프로그램이 풀므로 쓸 수 있다.

## 쓰이는 곳

- 튜토리얼 [4](../tutorial/04-files-and-folders.md)장: 설치 폴더와 그 기준.
- 튜토리얼 [11](../tutorial/11-registry-environment-ini.md)장: 레지스트리 값, 환경 변수, `%TEMP%`.
- 참조: [레지스트리 값](../rpk.md#레지스트리-값-registryid), [환경 변수](../rpk.md#환경-변수-envid),
  [변수](../rpk.md#변수).
