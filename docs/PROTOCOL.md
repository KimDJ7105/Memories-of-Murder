# WebSocket 프로토콜 (Phase 1)

모든 메시지는 JSON, `type` 필드로 구분한다. 서버는 `ws://localhost:9002` 하나에서 단일 게임방을 운영한다 (Phase 3에서 RoomManager 도입 전까지는 방이 하나뿐이다).

## Client → Server

| type | 필드 | 설명 |
|---|---|---|
| `join` | `name` | 최초 접속 시 반드시 먼저 보내야 함. Lobby 상태에서만 허용, 성공 시 `joined` 응답 |
| `start_game` | - | 방장만 가능, Lobby에서 3~6명일 때만 허용 |
| `submit_crime` | `text` | 범인만, `CrimeWriting` 상태에서만 허용 |
| `submit_guess` | `text` | 아직 못 맞춘 탐정만, `Investigation` 상태에서만 허용 |

## Server → Client

| type | 필드 | 설명 |
|---|---|---|
| `joined` | `player_id` | join 성공 응답 |
| `room_update` | `state`, `round`, `players[]` | 플레이어 목록/점수/상태가 바뀔 때마다 전체 브로드캐스트 |
| `round_start` | `round`, `location`, `weapon` | 새 라운드 시작, 장소/무기는 전원 공개 |
| `your_role` | `role` (`criminal`\|`detective`) | 각 플레이어에게 개별 전송, 범인 여부는 본인만 앎 |
| `investigation_start` | `attempt`, `pending_detectives[]` | 이번 시도에 추리를 제출해야 하는 탐정 id 목록 |
| `guess_result` | `attempt`, `results{id:bool}`, `still_pending[]` | 해당 시도의 판정 결과 (한 시도 = 전원 제출 후 배치 판정 1회) |
| `round_result` | `criminal_id`, `crime_text`, `crime_score`, `evaluation`, `key_facts[]`, `scores_gained{}`, `total_scores{}` | 라운드 종료 결과 |
| `game_over` | `total_scores{}`, `winner_id` | 전원이 한 번씩 범인을 마친 후 |
| `error` | `message` | 잘못된 상태/권한의 요청에 대한 거부 응답 |

## 상태 흐름

```
Lobby → RoleAssignment → CrimeWriting → AIJudging → Investigation(최대 3회 시도) → Result → NextRound → (다음 라운드 | GameOver)
```

`RoleAssignment`/`AIJudging`/`Result`/`NextRound`는 서버가 즉시 통과시키는 내부 상태로, 별도 클라이언트 입력을 기다리지 않는다. 클라이언트가 실제로 입력을 보내야 하는 상태는 `CrimeWriting`과 `Investigation`뿐이다.

## Phase 1의 임시 구현

- `CrimeEvaluator`, `GuessJudge`는 각각 `MockCrimeEvaluator`/`MockGuessJudge`로, AI 호출 없이 텍스트 길이·키워드 매칭만으로 동작하는 임시 구현이다. Phase 2에서 Ollama 기반 구현으로 교체될 인터페이스(`ICrimeEvaluator`, `IGuessJudge`, 둘 다 `server/include/`)만 유지하면 된다.
