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

Visual Studio 2022 (Desktop development with C++ workload)에 CMake, Ninja, vcpkg가 모두 번들로 포함되어 있어 별도 설치가 필요 없습니다.

- Ninja: `<VS 설치 경로>/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe`
- vcpkg: `<VS 설치 경로>/VC/vcpkg/vcpkg.exe`

### VSCode에서 빌드 (권장)

1. `CMake Tools` 확장 설치
2. Kit 선택: `Visual Studio Build Tools 2022 Release - x86_amd64`
3. Configure Preset: `windows-ninja-msvc`
4. Build

### 터미널에서 빌드

MSVC 컴파일러(`cl.exe`)가 PATH에 있어야 하므로 **Developer PowerShell for VS 2022**에서 실행합니다.

```powershell
cmake --preset windows-ninja-msvc
cmake --build --preset windows-ninja-msvc
```

첫 configure 시 vcpkg가 `boost-beast`, `boost-asio`, `nlohmann-json`을 빌드하므로 다소 시간이 걸릴 수 있습니다.

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
