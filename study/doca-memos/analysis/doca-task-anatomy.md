# DOCA task 의 구성 요소와 만들어지는 과정

> `trySubmitRequest` 가 만들어 제출하는 `doca_task` 한 장에 **무엇이 들어 있고,
> 그 값들이 언제 들어가는가**.
>
> 대상: `src/plugins/doca_memos/doca_memos_progress_engine.cpp` `trySubmitRequest()`
> 행 번호 표기는 **주석본**([../annotated/](../annotated/)) 기준.
> 흐름 전체는 [progress-engine-flow.md](./progress-engine-flow.md) 참조.

---

## 0. 한 줄

**descriptor 쌍 하나 → DOCA task 한 장.** 그 task 는 데이터가 아니라
**"무엇을 어디서 가져다 어디에 쓸지에 대한 기술"** 이다.

---

## 1. 구조는 알 수 없다 — 불투명 핸들

`struct doca_task` 의 실제 정의는 **비공개 DOCA 헤더 안**에 있다.
플러그인은 포인터만 들고 다니며 내부를 한 번도 들여다보지 않는다.

따라서 **크기도 필드 배치도 확인할 수 없다.** 아래는 "무엇을 넣었는가" 를
의미 기준으로 정리한 것이지 메모리 레이아웃이 아니다.

---

## 2. 무엇이 들어가는가

| 넣는 것 | 넣는 함수 | 크기 / 제약 |
|---|---|---|
| 어느 IO 큐 소속인지 | `alloc_init` 1번 인자 (`kvIo_`) | — |
| 명령 종류 (STORE / RETRIEVE / EXIST) | `alloc_init` — **함수 이름으로 결정** | — |
| 이름표 포인터 (`task_user_data`) | `alloc_init` 2번 인자 | 8바이트 |
| 키 주소 | `set_key_value_conf` | — |
| 키 길이 | `set_key_value_conf` | **≤ 16바이트** |
| 값의 위치 (`iovec` 배열 주소) | `set_key_value_conf` | 포인터. 실제 주소는 `iov_base` |
| 조각 개수 (`iovcnt`) | `set_key_value_conf` | 4바이트, **항상 1** |
| 값 길이 | `set_key_value_conf` | 4바이트, `≤ maxValueLen_` |
| 완료 상태 | DOCA 가 채움 | — |
| 실제 값 길이 (RETRIEVE) | DOCA 가 채움 | 4바이트 |

### 복사되는 것과 안 되는 것

**값 버퍼는 복사되지 않는다. 주소만 넘어간다.**

그래서 제약이 생긴다 — **제출한 순간부터 완료 콜백이 돌아올 때까지
그 메모리가 살아 있고 자리를 옮기지 않아야 한다.** 쓰기면 그 사이 내용을
바꿔서도 안 되고, 읽기면 다른 용도로 써서도 안 된다.

목에서는 **키는 복사되고 값 버퍼는 포인터만 잡힌다.** 실제 API 가 같은지는 미확인.

### 값 주소는 반드시 host DDR 가상주소다

`valueIovecs_[i]` 는 local descriptor 에서 오고, **local 은 `DRAM_SEG` 강제**다
(VRAM 은 거부). 따라서 이 주소는 항상 호스트 DRAM 이다 —
[adoption-report 2.1](./adoption-report.md) 의 "GPU 직행 불가" 가 코드에 드러나는 지점.

### `value_len` 과 `iov_len` 이 같은 것은 우연이다

`iovec` 안에도 길이가 있는데 값 길이를 따로 받는다. **조각이 여럿일 때를
위한 자리**이기 때문이다.

```
iovcnt = 3 이라면
    iovec[0].iov_len = 4KB  ┐
    iovec[1].iov_len = 4KB  ├─ 조각들
    iovec[2].iov_len = 2KB  ┘
    value_len        = 10KB  ← 합계. 어느 조각과도 다르다
```

지금은 조각이 1개라 둘이 같아지는 것뿐이다. **인자를 이렇게 설계했다는 것은
API 가 scatter-gather 를 실제로 상정했다는 뜻**이고,
[adoption-report 2.9-1](./adoption-report.md) 의 "SGL 미사용 — 근거 불명" 을 한 단계 굳혀준다.

---

## 3. 목이 만든 모델 — 참고용

DOCA 4.x 가 미공개인 지금 형태를 짐작할 수 있는 유일한 공개 코드.
**시그니처는 진짜 헤더에서 오므로 믿을 수 있으나, 필드 구성은 저자의 모델**이다.
실제 배치의 근거로 쓸 수 없다 (`std::string` 을 쓰고 있다).

```c
struct doca_task {
    enum Type { STORE, RETRIEVE, EXIST } type;
    std::string key;              // 키는 복사해 보관
    void *buffer;                 // 값은 주소만
    size_t buffer_size;
    uint32_t value_len;
    union doca_data user_data;    // 우리 이름표
    doca_error_t status;
    uint8_t do_not_overwrite;     // ← 플러그인이 쓰지 않음
    uint8_t must_exist;           // ← 플러그인이 쓰지 않음
    uint32_t result_value_len;
};
```

뒤의 두 필드가 [adoption-report 4장](./adoption-report.md) 의 "미사용 API 옵션" 항목 —
**조건부 쓰기 기능이 존재하는데 플러그인이 안 쓴다.**

---

## 4. 총량 — 단가는 모르지만 관계는 확실하다

명령서는 제출할 때마다 만드는 것이 아니라, **`set_num_tasks(kvIo_, num_tasks)`
시점에 `num_tasks` 장이 한꺼번에 확보**된다 (기본 8192).

