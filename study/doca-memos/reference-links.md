# CMX / DOCA MEMOS 참고 자료

분석 노트는 [analysis/README.md](./analysis/README.md) 참조.

---

## 1차 자료 — 발표

### NVIDIA GTC 2026 세션 S81773 — "Accelerate AI Inference Using DOCA for Storage"
<https://www.nvidia.com/en-us/on-demand/session/gtc26-s81773/?ncid=so-nvsh-645715&es_id=1058ea90ff>

DOCA Developer Day의 스토리지 세션. **이 프로젝트에 대한 가장 밀도 높은 단일 자료.**
발표자는 Ariel(DOCA 개괄), **Oren Duer**(리드 아키텍트, DOCA MEMOS 설계 철학), 그리고 NIXL/Dynamo 담당자.
DOCA MEMOS가 **처음 공개된 자리**이며, 다음 내용은 여기에만 있다:

- KV 캐시의 4가지 성질을 활용한 설계 논리 (고정 크기 / 불변 / 재계산 가능 / 최소 메타데이터)
- **"내구성 100% 보장 포기"** 결정과 그 근거
- 추론 프레임워크 개발자에게 요구하는 7가지 사항 (exist 금지, huge page, context hole 계산 등)
- CMX 박스 용량 **18PB**, hit rate 99.8%, goodput 96% 시뮬레이션 수치
- 컴퓨트/스토리지 양쪽 BlueField가 모두 필요한 이유 (Q&A)
- *"실측 성능 데이터는 아직 없다, POC 진행 중"* 이라는 자백

전문(AI 생성 스크립트): [gtc2026-s81773-transcript.txt](./gtc2026-s81773-transcript.txt)

---

## NVIDIA 공식

### NVIDIA Technical Blog — Introducing BlueField-4-Powered CMX
<https://developer.nvidia.com/blog/introducing-nvidia-bluefield-4-powered-inference-context-memory-storage-platform-for-the-next-frontier-of-ai/>

CMX 아키텍처를 다룬 **공식 기술 블로그**. G3.5 계층의 정의, BlueField-4의 역할(NVMe-oF 종단, 암호화·CRC 가속),
컴퓨트 노드(Rubin 플랫폼)와 스토리지 노드의 역할 분담, Dynamo/NIXL/Grove와의 연동을 설명한다.
**핵심 확인 지점**: *"standard NVMe and NVMe-oF transports, including NVMe KV extensions"* 지원을 명시 —
PR #1717이 `doca_nvme_kernel_kvdev`로 커널 NVMe 드라이버를 통과하는 이유를 뒷받침한다.

### NVIDIA CMX 제품 페이지
<https://www.nvidia.com/en-us/data-center/ai-storage/cmx/>

마케팅 관점의 제품 소개. BlueField-4가 **Vera CPU 기반**이라는 점, STX 모듈러 스토리지 기반이라는 점,
압축 처리량 3.29배 / CRC32C 무결성 검사 3.67배 같은 구체적 가속 수치가 여기 있다.
TPS 5배 / 전력 효율 5배 주장의 1차 출처. 다만 용량·대역폭 수치나 출시 일정은 공개하지 않는다.

### NVIDIA DOCA 공식 문서
<https://docs.nvidia.com/doca/sdk/>

**주의: 2026-08 기준 공개 문서는 DOCA 3.4.0까지다.**
PR #1717이 요구하는 `doca_kvdev.h`, `doca_kvdev_io.h`, `doca_nvme_kernel_kvdev.h` 및
`doca-kv` / `doca-common` pkg-config는 **DOCA 4.0 소속으로 아직 미공개**다.
즉 현재 이 플러그인은 외부에서 빌드할 수 없다. API 표면을 알고 싶으면
PR의 목 레이어(`test/unit/plugins/doca_memos/doca_memos_mocks.h`)가 사실상 유일한 명세다.

---

## 파트너 / 업계 시각

### Solidigm — Understanding CMX (Context Memory eXtension) in AI Workloads
<https://www.solidigm.com/products/technology/what-is-cmx-context-memory-storage.html>

