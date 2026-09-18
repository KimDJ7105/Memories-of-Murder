# WebSocket 프로토콜

모든 메시지는 JSON, `type` 필드로 구분한다. 서버는 `ws://localhost:9002` 하나에서 여러 방을 동시에 운영한다 (`RoomManager`, Phase 3) — 방은 4자리 방 코드로 구분되고, 코드를 아는 사람만 들어올 수 있다 (공개 목록 없음).

## Client → Server

| type | 필드 | 설명 |
|---|---|---|
| `create_room` | `name` | 최초 접속 시 `join_room` 대신 보낼 수 있음. 새 방을 만들고 그 방의 방장이 된다. 항상 성공하며, 성공 시 `room_created` 응답으로 방 코드를 받는다 |
| `join_room` | `room_code`, `name` | 최초 접속 시 `create_room` 대신 보낼 수 있음. 해당 코드의 방이 존재하고 아직 Lobby 상태(게임 시작 전)이며 인원(최대 6명)이 차지 않았어야 함. 성공 시 `joined` 응답, 실패 시 `error` |
| `select_map` | `map_id` | 방장만 가능, Lobby에서만 허용. 이 방에서 플레이할 지도를 바꾼다 (기본값은 `GameData::default_map()`, 지금은 "mansion"). 성공 시 전원에게 새 `board_info`가 브로드캐스트됨 |
| `start_game` | - | 방장만 가능, Lobby에서 3~6명일 때만 허용 |
| `restart_game` | - | 방장만 가능, `GameOver` 상태에서만 허용. 같은 방·같은 플레이어로 점수/범인 이력을 초기화하고 Lobby로 되돌린다 (연결이 끊긴 플레이어는 이때 제거됨). 이후 다시 `start_game`을 보내면 새 게임이 시작된다 |
| `submit_crime` | `text` | 범인만, `CrimeWriting` 상태에서만 허용. 장소·흉기 모두 별도 필드가 아니라 `text` 안에 지도의 방 이름 / 흉기 목록의 이름을 자연스럽게 포함해서 써야 함 — 서버가 문장에서 둘 다 찾아낸다 (`GameData::extract_room_mention`/`extract_weapon_mention`). 범인은 UI에서 클릭으로 고르는 것이 하나도 없다 |
| `submit_guess` | `text` | 현재 차례인 탐정만, `Investigation` 상태에서만 허용. 이전 제출이 아직 AI 판정 중이면(`guess_pending`을 받은 뒤 `guess_feedback`이 오기 전) 거부됨. `kGuessSeconds`(45초) 안에 제출하지 않으면 서버가 자동으로 오답 처리한다 |
| `next_turn` | - | 방금 추리를 제출한 탐정 본인 또는 방장만, `guess_feedback`이 온 뒤(`awaiting_advance_`)에만 허용. 자동 전환 타이머는 없음 — 방장을 허용하는 건 답변자가 자리를 비웠을 때 게임이 멈추지 않게 하기 위한 대비책 |
| `next_round` | - | 방장만 가능, `NextRound` 상태에서만 허용. 자동 전환 타이머는 없음 |
| `rejoin` | `room_code`, `player_id`, `token` | `create_room`/`join_room`처럼 아직 방에 속하지 않은 세션만 보낼 수 있음(둘과 마찬가지로 그 전까지는 다른 메시지가 전부 거부됨). `room_created`/`joined` 때 받은 `token`이 그 방의 그 `player_id`와 일치해야 성공. 성공 시 `rejoined` 응답 + `room_update`/`board_info`/`game_state_sync`, 실패 시 `error`. 자세한 설계는 `docs/RECONNECT_DESIGN.md` 참고 |

## Server → Client

