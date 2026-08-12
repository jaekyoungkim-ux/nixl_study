# 백엔드 생성부터 DOCA 제출 직전까지 — `doca_memos_backend.cpp`

> 사용자가 백엔드를 요청한 순간부터, progress engine 에 넘기기 직전까지 무슨 일이 일어나는가.
> 대상 파일: `src/plugins/doca_memos/doca_memos_backend.{h,cpp}` (579 + 264줄)
> 주석본: [../annotated/](../annotated/)

---

## 0. 코드가 실행되기 전에 이미 되어 있는 것

```
BlueField-4 가 NVMe KV 컨트롤러를 하드웨어 에뮬레이션
     ↓ PCIe 열거
리눅스 표준 NVMe 드라이버가 바인딩
     ↓
/dev/nvme0n1 이 존재한다              ← 우리 코드와 무관하게 이미 있음

CMX 박스에는 KV 볼륨이 만들어져 있다   ← 제어 평면이 미리 구성
```

플러그인은 이 상태를 **전제**한다. 만들지도, 찾지도 않는다.

---

## 1. 백엔드 생성

```
사용자:  agent.createBackend("DOCA_MEMOS", { "device_name": "/dev/nvme0n1", ... })
             ↓
plugin manager:  create_engine(init_params)
             ↓
             new nixlDocaMemosEngine(init_params)   ← constructor
```

constructor 는 네 단계를 순서대로 밟는다. 어느 단계든 실패하면 **예외를 던진다** —
반환값이 없기 때문이다. NIXL 이 그 예외를 잡아 `nullptr` 로 바꿔 사용자에게 알린다.

```
① parseInitParams()      사용자가 준 문자열 5개 → 멤버 변수
                         device_name / num_tasks / nguid /
                         query_mem_mode / ignore_read_not_found
                         ※ 장치와 무관. 사용자 map 에서 꺼낼 뿐

② device_name 이 비었는지 확인       비었으면 throw

③ initDocaDevice()       ← 아래 2장

④ createProgressEngine() ③이 확정한 numTasks_ / maxValueLen_ 로 엔진 생성
                         enableProgTh 에 따라 threaded / no-thread 중 하나
```

**결과: 반쯤 초기화된 백엔드는 존재할 수 없다.** 객체가 있다는 것 자체가
"장치가 켜져 있고 엔진이 돌고 있다"는 보증이 된다.

---

## 2. `initDocaDevice()` — 이 파일에서 유일하게 무거운 부분

```
[1] 경로 길이 확인      DOCA 라이브러리에게 "device_name 문자열을 몇 바이트까지 받나"
                        (장치가 아직 없으므로 장치가 답하는 게 아니다)

[2] NGUID 파싱          32자 hex → 16바이트

── 여기까지 객체 없음. 실패해도 정리할 것 없음 ──

[3] 장치 객체 생성      아직 어느 장치인지 모르는 빈 핸들
[4] set_path()          "/dev/nvme0n1" 을 그 핸들에 붙임   ★ 장치 매칭
[5] as_kvdev()          같은 장치를 일반 KV API 로 보는 별칭
[6] set_nguid()         [2]의 16바이트를 설정 (start 전에만 가능)
[7] start()             가동. 이 시점부터 명령 수신 가능

── 여기부터 실패 시 cleanupDocaResources() ──

[8] 능력 조회 3회       start 이후에만 물어볼 수 있다
     max_tasks      → numTasks_ 를 장치 한계로 깎음 (실패 아님, 조용히 조정)
     max_key_len    → 16 초과면 실패. 플러그인이 16바이트 고정 배열을 쓰므로
                      스펙이 바뀌었다는 신호로 보고 정직하게 멈춤
     max_value_len  → maxValueLen_ 에 저장. 이 볼륨의 최대 KV 블록 크기
```

**[4]가 "연결"의 전부다.** 탐색은 없다. 사용자가 지정한 경로 하나를 열 뿐이고,
틀리면 대안을 찾지 않고 실패한다.

---

## 2-1. `createProgressEngine()` — 두 파일이 만나는 이음매

constructor 의 마지막 단계. `initDocaDevice()` 가 **장치를 잡았다면**, 이것은
**그 장치를 굴릴 기계를 세운다.** 여기서부터 `doca_memos_progress_engine.cpp` 로 넘어간다.

### progress engine 이 무엇인가 — 이름이 오해를 부른다

"progress" 는 **전송을 진행시킨다**는 뜻이고, 여기엔 두 방향이 다 들어간다.

```
제출       요청을 DOCA task 로 만들어 넣는다
완료 수거   doca_pe_progress() 로 끝난 것을 거둬 카운터를 올린다
```

**둘 다 progress 다.** 완료 처리 전담 기계가 아니다.

특히 **DOCA task 를 조립하는 코드가 여기 있다** — `backend.cpp` 에는 task 를 만드는
줄이 하나도 없다. `prepXfer()` 는 재료(iovec 배열 + key 배열)만 모으고,
실제 조립·제출은 progress engine 의 `trySubmitRequest()` 가 한다.

