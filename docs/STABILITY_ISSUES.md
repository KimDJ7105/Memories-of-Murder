# Phase 5 안정성 테스트 상세 기록

`docs/DESIGN.md` 3단계(안정성 검증)에 나열된 아래 항목들을 실제로 스크립트로 재현해가며 검증한 기록이다.

```
* AI 응답 지연
* AI 응답 실패
* AI JSON 형식 오류
* Ollama 연결 실패
* 플레이어 Disconnect
* 범인 Disconnect
* 탐정 Disconnect
* 중복 요청
* 잘못된 상태에서의 요청
* 방 종료 중 AI 응답 도착
* 이미 종료된 GameRoom에 대한 AI Callback
```

문서 구성 원칙:

- **1부는 실제로 버그를 찾아서 고친 항목**, **2부는 구조를 의심해서 테스트했지만 문제가 없었던 항목**으로 완전히 분리한다. 문제가 없었던 항목도 "왜 문제가 있을 수 있다고 의심했는지"와 "무엇을 확인해서 안전하다고 판단했는지"를 남겨서, 나중에 그 구조가 바뀌었을 때 같은 시나리오를 다시 의심해볼 수 있게 한다.
- 모든 테스트는 Python `websockets` 라이브러리로 만든 스크립트가 실제로 뜬 서버(`build/server/game_server.exe`)에 WebSocket으로 접속해서 진행했다. 타이밍(경쟁 상태)이 중요한 테스트는 실제 Ollama 백엔드(`qwen2.5:7b`, 응답까지 수 초가 걸림 — 이 지연 시간 자체가 경쟁 상태를 만드는 데 필요한 여유 시간이 되어 준다)로, 상태 머신 자체의 정확성만 보는 테스트는 응답이 즉시 오는 Mock 백엔드(`MOM_AI_BACKEND=mock`)로 진행했다.

---

# 1부 — 실제로 문제를 발견하고 수정한 항목

## 1-1. 방 종료 중 AI 콜백 도착 → 서버 프로세스 전체가 죽는 use-after-free

### 구조적 배경과 왜 문제가 있을 거라고 예상했는가

`GameRoom`은 범행 평가와 추리 판정을 각각 `ICrimeEvaluator::evaluate()` / `IGuessJudge::judge()`에 위임하는데, 이 두 호출은 즉시 결과를 돌려주지 않고 콜백(`std::function<void(...)>`)을 통해 나중에 결과를 알려주는 비동기 방식이다. 실제 구현체(`OllamaCrimeEvaluator`, `OllamaGuessJudge`)는 내부적으로 `OllamaClient`를 통해 Ollama 서버에 HTTP 요청을 보내고, 응답이 올 때까지 최대 120초까지 기다릴 수 있다.

문제는 이 콜백들이 `GameRoom.cpp`에서 다음과 같이 `this`(즉 `GameRoom*`)를 그대로 캡처하는 람다로 작성돼 있었다는 점이다.

```cpp
// run_ai_judging() — 수정 전
evaluator_->evaluate(crime, *selected_map_, [this, crime, round, criminal_id](CrimeEvaluation eval) {
    crime_eval_ = std::move(eval);                 // this->crime_eval_ 에 대입
    sender_.broadcast({...});                       // this->sender_ 사용
    start_investigation();                           // this-> 멤버 함수 호출
});
```

```cpp
// handle_submit_guess() — 수정 전
judge_->judge(crime, *selected_map_, text, [this, player_id, text, crime, generation](GuessFeedback fb) {
    if (generation != turn_generation_) return;      // this->turn_generation_ 을 "먼저" 읽는다
    ...
});
```

그런데 `GameRoom`의 생명주기는 `GameRoom` 자신이 아니라 `RoomManager`가 관리한다. `RoomManager::Room` 구조체는 `game_room`을 `std::unique_ptr<GameRoom>`으로 들고 있고, `RoomManager::handle_disconnect()`는 플레이어가 나갈 때마다 `GameRoom::is_empty()`(연결된 플레이어가 한 명도 없으면 true)를 확인해서, 방이 비면 그 즉시 `rooms_.erase(it)`로 `GameRoom`을 통째로 파괴한다.

