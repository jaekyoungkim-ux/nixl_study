# progress engine — 요청이 DOCA task 가 되기까지

> [backend-flow.md](./backend-flow.md) 가 끝나는 지점에서 이어진다.
> 대상 파일: `src/plugins/doca_memos/doca_memos_progress_engine.{h,cpp}` (226 + 1,039줄)
> 주석본: [../annotated/](../annotated/)
>
> **행 번호 표기는 `원본 / 주석본` 순.** 주석 삽입으로 두 파일의 행이 어긋나 있다.
>
> **읽은 범위**: 계약 정리 → 베이스 생성자 → `cleanupDocaResources` → `trySubmitRequest`.
> 콜백·두 엔진·QUERY 경로는 미독. [8장](#8-남은-것) 참조.

---

## 0. 이 파일이 맡는 구간

`backend.cpp` 는 **키와 버퍼를 준비하는 데까지**만 한다. 실제로 DOCA 에 명령을 넣고
완료를 거두는 일은 전부 여기다.

```
backend.cpp  registerMem()   문자열 → 16바이트 키
             prepXfer()      descriptor 쌍 → 키 배열 + iovec 배열 + 빈 표식 배열
             postXfer()      ─┐
             checkXfer()      ├─ 전부 progressEngine_ 에 그대로 위임
             releaseReqH()   ─┘
────────────────────────────────────────────────────────
progress_engine   준비된 배열을 꺼내 DOCA task 로 만들고, 제출하고, 완료를 센다
```

`backend.cpp` 의 세 함수(`678` / `705` / `720` 주석본)는 한 줄짜리 위임이다.
**실질적인 전송 로직은 이 파일에만 있다.**

---

## 1. 생성 시점에 넘어오는 것 — 값 4개

`createProgressEngine()` (backend.cpp `386` 주석본)이 넘기는 것은 넷뿐이다.

| 넘기는 값 | 출처 | 확정 시점 | progress engine 에서 쓰이는 곳 |
|---|---|---|---|
| `nvmeKvdev_.get()` | `initDocaDevice()` 가 잡아 켜둔 장치 핸들 | `doca_ctx_start` 직후 | 생성자에서 **IO 큐를 만들 때 한 번만**. 이후 안 씀 |
| `numTasks_` | 사용자 `num_tasks` (기본 8192) → 장치가 보고한 `max_tasks` 로 하향 | 장치 능력 조회 시 | 생성자의 `set_num_tasks` — **명령서 뭉치 크기** |
| `maxValueLen_` | 장치의 `doca_kvdev_get_max_value_len()` 결과 | 장치 능력 조회 시 | `trySubmitRequest` 의 길이 검증 |
| `pthrDelay` | `nixlBackendInitParams` (agent 설정) | 백엔드 생성 요청 시 | 스레드형에서만. 폴링 간격 |

### 순서 의존성

넷 중 둘(`numTasks_` / `maxValueLen_`)이 **장치에게 물어봐야 나오는 값**이다.
그래서 생성자의 호출 순서가 고정돼 있다 — backend.cpp `448` 주석본이 이유를 남겨뒀다.

```
parseInitParams()    사용자가 준 문자열을 숫자로  (numTasks_ 초기값)
initDocaDevice()     장치를 켜고 능력을 조회      (numTasks_ 하향 / maxValueLen_ 확정)
createProgressEngine()  ← 위 두 값이 확정된 뒤라야 부를 수 있다
```

### 소유권

`nvmeKvdev_.get()` 은 **비소유 포인터**다. 장치의 실소유자는 끝까지 `nixlDocaMemosEngine`
(`unique_ptr` + `NvmeKvdevDeleter`) 이고, progress engine 은 빌려 쓸 뿐이다.
따라서 **파괴 순서가 강제된다** — progress engine 이 먼저 죽어야 한다.

---

## 2. 두 엔진 중 하나 — 갈라지는 지점

`init_params->enableProgTh` **한 개**가 클래스를 통째로 바꾼다.

| | `enableProgTh = false` | `enableProgTh = true` |
|---|---|---|
| 클래스 | `nixlNoThreadProgressEngine` | `nixlThreadedProgressEngine` |
| 추가 인자 | 없음 | `pthrDelay` |
| DOCA 를 부르는 주체 | **호출자의 스레드** | **전용 progress thread 단독** |
| 진행 계기 | 호출자가 `checkXfer()` 를 불러야 진행 | 스레드가 알아서 돈다 |
| 동기화 | mutex 하나로 전부 직렬화 | 더블버퍼 큐, 생산자는 push 만 |

`pthrDelay == 0` 이면 busy-spin 경고를 찍고 그대로 진행한다 (거부하지 않음).

> **운영상 함의**: agent 설정 한 줄이 백엔드 내부 구조를 통째로 교체한다.
> 스토리지 백엔드 중 이런 이중화를 가진 것은 이 플러그인뿐이다 —
> [adoption-report 2.9-2](./adoption-report.md) 참조.

---

## 3. 베이스 생성자가 만드는 것 — `168 / 209`

DOCA 함수 9개를 순서대로 부르는 **직선**이다. 분기 없음.
어디서 실패하든 `cleanupDocaResources()` → `initErr_ = true` → `return`.

| 함수 | 하는 일 |
|---|---|
| `doca_pe_create` | 완료를 확인해 줄 폴링 주체를 만든다 |
| `doca_nvme_kernel_kvdev_io_create` | 이 장치에 명령을 넣을 **큐 하나**를 연다 |
| `..._io_as_kvdev_io` | 같은 큐를 일반 KV 타입으로 보는 별칭 |
| `doca_kvdev_io_set_num_tasks` | 그 큐가 동시에 담을 명령 수를 정한다 (`numTasks_`) |
| `doca_kvdev_io_set_task_completion_cb` | 성공 시 부를 함수를 큐에 걸어둔다 |
| `doca_kvdev_io_set_task_error_cb` | 실패 시 부를 함수를 큐에 걸어둔다 |
| `doca_kvdev_io_as_ctx` | 같은 큐를 DOCA 코어 타입으로 보는 별칭 |
| `doca_pe_connect_ctx` | 폴링 주체에게 "이 큐도 봐라" 등록 |
| `doca_ctx_start` | 큐 가동. 이후 명령 수신 가능 |

### 멤버 4개 — 사실은 물건 2개

`kkvIo_` / `kvIo_` / `ctx_` 는 **같은 주소**다. C 에 상속이 없어 `as_` 변환으로
타입만 갈아 끼운다. `backend.cpp` 의 `initDocaDevice()` 가 **장치 레벨**에서 하던 것과
같은 관용구가 **큐 레벨**에서 한 번 더 반복된다.

| 멤버 | 실제로 쓰이는 곳 |
|---|---|
| `pe_` | `doca_pe_progress()` — 두 엔진의 폴링, 두 소멸자의 drain |
| `kvIo_` | `trySubmitRequest` / `trySubmitExistTask` 의 task 할당 |
| `kkvIo_` | 파괴할 때만 |
| `ctx_` | 정지할 때만 |

**동작 중에 등장하는 것은 `pe_` 와 `kvIo_` 둘뿐이다.**

### 두 가지 설계 결정

**콜백을 `ctx_start` 보다 먼저 건다.** 시작 후에는 바꿀 수 없다. 그리고 등록되는 것은
**함수 포인터 2개뿐** — "어느 요청인지" 정보는 여기 없다. 그건 task 마다 따로 실어 보낸다(5장).

**생성자가 예외를 던지지 않는다.** `initErr_` 플래그만 세우고 정상 반환하고,
`createProgressEngine()` 이 `hasInitError()` 로 확인해 예외로 바꾼다.
베이스 클래스 생성자에서 던지면 파생 클래스의 소멸자가 불리지 않기 때문이다.

---

## 4. `cleanupDocaResources()` — `252 / 293`

생성의 역순으로 DOCA 함수 3개: `doca_ctx_stop` → `doca_nvme_kernel_kvdev_io_destroy`
→ `doca_pe_destroy`. 각 포인터를 null 검사한 뒤 진행하므로 **9단계 중 어디서 실패해도
그대로 부를 수 있다.** 실패해도 경고만 남기고 계속한다 — 이미 정리 중이라 되돌릴 것이 없다.

`kkvIo_` 를 파괴할 때 `kvIo_` 도 같이 null 로 만든다. 같은 물건이기 때문.

---

## 5. `trySubmitRequest()` — `295 / 351` ★

**이 파일에서 가장 중요한 함수.** descriptor 쌍 하나를 NVMe KV 명령 한 장으로 만들어
큐에 넣는 일을 반복한다.

재료는 이미 `prepXfer()` 가 채워뒀다. **이 함수는 새로 계산하는 것이 없다.**

```
objectKeys_[i]    ← remote[i] 의 metadata 에서 꺼낸 16바이트 키   (값으로 복사돼 있음)
valueIovecs_[i]   ← local[i]  의 (주소, 길이)
taskContexts_[i]  ← 빈 칸. 이 함수가 채운다
```

### 5.1 진입 게이트

앞서 자리가 없어 밀린 요청들이 `pendingRequests_` 에 줄 서 있다.
**줄이 있는데 이번 요청이 그 줄 소속이 아니면**, 한 장도 제출하지 않고 물러난다.
새치기를 막아 FIFO 를 보장한다. 물러나기 전 진행 커서를 0 으로 되돌린다.

### 5.2 반복문이 0 에서 시작하지 않는다

```c
for (int i = req_h->nextDescriptorIndex_; i < local.descCount(); i++)
```

**이 파일 구조의 근원.** 요청 하나가 한 번에 다 제출된다는 보장이 없다.
100개 중 30개까지 넣고 자리가 떨어지면, **31번째라는 사실을 커서에 남기고** 물러났다가
다음 호출이 거기서 이어받는다. 요청 하나가 여러 번에 나뉘어 처리될 수 있다.

### 5.3 이름표 — 하강과 상승을 잇는 유일한 끈

완료 통보가 돌아왔을 때 **그게 어느 명령이었는지 알 방법이 없다.** 그래서 보내기 전에
표식을 만들어 딸려 보낸다. 표식에 적히는 것: 어느 요청 / 몇 번째 / 읽기인지 / 기대 길이.

`union doca_data task_user_data = {.ptr = task_ctx}` 가 그것을 봉투에 넣는 줄이고,
DOCA 는 **내용을 보지 않고 보관했다가 콜백에 그대로 돌려준다.**

**표식은 새로 만드는 것이 아니라 `taskContexts_` 배열의 i번 칸 주소다.** 이유:

| 후보 | 문제 |
|---|---|
| 지역 변수 | 반복문 한 바퀴면 소멸. 콜백이 올 때 이미 없음 |
| 매번 `new` | 콜백마다 `delete` 필요. task 당 힙 할당 1회 추가 |
| **핸들 안의 배열** | 핸들은 `releaseReqH` 까지 살고, 그건 모든 콜백 이후가 보장됨 |

**공짜로 정확한 수명을 얻는 방법**이다. 대가는 6장의 위험 하나.

### 5.4 길이 검증 두 번

보낼 데이터가 너무 큰지를 **서로 다른 두 기준**으로 따진다.

| 비교 대상 | 의미 |
|---|---|
| `UINT32_MAX` | 길이를 담는 자리가 32비트다. 약 4GB 초과면 숫자 자체를 넘길 수 없다 |
| `maxValueLen_` | **이 볼륨이 지원하는 "값 1개의 최대 크기"**. 볼륨 전체 용량이 아니다 |

두 번째가 [adoption-report 2.4](./adoption-report.md) 의 *"모델과 볼륨이 짝지어져야 한다"* 의 실체다.
용량 부족이 아니라 **규격 불일치**로 거절된다.

`maxValueLen_ == 0` 이면 검사 자체를 건너뛴다 — 장치가 값을 보고하지 않은 경우.

### 5.5 명령 조립 — STORE / RETRIEVE

두 갈래가 **함수 이름만 다르고 구조가 동일**하다. 각각 세 단계:

| 단계 | 하는 일 |
|---|---|
| `..._alloc_init(kvIo_, task_user_data, &task)` | 명령서 한 장을 받고 **그 자리에서 이름표를 각인한다** |
| `..._set_key_value_conf(...)` | 명령서를 채운다 — 이것이 NVMe KV 명령의 내용물 |
| `..._as_task(task)` | 제출 함수가 받는 일반 타입으로 변환 |

이름표가 붙는 시점이 **제출할 때가 아니라 할당할 때**라는 점에 주의.

`set_key_value_conf` 의 인자 6개:

```
(task, 키 주소, 키 길이, iovec 배열, 조각 개수, 값 길이)
                                     ↑ 1 로 고정
```

**조각 개수 자리에 `1` 이 박혀 있다.** 함수는 배열을 받도록 생겼는데 항상 하나만 넘긴다.
즉 **값 하나는 끊기지 않은 메모리 한 덩어리여야 한다.** 흩어진 조각을 모아 보낼 수 없다.
[adoption-report 2.9-1](./adoption-report.md) 의 근거가 이 두 글자다.

### 5.6 제출

`doca_task_submit` 은 **제출이지 전송 완료가 아니다.** 성공으로 돌아와도 명령은 진행 중이고,
결과는 나중에 콜백으로 온다.

성공한 뒤에야 `submittedTasks_++` 를 한다. 이유가 주석에 있다 —
**제출 함수가 실패할 때 완료 콜백을 그 자리에서 동기로 부를 수 있어서**,
미리 올려두면 그 콜백이 실행되는 시점에 카운터가 어긋난다.

### 5.7 실패의 두 부류

alloc 과 submit 양쪽에서 같은 기준으로 나눈다.

| 부류 | 해당 에러 | 판단 | 처리 |
|---|---|---|---|
| **자리 부족** | `DOCA_ERROR_FULL`, `NO_MEMORY` | 시간이 지나면 풀린다 (앞선 명령이 끝나면 명령서가 반납됨) | 커서에 위치를 남기고 `return false` |
| **그 외** | 나머지 전부 | 기다려도 안 풀린다 | `handleSubmissionFailure` → `return true` |

submit 단계에서 물러날 때는 **받아둔 명령서를 `doca_task_free` 로 반납**해야 한다.
받았는데 못 보냈으니 그냥 나가면 그 한 장이 영영 돌아오지 않는다.

---

## 6. `handleSubmissionFailure()` — `46 / 66`

`trySubmitRequest` 가 "요청 전체를 실패로 종결한다" 고 할 때 부르는 함수. 두 가지를 한다.

**에러를 기록한다.** 단 아직 아무것도 기록되지 않았을 때만 쓴다.
그래서 여러 번 실패해도 **맨 처음 원인이 남는다** (sticky).

**목표치를 낮춘다.** 이쪽이 핵심이다.

```c
req_h->totalTasks_ = req_h->submittedTasks_ + 1;
```

완료 판정은 "통보 받은 개수 ≥ 목표치" 로 한다. 100개 중 30개만 보내고 실패했다면
**나머지 70개는 통보가 영영 오지 않으므로**, 목표치를 100 으로 두면 요청이 영원히 끝나지 않는다.
그래서 목표치를 **"이미 보낸 30 + 이번 실패 1"** 로 다시 정한다.

**이미 떠 있던 30개는 취소되지 않는다.** 그대로 장치에서 처리되고 결과만 버려진다.
[adoption-report 5장⑤](./adoption-report.md) 의 *"첫 에러에서 제출 중단, 배치 전체 실패"* 의 실제 동작이다.
근본 원인은 **NIXL 에 per-task 상태 보고 API 가 없어 부분 성공을 표현할 수 없는 것.**

---

## 7. 세 카운터 — 이 파일 전체의 뼈대

progress engine 은 거의 무상태다. **진행 상태는 전부 요청 핸들 안에 있다**
(`backend.h` `154–179` 주석본).

```
totalTasks_  ≥  submittedTasks_  ≥  completedTasks_
     │              │                    └ 콜백이 올린다 (상승 경로)
     │              └ trySubmitRequest 가 올린다 (하강 경로)
     └ prepXfer 가 정하고, handleSubmissionFailure 만이 낮춘다
```

`submitted < total` 인 구간이 곧 **backpressure 로 끊긴 상태**다.

### `trySubmitRequest` 의 반환값 계약

**성공/실패가 아니라 "나를 다시 불러야 하는가" 다.** 이 파일 최대의 함정.

| 반환 | 언제 | 호출자가 할 일 |
|---|---|---|
| `true` | 전부 제출했다 | 없음 |
| `true` | **되살릴 수 없는 실패로 이미 종결시켰다** | 없음 |
| `false` | 줄이 있어 시작도 못 했다 | 재시도 큐에 넣기 |
| `false` | 가다가 자리가 떨어져 멈췄다 | 재시도 큐에 넣기 |

**실패인데 `true`** 인 경로가 5군데 있다. 호출자는 이 답만 보고 재시도 여부를 정한다.

---

## 8. 남은 것

| 단계 | 대상 | 원본 / 주석본 |
|---|---|---|
| 3 | `taskCompletionCallback` | `63 / 86` |
| 3 | `taskErrorCallback` — EXIST 의 역전된 규약 | `108 / 141` |
| 3 | 베이스 `checkXfer` — 깃발만 읽는다 | `282 / 325` |
| 4 | `nixlNoThreadProgressEngine` 전체 | `402–684 / 462–780` |
| 5 | `nixlThreadedProgressEngine` 전체 | `687–1039 / 781–1173` |
| 6 | QUERY 경로 (`trySubmitExistTask` + 헬퍼 3개 + `queryMem` 2개) | 흩어져 있음 |

**파일의 물리적 순서가 흐름과 어긋나 있다.** 콜백은 맨 앞(63)에 있지만 흐름상 마지막이고,
query 헬퍼 3개는 NoThread 블록 한가운데(512), `trySubmitExistTask` 는 형제 함수와
500줄 떨어진 Threaded 블록 안(810)에 있다. **위에서 아래로 읽으면 안 된다.**

---

## 9. 리포트에 반영할 것

### 정정

> **[adoption-report 5장⑤](./adoption-report.md) 의 "3초"** → 실제는 **10초**다.
> `kDestructorDrainBudget = std::chrono::seconds(10)` (`604 / 685`), 원본에 단일 상수로 존재하며
> 두 소멸자가 같은 값을 쓴다.

### 확인된 근거

| 리포트 항목 | 이번에 확인한 코드 근거 |
|---|---|
| 2.4 value 크기 고정 | `maxValueLen_` 검증. **`operation` 을 보지 않아** READ 에도 그대로 적용됨 |
| 2.9-1 SGL 미사용 | `set_key_value_conf` 의 조각 개수 인자에 `1` 하드코딩 (STORE/RETRIEVE 양쪽) |
| 2.9-2 progress 이중화 | `enableProgTh` 한 개로 클래스가 통째로 갈림. `pthrDelay==0` 은 경고만 |
| 5장⑤ fail-fast | `handleSubmissionFailure` 의 목표치 하향. **떠 있는 task 는 취소하지 않는다** |
| 7장 리스크 3 | `taskContexts_` 원소의 **주소**를 DOCA 에 게시. 벡터 resize 시 무효화 |

### 새로 관찰한 것

- **task pool 고갈은 장치가 바쁜 것이 아니라 명령서 소진**이다. `numTasks_`(기본 8192)가
  큐에 걸리는 값이므로, `DOCA_ERROR_FULL` 의 빈도는 **장치 성능이 아니라 이 설정값**에 좌우된다.
- **`nguid` 는 progress engine 에 넘어오지 않는다.** 장치 레벨에서만 쓰이고 끝난다.
  [adoption-report 4장](./adoption-report.md) 의 "nguid 의 역할" 미확정 항목에 정황 하나 추가 —
  적어도 **IO 경로에는 관여하지 않는다.**
- **장치 핸들은 생성자에서 큐를 만들 때 한 번 쓰이고 이후 등장하지 않는다.**
  제출은 전부 `kvIo_`(큐) 를 통한다. 장치와 큐의 분리가 코드에서 명확하다.

---

task 한 장에 무엇이 언제 들어가는지는 [doca-task-anatomy.md](./doca-task-anatomy.md) 참조.

관련: [README.md](./README.md) · [backend-flow.md](./backend-flow.md) · [request-flow.md](./request-flow.md) · [doca-task-anatomy.md](./doca-task-anatomy.md) · [adoption-report.md](./adoption-report.md) · [../annotated/](../annotated/)
