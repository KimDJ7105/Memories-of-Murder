# Phase 5 안정성 테스트 기록

`docs/DESIGN.md` 3단계(안정성 검증)에 나열된 시나리오를 실제로 스크립트로 재현해보며 찾은 문제와 조치를 시간순으로 남긴다. 통과한 항목도 "무엇을 어떻게 확인했는지"를 남겨서, 나중에 회귀가 생겼을 때 같은 시나리오를 그대로 다시 돌릴 수 있게 한다.

---

## 1. 방 종료 중 AI 콜백 도착 → 서버 전체 크래시 (use-after-free)

**가설**

`GameRoom::run_ai_judging()`과 `handle_submit_guess()`는 `evaluator_->evaluate()` / `judge_->judge()`에 `this`를 캡처하는 콜백을 넘긴다. `RoomManager`는 방이 비면(`GameRoom::is_empty()`) 그 방의 `GameRoom`을 `unique_ptr`째로 즉시 파괴한다. 만약 Ollama에 보낸 HTTP 요청이 아직 응답하기 전에 방의 마지막 플레이어까지 나가서 `GameRoom`이 파괴되면, 나중에 도착하는 AI 응답 콜백은 이미 해제된 메모리(`this`)를 참조하게 된다.

**테스트**

실제 Ollama 백엔드(`qwen2.5:7b`)로 서버를 띄우고:

1. 3명이 방에 들어가 게임을 시작한다.
2. 범인이 `submit_crime`을 제출한다 (`AIJudging` 상태로 전이, `evaluator_->evaluate()` 호출 시작).
3. AI 응답을 기다리지 않고 0.3초 뒤 3명 전원의 WebSocket 연결을 즉시 닫는다 — 방이 비어 `RoomManager`가 `GameRoom`을 파괴한다.
4. 20초 대기 후 서버 프로세스가 살아있는지 확인.

같은 방식으로 `Investigation` 상태(`judge_->judge()` 호출 중)에서도 동일하게 재현.

**결과 (수정 전)**

`tasklist` / `Get-Process game_server`가 프로세스를 찾지 못함 — **서버 프로세스 자체가 죽었다.** 이건 해당 방 하나만의 문제가 아니라, 그 순간 서버에 떠 있던 **다른 모든 방까지 함께 종료**시키는 심각도였다.

`handle_submit_guess`의 판정 콜백에는 `turn_generation_` 값을 비교하는 가드가 이미 있었지만, 이 가드 자체가 `this->turn_generation_`을 읽는 코드이므로 `this`가 이미 해제된 상황에서는 이 비교를 하는 시점에 이미 미정의 동작(UB)이다. 즉 기존 가드는 "같은 방 안에서 턴/라운드가 넘어간" 경우만 방어했을 뿐, "방 자체가 파괴된" 경우는 전혀 방어하지 못했다.

**조치**

`GameRoom`에 이 방만 소유하는 `std::shared_ptr<char> alive_` 토큰을 추가하고, AI 콜백 두 곳(`run_ai_judging`, `handle_submit_guess`) 모두에서 콜백 진입 시 **가장 먼저** `std::weak_ptr<char>`가 만료됐는지 확인하도록 수정. `GameRoom`이 파괴되면 `alive_`의 참조 카운트가 0이 되어 weak_ptr이 즉시 만료되므로, 콜백은 `this`의 다른 멤버(`turn_generation_` 포함)를 전혀 건드리지 않고 안전하게 반환한다.

```cpp
std::weak_ptr<char> alive = alive_;
evaluator_->evaluate(crime, *selected_map_, [this, ..., alive](CrimeEvaluation eval) {
    if (alive.expired()) return;  // 방이 이미 파괴됨
    if (generation != turn_generation_) return;  // 방은 살아있지만 라운드/턴이 넘어감
    ...
});
```

**재검증 결과**

동일한 두 시나리오(범행 평가 콜백 / 추리 판정 콜백)를 수정된 빌드로 재실행 — 두 경우 모두 서버 프로세스가 동일한 PID로 계속 살아있음을 확인. 이후 정상 플레이 경로(Mock 백엔드로 방 생성 → 게임 시작 → 범행 제출 → 추리 제출 → 정답 판정까지)를 다시 돌려 회귀가 없음도 확인.

---

## 2. 게임 진행 중 방장 Disconnect → 방장 승계가 로비에서만 동작

**가설**

`GameRoom::handle_disconnect`를 읽어보니 방장이 나갔을 때 다른 플레이어에게 방장을 넘기는 로직(`host_id_ = players_.front().id`)이 `state_ == GameState::Lobby` 분기 안에만 있었다. 게임이 시작된 뒤(`CrimeWriting` 이후)에는 이 재할당이 전혀 일어나지 않는다.