```cpp
void RoomManager::handle_disconnect(const std::string& room_code, int player_id)
{
    ...
    it->second->game_room->handle_disconnect(player_id);
    if (it->second->game_room->is_empty()) {
        rooms_.erase(it);   // GameRoom 파괴
    }
}
```

이 두 사실을 합치면, **AI에게 보낸 HTTP 요청이 아직 응답하지 않은 사이에 방의 마지막 플레이어까지 나가버리면**, `GameRoom`은 이미 메모리에서 사라졌는데 그 뒤에 도착하는 AI 응답 콜백은 사라진 `GameRoom`을 여전히 `this`로 붙잡고 그 멤버를 읽고 쓰려고 시도하는 상황이 만들어질 수 있다고 판단했다. 이건 C++에서 전형적인 use-after-free이고, 최적화 빌드에서는 크래시로 나타날 수도, 조용히 메모리를 오염시킬 수도 있다.

기존에 있던 `turn_generation_` 값 비교 가드(같은 방 안에서 턴이나 라운드가 이미 넘어간 경우, 지연된 AI 응답을 무시하기 위해 Phase 2에서 넣어둔 장치)는 이 시나리오를 막지 못한다고 판단했다 — 그 가드 자체가 `this->turn_generation_`을 읽는 코드이기 때문에, `this`가 이미 해제된 상태라면 "가드를 확인하는 행위" 자체가 이미 미정의 동작(UB)이다. 즉 기존 가드는 "방은 살아있지만 턴이 넘어간" 경우만 방어했을 뿐, "방 자체가 사라진" 경우에는 애초에 적용될 수 없는 구조였다.

또한 `run_ai_judging()`의 콜백(범행 평가)에는 이 `turn_generation_` 가드조차 없었다 — 오직 `handle_submit_guess()`의 판정 콜백에만 있었다. 그래서 이 경로는 "방이 살아있고 턴만 넘어간" 상황에서도 상태를 오염시킬 수 있는 별개의 문제도 겹쳐 있었다.

### 테스트 방법

실제 Ollama 백엔드(`qwen2.5:7b`)로 서버를 띄운 뒤, 두 가지 콜백 경로를 각각 재현했다.

**시나리오 A — 범행 평가 콜백(`run_ai_judging`)**

1. 3명이 방에 들어가 `start_game`을 보낸다.
2. 범인 역할을 받은 플레이어가 `submit_crime`으로 "나는 서재에서 칼로 피해자를 찔러 죽였다."를 제출한다. 서버는 이 시점에 상태를 `AIJudging`으로 옮기고 `evaluator_->evaluate()`를 호출해 Ollama에 HTTP 요청을 보낸다.
3. **AI 응답을 기다리지 않고** 0.3초만 대기한 뒤(요청이 서버에 도달할 시간만 확보) 3명 전원의 WebSocket 연결을 그 자리에서 즉시 닫는다. 방에 남은 플레이어가 없으므로 `RoomManager`가 `GameRoom`을 파괴한다.
4. 20초를 대기한다 (Ollama의 실제 응답 시간보다 충분히 긴 시간). 이 사이에 지연된 AI 응답이 도착해 콜백이 실행될 것이다.
5. 서버 프로세스가 여전히 살아있는지 `Get-Process game_server` / `tasklist`로 확인한다.

**시나리오 B — 추리 판정 콜백(`handle_submit_guess`)**

동일한 구조로, `submit_crime`이 아니라 `Investigation` 상태까지 진행한 뒤 현재 턴 탐정이 `submit_guess`를 제출한 직후(판정 콜백이 아직 응답 전) 전원 접속을 종료해서 재현했다.

### 결과 (수정 전)

두 시나리오 모두 `Get-Process game_server` / `tasklist`가 프로세스를 전혀 찾지 못했다 — **서버 프로세스 자체가 죽었다.** 서버 콘솔 로그에는 크래시에 대한 별다른 메시지 없이 그냥 프로세스가 사라졌다(Windows의 처리되지 않은 예외/액세스 위반으로 인한 강제 종료로 추정).

