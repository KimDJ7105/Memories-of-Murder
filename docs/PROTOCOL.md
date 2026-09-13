# WebSocket 프로토콜

모든 메시지는 JSON, `type` 필드로 구분한다. 서버는 `ws://localhost:9002` 하나에서 여러 방을 동시에 운영한다 (`RoomManager`, Phase 3) — 방은 4자리 방 코드로 구분되고, 코드를 아는 사람만 들어올 수 있다 (공개 목록 없음).

## Client → Server

| type | 필드 | 설명 |
|---|---|---|
| `create_room` | `name` | 최초 접속 시 `join_room` 대신 보낼 수 있음. 새 방을 만들고 그 방의 방장이 된다. 항상 성공하며, 성공 시 `room_created` 응답으로 방 코드를 받는다 |
| `join_room` | `room_code`, `name` | 최초 접속 시 `create_room` 대신 보낼 수 있음. 해당 코드의 방이 존재하고 아직 Lobby 상태(게임 시작 전)이며 인원(최대 6명)이 차지 않았어야 함. 성공 시 `joined` 응답, 실패 시 `error` |
| `start_game` | - | 방장만 가능, Lobby에서 3~6명일 때만 허용 |
| `restart_game` | - | 방장만 가능, `GameOver` 상태에서만 허용. 같은 방·같은 플레이어로 점수/범인 이력을 초기화하고 Lobby로 되돌린다 (연결이 끊긴 플레이어는 이때 제거됨). 이후 다시 `start_game`을 보내면 새 게임이 시작된다 |
| `submit_crime` | `text` | 범인만, `CrimeWriting` 상태에서만 허용. 장소·흉기 모두 별도 필드가 아니라 `text` 안에 지도의 방 이름 / 흉기 목록의 이름을 자연스럽게 포함해서 써야 함 — 서버가 문장에서 둘 다 찾아낸다 (`GameData::extract_room_mention`/`extract_weapon_mention`). 범인은 UI에서 클릭으로 고르는 것이 하나도 없다 |
| `submit_guess` | `text` | 현재 차례인 탐정만, `Investigation` 상태에서만 허용. 이전 제출이 아직 AI 판정 중이면(`guess_pending`을 받은 뒤 `guess_feedback`이 오기 전) 거부됨 |
| `next_turn` | - | 방금 추리를 제출한 탐정 본인만, 10초 자동 전환을 기다리지 않고 바로 다음 차례로 넘길 때 |
| `next_round` | - | 방장만 가능, `NextRound` 상태에서만 허용. 15초 자동 전환을 기다리지 않고 바로 다음 라운드로 넘길 때 |

## Server → Client

| type | 필드 | 설명 |
|---|---|---|
| `room_created` | `room_code`, `player_id` | `create_room` 성공 응답. 이 방 코드를 다른 플레이어에게 알려줘야 같이 플레이할 수 있다 |
| `joined` | `room_code`, `player_id` | `join_room` 성공 응답 |
| `board_info` | `map{id,name,image}`, `rooms[{id,name}]`, `edges[[id,id]]`, `weapons[{id,name}]` | 게임 시작 시 1회만 전송되는 정적 데이터. 지도(방+연결 관계)와 흉기 후보 — 라운드마다 반복되지 않음. `image`는 지금은 항상 null(추후 배경 이미지 지원용) |
| `room_update` | `state`, `round`, `players[]` | 플레이어 목록/점수/상태가 바뀔 때마다 전체 브로드캐스트 |
| `round_start` | `round` | 새 라운드 시작 알림. 장소/무기는 이번 라운드에도 범인이 자유롭게 고르므로 여기엔 포함되지 않음 |
| `your_role` | `role` (`criminal`\|`detective`) | 각 플레이어에게 개별 전송, 범인 여부는 본인만 앎 |
| `crime_score_revealed` | `score` | 범행 평가가 끝나자마자(탐정 조사가 시작되기 전) 점수만 전원에게 공개. 평가 이유·key_facts는 정답의 힌트가 될 수 있어 `round_result`까지 비공개 |
| `investigation_turn_start` | `detective_id`, `attempt` | 이번에 추리할 차례인 탐정과, 그 탐정의 몇 번째 시도인지 (탐정마다 최대 3회) |
| `guess_pending` | - | 방금 제출한 탐정 본인에게만: AI가 판정 중이라는 뜻. 판정이 끝날 때까지 그 탐정은 재제출할 수 없다 |
| `guess_feedback` | `player_id`, `guess_text`, `correct`, `attempt`, `aspects[{aspect,verdict}]` | 방금 제출된 추리에 대한 판정. `aspects`는 "장소"/"무기"/"살해 방법"/"은닉 장소"/"은닉 방법" 각각의 `일치`/`유사`/`불일치`. "은닉 장소"(어디에 숨겼는지)와 "은닉 방법"(어떻게 숨겼는지)은 서로 다른 항목. 전원에게 공개 |
| `round_result` | `criminal_id`, `crime_text`, `weapon`, `location`, `location_id`, `crime_score`, `evaluation`, `key_facts[]`, `scores_gained{}`, `total_scores{}` | 라운드 종료 결과 (이때 장소·무기가 공식적으로 공개됨) |
| `game_over` | `total_scores{}`, `winner_id` | 전원이 한 번씩 범인을 마친 후 |
| `error` | `message` | 잘못된 상태/권한의 요청에 대한 거부 응답 |

