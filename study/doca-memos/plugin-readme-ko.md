# 플러그인 README 정리본 — DOCA_MEMOS

> `src/plugins/doca_memos/README.md`의 한국어 정리본. **원문 그대로가 아니다** —
> 코드와 어긋나는 곳을 고치고, 쓸모없는 절을 잘라냈다. 원문과 다른 지점은 아래 **정정 사항**에 전부 적어 뒀다.
>
> **원문**: PR #1717이 추가한 `src/plugins/doca_memos/README.md` (290줄)
> SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
> SPDX-License-Identifier: Apache-2.0
>
> [study_guide.md](./study_guide.md)의 **①단계** 자료. ⓪단계는 [backend-guide-ko.md](./backend-guide-ko.md).

---

## 정정 사항 — 원문과 다른 곳

읽기 전에 이것부터. 원문을 그대로 믿으면 잘못된 모델이 생긴다.

### ① Usage Example 전체를 잘라냈다 — API 이름이 실제와 다르다

원문 예제는 **백엔드용 SB API 이름을 agent에 갖다 붙였다.** 그대로 따라 쓰면 컴파일되지 않는다.

| 원문 예제 | 실제 agent API |
|---|---|
| `agent.createDlist(...)` | **존재하지 않는 함수** |
| `agent.postXfer(op, local, remote, ...)` | `createXferReq()` → `postXferReq()` **두 단계** |
| `agent.checkXfer(handle)` | `getXferStatus()` |
| `agent.releaseReqH(handle)` | `releaseXferReq()` |
| `agent.registerMem(desc, OBJ_SEG)` | 이름은 맞으나 인자는 **단일 desc가 아니라 dlist** |

`postXfer` / `checkXfer` / `releaseReqH`는 **백엔드가 구현하는 이름**이고, 사용자가 부르는 이름이 아니다.
NB API ↔ SB API 구분이 무너지므로 예제는 참고하지 말 것.

### ② `registerMem()` 절의 "retrieved by `postXfer()`"는 틀렸다

키를 꺼내는 것은 **`prepXfer()`** 다.

```
prepXfer:  remote[i].metadataP 를 따라가 objKey 를 꺼내 배열에 저장
postXfer:  이미 저장된 키를 써서 DOCA task 조립
```

원문은 같은 문서 안에서 `prepXfer()` 절에 *"No DOCA operations are performed"* 라고만 쓰고 키 얘기를 빼서, 두 절이 서로 어긋난다.

### ③ 하드웨어 요구사항 BF-3은 의심스럽다

원문은 **BlueField-3 이상**이라고 한다. 그런데 CMX / DOCA MEMOS 공식 자료(GTC 발표, 기술 블로그, 제품 페이지)는 **전부 BF-4 기준**이다.
어느 쪽이 맞는지는 DOCA 4.4가 공개돼야 확인된다. **BF-3으로 되리라 가정하지 말 것.**

### ④ DOCA 버전은 4.4다

원문이 명시한다 — **DOCA SDK 4.4 이상**. 공개 문서는 3.4.0까지이므로 갭이 그만큼 크다.
(이 스터디 노트들이 한때 "DOCA 4.0"으로 적었던 것은 오류다.)

---

## 개요

BlueField 장치에서 **key-value 연산(store / retrieve / exist)** 을 수행하는 NIXL 백엔드 플러그인.
NVIDIA의 DOCA KV 라이브러리(`libdoca_kv`)를 호출한다.

NIXL 백엔드 이름은 **DOCA_MEMOS**이며, 원문에 *"the underlying DOCA package may be renamed in the future"* 라는 단서가 붙어 있다.

### 요구사항

| 항목 | 내용 |
|---|---|
| DOCA SDK | **4.4 이상** (미공개 → 현재 빌드 불가) |
| 하드웨어 | 원문은 BlueField-3 이상. **정정 ③ 참조** |
| 헤더 | `doca_kvdev.h`, `doca_kvdev_io.h`, `doca_nvme_kernel_kvdev.h`, `doca_nvme_kernel_kvdev_io.h` |
| 라이브러리 | `libdoca_kv`(KV), `libdoca_common`(Core), `libdoca_nvme_kernel_kvdev` |

---

## 설정 파라미터

### 필수

| 이름 | 설명 | 예 |
|---|---|---|
| `device_name` | NVMe KV 장치 경로 | `/dev/nvme0n1` |