이 크래시의 심각도를 특히 무겁게 본 이유는, 이게 문제를 일으킨 방 하나만의 문제가 아니라 **그 순간 서버에 떠 있던 다른 모든 방까지 통째로 종료**시킨다는 점이다. 한 방에서 우연히 발생한 타이밍 경쟁이 서버 전체의 가용성을 무너뜨리는 구조였다.

### 조치

`GameRoom`이 스스로만 소유하는 생존 확인용 토큰을 추가했다.

```cpp
// GameRoom.hpp에 추가
// Owned only by this GameRoom. Every AI callback (evaluate/judge)
// captures a weak_ptr to this and checks expired() as its very first
// line, before touching turn_generation_ or anything else.
std::shared_ptr<char> alive_ = std::make_shared<char>(0);
```

그리고 AI 콜백 두 곳 모두에서, 콜백이 실행되자마자 **가장 먼저** 이 토큰이 아직 살아있는지 확인하도록 수정했다. `weak_ptr::expired()`는 `GameRoom`이 파괴되어 `alive_`의 참조 카운트가 0이 되는 순간 true가 되므로, `this`의 다른 어떤 멤버도 건드리기 전에 안전하게 콜백을 종료할 수 있다.

```cpp
// run_ai_judging() — 수정 후
const int generation = turn_generation_;
std::weak_ptr<char> alive = alive_;
evaluator_->evaluate(crime, *selected_map_, [this, crime, round, criminal_id, generation, alive](CrimeEvaluation eval) {
    if (alive.expired()) return;                  // 방이 이미 파괴됨 — this를 절대 건드리지 않고 반환
    if (generation != turn_generation_) return;    // 방은 살아있지만 라운드가 이미 아보트됨
    ...
});
```

```cpp
// handle_submit_guess() — 수정 후
const int generation = turn_generation_;
std::weak_ptr<char> alive = alive_;
judge_->judge(crime, *selected_map_, text, [this, player_id, text, crime, generation, alive](GuessFeedback fb) {
    if (alive.expired()) return;                  // 동일 가드
    if (generation != turn_generation_) return;    // 기존 가드는 그대로 유지
    ...
});
```

부수적으로, `run_ai_judging()`의 콜백에는 없었던 `turn_generation_` 비교도 이번에 함께 추가했다 — "방은 살아있지만 범인 disconnect 등으로 라운드가 이미 강제 종료된" 경우까지 같은 방식으로 방어하기 위해서다.

### 재검증 결과

시나리오 A, B를 수정된 빌드로 각각 재실행 — 두 경우 모두 서버 프로세스가 테스트 전후로 **동일한 PID를 유지하며 계속 살아있음**을 확인했다. 지연된 AI 응답이 실제로 도착했을 시점(20초 대기 이후)에도 아무 크래시가 없었다.

이후 이 변경이 정상 플레이 경로에 부작용을 주지 않았는지 Mock 백엔드로 전체 라운드(방 생성 → 게임 시작 → 범행 제출 → 추리 제출 → 정답 판정 → `guess_feedback` 브로드캐스트)를 다시 실행해 회귀가 없음도 확인했다.

---

## 1-2. 게임 진행 중 방장 Disconnect → 방장 자리가 죽은 세션에 영구 고정

### 구조적 배경과 왜 문제가 있을 거라고 예상했는가

`GameRoom::handle_disconnect()`를 읽어보면, 방장이 나갔을 때 다른 플레이어에게 방장을 넘기는 승계 로직이 다음과 같이 **`state_ == GameState::Lobby`인 경우에만** 존재했다.

```cpp
void GameRoom::handle_disconnect(int player_id)
{
    Player* p = find_player(player_id);
    if (!p) return;

    if (state_ == GameState::Lobby) {
        players_.erase(...);
        if (host_id_ == player_id) {
            host_id_ = players_.empty() ? -1 : players_.front().id;   // 승계는 여기서만 일어남
        }
        broadcast_room_update();
        return;
    }

    p->connected = false;
    // 이 아래 경로(게임이 이미 시작된 이후)에는 host_id_ 재할당이 전혀 없다.
    ...
}
```