## 여러 방 운영 (RoomManager)

`RoomManager`가 방 코드(4자리 숫자, 예: `3821`)를 키로 `GameRoom` 인스턴스를 여러 개 관리한다. 방마다 완전히 독립적인 상태(플레이어, 라운드, 점수 등)를 가지며, 한 방의 브로드캐스트가 다른 방 플레이어에게 새는 일은 없다 — 각 방은 자기 세션 목록만 아는 전용 `MessageSender`(`RoomSender`)를 통해서만 메시지를 보낸다.

접속한 세션은 `create_room` 또는 `join_room`을 보내기 전까지는 어느 방에도 속하지 않은 상태다. 둘 중 하나가 성공하면 세션에 방 코드와 플레이어 id가 함께 부여되고, 그 뒤로 오는 모든 메시지는 해당 방의 `GameRoom`으로 그대로 전달된다. `GameRoom` 자체는 다른 방이 존재한다는 사실을 전혀 모른다 — 방을 여러 개 다루는 책임은 전부 `RoomManager`에 있다.

방은 공개 목록이 없다 — 코드를 아는 사람만 들어올 수 있다. 방에 연결된 플레이어가 한 명도 안 남으면(전원 접속 종료) 그 방은 자동으로 정리된다.

## 상태 흐름

```
Lobby → RoleAssignment → CrimeWriting → AIJudging → Investigation → Result → NextRound → (다음 라운드 | GameOver)
```

`RoleAssignment`/`AIJudging`/`Result`는 서버가 즉시 통과시키는 내부 상태다. `NextRound`는 예외로, `round_result`를 보낸 뒤 곧장 다음 라운드로 넘어가지 않고 실제로 15초(또는 방장의 `next_round`) 동안 이 상태에 머문다 — 그러지 않으면 결과 화면이 뜨자마자 다음 라운드의 `round_start`가 도착해 읽을 새도 없이 사라진다. 클라이언트가 실제로 입력을 보내야 하는 상태는 `CrimeWriting`, `Investigation`, `NextRound`(방장만) 세 가지다.

## Investigation은 턴제

탐정들은 동시에 제출하지 않고 한 명씩 순서대로 추리한다 (매 라운드 시작 시 순서를 셔플).

1. 서버가 `investigation_turn_start`로 현재 차례의 탐정을 알림
2. 그 탐정이 `submit_guess` 전송
3. 서버가 즉시 판정 후 `guess_feedback`을 전원에게 브로드캐스트 (정답/오답 + 항목별 근접도)
4. 방금 답한 탐정이 `next_turn`을 보내거나, 10초가 지나면 서버가 자동으로 다음 차례로 진행
5. 정답을 맞혔거나 3회 시도를 다 쓴 탐정은 순번에서 빠지고, 아직 남은 탐정이 있으면 계속 진행. 아무도 안 남으면 라운드 종료

원래 설계(섹션 11)의 "한 라운드 = 판정 1회" 배치 최적화 대신, 턴마다 즉시 피드백을 주기 위해 추리 1건당 판정 1회로 바꾼 것 — 응답성을 우선한 의도적 트레이드오프.

## 지도와 흉기는 정적 데이터, 서버가 고르지 않는다

장소와 흉기 둘 다 서버가 라운드마다 무작위로 배정하지 않는다. `board_info`로 전달되는 지도(방 목록 + 인접 관계)와 흉기 목록은 게임 내내 변하지 않는 보드 상태다. 범인은 장소든 흉기든 UI로 고르는 게 하나도 없다 — 범행을 자연어로 쓸 때 지도에 있는 방 이름과 흉기 목록의 이름을 문장에 자연스럽게 포함하면, 서버가 그 문장에서 둘 다 찾아낸다(`GameData::extract_room_mention`/`extract_weapon_mention`, 문장에서 가장 먼저 등장하는 이름을 채택). 둘 중 하나라도 언급되지 않은 범행은 `error`로 거부된다. 그래서 장소·무기 모두 `round_result`가 오기 전까지는 비공개다.

(한때는 흉기만 클릭으로 고르는 선택지를 따로 뒀었는데, 문장에 그 흉기를 실제로 언급하지 않아도 통과되는 바람에 "선택한 흉기"와 "실제 서술 내용"이 어긋나는 문제가 있었다. 장소와 완전히 같은 방식으로 텍스트에서 추출하도록 통일해 이 불일치 자체를 없앴다.)