| type | 필드 | 설명 |
|---|---|---|
| `room_created` | `room_code`, `player_id`, `token` | `create_room` 성공 응답. `room_code`는 다른 플레이어에게 알려줘야 같이 플레이할 수 있고, `token`은 이 플레이어 본인만 알아야 하는 재접속용 비밀값(다른 어떤 메시지에도 절대 다시 실리지 않음) |
| `joined` | `room_code`, `player_id`, `token` | `join_room` 성공 응답. `token` 설명은 위와 동일 |
| `rejoined` | `room_code`, `player_id` | `rejoin` 성공 응답. 이 직후(또는 그 전에, 순서를 보장하지 않음 — 아래 참고) `room_update`/`board_info`/`game_state_sync`가 뒤따른다 |
| `game_state_sync` | `state`, `round`, `role`, `crime_score`, `current_detective_id`, `current_attempt`, `answer_key`, `score_breakdown`, `round_result` | `rejoin` 성공 시에만 전송. 일반적인 이벤트 메시지들을 순서대로 재생하는 대신, 지금 이 순간의 상태를 있는 그대로 담아 클라이언트가 화면을 한 번에 재구성하게 한다. `role`은 게임이 진행 중이 아니면(`Lobby`/`GameOver`) null. `state`가 `Investigation`이 아니면 `current_detective_id`/`current_attempt`는 null. `role`이 `criminal`이 아니거나 아직 평가가 공개되지 않았으면 `answer_key`/`score_breakdown`은 null. `state`가 `Result`/`NextRound`가 아니면 `round_result`는 null — 그 두 상태일 땐 이미 한 번 나갔던(놓쳤을 수 있는) `round_result`와 완전히 같은 내용을 통째로 다시 담아 보낸다(단, 그 라운드의 탐정별 추리 로그까지 복원하지는 않는다 — 클라이언트가 그동안 쌓아온 화면상의 기록일 뿐 서버가 따로 저장해두지 않기 때문) |
| `board_info` | `available_maps[{id,name}]`, `map{id,name,image,surveillance_label}`, `rooms[{id,name,surveilled}]`, `edges[[id,id]]`, `weapons[{id,name}]` | 입장 시 및 방장이 `select_map`으로 지도를 바꿀 때마다 전송. `available_maps`는 고를 수 있는 모든 지도 목록, `map`/`rooms`/`edges`는 현재 선택된 지도, `image`는 그 지도의 이미지를 담은 `data:` URI(`<img src>`에 바로 사용 가능) 또는 이미지가 없으면 null. `surveillance_label`은 이 지도의 감시 시스템을 부르는 이름("CCTV", "당직 선원" 등)이고 그런 시스템이 없는 지도면 null. `rooms[].surveilled`가 true인 방은 항상 감시되는 위험 구역이며(로비 때부터 공개된 정보), 지도 이미지에도 표시되어 있다. `weapons`는 전체 흉기 목록이 아니라 **이 지도에서만 쓸 수 있는 흉기 부분집합**이다 |
| `room_update` | `state`, `round`, `players[]` | 플레이어 목록/점수/상태가 바뀔 때마다 전체 브로드캐스트 |
| `round_start` | `round`, `crime_writing_seconds` | 새 라운드 시작 알림. 장소/무기는 이번 라운드에도 범인이 자유롭게 고르므로 여기엔 포함되지 않음. `crime_writing_seconds`는 범인이 범행을 작성할 수 있는 제한 시간(초) — 클라이언트가 카운트다운을 보여주기 위한 값일 뿐, 실제 마감은 서버가 별도로 강제한다 |
| `your_role` | `role` (`criminal`\|`detective`) | 각 플레이어에게 개별 전송, 범인 여부는 본인만 앎 |
| `crime_score_revealed` | `score` | 범행 평가가 끝나자마자(탐정 조사가 시작되기 전) 점수만 전원에게 공개. 평가 이유·key_facts·항목별 정답은 정답의 힌트가 될 수 있어 `round_result`까지 비공개 (범인 본인은 예외 — 바로 아래 `crime_answer_key` 참고) |
| `crime_answer_key` | `answer_key[{aspect,answer}]`, `score_breakdown[]` | **범인에게만** 개별 전송(`crime_score_revealed` 직후). `guess_feedback`의 판정이 왜 "유사"나 "불일치"로 나오는지 범인 스스로도 이해할 수 있도록, AI가 범행을 어떻게 읽었는지를 판정과 동일한 다섯 항목(장소/무기/살해 방법/은닉 장소/은닉 방법)으로 보여준다. 장소·무기는 서버가 이미 확정적으로 아는 값을 그대로 넣고, 나머지 세 항목은 AI가 서술을 요약한 문장이다. `score_breakdown`은 아래 설명과 같은 형태 |
| `investigation_turn_start` | `detective_id`, `attempt`, `guess_seconds` | 이번에 추리할 차례인 탐정과, 그 탐정의 몇 번째 시도인지 (탐정마다 최대 3회). `guess_seconds`는 그 탐정이 추리를 제출할 수 있는 제한 시간(초) |
| `guess_pending` | - | 방금 제출한 탐정 본인에게만: AI가 판정 중이라는 뜻. 판정이 끝날 때까지 그 탐정은 재제출할 수 없다 |
| `guess_feedback` | `player_id`, `guess_text`, `correct`, `attempt`, `aspects[{aspect,verdict}]`, `timed_out`? | 방금 제출된 추리에 대한 판정. `aspects`는 "장소"/"무기"/"살해 방법"/"은닉 장소"/"은닉 방법" 각각의 `일치`/`유사`/`불일치`. "은닉 장소"(어디에 숨겼는지)와 "은닉 방법"(어떻게 숨겼는지)은 서로 다른 항목. 전원에게 공개. `timed_out`이 true면 실제 제출이 아니라 제한 시간 초과로 서버가 강제로 오답 처리한 것(`guess_text`는 빈 문자열) |
| `round_result` | `criminal_id`, `crime_text`, `weapon`, `location`, `location_id`, `crime_score`, `score_breakdown[]`, `evaluation`, `key_facts[]`, `scores_gained{}`, `total_scores{}` | 라운드 종료 결과 (이때 장소·무기가 공식적으로 공개됨). 범인이 시간 내에 범행을 제출하지 못해 종료된 라운드는 `crime_text`/`weapon`/`location`이 빈 문자열이고 `evaluation`이 그 사실을 설명한다 |
| `game_over` | `total_scores{}`, `winner_id` | 전원이 한 번씩 범인을 마친 후 |
| `error` | `message` | 잘못된 상태/권한의 요청에 대한 거부 응답 |