한편 방장 권한이 필요한 메시지 핸들러들을 확인해보면:

- `handle_next_round()` — `GameState::NextRound` 상태에서만, 방장만 보낼 수 있다. 다만 이건 `schedule_next_round_advance()`가 15초짜리 서버 타이머를 걸어두므로, 방장이 없어도 자동으로 다음 라운드로 넘어가는 안전장치가 있다.
- `handle_restart_game()` — `GameState::GameOver` 상태에서만, 방장만 보낼 수 있다. 그런데 이건 **타이머 안전장치가 전혀 없다.**

이 게임에는 재접속(reconnect) 기능이 없다는 것도 이미 알고 있는 설계 사실이다(끊긴 세션은 다시 같은 플레이어로 복귀할 방법이 없음). 이 세 가지를 종합하면: 게임이 로비를 벗어난 뒤 방장이 연결을 끊고 다시 들어오지 않으면, `host_id_`는 영원히 그 죽은 세션의 id를 가리키게 되고, 게임이 `GameOver`에 도달해도 남은 플레이어 중 누구도 `restart_game`을 보낼 권한이 없어 그 방이 **영구적으로 재시작 불가능한 상태로 남을 것**이라고 예상했다.

### 테스트 방법

3인 방에서 `start_game`을 보내 게임을 시작하고, 역할 배정에서 **범인이 아닌** 플레이어가 방장임을 확인한 뒤(범인 disconnect는 이미 별도로 처리되는 다른 코드 경로라 승계 문제와 분리해서 보기 위함), `CrimeWriting` 상태에서 방장의 WebSocket 연결을 그냥 닫았다. 그 직후 남은 플레이어들에게 오는 `room_update` 메시지의 `players[].is_host` 필드를 확인했다.

### 결과 (수정 전)

```json
{"id": 1, "name": "Host", "connected": false, "is_host": true}
{"id": 2, "name": "P2",   "connected": true,  "is_host": false}
{"id": 3, "name": "P3",   "connected": true,  "is_host": false}
```

연결이 끊긴(`connected: false`) 플레이어가 그대로 `is_host: true`로 남아있는 것을 확인했다 — 예상했던 대로 방장 자리가 죽은 세션에 고정되는 것을 실제로 재현했다.

### 조치

`handle_disconnect()`에서 `p->connected = false;` 처리 직후, **게임 상태와 무관하게** "나간 사람이 방장이었다면 지금 연결돼 있는 플레이어 중 아무에게나 즉시 넘긴다"는 로직을 추가했다. 로비 분기 안에 있던 기존 승계 로직과는 별개로, 로비 이후(게임 진행 중) 경로에도 동일한 사고방식을 적용한 것이다.

```cpp
p->connected = false;

// Host succession applies here too, not just in Lobby: without it, a
// host who disconnects mid-game leaves host_id_ pointing at a session
// that will never come back, which permanently locks out host-only
// actions for whoever's left — most importantly restart_game, which
// only fires on an explicit host message and has no timer fallback
// the way next_round does.
if (host_id_ == player_id) {
    auto it = std::find_if(players_.begin(), players_.end(),
                            [](const Player& pl) { return pl.connected; });
    host_id_ = (it != players_.end()) ? it->id : -1;
}
```

### 재검증 결과

동일한 시나리오를 재실행한 결과, 방장이 끊기자마자 바로 다음 `room_update`에서 `P2`가 `is_host: true`로 즉시 승계되는 것을 확인했다.

```json
{"id": 1, "name": "Host", "connected": false, "is_host": false}
{"id": 2, "name": "P2",   "connected": true,  "is_host": true}
{"id": 3, "name": "P3",   "connected": true,  "is_host": false}
```

이후 전체 라운드 회귀 테스트와 2부 2-1의 잘못된 상태 요청 테스트도 이 수정 이후 다시 전부 통과하는 것을 확인해, 이 변경이 다른 동작에 부작용을 주지 않았음을 확인했다.

---

# 2부 — 구조를 의심해서 테스트했지만 문제가 없었던 항목