방 인접 관계(`edges`)는 지금은 클라이언트 지도 렌더링에만 쓰이지만, 서버 쪽에도 데이터로 보존해 둔 이유는 향후 AI 평가(`docs/DESIGN.md` 섹션 7의 "이동 및 실행 가능성")가 "그 방에서 저 방으로 이동하는 게 말이 되는가"를 판단할 때 쓸 수 있게 하기 위함이다. Phase 1의 Mock 평가기는 아직 이 데이터를 실제로 사용하지 않는다.

## 지도/흉기는 코드가 아니라 파일

`server/data/maps/mansion.json`과 `server/data/weapons.json`이 실제 정의다 (`server/include/GameData.hpp`가 로딩). 새 지도를 추가하고 싶으면 같은 스키마로 JSON 파일을 하나 더 만들면 된다.

지도 데이터는 순수하게 방 목록(`id`, `name`)과 연결 관계(`edges`)뿐이다 — 화면에 그리기 위한 좌표는 들어있지 않다. 클라이언트는 지금 이 데이터를 방 이름 + 인접한 방 목록으로 된 텍스트 목록으로만 보여준다. `image` 필드는 나중에 실제 배경 이미지를 붙이게 될 때를 위해 남겨둔 자리이며, 그때 가서 이미지 위에 방 영역을 표시(클릭/강조 등)해야 하는 구체적인 기능이 생기면 그때 좌표를 추가하면 된다 — 지금은 그런 기능이 없어서 좌표를 미리 넣지 않았다.

## AI 백엔드 (Phase 2)

`ICrimeEvaluator`/`IGuessJudge`는 그대로지만, 기본 구현이 `MockCrimeEvaluator`/`MockGuessJudge`(키워드 매칭)에서 `OllamaCrimeEvaluator`/`OllamaGuessJudge`(로컬 Ollama 모델 호출)로 바뀌었다. `GameServer`가 시작 시 환경변수로 어느 쪽을 쓸지 고른다:

| 환경변수 | 기본값 | 설명 |
|---|---|---|
| `MOM_AI_BACKEND` | `ollama` | `mock`으로 주면 Ollama 없이도 예전처럼 결정적인 Mock으로 동작 (빠른 스크립트 테스트용) |
| `MOM_OLLAMA_MODEL` | `qwen2.5:7b` | 사용할 모델 태그. `ollama pull`로 받아둔 모델이어야 함 |
| `MOM_OLLAMA_HOST` | `localhost` | |
| `MOM_OLLAMA_PORT` | `11434` | |

`server/src/OllamaClient.cpp`가 `/api/chat`을 `format:"json"`, `stream:false`, `temperature:0.2`(판정관/평가관 역할이라 창의성보다 일관성이 중요)로 호출하는 비동기 HTTP 클라이언트다 — 게임 서버와 같은 io_context 스레드에서 동작하므로 AI 응답을 기다리는 동안에도 다른 플레이어의 WebSocket 트래픽은 막히지 않는다. 모델 응답은 필드 단위로 검증되고(타입이 안 맞거나 누락된 필드는 안전한 기본값으로 대체), 연결 실패·타임아웃·JSON 파싱 실패 시에도 예외를 던지지 않고 라운드가 계속 진행될 수 있는 값을 반환한다 (범행 평가 실패 시 50점 기본 부여, 추리 판정 실패 시 오답 처리).

**모델 선택 참고**: 처음엔 `exaone3.5:7.8b`(LG, 한국어 특화)를 기본으로 썼는데, 실제 플레이 중 "몰라"/"음..." 같은 의미 없는 추리에도 흉기 등 일부 항목을 "일치"로 잘못 판정하는 문제가 발견됐다. 프롬프트에 명시적 예시를 추가하고 temperature를 낮춰도 재현됐고(완전히 동일한 오판정이 반복됨), `qwen2.5:7b`로 바꾸니 같은 입력에서 문제가 없었다. 그래서 기본값을 `qwen2.5:7b`로 바꿨다 — `MOM_OLLAMA_MODEL=exaone3.5:7.8b`로 언제든 되돌려서 비교할 수 있다.

Ollama는 모델을 처음 요청받을 때 VRAM에 올리는데 이 콜드 스타트가 몇십 초씩 걸릴 수 있어서, `GameServer` 생성 시점에 더미 요청을 한 번 미리 보내 예열한다 (콘솔에 `Ollama warm-up complete.` 출력).

`AI_TEST_MODE=true`로 서버를 켜면 모든 범행 평가/추리 판정 호출이 `logs/ai_test/YYYY-MM-DD.jsonl`에 한 줄씩 기록된다 (원본 텍스트, 모델 응답 파싱 결과, 실패 사유 등) — 프롬프트 튜닝이나 모델 비교용.
