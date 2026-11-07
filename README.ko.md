# 커피 (Bloom 애드온)

[English](README.md)

[Bloom](https://github.com/hyeon1035298/Bloom) 런처의 애드온으로, Windows 11 작업표시줄에서 PC가 잠들지 않게 붙잡아 둡니다. 시계 옆에 커피잔이 있고, 오른쪽 클릭하면 PC가 깨어 있고 다시 오른쪽 클릭하면 잠들 수 있습니다. 켜져 있는 동안 잔이 Bloom에서 고른 강조색으로 채워집니다.

- 잔을 오른쪽 클릭해 바로 켜고 끄기
- 클릭하면 작은 창: 끌 때까지, 30분, 1시간, 2시간, 직접 입력한 분
- 화면도 켜 두기, 또는 시스템만 깨우고 화면은 평소처럼 꺼지게 두기
- 남은 시간은 잔의 툴팁과 창에 보입니다
- 탐색기가 다시 시작되어도 켜져 있던 것은 남은 시간만큼 이어 갑니다

기본 Windows 11 작업표시줄과 [PillTaskbar](https://github.com/hyeon1035298/PillTaskbar_BloomAddon) 어느 쪽에서도 똑같이 작동하며 PillTaskbar가 없어도 됩니다. 모니터 밝기 조정·메모리 정리 애드온과 함께 쓰면 커피, 밝기, RAM, 시계 순서로 나란히 놓이고, 창은 한 번에 하나만 열립니다. 화면을 켜 두는 동안에는 모니터 밝기 조정이 유휴 시 어둡게 하지 않습니다.

## 작동 방식

PillTaskbar의 애드온 런타임과 호스트가 `mod/`의 모드를 탐색기에 올립니다. 호스트는 모드가 작업표시줄 XAML에 닿는 데 쓰는 작업표시줄 함수를 찾고, 모드가 쓰는 Windhawk API를 제공하는 작은 런타임을 넣고, 계속 지켜봅니다. 호스트는 Bloom이 띄우는 별도의 작은 프로세스입니다.

탐색기 안에서 모드는 트레이에 잔을 넣고, 자기 스레드 하나에서 `SetThreadExecutionState`(`ES_CONTINUOUS | ES_SYSTEM_REQUIRED`, 화면도 켜 둘 때는 `ES_DISPLAY_REQUIRED`까지)를 부릅니다. Windows는 그 요청을 부른 스레드가 살아 있는 동안만 지키므로, 애드온을 끄거나 Bloom을 닫으면 PC는 다시 잠들 수 있습니다. 켜진 상태는 `%LOCALAPPDATA%\Bloom\addons\keep-awake\awake.json`에 적어 두고, 같은 부팅 안에서 탐색기가 다시 시작되면 이어 갑니다.

## 필요한 것

- Windows 11, 64비트
- Bloom 0.19.0 이상
- 처음 시작할 때 인터넷 연결(아래 참고)

## 설치

1. [Releases](https://github.com/hyeon1035298/KeepAwake_BloomAddon/releases)에서 `KeepAwake_BloomAddon-<버전>.zip`을 받습니다.
2. `%LOCALAPPDATA%\Bloom\user-addons\keep-awake\`에 풉니다. 폴더 이름은 `keep-awake`여야 합니다.
3. Bloom을 다시 시작하고 설정 › 애드온에서 "커피"를 켭니다.

Bloom은 애드온을 시작하기 전에 모든 파일을 `addon.json`의 SHA-256과 대조합니다. 지우려면 애드온을 끄고 폴더를 지우면 됩니다.

### 처음 시작

모드는 `taskbar.dll`의 내부 함수를 거쳐 작업표시줄 XAML에 닿으므로 호스트에 그 DLL의 기호가 필요합니다. 처음 시작할 때와 Windows 업데이트로 DLL이 바뀐 뒤에 Microsoft 기호 서버에서 맞는 공개 기호 파일을 받아 오프셋만 남깁니다. Microsoft의 파일은 애드온에 들어 있지 않습니다.

## 설정

Bloom의 설정 › 애드온 › 커피:

- **깨우기 방식**: 화면도 켜 두기(기본) 또는 시스템만 깨우기. 켜져 있는 동안 바꾸면 바로 적용됩니다.
- **기본 시간**: 잔을 오른쪽 클릭해 켤 때의 시간. 끌 때까지(기본), 30분, 1시간, 2시간, 직접 입력.
- **직접 입력 시간(분)**: 기본 시간이 직접 입력일 때의 분(처음 45). 창의 직접 입력 칸도 이 값에서 시작합니다.

창에서 고른 것은 그때 켠 한 번에만 쓰이고, 다음 오른쪽 클릭은 다시 이 설정에서 시작합니다. 라이트·다크는 Bloom의 테마를, 창의 유리는 Bloom의 유리 효과를, 잔과 스위치의 색은 Bloom의 강조색(Bloom 설정에서 읽음, Bloom이 없으면 Windows 강조색)을 따릅니다.

## 소스에서 빌드

`x86_64-w64-mingw32`를 대상으로 하는 clang(llvm-mingw나 Windhawk에 딸린 컴파일러), Python 3, Git Bash 같은 bash 셸이 필요합니다. PillTaskbar의 애드온 도구로 빌드하므로 두 저장소를 나란히 받습니다.

```
git clone https://github.com/hyeon1035298/KeepAwake_BloomAddon
git clone https://github.com/hyeon1035298/PillTaskbar_BloomAddon
cd KeepAwake_BloomAddon
export CXX=/path/to/llvm-mingw/bin/clang++.exe   # 없으면 Windhawk의 컴파일러
bash build.sh
```

PillTaskbar_BloomAddon을 다른 곳에 받았다면 `PILL_ROOT`로 가리킵니다. llvm-mingw에는 C++/WinRT 헤더가 없으므로, 워크플로처럼 Windows SDK의 `cppwinrt.exe`가 만든 폴더를 `CPPWINRT_INCLUDE`로 지정합니다. 결과는 `build/addon`에 생깁니다.

`.github/workflows/build.yml`은 푸시마다 `PILL_REF`에 고정한 PillTaskbar_BloomAddon 커밋으로 빌드합니다. `v*` 태그는 zip, 그 SHA-256, 두 저장소의 커밋을 적은 `SOURCE.txt`를 릴리스로 올립니다.

## 라이선스

이 애드온은 GNU General Public License 버전 3(3 한정)의 자유 소프트웨어입니다. `LICENSE`를 보십시오.

런타임과 호스트에는 MinHook(BSD-2-Clause)과 Zydis(MIT)가 들어 있습니다. 출처와 라이선스 전문은 `THIRD_PARTY.md`와 릴리스 zip의 `THIRD_PARTY_NOTICES.txt`에 있습니다.

이 프로그램에는 아무 보증이 없습니다. explorer.exe 안에서 코드를 실행하므로 각자 책임 아래 쓰십시오. Windows는 Microsoft의 상표입니다. 이 프로젝트는 Microsoft, Raycast, 다른 화면 깨우기 도구의 제작자와 관계가 없습니다.