## 2-1. 잘못된 상태에서의 요청 (11종)

### 왜 의심했는가

`GameRoom`은 하나의 상태 머신(`GameState`)이고, 각 메시지 핸들러(`handle_start_game`, `handle_submit_crime` 등)는 자신이 유효한 상태에 있는지, 보낸 사람이 그 행동을 할 권한이 있는지를 함수 맨 앞에서 확인하고 아니면 `error`를 돌려주는 패턴으로 짜여 있다. 이 검사들이 정말 모든 조합에 대해 빠짐없이 걸려 있는지, 실제로 클라이언트가 짓궂게(혹은 버그로) 순서를 어기고 메시지를 보냈을 때도 서버가 죽거나, 상태가 조용히 오염되거나, 응답 없이 멈추지 않는지를 코드 리뷰만으로 확신할 수 없어서 직접 하나씩 찔러봤다.

### 테스트 방법

Mock 백엔드로 띄운 서버에 대해 아래 11가지 요청을 각기 부적절한 시점에 보내고, 매번 `error` 타입 응답이 오는지, 그리고 그 과정에서 다른 플레이어에게 잘못된 브로드캐스트가 새어나가지 않는지 확인했다.

| # | 시나리오 | 기대 결과 |
|---|---|---|
| 1 | 로비 상태에서 `submit_guess` | 에러 |
| 2 | 로비 상태에서 방장이 아닌 사람이 `submit_crime` | 에러 |
| 3 | 방장이 아닌 사람이 `start_game` | 에러 |
| 4 | 방장이 아닌 사람이 `select_map` | 에러 |
| 5 | 존재하지 않는 지도 id로 `select_map` | 에러 |
| 6 | 로비 상태에서 `next_round` | 에러 |
| 7 | 로비 상태에서 `restart_game` | 에러 |
| 8 | `CrimeWriting` 상태에서 탐정(범인이 아닌 사람)이 `submit_crime` | 에러 |
| 9 | 게임이 이미 진행 중인데 다시 `start_game` | 에러 |
| 10 | 게임이 이미 시작된 뒤 `select_map` | 에러 |
| 11 | `Investigation` 시작 전(`AIJudging` 상태 등)에 `submit_guess` | 에러 |

### 결과

11개 전부 정확한 한국어 오류 메시지와 함께 `error` 타입 응답을 받았다. 예를 들어 8번은 `"범인만 범행을 제출할 수 있습니다."`, 3번은 `"방장만 게임을 시작할 수 있습니다."`처럼 각 상황에 맞는 메시지였고, 어느 경우에도 상태가 바뀌거나 다른 플레이어에게 예상치 못한 브로드캐스트가 가는 부수효과는 관찰되지 않았다.

**결론: 이 항목에서는 실제 버그를 찾지 못했다.** 각 핸들러가 함수 최상단에서 상태/권한을 확인하고 즉시 반환하는 패턴이 일관되게 지켜지고 있었기 때문으로 판단한다. 별도 코드 수정 없이, 이 11개 케이스를 회귀 테스트 스크립트로만 남겨서 나중에 핸들러 로직이 바뀔 때 같은 걸 다시 돌려볼 수 있게 했다.

---

## 2-2. 중복 요청

### 왜 의심했는가

서버는 하나의 `boost::asio::io_context` 스레드에서만 동작하기 때문에 두 메시지가 진짜로 "동시에" 처리되는 일은 없다 — 항상 도착 순서대로 순차 처리된다. 하지만 그렇다고 중복 제출이 자동으로 안전한 건 아니다. 특히 **AI 호출처럼 응답까지 시간이 걸리는 비동기 작업 중에 같은 종류의 메시지가 한 번 더 도착하는 경우**가 진짜 위험 지점이다 — 상태 전환이 AI 응답 콜백 안에서 일어난다면, 그 콜백이 오기 전까지의 "대기 중" 구간에 대한 별도의 가드가 없으면 두 번째 요청이 새로운 AI 호출을 또 시작시켜서 내부 자료구조(`pending_order_` 같은 큐)를 두 번 건드리는 사고로 이어질 수 있다.