> 같은 단어가 두 층에 있다. `doca_pe` 는 DOCA 의 객체로 완료를 거두는 것이고,
> `nixlDocaMemosProgressEngine` 은 그것을 **포함해서** IO 컨텍스트·제출 로직·재시도·취소를
> 전부 묶은 플러그인의 클래스다.

### 하는 일 — 둘 중 하나를 만든다

```
init_params->enableProgTh 가
     true  →  nixlThreadedProgressEngine
     false →  nixlNoThreadProgressEngine
```

넘겨주는 값 세 개가 전부 `initDocaDevice()` 가 확정한 것이다. **constructor 에서
③이 ④보다 먼저여야 하는 이유가 여기서 드러난다.**

| 넘기는 값 | 출처 | progress engine 에서의 쓰임 |
|---|---|---|
| `nvmeKvdev_` | [3]~[7] | IO 컨텍스트 생성 |
| `numTasks_` | [8a] (장치 한계로 깎인 값) | task pool 크기 |
| `maxValueLen_` | [8c] (볼륨 블록 크기) | 제출 시 버퍼 크기 검사 |

threaded 쪽만 `pthrDelay` 를 하나 더 받는다. 0 이면 "busy-spin 한다" 는 경고가 뜨고,
progress thread 가 condvar 로 자지 않고 계속 돈다.

### `enableProgTh` 의 출처가 다르다

플러그인 파라미터가 아니다.

```
agent 생성 시 설정      →  init_params->enableProgTh
createBackend 파라미터   →  init_params->customParams   (device_name 등)
```

**agent 를 만들 때 정해지고 그 agent 의 모든 백엔드가 공유한다.** 이 백엔드만 따로 정할 수 없다.

### 두 엔진의 차이 — 완료 처리만이 아니다

| | 무스레드형 | 스레드형 |
|---|---|---|
| DOCA 를 부르는 스레드 | 여럿 (호출자들) | **progress thread 하나뿐** |
| 제출 주체·시점 | 호출자가 `postXfer()` 안에서 **즉시** | progress thread 가 **나중에** |
| `postXfer()` 반환 시점 | 이미 제출 끝남 | **아직 제출 전** |
| dlist 복사 | 불필요 | **통째로 복사** (제출이 나중이라) |
| `checkXfer()` | **폴링 + 재시도 후** 카운터 확인 | 카운터만 읽음 |
| 동기화 | mutex 하나로 전부 직렬화 | DOCA 경로엔 락 없음, 큐만 짧게 |

**무스레드형은 `checkXfer()` 를 부르지 않으면 전송이 영원히 끝나지 않는다.**
장치가 일을 마쳐도 완료를 거둬올 사람이 없기 때문이다.

무엇이 같은지도 분명하다 — **task 를 조립하는 방법**은 베이스 클래스의
`trySubmitRequest()` 하나를 둘이 공유한다. 다른 것은 **누가 언제 그것을 부르는가** 다.

### 에러 처리가 두 겹인 이유

```
① try / catch          std::thread 생성 실패 같은 예외
② hasInitError() 확인   DOCA 자원 생성 실패
```

progress engine 의 생성자는 DOCA 실패에 **예외를 던지지 않고 플래그만 세운다.**
그래서 만든 뒤 따로 물어봐야 한다. 반면 `std::thread` 생성은 진짜 예외를 던진다.

---

## 3. 등록 — key 가 확정되는 시점

여기서부터 descriptor 가 등장하는데, **두 종류**임을 먼저 구분해야 한다.

| | 4번째 칸 | 쓰는 함수 |
|---|---|---|
| **등록용** (`nixlBlobDesc`) | `metaInfo` — **문자열** | `registerMem()` |
| **전송용** (`nixlMetaDesc`) | `metadataP` — **포인터** | `prepXfer()` |

같은 대상을 두 번, 다른 형태로 기술한다. 등록 시점에는 metadata 객체가 아직 없으니
문자열로 넘기고, 전송 시점에는 객체가 있으니 주소로 가리킨다.

### `registerMem()` — agent 가 descriptor 를 하나씩 넘긴다

```
DRAM_SEG  →  out = nullptr
             준비할 것이 없다. 주소·길이는 전송 때 descriptor 에 다시 실려 오므로
             미리 저장할 이유가 없다

OBJ_SEG   →  resolveMemosKey() 로 16바이트 key 확정
             그 key 를 담은 객체를 힙에 만들어 out = 그 주소
```

`resolveMemosKey()` 는 세 경로를 순서대로 시도한다:

| 조건 | key 가 되는 것 |
|---|---|
| `metaInfo` 비었음 | `devId` 8바이트 |
| hex 로 읽힘 (짝수 길이, 32자 이하, 전부 hex 글자) | 디코드한 바이트 ← **KV 캐시 정상 경로** |
| 그 외, 16바이트 이하 | 글자 바이트를 그대로 |

> 함정: `"abcd"` 는 전부 hex 글자라 디코드되어 2바이트, `"ckpt"` 는 `k` 때문에
> raw 경로로 가서 4바이트가 된다. 같은 길이인데 결과가 다르다.

