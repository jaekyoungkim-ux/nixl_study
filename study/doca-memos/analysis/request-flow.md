# 사용자 요청이 CMX에 닿기까지 — NIXL과 PR #1717의 자리

> 컴퓨트 노드 안에서 요청이 어떤 모습으로 변해가며 전달되는지, 그리고 그 사슬에서
> [PR #1717](https://github.com/ai-dynamo/nixl/pull/1717)이 정확히 어느 칸을 채우는지.
> 코드 인용은 모두 `pr-1717` 브랜치 기준.

---

## 1. NIXL은 왜 있는가

추론 프레임워크(vLLM, SGLang, TRT-LLM 등)는 **KV 캐시를 계속 옮겨야 한다.**
GPU 메모리에서 빼내고, 다시 넣고, 다른 노드로 보내고.

문제는 **옮길 곳이 너무 많다**는 것이다. 다른 GPU, 호스트 메모리, 로컬 SSD, S3, Azure Blob,
네트워크 스토리지, 그리고 이제 CMX. 각각 API가 완전히 다르다. 프레임워크가 이걸 전부 구현하면 감당이 안 된다.

**NIXL(NVIDIA Inference Transfer Library)은 이 다양성을 흡수하는 계층이다.**
어디로 옮기든 주문 방식은 하나로 고정하고, 목적지별 실제 구현은 갈아 끼울 수 있는 플러그인에 맡긴다.

### 세 부분으로 나뉜다

| 이름 | 하는 일 | 비유 |
|---|---|---|
| **프론트엔드 API** (northbound) | 프레임워크의 주문을 받는다 | 접수 창구 |
| **코어** | 주문을 보고 누가 처리할지 정한다 | 배차 담당 |
| **backend 플러그인** (southbound) | 실제로 옮긴다 | 운송 수단 |

**프론트엔드 API**는 프레임워크가 호출하는 함수들이다. 실제 이름은 이렇다 (`src/api/cpp/nixl.h`):

```
registerMem()     "이 메모리 영역을 쓸 것이다" 등록
createXferReq()   "여기서 저기로 옮겨라" 주문서 작성
postXferReq()     주문 제출
getXferStatus()   "다 됐나?" 확인
releaseXferReq()  주문서 폐기
```

주문을 **받기만** 한다. 실제로 옮기지 않는다.

**코어**는 접수된 주문의 메모리 종류를 보고 어느 플러그인이 담당인지 판단해 호출한다.
플러그인이 아직 로드되지 않았으면 Plugin Manager가 디스크에서 찾아 올린다.

**backend**는 UCX, GDS, POSIX, OBJ(S3), AZURE_BLOB … 그리고 이 PR이 추가하는 **DOCA_MEMOS**다.

> **주의: NIXL에서 backend는 서버가 아니다.** southbound API를 구현하는 쪽,
> 즉 **아래를 향해 말을 거는 어댑터**를 뜻한다. 일반 웹 개발에서 backend가 "서버"를 가리키는 것과 방향이 반대다.

### 형식을 정하는 쪽은 NIXL이다

여기서 흔한 오해를 하나 끊고 가야 한다.

> **NIXL은 프레임워크마다 다른 형식을 해석하지 않는다. 프레임워크가 NIXL의 형식에 맞춘다.**

공용 자료구조는 `nixlBasicDesc` / `nixlBlobDesc` **딱 하나뿐**이고,
vLLM이든 SGLang이든 TRT-LLM이든 이 구조체를 직접 채워야 한다.
그래서 NIXL을 각 프레임워크에 **통합해 넣는 작업**이 필요했다 —
발표의 *"You'll find NIXL integrated in all of them and default in most of them"* 이 그 얘기다.

NIXL이 흡수하는 다양성은 **위쪽(호출자)이 아니라 아래쪽(backend 종류)** 이다.
호출자는 하나의 형식으로 수렴시키고, 목적지의 차이를 플러그인으로 감춘다.

---

## 2. 요청이 키가 되기까지 — 프레임워크 영역

*"이 계약서 검토해줘"* + 긴 첨부문서를 보냈다고 하자.

| 단계 | 데이터의 모습 |
|---|---|
| **텍스트** | `"이 계약서 검토해줘..."` |
| **토큰 배열** | `[1523, 88, 9021, ...]` — 토크나이저가 숫자로 변환. 5만 개라고 하자 |
| **블록 분할** | KV 캐시는 블록 단위로 다룬다. 모델마다 블록 크기가 고정돼 있다 |
| **프리픽스 해시** | 블록마다 "맨 앞부터 여기까지의 토큰들"을 해시 → `a3f9c2...` |
| **캐시 조회** | "이 해시들 중 GPU 메모리에 있는 것은?" 없는 것들의 목록 확보 |

**프리픽스 해시가 곧 키다.** 같은 대화의 앞부분은 언제나 같은 해시가 나오므로,
다른 노드가 이미 계산해 저장해 둔 것을 그대로 재사용할 수 있다. 이것이 공유가 성립하는 원리다.

여기까지가 프레임워크의 일이다. 없는 블록을 가져와야 하는 시점부터 NIXL로 넘어간다.

---

## 3. descriptor — NIXL의 유일한 공용 형식

**descriptor는 "데이터 한 조각이 어디 있는지 적은 쪽지"다.** 실제 구조체는 이렇게 생겼다
(`src/api/cpp/nixl_descriptors.h`):

```cpp
class nixlBasicDesc {
    uintptr_t addr;   // 시작 주소
    size_t    len;    // 길이
    uint64_t  devId;  // deviceID / blockID / fileID
};
```

`devId` 주석이 핵심이다. **쪽지 한 종류로 완전히 다른 대상들을 가리켜야 하기 때문에** 의미가 문맥에 따라 바뀐다:

| 대상 | addr | devId |
|---|---|---|
| 호스트 메모리 | 진짜 포인터 | 0 |
| GPU 메모리 | GPU 포인터 | 몇 번 GPU인지 |
| 파일 | 파일 안의 오프셋 | 파일 번호 |

스토리지용으로는 문자열 하나가 더 붙은 파생형이 있다:

```cpp
class nixlBlobDesc : public nixlBasicDesc {
    nixl_blob_t metaInfo;   // 오브젝트 이름, 키 등
};
```

**전송 하나에는 쪽지가 두 묶음 필요하다** — `local`(내 쪽)과 `remote`(상대 쪽).
"이 조각을 → 저 조각으로"이므로 짝이 맞아야 한다. 여러 조각을 한 번에 옮기므로 묶음(list)이다.

CMX에서 KV 블록을 읽어오는 경우라면 이렇게 채워진다:

```
local  = { addr = 받아둘 DRAM 버퍼 주소, len = 블록 크기, devId = 0 }
remote = { metaInfo = "a3f9c2..." (프리픽스 해시), len = 블록 크기 }
```

이제 `postXferReq(NIXL_READ, local, remote)`로 주문이 제출되고,
코어가 메모리 종류(`DRAM_SEG` ← `OBJ_SEG`)를 보고 **DOCA_MEMOS 플러그인**을 호출한다.

---

## 4. PR #1717이 하는 번역 — 가는 길

플러그인이 `prepXfer()`에서 주문서를 뜯어본다 (`doca_memos_backend.cpp:488`).

### 짝이 맞는지부터 확인한다

```cpp
// :504
if (remote.descCount() != desc_count) {
    NIXL_ERROR << "Descriptor count mismatch: local=" << desc_count
               << " remote=" << remote.descCount();
    return NIXL_ERR_INVALID_PARAM;
}
```

**개수가 다르면 아예 거부한다.** local과 remote는 인덱스로 1:1 대응하는 관계다.

### local에서 iovec을, remote에서 키를 뽑는다

```cpp
// :519 — local 쪽
req_h->valueIovecs_[i] = {reinterpret_cast<void *>(local_desc.addr), local_desc.len};

// :532 — remote 쪽
req_h->objectKeys_.push_back(kv_md->objKey);
```

**`iovec`은 변환 과정이 아니라 변환 결과물이다.** 그리고 DOCA가 만든 형식이 아니라
**POSIX 표준 구조체**다 (`#include <sys/uio.h>`):

```c
struct iovec {
    void  *iov_base;   // 버퍼 시작
    size_t iov_len;    // 길이
};
```

리눅스 `readv()` / `writev()`에 쓰는 바로 그것이며, 흩어진 버퍼 여러 개를 한 번에 지정하기 위한
표준 수단이다. DOCA는 이 표준 타입을 **그대로 받아들인다** — 별도 변환이 없다.

키 쪽은 `remote_desc.metadataP`에 이미 들어 있다. `registerMem()` 시점에 `metaInfo` 문자열을
16바이트로 확정해 두었기 때문에, 여기서는 꺼내 쓰기만 한다.

### DOCA task 한 장을 만든다

**DOCA task는 "하드웨어에 넣을 작업 지시서 한 장"이다.** 비동기 작업의 단위다.

```
① alloc_init()  풀에서 빈 지시서 한 장을 받아온다
② set_conf()    내용을 채운다 — 키, 버퍼 위치, 길이
③ submit()      제출한다. 함수는 즉시 돌아온다 (비동기)
④ (대기)        하드웨어가 처리한다
⑤ 콜백          끝나면 미리 등록해둔 함수가 불린다
⑥ free()        지시서를 반납한다
```

지시서는 **미리 정해진 수만큼만 존재한다.** 그것이 `num_tasks` 옵션(기본 8192)이며,
다 쓰면 반납될 때까지 기다려야 한다. 코드는 이 상황을 별도로 처리한다
(`doca_memos_progress_engine.cpp:336` — `DOCA_ERROR_FULL`이면 실패가 아니라 재시도 대상으로 큐잉).

> **DOCA와 DOCA MEMOS는 같은 말이 아니다.**
> DOCA는 우산 이름이고 그 아래 여러 라이브러리가 있다 (DOCA DMA, DOCA RDMA, DOCA STA …).
> MEMOS는 그중 하나다.
>
> | 심볼 | 소속 | pkg-config |
> |---|---|---|
> | `doca_pe`, `doca_ctx`, `doca_task` | **DOCA Core** — 모든 DOCA 라이브러리 공용 | `doca-common` |
> | `doca_kvdev_*`, `doca_nvme_kernel_kvdev_*` | **DOCA MEMOS** | `doca-kv` |
>
> `meson.build`가 두 의존성을 따로 요구하고, 테스트가 core 헤더만 로컬에 스텁해 두고
> `doca_kvdev.h`는 진짜 SDK를 요구하는 것이 이 구분의 증거다.

### 지시서 한 장 = descriptor 한 "쌍"

```cpp
// doca_memos_progress_engine.cpp:305
for (int i = req_h->nextDescriptorIndex_; i < local.descCount(); i++) {
    const docaMemosKey &object_key = req_h->objectKeys_[i];   // remote[i]에서 온 키
    ...
    doca_kvdev_io_task_store_set_key_value_conf(store_task,
                                                object_key.key,          // remote[i]
                                                object_key.keyLen,
                                                &req_h->valueIovecs_[i], // local[i]
                                                1,                       // iovec 개수
                                                value_len);
```

**인덱스 `i` 하나로 양쪽을 함께 쓴다.** `local[i]`가 버퍼를, `remote[i]`가 키를 제공해
**합쳐서 지시서 한 장**이 된다. local 따로 remote 따로 한 장씩이 아니다.

descriptor 쌍이 10개면 지시서는 10장이다.

---

## 5. 그 아래 — 볼 수 없는 구간

지시서를 `doca_task_submit()`으로 제출하면 플러그인의 일은 끝난다. 그 뒤는 이렇게 이어진다.

| 무엇이 | 어디서 | 공개 |
|---|---|---|
| DOCA task를 받아 NVMe KV 명령으로 만든다 | `libdoca_kv` (MEMOS 호스트 컴포넌트) | **비공개** |
| **SQE**를 조립해 제출 큐에 넣고 doorbell을 친다 | **리눅스 커널 NVMe 드라이버** | 공개(커널) |
| 명령을 받아 처리한다 | BlueField 에뮬레이션 계층 → MEMOS DPU 컴포넌트 | **비공개** |
| 네트워크 너머로 보내고 드라이브에 쓴다 | Spectrum-X → CMX 박스 | **비공개** |

**DOCA task는 SQE가 아니다.** 자주 혼동되는 지점이라 명확히 구분하면:

| | DOCA task | NVMe SQE |
|---|---|---|
| 어디에 | 유저 공간, 미리 할당된 풀 | 커널이 관리하는 제출 큐 링 |
| 무엇을 담나 | 키, iovec 포인터, 콜백, user data | 64바이트 하드웨어 명령 (opcode, NSID, 키, 데이터 포인터) |
| 누가 만드나 | **PR #1717** | **커널 NVMe 드라이버** |

타입 이름의 `doca_nvme_**kernel**_kvdev`가 커널 드라이버 경유를 뜻하므로 이 큰 그림은 확실하다.
다만 **`libdoca_kv`가 커널에 넘길 때 어떤 인터페이스를 쓰는지**(ioctl passthrough인지 io_uring인지 등)는
라이브러리가 비공개이므로 **이 레포에서는 확인할 수 없다.** *(추론 표시)*

---

## 6. 돌아오는 길

데이터가 도착하면 DOCA가 지시서마다 **콜백**을 부른다
(`doca_memos_progress_engine.cpp:63` `taskCompletionCallback`).

여기서 PR #1717의 두 번째 임무가 시작된다. **양쪽의 방식이 다르기 때문이다:**

| NIXL | DOCA |
|---|---|
| 주문서 **1장** | 지시서 **N장** (쌍의 개수만큼) |
| **물어보는** 방식 — `getXferStatus()`를 반복 호출 | **알려주는** 방식 — 끝나면 콜백 |

플러그인은 콜백이 올 때마다 완료 카운터를 올리고, **N장이 전부 끝났을 때만**
`checkXfer()`가 성공을 반환하게 만든다. 하나라도 남아 있으면 `NIXL_IN_PROG`다.

**N장을 1장으로 접고, 콜백을 폴링으로 바꾸는 것** — progress engine 약 1,100줄이 하는 일이 이것이다.
여기에 태스크 풀 고갈 시 재시도, 전송 도중 취소, 진행 스레드 유무 두 모드까지 얹혀 있다.

마지막으로 프레임워크가 DRAM 버퍼의 KV를 GPU로 올리고, GPU는 **없던 부분만 계산**한다.
앞부분 prefill을 통째로 건너뛰는 것 — 이 전체 사슬이 존재하는 이유다.

> **KV는 GPU 메모리로 직접 오지 않는다.**
> `getSupportedMems()`가 `{OBJ_SEG, DRAM_SEG}`만 반환하고, `doca_memos_backend.cpp:74`가
> 로컬 쪽이 `DRAM_SEG`가 아니면 거부한다. `VRAM_SEG`는 지원하지 않는다.
> 즉 **반드시 호스트 DRAM을 경유**하며, GPU로 올리는 것은 프레임워크의 별도 단계다.

---

## 7. 전체 사슬 한눈에

★ 표시가 PR #1717이 구현하는 칸이다.

| # | 데이터의 모습 | 누가 |
|---|---|---|
| 1 | 텍스트 | 사용자 |
| 2 | 토큰 배열 | 토크나이저 |
| 3 | 블록 단위로 분할 | 프레임워크 |
| 4 | 블록별 프리픽스 해시 = **키** | 프레임워크 |
| 5 | "GPU에 없는 것" 목록 | KV 캐시 매니저 |
| 6 | **descriptor 두 묶음** (local / remote) | 프레임워크 |
| 7 | 주문 제출 `postXferReq()` | → NIXL 프론트엔드 |
| 8 | 담당 플러그인 선택 | → NIXL 코어 |
| 9 | **16바이트 키** (remote[i]에서) | ★ PR #1717 |
| 10 | **iovec** (local[i]에서, POSIX 구조체) | ★ PR #1717 |
| 11 | **DOCA task** 한 장 = 쌍 하나 | ★ PR #1717 |
| 12 | NVMe KV 명령 | libdoca_kv (비공개) |
| 13 | **SQE** → 큐 → doorbell | 커널 NVMe 드라이버 |
| 14 | BlueField → Spectrum-X → CMX → 귀환 | 하드웨어 (비공개) |
| 15 | **콜백** 수신, 카운터 증가 | ★ PR #1717 |
| 16 | N장 전부 완료 → 성공 반환 | ★ PR #1717 → 코어 |
| 17 | DRAM 버퍼 → GPU 업로드 | 프레임워크 |
| 18 | 없던 부분만 계산 | GPU |

**한 문장으로:**

> PR #1717은 NIXL 방식의 주문서(descriptor 쌍)를 DOCA 방식의 지시서(task)로 바꿔 넣고,
> 지시서들이 전부 끝났는지 세어서 NIXL에게 보고하는 **어댑터 한 층**이다.
> 저장도, 전송도, 에뮬레이션도 하지 않는다.

---

프로젝트 전체 분석은 [README.md](./README.md) 참조.
