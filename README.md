# 살인의 추억 (Memories of Murder)

멀티플레이어 텍스트 기반 추리 게임. 설계 배경과 전체 로드맵은 [docs/DESIGN.md](docs/DESIGN.md) 참고.

## 개발 환경

- **언어/표준**: C++20
- **빌드**: CMake + Ninja (MSVC 툴체인)
- **패키지 관리**: vcpkg (manifest mode, `vcpkg.json`)
- **네트워크**: Boost.Beast / Boost.Asio (WebSocket)
- **직렬화**: nlohmann::json
- **AI**: Ollama (Phase 2에서 통합 예정, HTTP 연동)

### 사전 준비

Visual Studio (Desktop development with C++ workload)에 CMake, Ninja, vcpkg가 모두 번들로 포함되어 있어 별도 설치가 필요 없습니다. `CMakePresets.json`의 `VCPKG_ROOT`는 현재 개발 환경 기준 경로(`D:/Visual Studio2026/VC/vcpkg`)로 설정되어 있으니, 다른 환경에서는 실제 설치 경로에 맞게 수정하거나 `VCPKG_ROOT` 환경변수로 덮어쓰세요.

- Ninja: `<VS 설치 경로>/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe`
- vcpkg: `<VS 설치 경로>/VC/vcpkg/vcpkg.exe`

### VSCode에서 빌드 (권장)

1. `CMake Tools` 확장 설치
2. Kit 선택: 설치된 MSVC amd64 Kit (예: `Visual Studio Build Tools ... - x86_amd64`)
3. Configure Preset: `windows-ninja-msvc`
4. Build

### 터미널에서 빌드

MSVC 컴파일러(`cl.exe`)가 PATH에 있어야 합니다. 저장소 루트의 `build.bat`가 `vcvarsall.bat` 호출부터 configure/build까지 처리해주므로 일반 PowerShell/cmd에서 바로 실행하면 됩니다.

```powershell
.\build.bat
```

Developer PowerShell for VS를 이미 사용 중이라면 직접 실행해도 됩니다.

```powershell
cmake --preset windows-ninja-msvc
cmake --build --preset windows-ninja-msvc
```

첫 configure 시 vcpkg가 `boost-beast`, `boost-asio`, `nlohmann-json`(사실상 Boost 전체)을 소스에서 빌드하므로 수 분 정도 걸립니다. 이후에는 캐시되어 몇 초 안에 끝납니다.

### 실행 및 연결 테스트

```powershell
./build/server/game_server.exe
```

서버가 `ws://localhost:9002`에서 대기합니다. `client/index.html`을 브라우저로 열어 연결/에코 테스트를 할 수 있습니다.

> 현재 `game_server`는 게임 로직이 없는 순수 WebSocket 에코 서버로, 툴체인(MSVC + Ninja + vcpkg + Boost.Beast)이 정상 동작하는지 검증하기 위한 최소 골격입니다. 실제 게임 로직은 [docs/DESIGN.md](docs/DESIGN.md)의 Phase 1부터 순차적으로 구현합니다.

## 디렉토리 구조

```
├── server/          C++ 게임 서버
│   └── src/
├── client/          테스트용 정적 HTML/JS 클라이언트
├── docs/            설계 문서
├── vcpkg.json       vcpkg manifest
└── CMakePresets.json
```
