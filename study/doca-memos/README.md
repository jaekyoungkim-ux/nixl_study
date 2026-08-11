# NVIDIA CMX + DOCA MEMOS 프로젝트 분석

> GTC 2026 세션 S81773 발표, NVIDIA 공식 문서, 그리고 [ai-dynamo/nixl PR #1717](https://github.com/ai-dynamo/nixl/pull/1717) 코드를 교차 대조한 정리.
> 작성 시점: 2026-08-11 / PR #1717 상태: **open (미머지)**

---

## 0. 한 줄 요약

**CMX는 하드웨어 제품(새 스토리지 계층), DOCA MEMOS는 그 위에서 도는 소프트웨어 SDK**다. 자료마다 두 이름을 섞어 쓰기 때문에 혼동하기 쉽다.
그리고 **PR #1717은 이 스택 전체 중 딱 한 조각** — 컴퓨트 노드 호스트에서 NIXL이 DOCA MEMOS를 호출하는 connector — 만 구현한다.

---

## 용어 정리

본문을 읽기 전에 짚고 갈 용어들. 셋 다 일상적인 단어지만 이 맥락에서 뜻이 좁게 정해져 있고, 특히 pod는 흔히 오해된다.

### 워커 (worker)

**두 가지 뜻으로 쓰인다. 발표에서도 섞어 쓴다.**

**(a) AI 에이전트를 사람 직원에 비유한 것** — 발표의 주 용법.
*"신입에게 매뉴얼을 잔뜩 가르쳤는데 다음 날 오니 다 잊어버렸다면 곤란하지 않겠나"* 라는 맥락에서 나온다.
이 문서에서 **"워커 하나당 50GB"** 라고 할 때의 워커가 이것 — **진행 중인 에이전트 세션 하나**를 가리킨다.
그 세션의 대화 이력 전체가 KV 캐시로 GPU 메모리를 점유한다.

**(b) GPU worker — 추론을 실제로 수행하는 서빙 프로세스** — 기술 용어.
추론 클러스터에서 GPU를 잡고 도는 실행 단위. Disaggregated serving에서는 두 종류로 나뉜다:
- **prefill worker** — 프롬프트를 읽어 KV 캐시를 생성하는 쪽
- **decode worker** — 토큰을 하나씩 생성하는 쪽

Dynamo가 요청을 어느 worker로 보낼지 라우팅한다.

**두 뜻이 만나는 지점이 이 프로젝트의 핵심이다.**
에이전트 세션(a)이 GPU worker(b) 위에서 도는데, **그 세션의 KV 캐시가 특정 worker의 로컬 메모리에 갇히는 것**이 문제였다.
CMX는 그 캐시를 worker 바깥의 공유 계층으로 빼낸다. 그래서 어느 worker가 받아도 이어서 일할 수 있게 된다.

### pod (팟)

**Kubernetes의 pod가 아니다.** 같은 발표에 Kubernetes(Dynamo Grove) 얘기가 나와서 더 헷갈리기 쉬운 지점이다.

여기서 pod = **하나의 고속 네트워크 패브릭으로 묶인 GPU 노드 집합**, 즉 AI 데이터센터를 증설할 때의 배치 단위다.
NVIDIA DGX SuperPOD가 대표 사례이고 랙 여러 개 규모다. 발표의 시뮬레이션은 **GPU 1,000장** 규모를 가정했다.

"pod-level 공유"가 의미하는 것은 **공유 범위의 단계**다:

| 범위 | 의미 |
|---|---|
| node-level | 노드 한 대 안에서만 → G3 로컬 SSD |
| **pod-level** | **패브릭으로 묶인 노드 전부가 접근 → CMX (G3.5)** |
| datacenter-level | 데이터센터 전역 → G4 네트워크 스토리지 |

CMX가 굳이 pod 레벨인 이유가 여기 있다. node-level은 공유가 안 되고, datacenter-level은 너무 멀다.
*"GPU 메모리의 연장선"* 이라는 표현도 이 거리감에서 나온다 — 패브릭 하나만 건너면 닿는다.

### 인터커넥트 계층 — scale-up / scale-out

위 표가 *범위*라면, 이건 그 범위를 **무엇이 물리적으로 이어주는가**다. NVIDIA는 scale-up / scale-out으로 구분한다.

| 범위 | NVIDIA 용어 | 연결 수단 | 통신 방식 |
|---|---|---|---|
| 노드(~랙) 안 | **scale-up** | **NVLink + NVSwitch** | 메모리 시맨틱 — load/store |
| pod 안 | **scale-out** | **InfiniBand(Quantum-X)** 또는 **Spectrum-X 이더넷** | 메시지/RDMA 시맨틱 |

**노드 안 — NVLink / NVSwitch**
- **NVLink** = GPU와 GPU를 잇는 링크 자체
- **NVSwitch** = 링크들을 모아 모든 GPU가 서로 full 대역폭으로 통신하게 하는 스위치 칩
- 결정적 성질은 **메모리 일관성(memory-coherent)**. GPU가 다른 GPU 메모리를 **자기 메모리처럼 직접 읽고 쓴다.** 명시적 "전송"이 아니라 포인터 접근이다
- 주의: **"노드 한 대"라는 경계는 이미 흐려졌다.** GB200 NVL72처럼 랙 하나(GPU 72장)가 통째로 하나의 NVLink 도메인인 구성이 있다. 정확히는 "노드 안"이 아니라 **"scale-up 도메인 안"**

**pod 안 — scale-out 패브릭**
- 여기서 성격이 근본적으로 바뀐다. **NVLink가 끊기고 NIC을 거친다**
- **RDMA** — 상대 메모리에 직접 쓰지만 load/store가 아니라 명시적 전송 요청
- **GPUDirect RDMA**로 NIC이 GPU 메모리에 직접 DMA. CPU와 호스트 메모리를 경유하지 않는다
- 발표의 *"move the KV cache on the east-west"* 에서 **east-west**가 이 구간 (노드 간 횡방향 트래픽)
- **CMX는 Spectrum-X 이더넷을 쓴다.** InfiniBand가 아니라 이더넷을 고른 것은 스토리지 파트너 생태계와 붙기 위한 선택으로 보인다 *(추론)*

#### 이 계층 구분이 이 프로젝트의 핵심인 이유

**NVLink와 scale-out 사이에는 대역폭 절벽이 있다.** 세대마다 다르지만 Blackwell 기준
NVLink는 GPU당 양방향 1.8TB/s 수준인 반면 800Gb/s NIC은 100GB/s다. 한 자릿수 배가 아니라 **십수 배** 차이다.

여기서 **CMX가 왜 이더넷 건너편에 있어야만 하는지**가 나온다.

> NVLink 도메인은 scale-up 경계(노드~랙) 안에서만 유효하다. 그런데 **pod 전체가 KV 캐시를 공유**하려면
> 그 경계를 넘어야 하고, 넘는 순간 **무조건 scale-out 패브릭을 통과**해야 한다. 선택지가 없다.

[1장의 계층 표](#1-문제-정의--왜-만들었나)와 겹쳐보면:

| 계층 | 어디에 | 어떻게 닿나 |
|---|---|---|
| G1 (HBM) | 자기 GPU | 직접 |
| G2 (호스트 DRAM) | 자기 노드 | PCIe |
| G3 (로컬 SSD) | 자기 노드 | PCIe — **그래서 공유 불가** |
| **G3.5 (CMX)** | **pod 안 별도 박스** | **Spectrum-X + RDMA** |
| G4 (네트워크 스토리지) | 데이터센터 | 같은 패브릭이지만 홉이 더 많음 |

그리고 이것이 **BlueField가 이 로직의 자리인 이유**다. 어차피 이더넷을 건너야 한다면,
그 관문에 앉아 있는 NIC/DPU가 KV 저장·암호화·무결성을 처리하는 것이 가장 자연스럽다. GPU도 CPU도 건드리지 않고.

바꿔 말하면 — **G3.5는 "NVLink로는 닿을 수 없지만 최대한 가깝게"를 물리적으로 구현한 계층**이다.

### TPS (Tokens Per Second)

**초당 생성 토큰 수.** 스토리지 문맥이라 transactions per second로 읽기 쉬운데 아니다.

- **TPS (총합)** — 클러스터 전체가 초당 뽑아내는 토큰. 처리량 지표
- **TPS per user** — 사용자 한 명이 체감하는 생성 속도. 응답이 흘러나오는 빠르기

**둘은 상충한다.** 동시 사용자를 늘리면 총 TPS는 오르지만 1인당 TPS는 떨어진다.
발표의 Pareto 곡선이 정확히 이 trade-off를 그린 것이고, 공유 스토리지를 넣으면 **곡선 자체가 위로 밀린다**는 게 주장이다:
- 같은 375명 동시 사용자에서 1인당 TPS가 크게 오르거나
- 같은 1인당 TPS를 유지하면서 500명 대신 **700~750명**을 받거나

**"TPS 5배"는 총합 기준이다.** 그리고 발표는 TPS만으로 부족하다며 **goodput**을 제안한다 —
재계산이 아닌 진짜 새 토큰만 세는 지표. [6장](#6-프로젝트-성숙도--냉정하게) 참조.

---

## 1. 문제 정의 — 왜 만들었나

에이전틱 AI로 넘어가면서 KV 캐시가 감당 불가능해졌다는 것이 출발점이다.

- 100K 토큰 컨텍스트 = **50GB**, 400K = **200GB**. 이건 [워커](#워커-worker) **하나당** 수치다.
- 워커가 여러 개고, 멀티스텝이고, 서로 지식을 공유해야 한다.
- 기존 메모리 계층이 이걸 감당하지 못한다.

### 기존 계층과 그 한계

| 계층 | 정체 | 문제 |
|---|---|---|
| G1 | GPU HBM | 가장 빠르지만 극도로 비쌈, 금방 넘침 |
| G2 | 호스트 CPU 메모리 | 커지고 있지만 여전히 부족 |
| G3 | 로컬 SSD | **공유 불가** — 치명적 |
| G4 | 네트워크 스토리지 | 공유되지만 컴퓨트에서 너무 멂 |

발표의 비유가 정확하다. G4 접근은 *"10코스 요리 도중 소금이 떨어져 마트에 다녀오는 것"* — 차라리 처음부터 다시 요리(recompute)하는 게 빠르다.
그래서 지금 GPU들이 재계산에 전력을 낭비하고, 스토리지가 병목이 되면 GPU가 논다.

**→ G3와 G4 사이에 새 계층을 끼워 넣었다. 그것이 G3.5 = CMX.**

---

## 2. CMX — 하드웨어

| 항목 | 내용 |
|---|---|
| 정식 명칭 | Context Memory eXtension |
| 공개 경로 | CES 2026 개념 발표 → GTC 2026 실물 박스 + 데모 |
| 위치 | [팟(pod)](#pod-팟) 레벨 공유. GPU 메모리의 연장선으로 취급 |
| 구성 | BlueField-4 스토리지 프로세서(Vera CPU 내장) + NVMe SSD 액침냉각 JBOF + [Spectrum-X 이더넷(RoCE)](#인터커넥트-계층--scale-up--scale-out) |
| 규모 | 박스 하나당 **약 18페타바이트** (발표에서 명시) |
| 주장 성능 | 일반 스토리지 대비 **[TPS](#tps-tokens-per-second) 5배, 전력 효율 5배** |
| 파트너 | NVIDIA 설계 → 스토리지 파트너가 제조·납품. Solidigm(D7-PS1010 Gen5 TLC / D5-P5336 QLC 122TB), ScaleFlux 등이 대응 발표 |

---

## 3. DOCA MEMOS — 소프트웨어와 설계 철학

아키텍트 Oren Duer의 논리를 그대로 옮기면:

> 가장 효율적인 스토리지 프로토콜은 **블록**이다. 그런데 블록은 공유가 어렵다.
> 공유하려면 분산 파일시스템을 얹어야 하는데, 그건 이미 너무 복잡하고 기능 과잉이다.
> **블록에 훨씬 가깝지만 블록보다는 조금 더 해주는, 그 중간의 새로운 스토리지**가 필요하다.

그 "조금 더"를 어디서 확보했는가 — **KV 캐시라는 데이터의 특수한 성질 4가지를 끝까지 활용**했다. 이것이 DOCA MEMOS 설계의 전부다.

| KV 캐시의 성질 | 얻어낸 단순화 |
|---|---|
| **크기가 고정** (모델마다 블록 크기 일정) | 스토리지를 그 granularity로 사전 구성. 파일시스템처럼 1바이트~기가바이트를 다 대응할 필요 없음 |
| **불변(immutable)** — 한 번 쓰면 수정 안 함 | 부분 수정 로직 전부 삭제. 삭제 후 재작성만 지원 |
| **재계산 가능** → **내구성 불필요** | **100% 보장을 포기.** 99.8%면 충분 — 가장 급진적인 결정 |
| **메타데이터 최소** | 파일명·키 룩업·free/busy 리스트 없음 → **write amplification ≈ 1**, SSD 수명 연장 |

### "내구성 포기"가 이 프로젝트의 정체성

발표에서 *"스토리지 하시는 분들은 이 지점을 알아보실 것"* 이라고 한 대목이다.
100% 보장과 99.9% 보장 사이의 구현 복잡도 차이는 어마어마하다. 이 하나를 버려서 성능과 단순성을 샀다.

### 대신 애플리케이션이 져야 하는 부담

발표에서 추론 프레임워크 개발자들에게 명시적으로 요구한 사항:

1. 키는 **128비트**까지 (프리픽스 해시를 그대로 사용)
2. **KV마다 메타데이터를 직접 관리하지 말 것** — 18PB에 대한 메타데이터는 DRAM을 다 잡아먹는다. MEMOS가 알아서 한다
3. **huge page를 쓸 것** — 값이 크기 때문에 4K 페이지 매핑 오버헤드가 크다
4. **retrieve 전에 exist 하지 말 것** — retrieve 자체가 exist 검사이고, exist가 yes여도 retrieve 시점엔 사라졌을 수 있다. 네트워크 왕복만 낭비
5. **retrieve 실패를 정상 케이스로 처리할 것**
6. **context hole을 계산할 것** — 중간 블록 하나 없다고 그 뒤 전부 버리지 말고 빠진 것만 재계산 (vLLM 대응 중, Dynamo도 대응 중)
7. **라우팅 함수를 고칠 것** — 이제 어느 노드에서든 KV에 접근 가능하므로 노드 고정이 불필요

---

## 4. 3단 구조 — 왜 BlueField가 양쪽에 다 필요한가

발표 Q&A에서 청중이 정확히 이걸 질문했다. *"컴퓨트 노드 쪽만 있으면 되지 않나? 양쪽에 두고 소프트웨어까지 쪼개면 오버헤드 아닌가?"*

답: **I/O 파이프라인을 의도적으로 양쪽에 쪼갰고, 둘 다 필수다.**

```
[컴퓨트 노드]
  Dynamo                          ← KV 라우팅, 어느 노드에 뭐가 있는지 인지
    └ NIXL                        ← 전송 계층 ★ PR #1717이 꽂히는 지점
      └ DOCA MEMOS 호스트 드라이버  ← store/retrieve 키-밸류 API
        └ [에뮬레이션 계층]         ← 호스트엔 그냥 "드라이브"로 보임
          └ BlueField-4 (컴퓨트측) ← 격리, 보안, 암호화, 무결성, KV 매핑
              │
              ↓ Spectrum-X 이더넷 (RDMA)
              │
[CMX 박스]
          BlueField-4 (스토리지측) ← 올바른 포맷·레이아웃으로 배치
            └ NVMe 드라이브들       ← 여기서만 블록 API
```

- **컴퓨트측 BF4**: SNAP 기술의 연장선. 호스트에는 평범한 드라이브로 **에뮬레이션**해서 보여주고, 실제 데이터 오케스트레이션은 DPU가 한다. 호스트 CPU를 쓰지 않으면서 격리/보안 계층을 확보
- **스토리지측 BF4**: 데이터 바로 옆에서 해야 효율적인 작업(배치, 무결성 등)을 담당
- **위 3개 API 중 앞의 둘은 키-밸류, 마지막 하나만 블록**이다
- 두 BlueField 사이에는 **NVIDIA 자체 프로토콜**이 돈다 (비공개). 서드파티가 CMX 박스를 직접 만들려면 NVIDIA에서 MEMOS SDK를 받아 통합해야 한다고 명시

### KV 볼륨 개념

모델별로 볼륨을 만든다. 드라이브 그룹을 묶어 **특정 max KV 블록 크기**를 가진 KV 볼륨으로 제공한다.

- DeepSeek (KV 블록 ~800K) → 1MB 볼륨을 만들어 DeepSeek 쓰는 컴퓨트 노드끼리 공유
- Llama 70B (KV 블록 ~10MB) → 별도 볼륨, 다른 드라이브 할당
- 한 노드가 두 모델을 돌리면 두 볼륨을 모두 붙일 수 있다

---

## 5. PR #1717은 이 그림의 어디인가 — 코드가 증언하는 것

**코드가 발표 내용과 1:1로 맞아떨어진다.**

| 발표에서 말한 것 | PR #1717 코드의 증거 |
|---|---|
| "128비트 키까지 지원" | `DOCA_MEMOS_MAX_OBJECT_KEY_LEN = 16` (=128비트). 주석에 *"현재 스펙 값(128-bit)"* 명시 |
| "retrieve 전에 exist 하지 마라" | `query_mem_mode` 기본값 = **`assume_success`** — 디바이스에 묻지 않고 무조건 "있다"고 답해 프레임워크가 바로 retrieve로 가게 만듦 |
| "retrieve 실패를 정상 처리하라" | `ignore_read_not_found` 옵션 — 키가 없어도 성공 반환 (버퍼 내용은 undefined) |
| "KV별 메타데이터 만들지 마라" | `registerMem()`이 **DOCA 호출을 전혀 하지 않음.** 키만 메타데이터 객체에 넣고 끝. 완전 stateless |
| "모델별로 KV 볼륨을 만들어 공유" | `nguid` 파라미터 = NVMe 네임스페이스 GUID → **볼륨 선택자로 보임** *(추론)* |
| "볼륨마다 최대 KV 블록 크기 지정" | `doca_kvdev_get_max_value_len()`으로 읽어 `maxValueLen_`에 저장, 초과 시 제출 거부 |
| "호스트에 드라이브로 에뮬레이션" | `device_name = /dev/nvme0n1`, API가 `doca_nvme_kernel_kvdev_*` — **리눅스 커널 NVMe 드라이버를 통과.** NVIDIA 블로그의 *"NVMe KV extensions 지원"* 과 일치 |

**결론: PR #1717 = 위 스택 그림에서 "NIXL ↔ DOCA MEMOS 호스트 드라이버" 구간 하나.**
DPU 펌웨어도, CMX 박스 소프트웨어도, 두 BlueField 간 프로토콜도 이 PR에 없다. 그것들은 비공개 DOCA SDK 안에 있다.

### PR 구성

작성자: **Ben Walker (benlwalker, NVIDIA)** — SPDK 창시자 중 한 명. 스토리지 스택 설계 이력이 최상급이고, 코드 품질이 높은 이유를 설명한다.

```
978fffa  doca_memos: add DOCA_MEMOS cache backend plugin   ← 플러그인 본체
3d7b20f  doca_memos: add unit test suite                    ← 목 기반 테스트
348c87e  nixlbench: add DOCA_MEMOS backend support          ← 벤치마크 통합
```

### 추가되는 파일 (약 5,400 LOC)

```
src/plugins/doca_memos/
├── doca_memos_backend.{h,cpp}           (~660 LOC)  nixlBackendEngine 구현
├── doca_memos_progress_engine.{h,cpp}   (~1,100 LOC) threaded / no-thread 이중화
├── doca_memos_plugin.cpp                             플러그인 등록
├── meson.build
└── README.md                            (290 LOC)

test/unit/plugins/doca_memos/
├── doca_memos_backend_test.cpp          (70KB, TEST_F 44개)
├── doca_memos_mocks.{h,cpp}             (33KB) DOCA API 목 레이어
├── doca_{compat,ctx,error,pe,types}.h   DOCA 코어 헤더 스텁
├── FAILURE_MODES_ANALYSIS.md            DOCA 32개 에러코드별 커버리지 감사
└── meson.build
```

### 플러그인 설계 요점

- `getSupportedMems() = {OBJ_SEG, DRAM_SEG}` — 로컬 DRAM ↔ 원격 KV 키. `NIXL_WRITE`=STORE, `NIXL_READ`=RETRIEVE
- **키 해석 3단계**: `metaInfo`가 hex(≤32자)면 디코드 / 아니면 raw 바이트(≤16B) / 비어 있으면 `devId` 8바이트
- **progress engine 이중화**: `enableProgTh=true`면 전용 스레드가 DOCA API를 단독 호출하고 더블버퍼 producer 큐 사용(생산자 임계구역 = push 1회 + 포인터 스왑). false면 caller가 `checkXfer()`에서 직접 폴링, 단일 mutex로 직렬화
- **task pool 고갈 시 backpressure**: 남은 descriptor를 내부 큐잉 후 재시도 (FIFO 보장 테스트 존재)
- **fail-fast 배치 처리**: NIXL에 per-task 상태 API가 없어 partial success를 표현할 수 없음 → 첫 에러에서 제출 중단, 에러는 sticky
- RAII: `NvmeKvdevDeleter`가 started 여부 확인 후 stop→destroy

### 설정 파라미터

| 이름 | 기본값 | 설명 |
|---|---|---|
| `device_name` | (필수) | NVMe KV 디바이스 경로. 예 `/dev/nvme0n1` |
| `num_tasks` | 8192 | DOCA task pool 크기. `doca_kvdev_get_max_tasks()`로 clamp |
| `nguid` | all zeros | 32자 hex NVMe 네임스페이스 NGUID |
| `query_mem_mode` | `assume_success` | `assume_success` \| `actual` |
| `ignore_read_not_found` | `false` | 키 없는 read를 성공 처리 |

### nixlbench 통합

- `--backend=DOCA_MEMOS` + `--doca_memos_*` 5개 옵션
- **`--op_type=QUERY` 신규** — `queryMem()` 자체를 벤치마킹 (동기 호출이라 latency 전량이 `transfer_duration`에 기록). `actual` 모드용으로 사전에 키를 pre-populate
- `isObjStorageBackend()`에는 포함하되 **`hasObjectStorageHelpers()`를 새로 분리** — DOCA_MEMOS는 putObj/getObj/rmObj가 없어 기존 OBJ 정리 경로(rmObj 호출)를 타면 안 됨. 같은 이유로 remote IOV를 `NixlMemRegion`이 아닌 별도 `remote_iovs` 벡터로 관리
- `--check_consistency`는 명시적으로 거부

### 사용하는 DOCA API 표면 (39개)

공개 문서가 없으므로 이 목록이 사실상 `doca_kvdev` API의 유일한 명세다.

```
디바이스:  doca_nvme_kernel_kvdev_{create,destroy,set_path,as_kvdev}
          doca_nvme_kernel_kvdev_cap_get_max_path_len
          doca_kvdev_{start,stop,is_started,set_nguid}
          doca_kvdev_get_max_{tasks,key_len,value_len}
IO 컨텍스트: doca_nvme_kernel_kvdev_io_{create,destroy,as_kvdev_io}
          doca_kvdev_io_{as_ctx,set_num_tasks}
          doca_kvdev_io_set_task_{completion,error}_cb
태스크:    doca_kvdev_io_task_{store,retrieve,exist}_alloc_init
          doca_kvdev_io_task_{store,retrieve}_set_key_value_conf
          doca_kvdev_io_task_exist_set_key_conf
          doca_kvdev_io_task_retrieve_get_result_value_len
          doca_task_{submit,free,get_status}
PE/CTX:   doca_pe_{create,destroy,progress,connect_ctx}
          doca_ctx_{start,stop}
```

---

## 6. 프로젝트 성숙도 — 냉정하게

**아직 완성품이 아니다.**

- **PR #1717은 현재 열려 있다** (미머지). 리뷰는 CodeRabbit 자동 리뷰가 대부분이고 **설계에 대한 근본적 반대는 없다**. 지적은 clang-format, C++20 지정 초기화자를 C++17에서 쓴 것, 헤더 가드 명명, SPDX 헤더 누락 수준
- **DOCA 4.0이 아직 공개되지 않았다.** 공개 문서는 3.4.0까지고 `doca_kvdev.h` / `doca-kv` pkg-config는 검색되지 않는다. **이 플러그인은 현재 빌드 불가**이며, meson이 `required: false`로 조용히 스킵하도록 짜여 있는 이유다
- **실측 성능 데이터가 없다.** 발표에서 *"We don't have real performance data yet. We're just in the middle of POCing all that"* 라고 직접 말한다. 5x, 99.8% hit rate, 96% goodput은 전부 **시뮬레이션**이다
- 사소한 발견: 커밋 메시지는 `subnqn`, `ns_id` 파라미터를 언급하지만 **최종 코드엔 없다.** 초기 리비전에서 NVMe-oF 주소 지정을 시도했다가 `device_name` + `nguid`로 단순화한 흔적

### goodput — 기억해 둘 새 지표

총 토큰이 아니라 **재계산이 아닌 진짜 새 토큰의 생성률**. NVIDIA가 이 프로젝트를 정당화하는 핵심 논거다.
시뮬레이션 조건(18PB CMX, GPU 1,000장 전부 pre-fill)에서 hit rate 99.8%, goodput 96% 이상을 주장한다.

---

## 7. 다음 단계 — 미리 짚어둘 관문

BlueField를 NIXL backend로 붙이는 작업을 할 때 부딪힐 지점들:

1. **빌드 자체가 막힌다.** `doca-kv` / `doca-common` pkg-config가 필요한데 공개 DOCA에 없다. NVIDIA 얼리액세스 없이는 컴파일 불가 → **유닛 테스트도 못 돌린다** (목 헤더는 `doca_ctx.h`, `doca_pe.h` 등 코어만 스텁하고 `doca_kvdev.h`는 **진짜 헤더를 요구**한다)
2. **하드웨어가 BlueField-3가 아니라 BlueField-4를 요구할 가능성**이 크다. 플러그인 README는 "BF-3 이상"이라고 하지만 CMX/MEMOS 공식 자료는 전부 BF-4 기준이다
3. **양쪽 BlueField가 다 필요하다.** 컴퓨트 노드 BF만으로는 동작하지 않는다 (발표에서 명시적으로 확인됨)
4. **`assume_success` 기본값**은 벤치마킹 시 실제 비용을 숨긴다

### 관찰된 코드 리스크

1. `query_mem_mode=assume_success`가 기본값 — QUERY 벤치 시 `actual` 지정 필요
2. `ignore_read_not_found=true`면 버퍼 내용이 undefined인 채로 성공 반환 — 벤치마크 전용 성격
3. `docaMemosTaskContext`가 `reqH->taskContexts_` 벡터 내부에 살아서, **제출 후 그 벡터를 resize하면 DOCA에 넘긴 주소가 무효화**된다 (헤더 주석에 invariant로 명시돼 있으나 컴파일러가 강제하지 못함)
4. `FAILURE_MODES_ANALYSIS.md`가 스스로 미커버 케이스를 나열 중 (init 경로의 `DOCA_ERROR_NOT_FOUND`, `NO_MEMORY`, `OPERATING_SYSTEM` 등)
5. 알려진 제약: notification 미지원, progress engine 1개 공유

---

## 부록: 분석 환경 재현

```bash
# 업스트림 등록 후 PR을 브랜치로 가져오기
git remote add upstream https://github.com/ai-dynamo/nixl.git
git fetch upstream pull/1717/head:pr-1717
git worktree add ../nixl-pr1717 pr-1717

# 순수 PR 변경분만 보기 (3-dot: merge-base 기준, base drift 제거됨)
git diff main...pr-1717 --stat
git diff main...pr-1717 -- src/plugins/doca_memos/
git log --oneline main..pr-1717
```

> **주의**: 폴더끼리 직접 diff하면 base drift 때문에 노이즈가 크게 섞인다.
> PR #1717은 main보다 오래된 커밋에서 갈라져 나왔으므로, main 쪽의 최신 변경
> (prometheus_mp 텔레메트리, stale-generation 핸들 가드 #2027, UCX sendAm 최적화 #2057 등)이
> 전부 diff에 나타난다. 반드시 3-dot을 쓸 것.

---

관련 자료 목록은 [reference-links.md](./reference-links.md) 참조.
발표 전문은 [gtc2026-s81773-transcript.txt](./gtc2026-s81773-transcript.txt).