장당 크기를 모르니 총량도 모르지만, **`num_tasks` 를 키우면 호스트 메모리를
그만큼 더 잡는다**는 관계는 확실하다. 튜닝 시 인지해야 할 사항.

같은 이유로 **`DOCA_ERROR_FULL` 은 장치가 바쁜 것이 아니라 명령서가 소진된 것**이다.
발생 빈도는 장치 성능이 아니라 이 설정값에 좌우된다.

---

## 5. 언제 들어가는가 — `trySubmitRequest` 안의 순서

**두 번에 나눠 들어간다.** 한 곳이 아니다.

```
[364-368]  taskContexts_[i] 를 채운다
             어느 요청 / 몇 번째 / 읽기인가 / 기대 길이
           → 아직 DOCA 와 무관. 우리 배열에 쓰는 것뿐

[369]      그 칸의 주소를 봉투(union doca_data)에 담는다
           → 아직 지역 변수. DOCA 로 안 감

[390/412]  alloc_init(kvIo_, task_user_data, &task)
           ★ 여기서 처음 DOCA 안으로 들어간다
               · 어느 큐 소속인지     ← 1번 인자
               · 이름표               ← 2번 인자
               · 명령 종류            ← 어느 함수를 불렀는지로 결정

[403/425]  set_key_value_conf(task, 키, 키길이, iovec주소, 1, 값길이)
           ★ 명령의 내용 전부가 이 한 줄에서 확정된다

[409/431]  as_task(task)
           아무것도 안 넣음. 타입만 바꿈

[437]      submit(task)
           아무것도 안 넣음. "처리하라" 신호
```

### 짚을 점 셋

**이름표는 할당할 때 붙는다.** 제출할 때가 아니라 `alloc_init` 의 두 번째 인자다.
명령서를 받는 순간 이미 "누구 것인지" 가 정해진다. 그래서 alloc 은 성공했는데
그 뒤에 실패해 반납하는 경로(`440`, `446`)에서도 이름표가 붙은 채로 반납된다.

**명령 종류는 인자가 아니라 함수 이름으로 정해진다.** `store_alloc_init` 이냐
`retrieve_alloc_init` 이냐. 그래서 WRITE/READ 분기가 **함수 이름만 다르고
구조가 같은** 모양이 된다.

**`as_task` 와 `submit` 은 아무것도 채우지 않는다.**
**명령이 완성되는 시점은 `set_key_value_conf` 가 반환되는 순간**이고,
그 한 줄이 사실상 NVMe KV 명령 그 자체다.

---

## 6. 이름표 — 하강과 상승을 잇는 유일한 끈

DOCA 는 이 요청에 대해 아무것도 모른다. 우리가 보낸 포인터를 **내용을 보지 않고
보관했다가 완료 콜백에 그대로 돌려줄 뿐**이다.

```
trySubmitRequest   task_user_data = {.ptr = &taskContexts_[i]}   →  DOCA
                                                                     │ 보관
콜백               static_cast<docaMemosTaskContext*>(user_data.ptr) ←┘
```

이름표를 **새로 만들지 않고 `taskContexts_` 배열의 i번 칸 주소**를 쓰는 이유:

| 후보 | 문제 |
|---|---|
| 지역 변수 | 반복문 한 바퀴면 소멸. 콜백이 올 때 이미 없음 |
| 매번 `new` | 콜백마다 `delete` 필요. task 당 힙 할당 1회 추가 |
| **핸들 안의 배열** | 핸들은 `releaseReqH` 까지 살고, 그건 모든 콜백 이후가 보장됨 |

**공짜로 정확한 수명을 얻는 방법**이다. 대가는 하나 —
**제출 후 그 벡터를 resize 하면 게시한 주소가 전부 무효화된다.**
헤더에 불변식으로 적혀 있으나 컴파일러가 강제하지 못한다
([adoption-report 7장 리스크 3](./adoption-report.md)).

---

## 7. 미확인

| 항목 | 상태 |
|---|---|
| `doca_task` 의 실제 크기·배치 | **확인 불가** (불투명 핸들, 비공개 헤더) |
| 실제 API 가 키를 복사하는가 | 목은 복사. 실제는 미확인 |
| `submit` 이 doorbell 까지 치는가, 폴링 때 몰아 치는가 | **미확인.** 목은 후자로 모델링 |
| 값 버퍼의 핀 고정을 누가 하는가 | 미확인. `registerMem` 은 아무것도 안 하므로 아래 어딘가 |
| 위 인자 6개의 NVMe KV 명령 필드 대응 | **NVMe KV 스펙 Rev 1.4 대조 필요** |

---

## 8. 관찰된 `submit` 의 숨은 동작

플러그인 주석(`434–436`)이 명시한다:

> *"doca_task_submit 이 실패 시 콜백을 동기로 부를 수 있으므로,
> 카운터는 제출 성공 후에만 올린다"*

즉 **`submit` 이 반환되기 전에 에러 콜백이 이미 실행됐을 수 있다.** 재진입이다.
`submittedTasks_++` 가 하필 그 줄에 있는 이유이며,
**락을 잡은 채 `submit` 을 부르면 같은 스레드로 콜백이 돌아 들어온다**는 뜻이기도 하다.

---

관련: [progress-engine-flow.md](./progress-engine-flow.md) · [backend-flow.md](./backend-flow.md) · [adoption-report.md](./adoption-report.md) · [../annotated/](../annotated/)