없으면 constructor가 예외를 던지고 백엔드가 생성되지 않는다.

### 선택

| 이름 | 기본값 | 설명 |
|---|---|---|
| `num_tasks` | `8192` | DOCA task pool 크기. `doca_kvdev_get_max_tasks()`로 장치 한계까지 clamp |
| `nguid` | all zeros | 32자 hex NVMe 네임스페이스 NGUID |
| `query_mem_mode` | `assume_success` | `assume_success` \| `actual` |
| `ignore_read_not_found` | `false` | `true`면 없는 키 retrieve도 성공 처리 (**버퍼 내용은 undefined**) |

---

## 지원하는 동작

### 메모리 등록

| 타입 | 정체 |
|---|---|
| **DRAM_SEG** | 데이터가 오갈 host DDR 버퍼 |
| **OBJ_SEG** | NVMe KV 장치 위의 key-value 쌍 |

**키 해석 3단계** — `metaInfo`를 이 순서로 시도한다:

```
① hex 문자열인가?   (최대 32자 = 16바이트)   → 디코드해서 키로
② 비어 있지 않은가? (최대 16바이트)          → raw 바이트를 그대로 키로
③ 비어 있으면                                → devId 8바이트를 키로
```

코드의 `resolveMemosKey()`와 1:1 대응한다.

### 전송

| NIXL op | DOCA 명령 |
|---|---|
| `NIXL_WRITE` | **STORE** |
| `NIXL_READ` | **RETRIEVE** |

### 조회 — `queryMem()`

| 모드 | 동작 |
|---|---|
| `assume_success` (기본) | 장치에 묻지 않고 **모든 descriptor에 성공 반환** |
| `actual` | 실제 DOCA EXIST 발행. 있으면 성공, 없거나 에러면 nullopt |

기본값이 `assume_success`인 것은 발표의 *"retrieve 전에 exist 하지 마라"* 요구와 맞물린다.
벤치마킹 시 실제 비용을 숨기므로 주의.

---

## 아키텍처

### DOCA 자원 여섯 개

constructor가 이 순서로 잡고, destructor가 역순으로 놓는다 (RAII).

| 자원 | 역할 |
|---|---|
| **nvmeKvdev** | NVMe-kernel KV 장치. start 전에 path + NGUID 설정 |
| **kvdev** | 위 장치를 `doca_kvdev` API로 본 것. 능력 조회용 |
| **kkvIo / kvIo** | 엔진별 I/O 컨텍스트. start 전에 task 수와 콜백 두 개 설정 |
| **ctx** | progress engine에 연결되는 DOCA 컨텍스트 |
| **PE (Progress Engine)** | task 제출과 완료 폴링 |

### 스레드 모델 — `enableProgTh`로 갈린다

**progress thread 있음 (`true`)**
전용 백그라운드 스레드가 **DOCA API의 유일한 호출자**다. 다른 스레드는 producer 큐에 넣기만 한다.
큐를 지키는 mutex는 **포인터/벡터 swap 동안만** 잡히므로, 호출자 쪽 핫패스가 사실상 lock-free다.

**progress thread 없음 (`false`)**
스레드를 만들지 않는다. `checkXfer()` / `queryMem()` 안에서 **호출자가 직접** 진행시킨다.
mutex 하나가 제출·폴링·취소를 전부 직렬화한다. 경합이 크므로 가능하면 threaded 모드를 권장.

### 요청 처리

전송·조회 하나가 `nixlDocaMemosBackendReqH` 하나로 표현된다.
이 핸들이 **자신이 만든 모든 DOCA task의 상태를 집계**한다.

```
checkXfer()  →  제출한 task가 전부 결과를 보고할 때까지  NIXL_IN_PROG
             →  전부 성공하면                          NIXL_SUCCESS
             →  아니면 처음 본 에러 상태
```

완료는 release ordering으로 게시되므로 호출자가 별도 락을 잡을 필요가 없다.

**실패는 sticky다** — 한 task라도 hard error를 내면, 이후 task가 성공해도 그 상태가 유지된다.

### 에러 처리 — fail-fast

- **첫 에러에서** 새 task 제출을 즉시 중단
- 이미 제출된 것은 완료를 기다림
- 배치 전체를 실패로 표시

