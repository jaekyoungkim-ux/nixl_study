# 스터디 가이드 — 무엇을 어떤 순서로 읽을 것인가

> PR #1717(DOCA_MEMOS 백엔드)을 읽기 위한 순서와 경로.
> 배경 분석이 필요하면 [analysis/README.md](./analysis/README.md),
> 요청이 어떻게 흘러가는지는 [analysis/request-flow.md](./analysis/request-flow.md).

## 작업 위치

| 무엇 | 경로 | 브랜치 |
|---|---|---|
| **PR #1717 코드** | `DOCA MEMOS\nixl-pr1717\` | `pr-1717` |
| upstream main (레퍼런스) | `DOCA MEMOS\nixl_study\` | `main` |

아래 경로는 모두 **`nixl-pr1717\` 기준**이다.

```bash
# 순수 PR 변경분만 보기 (3-dot 필수 — base drift 제거됨)
git diff main...pr-1717 --stat
git diff main...pr-1717 -- src/plugins/doca_memos/
```

---

## 1단계 — 문서 (코드보다 먼저)

### ⓪ `docs/BackendGuide.md` — 27KB

**PR 소속이 아니라 원래 있던 공식 가이드.** 이걸 안 읽고 플러그인 코드를 보면
그 함수들이 왜 존재하는지 알 수 없다. **반드시 첫 번째.**

읽을 절:
```
## The South Bound API              백엔드가 구현해야 할 계약
   ### Capability Indicators        supportsRemote / supportsLocal / supportsNotif
   ### Memory Management            registerMem / deregisterMem
   ### Transfer Operations          prepXfer / postXfer / checkXfer / releaseReqH
## Descriptor List Abstraction      descriptor 개념
## Plugin Manager API               플러그인이 어떻게 로드되는가
## Comparing two plugins as an example    ← 특히 유용. 기존 백엔드와 비교
```

### ① `src/plugins/doca_memos/README.md` — 290줄

**저자(benlwalker)가 직접 쓴 명세.** PR이 추가하는 문서 중 가장 중요하다.
코드를 읽기 전에 "무엇을 의도했는가"를 잡아두면 훨씬 빨리 읽힌다.

- 설정 파라미터 5개의 의미와 기본값
- `registerMem`의 키 해석 3단계 규칙
- threaded / no-thread 두 progress engine의 동시성 설계
- 요청 생명주기와 fail-fast 에러 정책
- 알려진 제약 (notification 미지원, progress engine 1개 공유)

> **⓪ → ① 두 개만 먼저 읽어도 충분하다.** 합쳐 40KB 정도.
> 아래 ②③은 코드 읽는 중에 참조용으로 여는 편이 효율적이다.

### ② `test/unit/plugins/doca_memos/FAILURE_MODES_ANALYSIS.md` — 455줄

DOCA API 호출 하나하나에 대해 "어떤 에러가 날 수 있는가 / 테스트가 있는가"를 감사한 문서.
**저자가 스스로 미커버 영역을 ⚠️로 표시해 뒀다.** 취약점을 직접 찾을 필요가 없다.

### ③ `test/unit/plugins/doca_memos/README.md` — 278줄

테스트 44개의 범주 지도. 특정 동작을 검증하는 테스트를 역으로 찾는 색인으로 쓴다.

### ④ `src/api/cpp/backend/backend_engine.h` — 11KB

문서가 아니라 코드지만 성격은 계약서. `virtual` 메서드 **29개** 중
DOCA_MEMOS가 무엇을 구현했고 무엇을 안 했는지 대조한다. ⓪ 다음에 보면 좋다.

---

## 2단계 — 코드

전부 `src/plugins/doca_memos/` 아래.

| 순서 | 파일 | 크기 | 무엇 |
|---|---|---|---|
| **1** | `doca_memos_backend.h` | 9.5KB | 자료구조 + 클래스 선언. **30-32행의 forward 선언이 "여기까지가 우리 몫"이라는 경계** |
| **2** | `doca_memos_backend.cpp` | 20KB | ★ 시작점 |
| **3** | `doca_memos_progress_engine.h` | 8.9KB | 두 엔진의 설계 주석이 상세함 |
| **4** | `doca_memos_progress_engine.cpp` | 40KB | 가장 크고 어려운 부분 |
| — | `doca_memos_plugin.cpp` | 2KB | 등록 보일러플레이트. 나중에 잠깐 |

### `doca_memos_backend.cpp` 안에서의 순서

| 행 | 함수 | 왜 |
|---|---|---|
| **202** | `initDocaDevice()` | ★ **여기부터.** 에뮬레이션된 NVMe 디바이스를 잡아 쓸 수 있게 만드는 전 과정 |
| 136 | `parseInitParams()` | 위에서 쓰는 파라미터들이 어디서 오는지 |
| 386 / 413 | `convertToMemosKey()` / `resolveMemosKey()` | metaInfo → 16바이트 키 |
| 436 | `registerMem()` | 키가 확정되는 시점 (DOCA 호출 없음) |
| 488 | `prepXfer()` | descriptor 쌍 → iovec + 키 |
| 543 | `postXfer()` | progress engine에 위임 |
| 563 / 569 | `checkXfer()` / `releaseReqH()` | 완료 확인, 정리 |

### `doca_memos_progress_engine.cpp` 안에서의 순서

| 행 | 함수 | 왜 |
|---|---|---|
| 168 | 베이스 생성자 | IO 컨텍스트 생성 → PE 연결 → 가동 |
| 295 | `trySubmitRequest()` | **실제 NVMe KV 명령이 조립되는 곳** (347 store / 369 retrieve) |
| 63 / 108 | `taskCompletionCallback()` / `taskErrorCallback()` | 콜백 → 카운터 |
| 402~ | `nixlNoThreadProgressEngine::*` | 단순한 쪽 먼저 |
| 687~ | `nixlThreadedProgressEngine::*` | 더블버퍼 큐. 마지막에 |

---

## 미리 알아둘 것

**빌드가 안 된다.** `doca-kv` / `doca-common` pkg-config가 필요한데 DOCA 4.0이 미공개다.
유닛 테스트도 못 돌린다 — 목 헤더는 DOCA Core만 스텁하고 `doca_kvdev.h`는 진짜 SDK를 요구한다.
**읽기만 가능하다.**

**볼 수 있는 범위는 어댑터 한 층뿐이다.** 그 아래 `libdoca_kv`, BlueField 펌웨어, CMX 박스는 전부 비공개.
따라서 이 리뷰의 실질은 **"PR #1717이 비공개 MEMOS와 맺는 계약이 무엇인가"** 를 읽어내는 일이다.

**목 레이어가 의외로 값지다.** `test/unit/plugins/doca_memos/doca_memos_mocks.cpp`가
`std::map` 하나로 KV 디바이스를 흉내 낸다. 에뮬레이션된 디바이스가 호스트에게 어떻게
보여야 하는지를 읽을 수 있는 **유일한 공개 코드**다.

**PR #1837은 리뷰 대상이 아니다.** kvbench(Python)에 같은 백엔드를 붙이는 별개 PR(Draft, 외부 기여자).
담고 있는 C++ 사본이 1717보다 **945줄 오래됐고**, 브랜치 기반이 **2개월 낡아** main과 충돌한다.
Python에서 어떻게 쓰는지 궁금할 때만 `benchmark/kvbench/test/storage_backend.py`의
`DocaMemosBackend` 클래스를 참고. 로컬 브랜치 `pr-1837`로 받아뒀다.