`score_breakdown`은 `[{"category": "장소/환경 일치성", "score": 20, "max": 25}, ...]` 형태의 배열로, `docs/DESIGN.md` 7장의 평가 기준 다섯 항목(장소/환경 일치성 25점, 이동 및 실행 가능성 20점, 무기 사용의 개연성 20점, 범행 과정의 개연성 20점, 증거/무기 은닉의 개연성 15점)을 항상 이 순서 그대로 담는다. `crime_score`는 이 다섯 항목 점수의 합과 항상 정확히 일치한다 — AI가 총점을 별도로 지어내지 않고 서버가 `score_breakdown`을 합산해서 계산하기 때문(`OllamaCrimeEvaluator::parse_evaluation` 참고). `score_breakdown`은 범인에게는 `crime_answer_key`로 먼저 공개되고, 나머지 전원에게는 `round_result`에서만 공개된다.

## 재접속 (Reconnect)

전체 설계 배경과 왜 이렇게 했는지는 `docs/RECONNECT_DESIGN.md`에 있다. 프로토콜 관점에서 알아야 할 것만 정리하면:

- **범위**: 게임이 시작된 이후(로비 이후)만 대상. 로비 상태에서 끊긴 플레이어는 `players_` 목록에서 아예 제거되므로 재접속 대상이 아니다 — 그냥 같은 방 코드로 새로 들어오면 된다.
- **토큰은 방 전체 브로드캐스트(`room_update` 등)에 절대 포함되지 않는다** — `room_created`/`joined`로 그 세션에게 딱 한 번만 전달된다.
- **방장 자리는 재접속으로 되찾을 수 없다.** 방장이 끊기면 즉시 다른 접속 중인 플레이어에게 넘어가고(기존 Phase 5 로직), 원래 방장이 재접속해도 그 자리를 돌려받지 못한다.
- **같은 플레이어로 두 번째 연결이 들어오면 새 연결이 이긴다** — 기존 소켓이 아직 살아있어도 서버가 강제로 닫는다.
- **메시지 순서를 엄격히 보장하지 않는다**: `rejoin`이 성공하면 서버 내부적으로 그 플레이어를 다시 연결시키는 과정에서 `room_update`가 먼저 브로드캐스트되고, 그다음 `rejoined` 응답, 그리고 `room_update`/`board_info`/`game_state_sync`가 뒤따를 수 있다. 클라이언트는 `rejoined`가 반드시 첫 메시지라고 가정하면 안 되고(실제로 `myId`는 `rejoin`을 보내는 시점에 이미 로컬에 저장해둔 값으로 설정해야 한다), 대신 `game_state_sync`가 도착하면 그걸로 화면을 확정적으로 맞춘다.