`restart_game`(`GameOver`에서만 허용)과 `next_round`(`NextRound`에서만 허용)는 둘 다 `player_id == host_id_` 검사를 통과해야 하는데, `next_round`는 15초 서버 타이머로 자동 진행되는 안전장치가 있지만 `restart_game`은 그런 타이머가 전혀 없다. 즉 게임 도중 방장이 나가고 다시 들어오지 않으면(이 게임엔 재접속 기능이 없다), 게임이 끝나도 아무도 다시 시작할 수 없는 방이 영구적으로 남을 것으로 예상.

**테스트**

3인 방에서 게임을 시작하고, `CrimeWriting` 상태에서 방장(범인이 아닌 경우로 확인)의 WebSocket 연결을 닫은 뒤, 남은 플레이어에게 오는 `room_update`의 `is_host` 필드를 확인.

**결과 (수정 전)**

```json
{"id": 1, "name": "Host", "connected": false, "is_host": true}
{"id": 2, "name": "P2",   "connected": true,  "is_host": false}
{"id": 3, "name": "P3",   "connected": true,  "is_host": false}
```

연결이 끊긴 플레이어가 계속 `is_host: true`로 남아있음을 확인 — 가설대로 방장 자리가 죽은 세션에 영구히 고정된다.

**조치**

`handle_disconnect`에서 `p->connected = false` 직후, `state_`와 무관하게 "나간 사람이 방장이었다면 연결돼 있는 다른 아무나에게 즉시 넘긴다"는 로직을 추가:

```cpp
if (host_id_ == player_id) {
    auto it = std::find_if(players_.begin(), players_.end(),
                            [](const Player& pl) { return pl.connected; });
    host_id_ = (it != players_.end()) ? it->id : -1;
}
```

**재검증 결과**

같은 시나리오 재실행 — 방장이 끊기자마자 다음 `room_update`에서 `P2`가 `is_host: true`로 즉시 승계됨을 확인. 이후 전체 라운드 회귀 테스트와 잘못된 상태 요청 테스트(아래 3번)도 다시 통과.

---

## 3. 잘못된 상태에서의 요청 / 중복 요청 — 스캔 결과 (수정 불필요)

**배경**

`GameRoom`의 모든 메시지 처리는 하나의 `io_context` 스레드에서 순서대로 실행되고, 상태 전환(`state_` 대입)이 비동기 호출(AI 평가/판정) 이전에 이미 동기적으로 끝나 있는 구조라, 설계상 이중 처리/잘못된 상태 요청에 이미 안전할 것으로 예상했다. 이를 직접 스크립트로 확인했다.

**테스트**

아래 11가지 요청을 각 상태에서 보내고 매번 `error` 응답이 오는지 확인 (mock 백엔드, 회귀 확인 목적):

- 로비 상태: 존재하지 않는 진행 단계에 `submit_guess`, 방장 아닌 사람의 `submit_crime`/`start_game`/`select_map`, 존재하지 않는 지도 id로 `select_map`, 로비에서 `next_round`/`restart_game`
- 게임 진행 중: 탐정의 `submit_crime`, 이미 시작된 게임에 `start_game`/`select_map`, `Investigation` 시작 전 `submit_guess`

이미 알려져 있던 이중 제출 케이스(`submit_guess` 연타, `guess_in_flight_`로 방어됨)도 별도로 회귀 확인.

**결과**

11개 전부 정확한 한국어 오류 메시지와 함께 `error` 응답을 받았고, 부수효과(잘못된 브로드캐스트, 상태 오염)는 관찰되지 않았다. 이 항목들에서는 실제 버그를 찾지 못했다 — 상태 머신이 처음부터 이 케이스들을 잘 방어하도록 짜여 있었던 것으로 판단, 별도 조치 없이 회귀 테스트 스크립트로만 남겨둔다.

---

## 결론 및 Phase 4 판단에 대한 시사점

이번 라운드에서 나온 두 실제 버그(1번, 2번)는 모두 **AI 비동기 호출 자체의 처리량 문제가 아니라 콜백/상태 수명 관리 문제**였다. Job Queue/Worker Thread를 추가한다고 해서 이 두 버그가 자동으로 해결되지는 않았을 것 — 오히려 워커 스레드가 게임 스레드와 별도로 존재했다면 `this` 캡처 문제가 스레드 경합까지 겹쳐 더 심각해졌을 수 있다. 즉 지금까지의 결과는 "Phase 4가 필요 없다"는 가설을 더 강화한다: 실제로 발견된 문제는 전부 지금의 단일 `io_context` 구조 안에서 완결적으로 고칠 수 있었다.

다음은 다중 Room 동시 테스트(4번 항목)와 Phase 6 부하 테스트다.