이건 실제로 Phase 2 개발 중 사용자가 직접 겪은 버그였다 — "탐정 1이 답변을 제출하고 AI가 판단을 완료하기 전에 제출을 연타하면 중복 제출이 되면서 턴까지 꼬이더라"는 리포트로 발견되어, 그때 이미 `guess_in_flight_` 플래그로 고쳐진 이력이 있다. 이번 Phase 5에서는 그 수정이 여전히 유효한지, 그리고 비슷한 패턴을 가진 다른 메시지(`submit_crime`, `next_turn`, `next_round`, `start_game`)에도 같은 종류의 취약점이 남아있지 않은지를 다시 확인했다.

### 테스트 방법

- `submit_guess`: 같은 턴에 연속으로 두 번 보내서 두 번째가 `error`("이미 이번 차례에 제출했습니다.")로 거부되는지 확인 (기존 회귀 확인).
- `submit_crime`, `start_game`, `next_round`: 코드 확인 결과 이 세 메시지는 상태 전환(`state_` 대입)이 AI 호출 등 비동기 작업 **이전에** 동기적으로 즉시 일어나므로, 두 번째 요청이 처리될 때는 이미 상태가 바뀌어 있어 2-1의 "잘못된 상태 요청" 검사에 자동으로 걸리는 구조다. 실제로 2-1의 9번 케이스("게임이 이미 진행 중인데 다시 `start_game`")가 이 경로를 그대로 검증한다.

### 결과

`submit_guess` 연타는 기존 수정(`guess_in_flight_`)이 그대로 유효하게 동작해 두 번째 시도가 즉시 거부됐다. 다른 메시지들은 "상태 전환이 비동기 호출 전에 동기적으로 끝난다"는 구조 자체가 애초에 중복 처리를 불가능하게 만들고 있어서, 별도의 명시적 가드 없이도 안전함을 확인했다. **이 항목에서도 새로운 버그는 찾지 못했다.**

---

## 2-3. 탐정 Disconnect (현재 턴인 사람이 끊기는 경우)

### 왜 의심했는가

`Investigation` 상태는 `pending_order_`라는 큐로 "누구 차례인지"를 관리하는데, 만약 지금 막 차례가 된 탐정이 추리를 제출하지 않고 그냥 연결을 끊어버리면 어떻게 되는지가 걱정거리였다. 코드상으로는 `handle_disconnect`에 `was_current` 분기가 있어서 이 경우를 처리하는 것처럼 보였지만("현재 턴인 사람이 나가면 즉시 타이머를 취소하고 다음 턴으로 넘긴다"), 실제로 게임이 그 지점에서 멈추지 않고 정말 다음 사람에게 넘어가는지를 라이브로 확인하지 않은 상태였다.

추가로, 만약 그 탐정이 disconnect 직전에 `submit_guess`를 이미 보내서 AI 판정이 진행 중이었다면 어떻게 되는지도 궁금했다 — 이건 사실상 1-1에서 고친 use-after-free 시나리오와 겹치는 경로라, 1-1의 수정이 여기서도 제대로 작동하는지 확인하는 의미도 있었다.

### 테스트 방법

3인 방에서 게임을 시작하고 `Investigation` 상태까지 진행한 뒤, 현재 턴으로 지정된 탐정의 WebSocket 연결을 (추리를 제출하지 않은 채로) 그냥 닫았다. 남은 두 플레이어가 받는 메시지를 계속 지켜보며 새로운 `investigation_turn_start`가 오는지 확인했다.

### 결과

연결을 끊자마자 남은 두 플레이어 모두에게 새로운 `investigation_turn_start`(다음 탐정의 차례)가 즉시 도착했다. 게임이 멈추거나, 죽은 플레이어의 차례를 계속 기다리는 일 없이 정상적으로 다음 차례로 넘어감을 확인했다. 범인 Disconnect(라운드 강제 종료)는 Phase 2~3 개발 과정에서 이미 여러 번 다뤄지고 검증된 경로라 이번엔 코드 리뷰로만 재확인했고, `docs/PROTOCOL.md`에도 이미 그 동작이 문서화돼 있다.