## 여러 방 운영 (RoomManager)

`RoomManager`가 방 코드(4자리 숫자, 예: `3821`)를 키로 `GameRoom` 인스턴스를 여러 개 관리한다. 방마다 완전히 독립적인 상태(플레이어, 라운드, 점수 등)를 가지며, 한 방의 브로드캐스트가 다른 방 플레이어에게 새는 일은 없다 — 각 방은 자기 세션 목록만 아는 전용 `MessageSender`(`RoomSender`)를 통해서만 메시지를 보낸다.

접속한 세션은 `create_room` 또는 `join_room`을 보내기 전까지는 어느 방에도 속하지 않은 상태다. 둘 중 하나가 성공하면 세션에 방 코드와 플레이어 id가 함께 부여되고, 그 뒤로 오는 모든 메시지는 해당 방의 `GameRoom`으로 그대로 전달된다. `GameRoom` 자체는 다른 방이 존재한다는 사실을 전혀 모른다 — 방을 여러 개 다루는 책임은 전부 `RoomManager`에 있다.

방은 공개 목록이 없다 — 코드를 아는 사람만 들어올 수 있다. 방에 연결된 플레이어가 한 명도 안 남으면(전원 접속 종료) 그 방은 자동으로 정리된다.

## 상태 흐름

```
Lobby → RoleAssignment → CrimeWriting → AIJudging → Investigation → Result → NextRound → (다음 라운드 | GameOver)
```

`RoleAssignment`/`AIJudging`/`Result`는 서버가 즉시 통과시키는 내부 상태다. `NextRound`는 예외로, `round_result`를 보낸 뒤 곧장 다음 라운드로 넘어가지 않고 방장이 `next_round`를 보낼 때까지 이 상태에 머문다 — 그러지 않으면 결과 화면이 뜨자마자 다음 라운드의 `round_start`가 도착해 읽을 새도 없이 사라진다. 클라이언트가 실제로 입력을 보내야 하는 상태는 `CrimeWriting`, `Investigation`, `NextRound`(방장만) 세 가지다.

**자동 진행 타이머는 없다.** 플레이테스트에서 다들 다 읽기도 전에 다음 화면으로 넘어가는 일이 계속 발생해서, "다 읽으면 직접 버튼을 누른다"는 방식으로 완전히 바꿨다 — `next_turn`/`next_round` 둘 다 순수하게 수동이다. 대신 답변자가 자리를 비워 아무도 못 누르는 상황을 막기 위해 `next_turn`은 방장도 보낼 수 있다(`next_round`는 원래도 방장 전용이라 문제없음). 반대로 **입력을 실제로 작성하는 두 구간**(범행 서술, 추리 서술)에는 시간 제한을 새로 뒀다 — 이건 "다 읽었으니 넘어가자"가 아니라 "너무 오래 끌지 말자"는 반대 방향의 문제라 별도로 다룬다.

## Investigation은 턴제

탐정들은 동시에 제출하지 않고 한 명씩 순서대로 추리한다 (매 라운드 시작 시 순서를 셔플).

1. 서버가 `investigation_turn_start`로 현재 차례의 탐정과 제한 시간(`guess_seconds`)을 알림
2. 그 탐정이 시간 안에 `submit_guess`를 보내거나, 시간을 넘기면 서버가 자동으로 오답(`guess_feedback`에 `timed_out: true`) 처리
3. 서버가 즉시 판정 후 `guess_feedback`을 전원에게 브로드캐스트 (정답/오답 + 항목별 근접도)
4. 방금 답한 탐정 본인 또는 방장이 `next_turn`을 보내야 다음 차례로 진행 (자동 진행 없음)
5. 정답을 맞혔거나 3회 시도를 다 쓴 탐정은 순번에서 빠지고, 아직 남은 탐정이 있으면 계속 진행. 아무도 안 남으면 라운드 종료

