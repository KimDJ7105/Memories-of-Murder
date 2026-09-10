# WebSocket 프로토콜 (Phase 1)

모든 메시지는 JSON, `type` 필드로 구분한다. 서버는 `ws://localhost:9002` 하나에서 단일 게임방을 운영한다 (Phase 3에서 RoomManager 도입 전까지는 방이 하나뿐이다).

## Client → Server

| type | 필드 | 설명 |
|---|---|---|
| `join` | `name` | 최초 접속 시 반드시 먼저 보내야 함. Lobby 상태에서만 허용, 성공 시 `joined` 응답 |
| `start_game` | - | 방장만 가능, Lobby에서 3~6명일 때만 허용 |
| `submit_crime` | `text`, `weapon`, `location` | 범인만, `CrimeWriting` 상태에서만 허용. `weapon`/`location`은 `board_info`의 `weapons`/`rooms` 중 하나의 id/name이어야 함 |
| `submit_guess` | `text` | 현재 차례인 탐정만, `Investigation` 상태에서만 허용 |
| `next_turn` | - | 방금 추리를 제출한 탐정 본인만, 10초 자동 전환을 기다리지 않고 바로 다음 차례로 넘길 때 |

## Server → Client

| type | 필드 | 설명 |
|---|---|---|
| `joined` | `player_id` | join 성공 응답 |
| `board_info` | `rooms[{id,name}]`, `edges[[id,id]]`, `weapons[{id,name}]` | 게임 시작 시 1회만 전송되는 정적 데이터. 지도(방+연결 관계)와 흉기 후보 — 라운드마다 반복되지 않음 |
| `room_update` | `state`, `round`, `players[]` | 플레이어 목록/점수/상태가 바뀔 때마다 전체 브로드캐스트 |
| `round_start` | `round` | 새 라운드 시작 알림. 장소/무기는 이번 라운드에도 범인이 자유롭게 고르므로 여기엔 포함되지 않음 |
| `your_role` | `role` (`criminal`\|`detective`) | 각 플레이어에게 개별 전송, 범인 여부는 본인만 앎 |
| `investigation_turn_start` | `detective_id`, `attempt` | 이번에 추리할 차례인 탐정과, 그 탐정의 몇 번째 시도인지 (탐정마다 최대 3회) |
| `guess_feedback` | `player_id`, `guess_text`, `correct`, `attempt`, `aspects[{aspect,verdict}]` | 방금 제출된 추리에 대한 판정. `aspects`는 "장소"/"무기"/"살해 방법"/"은닉 방법" 각각의 `일치`/`유사`/`불일치`. 전원에게 공개 |
| `round_result` | `criminal_id`, `crime_text`, `weapon`, `location`, `location_id`, `crime_score`, `evaluation`, `key_facts[]`, `scores_gained{}`, `total_scores{}` | 라운드 종료 결과 (이때 장소·무기가 공식적으로 공개됨) |
| `game_over` | `total_scores{}`, `winner_id` | 전원이 한 번씩 범인을 마친 후 |
| `error` | `message` | 잘못된 상태/권한의 요청에 대한 거부 응답 |

## 상태 흐름

```
Lobby → RoleAssignment → CrimeWriting → AIJudging → Investigation → Result → NextRound → (다음 라운드 | GameOver)
```

`RoleAssignment`/`AIJudging`/`Result`/`NextRound`는 서버가 즉시 통과시키는 내부 상태로, 별도 클라이언트 입력을 기다리지 않는다. 클라이언트가 실제로 입력을 보내야 하는 상태는 `CrimeWriting`과 `Investigation`뿐이다.

## Investigation은 턴제

탐정들은 동시에 제출하지 않고 한 명씩 순서대로 추리한다 (매 라운드 시작 시 순서를 셔플).

1. 서버가 `investigation_turn_start`로 현재 차례의 탐정을 알림
2. 그 탐정이 `submit_guess` 전송
3. 서버가 즉시 판정 후 `guess_feedback`을 전원에게 브로드캐스트 (정답/오답 + 항목별 근접도)
4. 방금 답한 탐정이 `next_turn`을 보내거나, 10초가 지나면 서버가 자동으로 다음 차례로 진행
5. 정답을 맞혔거나 3회 시도를 다 쓴 탐정은 순번에서 빠지고, 아직 남은 탐정이 있으면 계속 진행. 아무도 안 남으면 라운드 종료

원래 설계(섹션 11)의 "한 라운드 = 판정 1회" 배치 최적화 대신, 턴마다 즉시 피드백을 주기 위해 추리 1건당 판정 1회로 바꾼 것 — 응답성을 우선한 의도적 트레이드오프.

## 지도와 흉기는 정적 데이터, 서버가 고르지 않는다

장소와 흉기 둘 다 서버가 라운드마다 무작위로 배정하지 않는다. `board_info`로 전달되는 지도(방 목록 + 인접 관계)와 흉기 목록은 게임 내내 변하지 않는 보드 상태이고, 실제로 어느 방에서 어떤 흉기를 썼는지는 범인이 `submit_crime`을 보낼 때 자유롭게 고르는 값이다. 그래서 장소도 무기처럼 `round_result`가 오기 전까지는 비공개다.

방 인접 관계(`edges`)는 지금은 클라이언트 지도 렌더링에만 쓰이지만, 서버 쪽에도 데이터로 보존해 둔 이유는 향후 AI 평가(`docs/DESIGN.md` 섹션 7의 "이동 및 실행 가능성")가 "그 방에서 저 방으로 이동하는 게 말이 되는가"를 판단할 때 쓸 수 있게 하기 위함이다. Phase 1의 Mock 평가기는 아직 이 데이터를 실제로 사용하지 않는다.

## Phase 1의 임시 구현

- `CrimeEvaluator`, `GuessJudge`는 각각 `MockCrimeEvaluator`/`MockGuessJudge`로, AI 호출 없이 텍스트 길이·키워드 매칭만으로 동작하는 임시 구현이다. Phase 2에서 Ollama 기반 구현으로 교체될 인터페이스(`ICrimeEvaluator`, `IGuessJudge`, 둘 다 `server/include/`)만 유지하면 된다.
- `aspects`의 "살해 방법"/"은닉 방법" 구분도 지금은 범행 텍스트를 절반으로 대충 나눈 휴리스틱이다. 실제 구조화 추출은 Phase 2에서 AI가 `key_facts` 기반으로 담당한다.