이유가 명시돼 있다 — **NIXL에 per-task 상태를 보고할 API가 없어서 partial success를 표현할 수 없다.**
계약의 한계가 구현을 결정한 사례다.

---

## API별 동작

| 함수 | 하는 일 | DOCA 호출 |
|---|---|---|
| `registerMem()` | OBJ_SEG면 키를 확정해 metadata 객체에 담음 | **없음** |
| `prepXfer()` | 요청 핸들 생성. local에서 주소·길이, **remote에서 키**를 꺼내 짝지어 저장 | **없음** |
| `postXfer()` | DOCA task 조립·제출 | 있음 |
| `checkXfer()` | 완료 여부 보고 | 무스레드 모드에서만 폴링 |
| `queryMem()` | 모드에 따라 즉시 성공 또는 EXIST 발행 | `actual`에서만 |
| `releaseReqH()` | 핸들 해제 | — |
| `deregisterMem()` | metadata 객체 해제. **장치의 데이터는 지우지 않음** | 없음 |

### postXfer의 두 모드

| 모드 | 동작 |
|---|---|
| threaded | producer 큐에 넣고 **즉시 반환**. 할당·제출은 progress thread가 |
| no-thread | 엔진 mutex를 잡고 **그 자리에서** 제출 |

**task pool이 가득 차면** 남은 descriptor를 내부 큐에 넣고 재시도한다 —
threaded면 progress thread가, no-thread면 다음 `checkXfer()` / `queryMem()`이.

각 task의 `task_user_data`는 descriptor별 `docaMemosTaskContext`이고, 요청 핸들을 역참조한다.

### releaseReqH의 취소 처리

진행 중인 task가 있으면 **핸들을 "취소됨"으로 표시만 하고, 삭제는 마지막 콜백이 온 뒤로 미룬다.**
전송 완료 여부와 무관하게 호출해도 안전하다.

가이드의 *"releaseXferReq는 block하면 안 된다"* 규약을 지킨 방식이다.

### queryMem `actual` 모드의 대기 방식

키마다 EXIST task를 제출하고 **`std::this_thread::yield()`로 busy-poll** 하며 완료를 기다린다.
동기 호출이므로 지연 전량이 호출자에게 노출된다.

---

## 알려진 제약

- **notification 미지원** — NIXL의 알림 메커니즘을 쓰지 않는다
- **progress engine 하나를 공유** — 극단적 동시성에서 확장성 제한

## 스레드 안전성

- 두 모드 모두 공개 API는 thread-safe
- threaded: DOCA 호출은 progress thread에서만. 다른 스레드는 짧게 잡히는 mutex로 큐에 넣음
- no-thread: mutex 하나가 제출·진행·취소를 직렬화
- 요청 핸들의 완료 상태는 atomic이라 `checkXfer()`가 엔진 mutex 없이 폴링 가능

---

## 잘라낸 절과 이유

| 원문 절 | 왜 뺐나 |
|---|---|
| **Usage Example** | API 이름이 실제와 다름. 정정 ① 참조 |
| **Example Configuration** | 위와 같은 문제. 파라미터 표로 충분 |
| **Troubleshooting** | 빌드·실행이 불가능한 현 시점에 쓸모 없음 |
| **Performance Considerations** | 대부분 일반론. 핵심(`num_tasks`, 스레드 선택)은 위에 흡수 |

---

## 읽고 나서 — 코드 진입 순서

```
① (이 문서)
② doca_memos_backend.h        자료구조 + 클래스 선언
③ doca_memos_backend.cpp      initDocaDevice() → parseInitParams() → registerMem()
                              → prepXfer() → postXfer() → checkXfer()
④ doca_memos_progress_engine.h/.cpp   가장 크고 어려움 (1,039줄)
```

구체적인 감이 필요하면 ② 앞에 **`test/unit/plugins/doca_memos/doca_memos_mocks.cpp`** 를 훑어도 좋다.
`std::map` 하나로 KV 장치를 흉내 낸 코드라 "저장한다 / 가져온다"가 실제로 무엇인지 가장 구체적으로 보인다.

---

관련 문서: [study_guide.md](./study_guide.md) · [backend-guide-ko.md](./backend-guide-ko.md) · [analysis/README.md](./analysis/README.md) · [analysis/request-flow.md](./analysis/request-flow.md)