원래 설계(섹션 11)의 "한 라운드 = 판정 1회" 배치 최적화 대신, 턴마다 즉시 피드백을 주기 위해 추리 1건당 판정 1회로 바꾼 것 — 응답성을 우선한 의도적 트레이드오프.

## 시간 제한 (범행 작성 / 추리 작성)

범행 작성(`CrimeWriting`)과 추리 작성(각 `Investigation` 턴)은 각각 90초/45초의 서버 시행 제한 시간을 가진다 (`GameRoom.cpp`의 `kCrimeWritingSeconds`/`kGuessSeconds`). 시간 안에 제출하면 타이머가 취소되고 정상 진행되며, 시간을 넘기면:

- **범행 작성 시간 초과**: 그 라운드는 범행 없이 즉시 종료된다. `round_result`에 빈 `crime_text`/`weapon`/`location`과 "범인이 시간 내에 범행을 작성하지 못했습니다"라는 `evaluation`이 담기고, 범인은 0점을 받는다.
- **추리 작성 시간 초과**: 그 시도는 5개 항목 모두 `불일치`인 오답으로 자동 처리된다 (`guess_feedback`에 `timed_out: true`). 이후 흐름은 실제로 틀린 답을 제출했을 때와 완전히 동일 — 시도 횟수가 소진됐으면 순번에서 빠지고, 아니면 다시 대기열에 들어간다.

둘 다 "제출을 안 하면 게임이 멈춘다"는 문제를 막기 위한 장치이고, 위 "자동 진행 타이머는 없다" 항목과는 반대 방향의 문제(느긋하게 읽을 시간 vs. 무한정 끌 수 없는 시간)를 다룬다는 점에 유의.

## 지도와 흉기는 정적 데이터, 서버가 고르지 않는다

장소와 흉기 둘 다 서버가 라운드마다 무작위로 배정하지 않는다. `board_info`로 전달되는 지도(방 목록 + 인접 관계)와 흉기 목록은 라운드 사이에는 변하지 않는 보드 상태다(방장이 Lobby에서 `select_map`으로 지도 자체를 바꾸는 것과는 별개). 범인은 장소든 흉기든 UI로 고르는 게 하나도 없다 — 범행을 자연어로 쓸 때 지도에 있는 방 이름과 흉기 목록의 이름을 문장에 자연스럽게 포함하면, 서버가 그 문장에서 둘 다 찾아낸다(`GameData::extract_room_mention`/`extract_weapon_mention`, 문장에서 가장 먼저 등장하는 이름을 채택). 둘 중 하나라도 언급되지 않은 범행은 `error`로 거부된다. 그래서 장소·무기 모두 `round_result`가 오기 전까지는 비공개다.

흉기 목록은 전체 게임에서 하나만 있는 게 아니라 **지도마다 다른 부분집합**을 쓴다(아래 "지도는 코드가 아니라 파일" 참고) — 플레이테스트에서 모든 지도에 같은 흉기 6종이 다 있으니 "이 흉기가 왜 여기에?" 싶은 조합(예: 학교에 총, 유람선에 촛대)이 나온다는 피드백을 받고 나눴다. `extract_weapon_mention`도 전체 흉기 목록이 아니라 그 방의 지도가 실제로 제공하는 흉기 목록 안에서만 찾는다.

(한때는 흉기만 클릭으로 고르는 선택지를 따로 뒀었는데, 문장에 그 흉기를 실제로 언급하지 않아도 통과되는 바람에 "선택한 흉기"와 "실제 서술 내용"이 어긋나는 문제가 있었다. 장소와 완전히 같은 방식으로 텍스트에서 추출하도록 통일해 이 불일치 자체를 없앴다.)

방 인접 관계(`edges`)는 지금은 클라이언트 지도 렌더링에만 쓰이지만, 서버 쪽에도 데이터로 보존해 둔 이유는 향후 AI 평가(`docs/DESIGN.md` 섹션 7의 "이동 및 실행 가능성")가 "그 방에서 저 방으로 이동하는 게 말이 되는가"를 판단할 때 쓸 수 있게 하기 위함이다. Phase 1의 Mock 평가기는 아직 이 데이터를 실제로 사용하지 않는다.

## 지도는 코드가 아니라 파일 (여러 개 가능)