**이 항목에서도 새로운 버그는 찾지 못했다** — `was_current` 분기가 실제로 의도대로 동작하고 있음을 확인했다.

---

## 2-4. AI 응답 지연 / AI 오류 / Ollama 연결 실패

### 왜 의심했는가

로컬 LLM 서빙은 네트워크 호출인 이상 언제든 실패할 수 있다 — Ollama 프로세스가 죽어있거나, 응답이 비정상적으로 늦거나, JSON 형식이 깨져서 올 수 있다. 이 세 가지 모두 게임을 멈추게 하거나 크래시시키지 않고 "AI가 실패했을 때의 안전한 기본값"으로 넘어가야 한다는 게 설계 의도였는데(`OllamaCrimeEvaluator::fallback_evaluation`, `OllamaGuessJudge::failure_feedback`), Phase 5 시점에 여전히 그 경로가 살아있는지 다시 확인이 필요하다고 판단했다.

이 중 "AI 응답 지연으로 인한 타임아웃"과 "AI JSON 형식 오류"는 사실 Phase 2에서 이미 한 번 실제로 겪고 고친 이력이 있다 (`docs/AI_ISSUES.md`의 1번 항목 — Ollama 콜드 스타트로 인한 최초 요청 타임아웃). 그 코드(`parse_evaluation`/`parse_feedback`의 필드별 방어적 파싱, `OllamaClient`의 120초 타임아웃)가 이후 리팩터링(멀티맵 지원으로 인터페이스에 `MapDef` 파라미터 추가 등)을 거치면서도 깨지지 않았는지 재확인하는 차원에서, 이번엔 "Ollama 연결 자체가 완전히 불가능한 경우"를 새로 테스트했다.

### 테스트 방법

Ollama 프로세스(`ollama.exe`)를 완전히 종료한 상태(포트 11434에 아무도 응답하지 않음)에서 3인 방으로 게임을 시작하고 범인이 범행을 제출했다.

### 결과

`OllamaClient`의 TCP 연결 시도가 즉시 거부되어(포트에 아무것도 없으니 타임아웃까지 기다릴 필요 없이 바로 실패) `crime_score_revealed` 메시지에 fallback 기본값인 `{"score": 50}`이 곧바로 반영됐고, 게임은 멈추거나 죽지 않고 정상적으로 `Investigation` 상태까지 진행됐다.

```
[Host] <- {'players': [...], 'round': 1, 'state': 'AIJudging', ...}
[Host] <- {'score': 50, 'type': 'crime_score_revealed'}
[Host] <- {'attempt': 1, 'detective_id': 2, 'type': 'investigation_turn_start'}
[Host] <- {'players': [...], 'round': 1, 'state': 'Investigation', ...}
```

Phase 2에서 구현해 둔 폴백 경로가 이후 리팩터링을 거치고도 여전히 정상 동작함을 재확인했다. **AI 응답 지연(타임아웃)과 AI JSON 형식 오류**는 `docs/AI_ISSUES.md` 1번 항목에서 이미 실제로 재현·수정·검증된 이력이 있고, 그 수정을 이루는 코드 구조(방어적 파싱, 타임아웃 값)에 이번 Phase 5 작업으로 손댄 부분이 없어 별도 재현 없이 코드 리뷰로 유효성만 재확인했다. **이 항목에서도 새로운 버그는 찾지 못했다.**

---

## 2-5. 다중 Room 동시 테스트

### 왜 의심했는가

`RoomManager`는 방마다 독립된 `sessions`(그 방에 속한 플레이어의 세션 맵), `RoomSender`(그 세션 맵만 보고 브로드캐스트하는 발신기), `GameRoom`을 묶어서 관리한다. 설계상으로는 방 사이에 상태가 새어나갈 여지가 없어 보이지만, 다음 두 가지가 실제로도 그런지 검증이 필요했다.

