# QA 체크리스트

다른 사람이나 AI가 이 문서만 보고 QA를 돌릴 수 있게 정리했습니다.
결과는 각 항목에 **통과 / 실패 / 확인 불가**와 재현 방법, 스크린샷, 로그를 붙여 주세요.

## 0. 준비

- 위치: `D:\project\Mulddungddunge` (GitHub: https://github.com/ilfpns/Mulddungddunge)
- 요구 사항: Windows 11 (10도 가능), Visual Studio 2022 이상 C++ 도구, Windows SDK
- 빌드: `D:\project\Mulddungddunge\build.bat` → `bin\stage-manager.exe`
- 패키지: `powershell -ExecutionPolicy Bypass -File D:\project\Mulddungddunge\packaging\package.ps1`
  → `dist\StageManager-<버전>-win64.exe`(설치형), `dist\StageManager-<버전>-portable.zip`
- 이벤트 추적 켜기(권장): 앱을 시작하기 전에 빈 파일 `%TEMP%\stage-manager.trace`를 만듭니다.
- 오류 로그: `%TEMP%\stage-manager.log`

### 반드시 지킬 것

1. **앱을 강제 종료하지 마세요** (`taskkill /F`, 작업 관리자 "작업 끝내기"). 앱은 실행 중에 Windows 최소화 애니메이션을 끄고 정상 종료할 때 되돌립니다. 끄려면 트레이 아이콘 우클릭 → **종료**, 또는 아래 명령을 쓰세요.
   ```powershell
   Add-Type -Namespace Q -Name N -MemberDefinition '[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, IntPtr t); [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);'
   [Q.N]::PostMessage([Q.N]::FindWindow('StageManagerSidebar', [IntPtr]::Zero), 0x111, [IntPtr]1, [IntPtr]::Zero)
   ```
   실수로 강제 종료했다면 앱을 다시 한 번 실행했다가 정상 종료하면 원래 설정이 복구됩니다.
2. 앱을 켜면 **현재 창 하나만 남기고 나머지 창은 최소화**됩니다. 저장하지 않은 작업이 있으면 먼저 저장하세요.
3. 테스트용 창이 필요하면 메모장처럼 여러 창이 한 프로세스를 공유하는 앱보다, 별도 프로세스로 뜨는 창을 쓰세요. 테스트 창을 끌 때 프로세스 이름으로 끄면 사용자 창까지 꺼질 수 있습니다(Windows 11 메모장은 모든 탭이 한 프로세스). 반드시 **PID로** 끄세요.
   ```powershell
   # 색이 다른 테스트 창 5개 (python 필요), PID는 $pids에 보관
   $pids = 1..5 | ForEach-Object { (Start-Process pythonw -PassThru -ArgumentList "-c `"import tkinter as t;r=t.Tk();r.title('QA $_');r.geometry('1000x700+$(300+60*$_)+$(150+40*$_)');r.configure(bg='#$(('c0392b','2980b9','27ae60','8e44ad','d35400')[$_-1])');r.mainloop()`"").Id }
   # 끝나면: $pids | ForEach-Object { Stop-Process -Id $_ }
   ```

## 1. 설치 / 제거

| # | 절차 | 기대 결과 |
|---|---|---|
| 1.1 | `StageManager-<버전>-win64.exe` 실행 | UAC(관리자 권한) 창이 뜨지 않음. 자동 실행 여부를 묻는 창이 뜨고, 설치 완료 안내 후 앱이 실행됨 |
| 1.2 | 시작 메뉴에서 "Stage Manager" 검색 | 전용 아이콘의 바로가기가 있음 |
| 1.3 | 설정 > 앱 > 설치된 앱 | "Stage Manager for Windows" 1.0.0, 게시자 Mulddungddunge |
| 1.4 | 앱 실행 중에 설치형 exe를 다시 실행 | 기존 앱이 몇 초 안에 정상 종료되고 새로 설치·실행됨 (강제 종료 아님) |
| 1.5 | 설정 > 앱에서 제거 | 확인 창 후 앱 종료, 바로가기·제거 항목·자동 실행 항목·설치 폴더 삭제 |
| 1.6 | 제거 후 창 최소화 | Windows 기본 최소화 애니메이션이 정상적으로 보임 |
| 1.7 | 휴대용 zip 압축 해제 후 `stage-manager.exe` 실행 | 설치 없이 동작 |

## 2. 기본 동작

| # | 절차 | 기대 결과 |
|---|---|---|
| 2.1 | 창 여러 개를 띄운 상태에서 앱 실행 | 왼쪽에 기울어진 카드가 세로 중앙 정렬로 최대 4개. 전경 창만 남고 나머지는 최소화 |
| 2.2 | 카드 모양 | 모두 같은 3:2 모양, 같은 기울기. 카드 사이 틈이 **평행한 일자**(삼각형 아님) |
| 2.3 | 앱 아이콘 배지 | 모든 카드에서 카드 왼쪽 아래 모서리의 같은 위치. 사이드바 왼쪽 밖으로 잘리지 않음. 가장자리가 깨지지 않음 |
| 2.4 | 이미 최소화돼 있던 창의 카드 | 회색 빈 카드가 아니라 앱 아이콘과 창 제목이 선명하게 보임 |
| 2.5 | 카드에 마우스 올리기 | 카드가 살짝 커짐 |
| 2.6 | 카드 클릭 | 클릭한 창이 기울기를 풀며 날아와 원래 위치/크기로 복원·활성화. 이전 창은 사이드바 첫 칸으로 기울며 들어감. 나머지 카드는 한 칸씩 밀림 |
| 2.7 | 2.6을 60fps로 녹화 | **깜빡임 없음**: 사이드바가 사라지는 프레임, 창이 번갈아 보였다 사라지는 프레임이 없어야 함 |
| 2.8 | Alt+1, Alt+2, Alt+3, Alt+4 | 해당 순서의 카드로 전환 (2.6과 같은 애니메이션) |
| 2.9 | 트레이 아이콘 우클릭 | "Windows 시작 시 실행"(체크 표시), "종료" |

## 3. 외부 동작

| # | 절차 | 기대 결과 |
|---|---|---|
| 3.1 | Alt+Tab으로 다른 창 선택 | 이전 창이 사이드바로 날아 들어감 |
| 3.2 | 작업 표시줄에서 최소화된 창 클릭 | 그 창이 뜨고, 사이드바에서 그 카드가 빠지고, 이전 창이 사이드바로 들어감 |
| 3.3 | 새 프로그램 실행 | 이전 창이 사이드바로 들어감 |
| 3.4 | 가운데 창의 최소화 버튼 (창이 뜬 지 1초 이상 지난 뒤) | 창이 제자리에서 사이드바 첫 칸으로 빨려 들어감 (작업 표시줄로 줄어드는 기본 애니메이션 없음) |
| 3.5 | 터치패드 네 손가락 아래로 쓸기 / Win+D | 3.4와 같음 |
| 3.6 | 사이드바에 있는 창을 작업 표시줄에서 닫기 | 카드가 사라지고 나머지가 채워짐 |
| 3.7 | 가상 데스크톱 전환 (Win+Ctrl+←/→) | 사이드바가 그 데스크톱의 창만 보여줌. 이전 데스크톱의 창이 섞이지 않음 |
| 3.8 | 전체화면 영상(브라우저 F11, 유튜브 전체화면) 또는 게임 | 사이드바가 왼쪽으로 미끄러져 사라짐. 그동안 Alt+1~4가 그 앱(VS Code 탭 전환 등)으로 전달됨. 전체화면 해제 시 다시 나옴 |
| 3.9 | 항상 위 창(데스크톱 펫, PIP) | 관리 대상이 아님: 최소화되거나 카드가 되지 않음 |
| 3.10 | 가운데 창 최대화 | 창이 화면 왼쪽 끝까지 전부 사용. 사이드바는 왼쪽으로 미끄러져 접힘 |
| 3.11 | 3.10 상태에서 화면 왼쪽 끝에 마우스 | 사이드바가 최대화 창 위로 나옴. 마우스를 떼면 약 0.35초 뒤 다시 접힘 |
| 3.12 | 3.10 상태에서 Alt+1 | 사이드바가 나오며 전환 애니메이션, 새 창이 최대화면 끝난 뒤 다시 접힘 |
| 3.13 | 창을 끌어 사이드바 영역에 걸치게 두기 | 사이드바 접힘. 창을 치우면 다시 나옴 |
| 3.14 | 창을 전환하고 0.5초 안에 최소화 | 회색 카드가 아니라 그 창의 (마지막으로 찍힌) 모습으로 들어감 |
| 3.15 | 여러 창을 최소화해 둔 상태에서 앱 시작 | 최소화된 창의 카드도 실제 모습으로 보임 (작업 표시줄 미리보기와 같은 마지막 모습) |

## 4. 드래그 / 메뉴 / 고정

| # | 절차 | 기대 결과 |
|---|---|---|
| 4.1 | 카드를 끌어 화면 가운데에 놓기 | 카드가 커서를 따라오며 기울기가 풀림. 놓으면 지금 창은 그대로, 놓은 자리에 그 창이 원래 크기로 함께 열림 (최대화 창은 작업 영역의 2/3) |
| 4.2 | 4.1 후 두 창 사이 클릭 | 전환 애니메이션 없이 포커스만 이동 |
| 4.3 | 4.1 후 다른 카드 클릭 | 가운데 두 창이 각자 카드로 동시에 날아감 |
| 4.4 | 카드를 끌다가 사이드바 위에 놓기 | 카드가 제자리로 돌아감 |
| 4.5 | 카드 우클릭 | 카드 옆에 어두운 둥근 메뉴: 📌 탭 고정, ✕ 창 닫기. 호버 강조가 부드럽게 이동 |
| 4.6 | 메뉴 바깥 클릭 | 메뉴 닫힘 |
| 4.7 | 탭 고정 | 카드 오른쪽 위에 흰 점. 다른 창을 오가도 그 카드가 사이드바 위쪽에 계속 있음. 그 카드를 클릭해도 카드는 사이드바에 남음 |
| 4.8 | 앱 종료 후 다시 실행 | 4.7의 고정이 유지됨 (`HKCU\Software\StageManager\Pinned`) |
| 4.9 | 고정한 앱(카카오톡 등)을 닫았다가 다시 열기 | 새 창도 고정 상태 |
| 4.10 | 고정 해제 | 흰 점 사라지고 최근 사용 순서로 돌아감 |
| 4.11 | 창 닫기 | 그 창에 닫기 요청 (저장 확인 창이 뜰 수 있음). 닫히면 카드 제거 |

## 5. 자원 사용 (가장 중요)

앱을 켜고 아무것도 하지 않은 상태로 10초 기다린 뒤 20초간 측정합니다. **측정 중에는 마우스와 키보드를 쓰지 마세요.**

```powershell
$p = Get-Process stage-manager; $id = $p.Id; $c0 = $p.CPU
$gpu = Get-Counter "\GPU Engine(pid_${id}_*)\Utilization Percentage" -SampleInterval 2 -MaxSamples 10
$p.Refresh()
"CPU 초 (20초 동안): " + ($p.CPU - $c0)
"GPU 사용률 최대 %: " + ($gpu.CounterSamples | Measure-Object CookedValue -Maximum).Maximum
"작업 관리자 메모리 MB: " + [math]::Round((Get-Counter "\Process(stage-manager)\Working Set - Private").CounterSamples[0].CookedValue / 1MB, 1)
"GPU 메모리 MB: " + [math]::Round((Get-Counter "\GPU Process Memory(pid_${id}_*)\Total Committed").CounterSamples[0].CookedValue / 1MB, 1)
```

| # | 기준 |
|---|---|
| 5.1 | 대기 CPU: 20초 동안 0.02초 이하 |
| 5.2 | 대기 GPU 사용률: 0.1% 이하 |
| 5.3 | 작업 관리자 메모리: 10MB 이하 |
| 5.4 | GPU 메모리: 40MB 이하 (창 수에 따라 변함) |
| 5.5 | 전환 20회 전후 핸들 수 비교(`(Get-Process stage-manager).HandleCount`): 계속 늘어나지 않음 |
| 5.6 | 앱을 켠 상태와 끈 상태에서 게임/영상 FPS 차이 없음 |

## 6. 안정성

| # | 절차 | 기대 결과 |
|---|---|---|
| 6.1 | 빠르게 연속 클릭, Alt+1~4 연타, 드래그 중 Alt+Tab | 멈춤·크래시 없음. 애니메이션 도중 입력은 무시되거나 순서대로 처리 |
| 6.2 | 1시간 이상 평소처럼 사용 | 크래시 없음. `%TEMP%\stage-manager.log`에 `crash` 줄이 없어야 함 |
| 6.3 | 앱 실행 중 강제 종료 후 재실행 → 정상 종료 | Windows 최소화 애니메이션이 원래대로 돌아옴 |
| 6.4 | 모니터 해상도/배율 변경 | 사이드바가 새 크기에 맞게 다시 배치 |

## 7. 보고할 때 첨부

- `%TEMP%\stage-manager.log`, `%TEMP%\stage-manager.trace`
- 크래시가 있었다면 로그의 `rva=0x...` 값과 그때의 커밋 해시 (`git -C D:\project\Mulddungddunge log -1 --oneline`)
- 화면 녹화(가능하면 60fps), 모니터 배율, Windows 버전, GPU