**DOCA 호출이 한 번도 없다.** KV 캐시는 새 key 가 계속 생겨 이 함수가 워크로드 내내
반복 호출되는데, 장치 왕복이 없어 값싸다.

### 소유권

```
registerMem      객체 생성 → 주소를 out 에 씀
agent            그 주소를 보관
전송 시점         descriptor 의 metadataP 칸에 그 주소를 채워 되돌려줌
deregisterMem    delete
```

백엔드가 "어느 주소에 어느 key" 표를 만들지 않아도 되는 구조다.

---

## 4. 전송 준비 — `prepXfer()`

```
① 검사        operation 이 READ/WRITE 인가
              local 이 DRAM_SEG, remote 가 OBJ_SEG 인가
              양쪽 descriptor 개수가 같은가        ← 다르면 거부

② 핸들 1개 생성   nixlDocaMemosBackendReqH
                  totalTasks_ = descriptor 개수
                  ※ descriptor 가 몇 개든 핸들은 하나

③ local[i]     addr, len  →  valueIovecs_[i]   (iovec 으로 형변환)
④ remote[i]    metadataP 따라가 key 를 꺼냄  →  objectKeys_[i]
```

③④가 인덱스 `i` 로 짝을 이룬다. 이 짝 하나가 나중에 DOCA task 한 장이 된다.

```
i=0:  버퍼 0x7000, 1MB   +   key a3f9c2...
i=1:  버퍼 0x7100, 1MB   +   key b7e1d4...
```

**iovec 으로 옮겨 담는 이유**는 두 가지다. DOCA 가 `struct iovec` 을 요구하고,
DOCA 에 넘기는 것이 *그 자리의 주소*라 플러그인이 소유한 곳에 있어야 하기 때문이다.
NIXL 코어의 descriptor 목록은 함수가 끝난 뒤 살아 있다는 보장이 없다.

**DOCA 호출 없음. 전송 시작 안 함.**

---

## 5. 제출 이후 — 셋 다 위임

```
postXfer      핸들을 우리 타입으로 형변환 → progressEngine_->postXfer()
checkXfer     "                        → progressEngine_->checkXfer()
releaseReqH   "                        → progressEngine_->cancelRequest()
```

세 함수가 세 줄로 끝나는 이유는 2-1 에 있다. 두 엔진이 같은 이름의 함수들을 갖고 있어,
`progressEngine_->postXfer(...)` 한 줄이 담긴 쪽에 따라 다르게 동작한다.
**부르는 코드는 어느 엔진이 담겼는지 알 필요가 없다.**

형변환이 필요한 이유: NIXL 코어는 백엔드별 핸들 타입을 모르므로 공통 조상 타입으로만
다룬다. 돌아온 것을 원래 타입으로 되돌려야 한다.

`releaseReqH` 는 **즉시 성공을 반환**한다. 진행 중인 task 가 있으면 "취소됨" 표시만 하고
실제 삭제는 마지막 콜백 이후로 미룬다 — 가이드의 "non-blocking 이어야 한다" 규약.

---

## 6. 경계 — 여기까지가 볼 수 있는 범위

| 층 | 누가 | 공개 |
|---|---|---|
| 어느 경로인지 지정, key 확정, 재료 모으기 | **PR #1717** | ✅ |
| DOCA task → NVMe KV 명령 변환, 커널에 전달 | `libdoca_kv` (호스트, 같은 프로세스) | ❌ |
| SQE 조립, 큐, doorbell | 커널 NVMe 드라이버 | 공개(커널) |
| 에뮬레이션, 라우팅, CMX 전송 | BlueField-4 → Spectrum-X → CMX | ❌ |

---

## 이 파일에서 확인된 것

- **플러그인은 장치를 찾지 않는다.** 사용자가 준 경로 하나를 열 뿐이다.
  노드에 NVMe 가 여럿이면 어느 것이 CMX 백엔드인지 **배포하는 사람이 알아야 한다.**
- **DOCA 실작업은 `initDocaDevice()` 하나뿐**이다. 579줄 중 나머지는
  문자열 다루기, key 확정, 재료 모으기, 위임이다.
  **DOCA task 를 조립하는 코드조차 이 파일에 없다** — progress engine 에 있다.
- **`registerMem` 과 `prepXfer` 에 DOCA 호출이 없다.** KV 캐시처럼 key 가 계속
  바뀌는 워크로드에서 장치 왕복을 만들지 않으려는 설계로 보인다.
- 가이드의 OBJ descriptor 표와 두 곳이 어긋난다 — remote 의 `addr`/`len` 을 읽지 않고
  (KV value 부분 읽기 불가), `devID`/`metaInfo` 우선순위가 뒤집혀 있다.

---

다음: `doca_memos_progress_engine.cpp` (1,039줄) — 실제 KV 명령이 조립되는 곳.
이어지는 노트는 [progress-engine-flow.md](./progress-engine-flow.md).

관련: [README.md](./README.md) · [request-flow.md](./request-flow.md) · [progress-engine-flow.md](./progress-engine-flow.md) · [../annotated/](../annotated/)
