# 주석본 — DOCA_MEMOS 플러그인 코드

PR #1717의 소스에 한글 주석을 단 사본. **코드 자체는 원본 그대로**이고,
`▶` 로 시작하는 줄만 추가된 주석이다.

> SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
> SPDX-License-Identifier: Apache-2.0

## 파일

| 파일 | 원본 | 원본 행수 |
|---|---|---|
| `doca_memos_backend.h` | `src/plugins/doca_memos/doca_memos_backend.h` | 264 |
| `doca_memos_backend.cpp` | `src/plugins/doca_memos/doca_memos_backend.cpp` | 579 |

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

## 아직 안 한 것

```
doca_memos_progress_engine.h     226줄
doca_memos_progress_engine.cpp  1,039줄   ← 가장 어려움
doca_memos_plugin.cpp             51줄   ← 등록 보일러플레이트
```

progress engine 은 위 두 파일을 이해한 뒤에. 그 안에서도 순서가 있다 —
생성자 → `trySubmitRequest()`(**실제 KV 명령이 조립되는 곳**) → 콜백 → 무스레드 엔진 → 스레드 엔진.

---

관련: [../study_guide.md](../study_guide.md) · [../translations/plugin-readme-ko.md](../translations/plugin-readme-ko.md) · [../analysis/request-flow.md](../analysis/request-flow.md)