SSD 벤더 관점의 CMX 해설. **메모리 계층을 Tier 0(GPU 레지스터)부터 G4까지 정리**한 것이 유용하다.
CMX용으로 권장하는 실제 드라이브 모델을 명시한다:
- **D7-PS1010** (PCIe Gen5 TLC) — 토큰 생성 크리티컬 패스의 지연 민감 읽기용
- **D5-P5336** (QLC, 최대 122TB) — 고밀도 옵션

액침냉각 JBOF 인클로저 형태라는 점도 여기서 확인된다.

### ScaleFlux — KV-Cache Churn Burns Through SSDs (TechTimes, 2026-08-01)
<https://www.techtimes.com/articles/322601/20260801/kv-cache-churn-burns-through-ssds-scaleflux-built-drive-level-storage-nvidia-cmx.htm>

또 다른 파트너의 대응. KV 캐시의 잦은 쓰기가 SSD 수명을 갉아먹는 문제를
드라이브 레벨에서 다룬 접근. DOCA MEMOS가 write amplification ≈ 1을 목표로 하는 이유와 맞물린다.

### NAND Research — Research Note: Improving Inference with NVIDIA's CMX
<https://nand-research.com/research-note-improving-inference-nvidias-inference-context-memory-storage-platform/>

독립 애널리스트의 분석. NVIDIA 자료 밖의 제3자 시각이 필요할 때.

---

## 표준

### NVM Express — Key Value Command Set Specification
<https://nvmexpress.org/wp-content/uploads/NVM-Express-Key-Value-Command-Set-Specification-Revision-1.3-2025.08.01-Ratified.pdf>

**DOCA MEMOS 호스트 API의 정체.** BlueField가 호스트에 에뮬레이션해 보여주는 것이 이 표준을 따르는
NVMe 컨트롤러다. DOCA 4.0 문서가 비공개인 지금, **`doca_kvdev` API의 의미를 해석할 수 있는 유일한 공개 근거**다.

확인되는 대응:
- **최대 키 16바이트** — 플러그인의 `DOCA_MEMOS_MAX_OBJECT_KEY_LEN = 16`, 발표의 *"128-bit keys"* 와 정확히 일치
- 명령 동사 **Store / Retrieve / Delete / Exist / List** — 플러그인이 쓰는 STORE / RETRIEVE / EXIST가 그중 셋
- Identify Namespace의 KV 용량 필드 — `doca_kvdev_get_max_key_len()` / `_max_value_len()`

리비전은 1.0a(2021)부터 1.3(2025-08)까지 있으며 위 링크는 최신본이다.
16바이트 키 상한은 전 리비전 공통.

---

## 코드

### ai-dynamo/nixl PR #1717 — DOCA MEMOS Backend
<https://github.com/ai-dynamo/nixl/pull/1717>

**이 스터디의 주 분석 대상.** 작성자 **benlwalker (Ben Walker, NVIDIA)** — SPDK 창시자 중 한 명.
2026-08-11 기준 **open(미머지)** 상태. 커밋 3개, 약 5,400 LOC.
리뷰는 CodeRabbit 자동 리뷰가 대부분이며 **설계에 대한 근본적 반대는 없다**
(지적 사항: clang-format, C++17에서 C++20 지정 초기화자 사용, 헤더 가드 명명, SPDX 헤더 누락).

```bash
git fetch upstream pull/1717/head:pr-1717
git diff main...pr-1717 --stat
```

### ai-dynamo/nixl — 업스트림 본체
<https://github.com/ai-dynamo/nixl>

NIXL(NVIDIA Inference Transfer Library). 2025 GTC 공개, **v1이 GTC 2026 주간에 릴리스**됐다.
프론트엔드 API와 백엔드 API를 분리한 구조라 DOCA MEMOS 같은 신규 백엔드를 플러그인으로 붙일 수 있다.
TRT-LLM, vLLM, SGLang, Dynamo KVBM, LMCache, SGLang HiCache, Anyscale Ray에 통합돼 있고 대부분에서 기본값.