1. 방 코드가 정말 충돌 없이 발급되는가 — 특히 여러 방이 거의 동시에 만들어질 때.
2. 1-1에서처럼 한 방이 크래시급 상황(강제 종료)을 겪을 때, 그 여파가 같은 서버 프로세스 안의 다른 방에도 번지지는 않는가. 1-1의 수정으로 "서버 프로세스가 죽지 않는다"는 것까진 확인했지만, "다른 방이 그 순간에도 계속 정상적으로 응답하는가"는 별개로 확인해야 하는 질문이었다 — 예를 들어 어느 한 방이 파괴되는 동안 `io_context`가 잠깐이라도 멈추거나, 어떤 공유 자원을 오염시킬 가능성을 배제하기 위함이다.

### 테스트 방법

방 A와 방 B를 거의 동시에(백투백으로) 만들고 각각 3명씩 넣은 뒤, 두 방에서 동시에 `start_game`을 보내 서버의 단일 `io_context` 스레드에서 두 방의 메시지 처리가 실제로 인터리빙되도록 만들었다. 이후:

1. 두 방의 코드가 서로 다른지 확인.
2. 방 A의 `room_update` 브로드캐스트에 방 B 플레이어 이름이 섞여 들어오는지(그 반대도) 확인.
3. 방 A를 1-1의 크래시 시나리오(범행 제출 직후 전원 접속 종료, AI 응답이 오기 전에 방 파괴)로 몰아넣고, 그 동안과 그 이후에 방 B가 계속 정상적으로 응답하는지(방 B의 호스트에게 일부러 잘못된 메시지를 보내 `error` 응답이 여전히 정상적으로 오는지) 확인.

### 결과

- 방 코드 충돌 없음 (`2175` vs `0272` 등 매번 다른 4자리 코드가 발급됨).
- 방 A의 브로드캐스트에서 방 B 플레이어 이름(`B-P2`, `B-P3`, `HostB`)이, 방 B의 브로드캐스트에서 방 A 플레이어 이름이 전혀 발견되지 않음 — 교차 누출 없음.
- 방 A를 AI 콜백이 진행 중인 상태에서 강제로 파괴한 뒤에도, 방 B는 곧바로 보낸 (의도적으로 잘못된) `start_game` 요청에 정상적으로 `error`를 응답했다. 서버 프로세스도 테스트 시작 전과 동일한 PID로 계속 살아있었다.

`RoomManager`가 방마다 독립된 소유권 구조(`sessions`/`RoomSender`/`GameRoom`)를 갖도록 설계한 것이 실제로 격리를 보장하고 있음을 확인했다. **이 항목에서도 새로운 버그는 찾지 못했다.**

---

# 결론 및 Phase 4 판단에 대한 시사점

1부에서 발견된 두 실제 버그(1-1, 1-2)는 둘 다 **AI 비동기 호출 자체의 처리량이나 큐잉 문제가 아니라, 콜백/상태의 수명 관리(lifetime management) 문제**였다. Job Queue나 별도 Worker Thread를 추가한다고 해서 이 두 버그가 저절로 해결되지는 않았을 것이다 — 오히려 워커 스레드가 게임 스레드와 물리적으로 분리돼 있었다면, `this` 캡처 문제에 스레드 간 데이터 경합까지 겹쳐 디버깅이 훨씬 더 어려운 문제가 됐을 가능성이 크다.

2부의 다섯 항목(잘못된 상태 요청, 중복 요청, 탐정 disconnect, AI 오류/연결 실패, 다중 Room 격리)은 전부 기존 설계(단일 `io_context` 스레드에서의 동기적 상태 전환, 방어적 파싱, 방별 소유권 분리)가 의도한 대로 잘 작동하고 있음을 확인했을 뿐, 새로운 코드 변경은 필요하지 않았다.

종합하면, 이번 라운드에서 실제로 발견된 문제는 전부 지금의 단일 `io_context` 구조 안에서 완결적으로 고칠 수 있었고, "AI 처리 자체가 게임 스레드를 막고 있다"는 종류의 병목은 어디에서도 관찰되지 않았다. 이는 "Phase 4(비동기 Job Queue)가 지금 시점에는 필요하지 않다"는 기존 가설을 뒷받침하는 추가 근거다. 다음 단계는 Phase 6 부하 테스트(동시 Room 수를 늘려가며 실제 병목이 나타나는지 측정)다.