`server/data/maps/` 아래의 `*.json` 파일 하나하나가 지도 하나다 — `GameData::load_default()`가 그 디렉터리를 스캔해서 있는 대로 전부 로드한다. 새 지도를 추가하고 싶으면 같은 스키마로 JSON 파일을 하나 더 넣기만 하면 되고, 코드를 고칠 필요는 없다. `server/data/weapons.json`은 지도와 무관하게 전체 게임에서 공유되는 흉기 목록이다.

각 지도 JSON의 스키마:

```json
{
  "id": "mansion",
  "name": "낡은 저택",
  "image": "mansion.svg",
  "surveillance_label": "CCTV",
  "weapons": ["knife", "blunt", "rope", "poison", "gun", "candlestick"],
  "rooms": [{"id": "attic", "name": "다락방"}, ...],
  "edges": [["attic", "library"], ...]
}
```

`weapons`는 `server/data/weapons.json`에 정의된 흉기 id 중 이 지도에서 실제로 쓸 것만 골라 담은 목록이다. 필드 자체를 생략하면(예: 앞으로 추가되는 지도가 신경 쓰지 않는 경우) 전체 흉기가 전부 허용된다 — 기존 동작과 호환.

지도 데이터 자체는 방 목록(`id`, `name`)과 연결 관계(`edges`)뿐이다 — 화면에 그리기 위한 좌표는 들어있지 않다 (좌표를 넣었다가 아무 기능도 쓰지 않길래 도로 뺀 적이 있다 — git log 참고). `image`는 지도와 같은 폴더에 있는 이미지 파일 이름(`mansion.svg`처럼)을 가리키고, `GameData`가 서버 시작 시 그 파일을 딱 한 번 읽어서 base64 `data:` URI로 인코딩해 메모리에 들고 있는다 — 매 요청마다 다시 읽거나 인코딩하지 않고, `board_info`를 보낼 때 이미 완성된 문자열을 그대로 끼워 넣기만 한다. 이 방식 덕분에 이미지 하나 보여주자고 별도의 HTTP 정적 파일 서버를 둘 필요가 없다 (지금 서버는 WebSocket 하나뿐이다). 확장자로 MIME 타입을 판단하므로(`.svg`→`image/svg+xml`, `.png`→`image/png` 등) 나중에 실제 손그림/AI 생성 PNG로 바꾸고 싶으면 그 확장자의 파일로 교체하고 JSON의 `image` 값만 바꾸면 된다 — 코드 변경 없음.

`GameRoom`은 방마다 `selected_map_`(기본값은 `GameData::default_map()`, 지금은 "mansion")을 들고 있고, 호스트가 Lobby에서 `select_map`을 보내면 그때그때 바뀐다. `ICrimeEvaluator`/`IGuessJudge`는 방 생성 시 한 번만 만들어지므로(지도 선택보다 먼저 존재), 어떤 지도를 쓸지는 생성자가 아니라 `evaluate`/`judge` 호출마다 인자로 넘겨받는다.

## 감시 구역 (CCTV / 경비)

일부 지도는 항상 감시되는 방을 하나씩 가진다 — 방 데이터의 `surveilled: true`와 지도 데이터의 `surveillance_label`(그 지도에서 감시 시스템을 부르는 이름, 예: "CCTV", "당직 선원")로 표현된다. 시간대나 순찰 경로처럼 검증할 수 없는 요소는 의도적으로 넣지 않았다 — 범인의 자유 서술을 반박할 근거가 서버에 없는 요소는(예: "새벽이라 경비가 없었다") 사실상 페널티 없는 장식 텍스트가 되어버리기 때문에, 오직 "이 방은 항상 위험하다"는 정적 속성 하나만 존재한다.

감시 구역은 흉기·지도 목록과 마찬가지로 로비 때부터 공개된 정보다(숨겨뒀다가 나중에 드러내는 함정이 아니라, 범인이 그 위험을 감수할지 미리 고민하게 만드는 요소). 지도 이미지에도 그 방이 표시되어 있고, `OllamaCrimeEvaluator`는 범인의 서술이 감시 구역을 지나가면서 그 위험을 어떻게 처리했는지를 "범행 과정의 개연성" 항목(`docs/DESIGN.md` 7장) 채점에 반영한다 — 별도의 6번째 채점 항목을 새로 만들지 않고 기존 100점 배분을 그대로 유지했다.

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
