# 주석본 — DOCA_MEMOS 플러그인 코드

PR #1717의 소스에 한글 주석을 단 사본. **코드 자체는 원본 그대로**이고,
`▶` 로 시작하는 줄만 추가된 주석이다.

> SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
> SPDX-License-Identifier: Apache-2.0

## 파일

| 파일 | 원본 행수 | 읽는 순서 |
|---|---|---|
| `doca_memos_backend.h` | 264 | ① 참조용 |
| `doca_memos_backend.cpp` | 579 | ② 흐름은 이쪽을 따라간다 |
| `doca_memos_progress_engine.h` | 226 | ③ 두 엔진의 설계 |
| `doca_memos_progress_engine.cpp` | 1,039 | ④ 가장 어려움 |
| `doca_memos_plugin.cpp` | 51 | ⑤ 등록 보일러플레이트 |

원본은 모두 `src/plugins/doca_memos/` 아래에 있다.
`backend.*` 두 개는 손으로, `progress_engine.*` 와 `plugin.cpp` 는 원본을 복사한 뒤
스크립트로 주석을 삽입해 만들었다 — **삭제·변경 0줄로 검증됨.**

**주석이 들어가 행 번호가 원본과 어긋난다.** 행 번호로 인용할 때는 반드시 원본 파일(`nixl-pr1717/`)을 볼 것.

## 읽는 순서

`.cpp` 를 따라가며 구조체가 나올 때만 `.h` 를 참조하는 방식을 권한다.

`.cpp` 안에서는 파일 순서가 아니라 이 순서로:

```
① initDocaDevice()      ★ 시작점. 장치를 잡아 쓸 수 있게 만드는 전 과정
② parseInitParams()     위에서 쓰는 파라미터의 출처
③ convertToMemosKey / resolveMemosKey    키 해석 3단계
④ registerMem()         키가 확정되는 지점 (DOCA 호출 없음)
⑤ prepXfer()            descriptor 쌍 → iovec + 키
⑥ postXfer()            progress engine 에 위임
⑦ checkXfer / releaseReqH
```

`.h` 에서 찾을 것:

| 무엇 | 별명 |
|---|---|
| `docaMemosKey` | 16바이트 상자 |
| `docaMemosTaskContext` | task 한 장의 표식 |
| `nixlDocaMemosBackendReqH` | 전송 핸들 (카운터 3인방) |
| forward 선언 2개 | "여기까지가 우리 몫" 경계선 |

## progress_engine.cpp 안에서의 순서

파일 순서대로 읽지 말 것. 각 함수 위 주석에 표시해 뒀다.

```
① 베이스 생성자        DOCA 자원을 만들고 켜는 과정 (initDocaDevice 와 같은 패턴)
② trySubmitRequest()   ★ 실제 NVMe KV 명령이 조립되는 곳
③ 콜백 2개             완료/에러가 카운터로 접히는 지점
                       — EXIST 는 항상 에러 콜백으로 온다는 특이 규약도 여기
④ nixlNoThread*        단순한 쪽 먼저
⑤ nixlThreaded*        더블버퍼 큐. 마지막에
```

## 목 레이어 — `mocks.{h,cpp}`

원본은 `test/unit/plugins/doca_memos/` 아래에 있다.

| 파일 | 원본 행수 | 무엇 |
|---|---|---|
| `doca_memos_mocks.h` | 204 | 가짜 장치의 자료구조 + 에러 주입 손잡이 |
| `doca_memos_mocks.cpp` | 771 | 그 구현 |

**DOCA 4.4 가 비공개인 지금 `doca_kvdev` API 의 모양을 읽을 수 있는 사실상 유일한 근거다.**
다만 **시그니처는 진짜 헤더에서 오므로 믿을 수 있으나, 동작은 저자가 만든 모델**이다.
실제 장치 명세가 아니라 "플러그인이 장치에게 기대하는 것" 으로 읽어야 한다.

읽을 곳은 사실상 두 군데뿐이다:

```
alloc_typed_task()   에러 주입이 걸리는 지점
doca_pe_progress()   ★ 가짜 장치가 실제로 일하는 유일한 곳
                       STORE    → kv_store[key] 에 복사
                       RETRIEVE → kv_store 에서 꺼내 buffer 로
                       EXIST    → 있으면 ALREADY_EXIST, 없으면 NOT_FOUND
                                  어느 쪽이든 **에러 콜백** 으로
```

나머지는 대부분 getter/setter 보일러플레이트다.

**progress_engine 을 읽을 때 옆에 두고 쓰는 것을 권한다.** DOCA 함수가 나오면
`mocks.cpp` 에서 그 함수를 찾아보면 "이 호출이 실제로 무엇을 하는가"가 그 자리에서 풀린다.

### 목이 드러낸 것 둘

- `set_key_value_conf` 가 **iovec 배열과 개수**를 받는다 → API 는 scatter-gather 를
  지원하는데 플러그인이 `1` 로 고정하고 있다
- `set_do_not_overwrite` / `set_must_exist` 가 존재한다 → **조건부 쓰기 옵션이 있으나
  플러그인이 쓰지 않는다.** 실제 API 표면이 플러그인이 쓰는 범위보다 넓다

## 아직 주석 안 단 것

```
doca_{compat,ctx,error,pe,types}.h    ~430줄  DOCA Core 헤더 스텁 (기계적)
doca_memos_backend_test.cpp          1,850줄  TEST_F 44개
```

스텁 헤더들은 상수와 타입 정의뿐이라 주석할 것이 없다.
`doca_kvdev.h` 는 스텁이 없다 — **진짜 SDK 를 요구하므로 현재 빌드·테스트가 불가능한 원인.**

---

관련: [../study_guide.md](../study_guide.md) · [../translations/plugin-readme-ko.md](../translations/plugin-readme-ko.md) · [../analysis/request-flow.md](../analysis/request-flow.md)
