# BackendGuide 번역 — NIXL Backend Plugin Interface

> `docs/BackendGuide.md`의 한국어 번역. API 이름·타입명·핵심 개념어는 영어를 유지했다.
>
> **원문**: [`docs/BackendGuide.md`](../../docs/BackendGuide.md)
> SPDX-FileCopyrightText: Copyright (c) 2024-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
> SPDX-License-Identifier: Apache-2.0
>
> [study_guide.md](./study_guide.md)의 **⓪단계** 자료다. 이 문서를 읽지 않고 플러그인 코드를 보면
> 그 함수들이 왜 존재하는지 알 수 없다. 계약 원문은 산문이 아니라
> [`src/api/cpp/backend/backend_engine.h`](../../src/api/cpp/backend/backend_engine.h)의 virtual 29개다.

---

# NIXL Backend Plugin Interface 개요

NIXL(NVIDIA Inference Xfer Library)은 LLM serving 같은 분산 inference workload에서 효율적인 데이터 전송을 위해 **고대역폭·저지연 통신**을 제공하도록 설계되었다. 이 라이브러리는 CPU, GPU, 그리고 다양한 storage type을 포함하는 이기종(heterogeneous) 디바이스 전반에 걸쳐 통신 메커니즘과 memory access를 추상화한다. 이 추상화는 NIXL 라이브러리 사용자에게 **North Bound API(NB API)** 로 제공되며, 사용자는 단순한 buffer list primitive를 통해 NIXL agent에 transfer request를 표현하고, request 생성 후 non-blocking·비동기 방식으로 전송을 시작할 수 있다. NIXL은 그 전송을 최적의 backend plugin에 위임하며, 그 결과 이기종 memory 및 storage 시스템 전반에서 **Speed of Light(SOL)** 수준의 매끄러운 데이터 이동이 이루어진다.

이는 NIXL의 Transfer Agent와 여러 backend plugin 사이의 표준화된 인터페이스 역할을 하는 **South Bound API(SB API)** 를 통해 달성된다. NIXL agent는 backend plugin에 등록된 local memory에 대한 bookkeeping을 담당하며, local 또는 remote 전송에 대한 one-sided transfer(즉 Read 및 Write 연산)에 필요한 metadata 관리도 담당한다. 다음 다이어그램이 이 구성 요소들을 더 자세히 보여준다.

![Figure of NIXL high level architecture](../../docs/figures/nixl_high_level.png)

각 backend는 고유한 특성과 기능을 가질 수 있다. 예를 들어 UCX는 system memory 및/또는 GPU memory 사이에서 데이터 이동을 수행하는 고성능 통신 라이브러리인 반면, GPUDirect Storage(GDS)는 storage disk와 GPU memory 사이에서 데이터를 옮길 수 있다. 이렇게 다양한 transport를 관리하고 NIXL의 dynamicity를 보장하려면, inference app의 transport 요구사항에 따라 라이브러리를 **on-demand로 로드**할 수 있어야 한다. 이 on-demand 요구를 위한 추가 API 집합이 SB API와 함께 라이브러리에 구현되어야 하며, 그래야 NIXL에 pluggable한 **NIXL Plugin**이라 불릴 수 있다.

plugin 구현은 SOL을 달성하기 위해 C++로 작성되며, backend plugin용 header file은 [backend directory](https://github.com/ai-dynamo/nixl/tree/main/src/api/cpp/backend)에서 찾을 수 있다.

# Plugin Architecture 및 구현 개요

NIXL은 모듈식 plugin architecture를 구현하며, 각 backend는 SB API를 통해 자신의 기능을 노출하는 라이브러리 안에 캡슐화된다 — [UCX](https://github.com/ai-dynamo/nixl/tree/main/src/plugins/ucx), [GPUDirect Storage(GDS)](https://github.com/ai-dynamo/nixl/tree/main/src/plugins/cuda_gds), 또는 임의의 custom 구현이 그 예다. NIXL 내부의 **Plugin Manager** 컴포넌트가 backend plugin의 discovery, loading, instantiation을 처리하며, 이는 plugin이 동적으로 로드되든 NIXL 라이브러리에 정적으로 빌드되어 들어가든 마찬가지다. 각 plugin은 SB API와 더불어 plugin manager를 위한 몇 가지 method를 구현해야 하며, 이는 이후 절에서 다룬다.

SB API의 일부 method는 반드시 구현할 필요는 없다는 점에 유의하라. 예를 들어 어떤 backend가 notification을 지원하지 않는다면 `supportsNotif()` method를 통해 그 사실을 알릴 수 있고, 이 method가 false를 반환하면 agent는 그 backend에 notification이 포함된 request를 보내지 않는다. 이러한 **capability indicator가 4개** 있으며, 각각에 대해 어떤 API를 구현해야 하는지는 아래에서 더 자세히 설명한다.

![NIXL SB API](../../docs/figures/nixl_sb_api.png)

## The South Bound API

backend가 NIXL과 호환되려면 다음을 포함한 여러 핵심 SB API method를 구현해야 한다.

### Constructor 및 Destructor

* **Constructor**: Agent 이름과 함께 key/value 형태의 parameter 집합이 backend로 전달된다.
* **Destructor**: 남아 있는 resource를 해제한다.

key/value parameter는 Agent에서 전달되는 string → byte array의 map이다. backend가 초기화 parameter로 필요로 하는 것은 무엇이든 담을 수 있다. 이를 어떻게 지정하는지에 대한 자세한 내용은 `get_backend_options` plugin API를 참고하라. `src/utils/common/backend.h`에 있는 함수들을 사용하면 parameter에 접근하고 흔히 쓰이는 타입으로 변환할 수 있다.

### Capability Indicators

* `supportsLocal()`: backend가 **node 내부** 전송을 지원하는지 표시
* `supportsRemote()`: backend가 **node 간** 전송을 지원하는지 표시
* `supportsNotif()`: backend가 notification을 지원하는지 표시
* `getSupportedMems()`: backend가 지원하는 memory type을 표시

앞의 세 method(`supports*`)에 따라 구현해야 하는 method가 달라진다. 예를 들어 UCX backend는 모든 시나리오를 지원하므로 전부 구현하는 반면, GDS backend는 `supportsLocal`만 갖는다(Example implementations에서 더 자세히 다룬다).

**network backend는 `supportsRemote`와 `supportsNotif`가 true여야 하며**, local 전송을 위해 다른 backend가 개입할 필요가 없도록 `supportsLocal`도 true인 것이 바람직하다. **storage backend는 `supportsLocal`을 가져야 하며 `supportsNotif`는 선택**이다.

### Connection Management

* `connect()`: remote agent로의 connection을 개시한다
* `disconnect()`: remote agent와의 connection을 종료한다
* `getConnInfo()`: remote agent를 위한 connection 정보를 serialize된 byte array 형태로 제공한다
* `loadRemoteConnInfo()`: remote agent로부터 받은 connection 정보(byte array)를 로드한다

일부 backend는 loopback을 위한 자기 자신과의 connection이 필요하므로, `connect`와 `disconnect`는 **항상 필수**다 — backend는 local 통신이나 remote 통신, 또는 둘 다를 지원하기 때문이다. 반면 `getConnInfo`와 `loadRemoteConnInfo`는 `supportsRemote`가 설정된 경우에만 필요하다.

`loadRemoteConnInfo`는 connection을 개시하지 않는다는 점에 유의하라. 사용자가 첫 전송 전에 connection을 미리 맺어두고 싶다면 `connect` method 호출이 이루어진다. `connect`가 호출되는 또 다른 경우는 backend가 `supportsLocal`을 가질 때로, instantiation 직후 agent 자기 자신에 대한 connection이 호출된다. 어떤 agent로의 첫 전송 시점에 connection이 미리 맺어져 있지 않았다면, backend는 `prepXfer()`나 `postXfer()` 중 한 곳에서 connection을 맺어야 한다.

### Memory Management

* `registerMem()`: backend에 memory region을 등록한다. **단일 연속(contiguous) memory descriptor 하나**와 memory space의 종류만 전달된다.
* `deregisterMem()`: memory region 등록을 해제한다

각 backend는 registration마다 필요한 metadata를 저장하기 위해 `nixlBackendMD` base class를 상속한다. 이 class 객체에 대한 pointer가 `registerMem`의 출력이 되며, `deregisterMem`의 **유일한 입력**이 된다.

`FILE_SEG`를 지원하는 backend를 위해 NIXL은 공용 **path-mode** helper(`nixl::parsePathMeta()` + `nixlFilePathMD`)를 제공한다. 이를 통해 호출자는 미리 open된 fd 대신 `nixlBlobDesc::metaInfo`에 path를 담아 파일을 등록할 수 있다. [`src/utils/file/README.md`](../../src/utils/file/README.md#path-mode-file-registration)를 참고하라.

### Metadata Management

* `getPublicData()`: 등록된 memory에 대한 remote identifier를 serialize된 byte array로 제공한다
* `loadRemoteMD()`: remote agent로부터 받은 remote byte array를 로드한다
* `loadLocalMD()`: local memory metadata를 local metadata 객체로부터 직접 로드한다
* `unloadMD()`: remote identifier metadata 객체의 resource를 해제한다

registration과 마찬가지로, 각 backend는 `nixlBackendMD`를 상속하는 class를 만들어 byte array로부터 deserialize된 형태의 remote identifier를 저장할 수 있으며, 이는 `loadRemoteMD`로 이루어진다. local 전송의 경우 serialization/deserialization 부분이 생략되므로, `loadLocalMD`는 remote identifier에 해당하는 객체의 pointer를 생성하거나, 등록된 memory에 대해 **입력 pointer를 그대로 출력**할 수 있다.

`getPublicData`와 `loadRemoteMD`는 backend가 `supportsRemote`인 경우 필수이고, `loadLocalMD`는 `supportsLocal`인 경우 필수이며, `unloadMD`는 deserialize된 remote identifier 객체를 해제하기 위해 **모든 경우에 필수**다.

### Transfer Operations

* `prepXfer()`: 양쪽의 descriptor list, read 또는 write 연산, 그리고 remote agent 이름(지원된다면 자기 자신으로의 loopback도 가능)이 주어지면, 전송을 위한 준비 작업을 여기서 수행할 수 있다. transfer request의 상태를 저장하기 위해 backend가 상속할 base class인 `nixlBackendReqH`에 대한 pointer를 생성한다.
* `estimateXferCost`: `prepXfer`와 동일한 정보에 더해 `prepXfer`가 출력한 transfer request가 주어지면, backend는 전송 소요 시간을 noise margin 및 추정 방식과 함께 추정할 수 있다. **선택 사항**이다.
* `postXfer()`: transfer request를 post한다. 즉 backend가 전송을 시작해야 한다. 이 호출은 **비동기**이며, 전송이 끝날 때까지 기다려서는 안 된다. 전송이 아주 작다면 이 호출 직후 DONE을 반환해도 무방하다.
* `checkXfer()`: transfer request의 상태를 확인한다.
* `releaseReqH()`: transfer request handle을 해제한다. 이 handle은 `nixlBackendReqH`의 확장이어야 한다. NIXL agent는 여러 error 상황에서 이 handle을 해제할 수 있으며, 이 함수가 resource 해제뿐 아니라 request의 적절한 **cancellation**까지 처리하기를 기대한다는 점에 유의하라.

각 transfer request 안에는 descriptor list가 전달되는데, 이는 서로 다른 연속 memory 위치들 사이에 병렬화의 여지가 있는 경우를 위한 것이다(예: 여러 GPU에 걸친 경우 — 하나의 전송이 여러 GPU로 확장될 수 있다). 선택적으로 사용자는 notification을 요청할 수 있으며, 이는 **transfer request 내의 모든 descriptor가 전송된 후에** 보내져야 한다. backend가 `supportsNotifications`를 설정하지 않았다면 그런 notification은 요청되지 않는다.

**어떤 transfer request든 prep는 단 한 번만 이루어지지만, post는 여러 번 가능하다** — 단 다시 post되기 전에 DONE 상태에 도달해야 한다. transfer request들 사이에 **순서 보장은 없으며**, 특정 memory region에 대한 locking 메커니즘도 없다. 같은 위치에 두 개의 전송이 동시에 일어나 memory를 손상시키지 않도록 하는 것은 **사용자의 책임**이다.

마지막으로, `releaseXferReq` 호출은 block되지 않고 비동기여야 한다는 점에 유의하라. 특히 전송을 abort할 때 중요하다. 이 함수는 error를 반환할 수 있으며, 이는 request abort에 실패했다는 뜻이다. backend가 stall 없이 전송을 빠르게 abort할 수 있다면 `releaseXferReq`는 즉시 success를 반환해도 된다. 그렇지 않다면 한 가지 방법은 전송이 완료될 때까지 기다리는 것이다. 그 사이 `checkXferReq`는 성공하지 못한 abort 호출이 있었다면 error를 반환해야 하고, `releaseXferReq` 호출은 전송이 완료될 때까지 error를 반환하다가 완료 시 success를 반환한다. 만약 blocking 호출로 전송 완료 전에 abort할 수 있는 시나리오가 있다면, 그 blocking 호출을 별도 thread(또는 backend의 progress thread 내부)에서 시작할 수 있다. 다시 말해 **내부적으로는 blocking 호출을 쓰되 사용자에게는 non-blocking API를 제공하라.**

### Notification Handling

* `getNotifs()`: remote agent(loopback인 경우 local)로부터 받은 notification을 가져온다. 출력은 remote agent 이름 → notification 리스트(vector)의 map이며, notification은 byte array 형태다.
* `genNotif()`: remote agent에게 notification을 생성한다. 제어용 또는 dummy notification에 사용된다.

`getNotifs`는 어느 agent를 대상으로 notification을 찾아야 하는지 알지 못한다는 점에 유의하라. 따라서 수신된 notification으로부터 그에 해당하는 전송의 agent 이름을 추출하는 방법이 있어야 한다. `genNotif`는 어떤 전송에도 묶이지 않은 notification을 생성하며, 어떤 순서 보장도 제공하지 않는다. backend가 `supportsNotifications`를 설정하지 않았다면 이 두 method는 필요 없다.

## Descriptor List Abstraction

NIXL 라이브러리의 핵심 기반 추상화는 **descriptor list**로, 이는 memory space(host/GPU/block/File/Obj-Store)와 descriptor의 리스트로 구성된다. SB API에는 두 종류의 descriptor가 쓰인다.

* **전송용**: `(addr, len, devID, metadata)`. 여기서 metadata는 이 descriptor가 속하는 등록된 memory에 해당하는 `nixlBackendMD` 객체에 대한 pointer다.
* **registration용**: `(addr, len, devID, str)`. 여기서 str은 추가 정보를 위한 선택적 byte-array다. 아래 표는 memory space별 devID의 의미와, File 및 Object-Store에 대한 선택적 의미를 보여준다.

| mem type | addr | len | devID | str (byte-array) |
| --- | --- | --- | --- | --- |
| DRAM | | | 0 (또는 region) | - |
| VRAM | | | GPU ID | - |
| BLK | | | Vol ID | - |
| FILE | offset | 또는 0 | fd | Path + (access mode) |
| OBJ | offset | 또는 0 | key | Extended key (+ bucket ID) |

## Plugin Manager API

NIXL agent와 대화하는 사용자 관점에서는 backend transport의 종류(예: "UCX" 또는 "GDS")를 선택하면, NIXL이 알아서 적절한 plugin을 찾아 memory로 로드하고, SB API를 지원하는 그 backend engine의 인스턴스를 생성한다. NIXL 내부에는 다양한 plugin의 능동적 discovery, loading, unloading, 인스턴스 생성을 처리하는 **Plugin Manager**라는 주요 컴포넌트가 있다. NIXL 라이브러리가 로드되면 Plugin Manager는 NIXL plugin이 존재할 수 있는, 미리 알려졌거나 설정 가능한 디렉토리를 읽는다. 그런 다음 해당 라이브러리에서 동적으로 로드되는 symbol을 찾아 그 plugin이 NIXL API 집합을 준수하는지 검증한다. 이 요구사항이 충족되면 Plugin Manager는 애플리케이션이 도는 동안 그 plugin을 memory에 유지한다.

southbound API에 더해, plugin은 plugin manager를 위해 다음 method들을 구현해야 한다.

* `get_plugin_name`: backend plugin의 이름을 반환
* `get_plugin_version`: plugin의 현재 version을 반환
* `create_engine`: backend engine의 인스턴스를 반환
* `destroy_engine`: engine 인스턴스를 파괴
* `get_backend_mems`: 이 backend가 지원하는 memory type을 반환
* `get_backend_options`: plugin이 초기화 중에 사용할 수 있는 configuration option과 parameter를 반환. 사용자는 이 정보를 통해 **runtime에**, 그리고 plugin의 서로 다른 version에 걸쳐 그런 parameter들을 알 수 있다.

plugin manager는 위 API들의 API versioning을 관리한다. 이를 통해 NIXL은 훨씬 더 많은 plugin에 대해 하위/상위 호환성을 보장할 수 있다. 나아가 **static plugin과 dynamic plugin이 모두** 존재할 수 있는데, 각각 NIXL 라이브러리에 직접 자동 로드/내장되는 것과 필요 시 디스크에서 로드되는 것을 뜻한다. static plugin은 애플리케이션 크기가 커지는 대신 약간 더 나은 성능을 제공할 수 있다. **두 옵션의 API는 동일하다.**

## 두 plugin 비교 예시

NIXL UCX plugin은 서로 다른 node 간 networking을 제공하고, GDS plugin은 storage access를 제공한다. UCX plugin은 모든 "supports" flag를 설정하지만, GDS는 `supportsLocal` flag만 설정한다. 그 이유는 UCX가 agent 간 통신과 notification을 지원해야 하는 network plugin이며, 동시에 agent 내부 전송(예: GPU에서 CPU로)도 지원하기 때문이다.

그러나 **NIXL storage backend의 경우, remote storage node에서 NIXL agent를 돌릴 필요가 없다.** 대신 local agent 위의 분산 storage client가 remote 분산 storage와 통신하며, 따라서 NIXL agent 관점에서는 local이든 remote든 모든 storage에 대해 이 local storage client와 대화해야 한다. 다시 말해 **모든 전송은 agent 자기 자신으로의 loopback**이다. 현재 use case에서는 동일 agent 내부에서의 notification이 필요 없다.

또한 GDS plugin은 자기 자신으로의 local connection이 필요 없으므로 `connect`와 `disconnect`에 대해 SUCCESS를 반환하고, `loadLocal`은 단순히 입력 pointer를 그대로 출력으로 돌려준다. **구현해야 하는 나머지 method는 6개뿐이다:**

* `registerMem`
* `deregisterMem`
* `prepXfer`
* `postXfer`
* `checkXfer`
* `releaseReqH`

---

# NIXL Agent의 Plugin manager 및 SB API 사용

이 절에서는 사용자 관점에서 NIXL agent와의 상호작용을 개괄하고, 그것이 plugin manager 및 각 plugin이 제공하는 SB API에 어떻게 대응되는지 살펴본다.

### Create agent

이 단계에서 일부 설정이 backend plugin들 사이에 공유된다. 예를 들어 progress thread 사용이 허용되는지, 또는 어떤 주기로 호출되어야 하는지 등이다. 또한 이 단계에서 plugin manager가 호출되어 사용 가능한 plugin을 찾아둘 수 있으며, 그래야 사용자의 요청에 따라 인스턴스화할 수 있다.

### Create transfer backends

사용자가 특정 backend를 이름으로 요청하면서 key-value 쌍 형태의 parameter 리스트를 함께 넘기면, NIXL agent는 plugin manager를 통해 그 backend plugin의 인스턴스를 만들고 초기화 parameter와 함께 SB API의 **initializer**(constructor) method를 호출한다. 여기에는 agent 생성 시 설정된 parameter와 사용자가 이 method에 전달한 parameter가 모두 포함된다. 이러한 인스턴스화에서 error가 발생할 수 있으며, 실패는 사용자에게 보고된다.

이 단계에서, plugin이 remote agent와의 통신을 지원한다면 다른 agent들이 이 plugin과 대화하기 위해 필요한 connection 데이터를 SB API의 **`getConnInfo`** 로 획득한다. 그리고/또는 node 내부 전송을 지원한다면 자기 자신으로의 **connection** 호출이 이루어진다 — 일부 backend가 이를 요구하기 때문이다.

### Make connections (선택)

remote agent로의 connection이 요청되면(이는 그 remote agent의 metadata가 이미 로드된 경우 가능하다), local Agent는 자신과 remote agent 사이의 공통 backend plugin을 찾고, 각각에 대해 해당 backend의 SB API **`connect`** 를 사용해 connect를 개시한다.

### Register (Deregister) memory with NIXL

agent는 사용자로부터 할당된 memory 리스트와 원하는 backend를 받은 뒤, 지정된 backend에 **한 번에 하나의 원소씩만** 넘긴다. backend는 보통 전송 중 접근할 memory를 등록해야 하며, 그 registration을 근거로 해당 memory region에 대한 metadata를 보관한다. 예를 들어 UCX의 경우 연속된 memory region마다 그 region에 대한 local metadata를 생성한다.

Agent는 SB API의 **register** 호출에 단일 연속 memory region 하나만 넘기고, 그 대가로 backend가 이 memory region을 위해 만든 metadata에 대한 key(pointer)를 받는다. 이후 전송 시점에 agent가 그 key를 backend에 다시 돌려주므로, **backend는 그런 metadata에 대한 bookkeeping을 전혀 할 필요가 없다.**

backend가 agent 내부 전송을 지원한다면, 어떤 memory가 연산의 **target**인 경우와 연산이 **개시되는** 쪽인 경우에 서로 다른 metadata가 필요할 수 있다. 따라서 이 단계에서 등록된 memory마다 SB API의 **`loadLocalMD`** method가 호출되어 그런 metadata를 획득하고, backend는 새 metadata 객체에 대한 key(pointer)를 반환한다. 마찬가지로 agent가 bookkeeping을 하고 전송 시점에 적절한 metadata를 제공한다. 두 metadata 객체가 동일할 수도 있으며, 그 경우 두 key가 같은 metadata를 가리킨다.

SB API의 **deregister** 호출에서는 backend에 metadata 객체의 key와 함께 그 memory가 등록될 때 쓰인 원래 descriptor가 주어지므로, backend가 그 metadata를 해제할 수 있다.

### Get local agent metadata

앞서 언급했듯 이 API를 통해 agent별로 필요한 정보의 serialize된 형태를 얻을 수 있다. 이 정보에는 remote Agent를 지원하는 각 backend별 connection info가 포함되며, 이는 backend 생성 시점에 획득된 것이다. 그 위에, 각 backend에 등록된 memory마다 다른 agent의 대응 backend가 그 memory에 접근할 수 있도록 remote identifier를 제공할 수 있다.

예를 들어 Agent A의 GPU0이 UCX backend를 통해 Agent B의 GPU2로 데이터를 보내려 한다면, Agent A는 local GPU0의 데이터에 관련된 local metadata와 함께 Agent B의 GPU2에 있는 목적지에 대한 remote identifier가 있어야 전송을 수행할 수 있다. remote identifier 정보를 얻기 위해 agent는 각 backend의 등록된 memory마다 SB API의 **`getPublicData`** 를 호출해 각각의 serialize된 출력을 받는다. 마지막으로 agent는 backend 전반의 모든 connection 정보와 등록된 memory의 remote identifier를, 자신의 이름 같은 부가 metadata와 함께 serialize하여 완전한 metadata 객체를 serialize된 형식으로 만든다.

**`getConnInfo`** 도 여기서 사용될 수 있으나, backend가 생성 이후 connection info를 바꾸리라고 기대하지 않기 때문에 우리 구현에서는 backend 생성 시점에 수행했다.

### Load remote agent metadata

remote agent의 metadata를 serialize된 형식으로 받으면, agent는 정보를 파싱하고 deserialize한다. 각 backend의 connection info마다, 그 backend가 local에 존재한다면 remote agent에 있는 대응 backend의 connection info와 함께 그 backend의 SB API **`loadRemoteConnInfo`** 를 호출하여, 이후 local backend가 remote backend와 통신할 수 있게 한다.

나아가, local에 대응 backend가 존재하는 remote backend의 등록된 memory 조각마다, 그 등록된 memory 정보와 serialize된 remote identifier 정보가 SB API의 **`loadRemoteMD`** 를 통해 local backend engine에 주어지며, 이는 metadata 객체를 생성하고 그 key(pointer)를 반환한다 — local 전송에서 register memory 설명에 나온 target metadata 객체와 유사하다. 마찬가지로 전송 시점에 agent가 적절한 metadata 객체 key를 제공한다.

이 단계 이후, 두 agent 사이에 공통 backend가 하나라도 있었다면 두 agent 간 전송이 가능해진다.

### Invalidate remote agent metadata

Agent는 해당 agent의 remote metadata를 로드하던 시점에 어떤 backend가 공통이었는지 판단하고, 그런 backend들에 대해 그 agent를 향해 SB API의 **`disconnect`** 를 호출한다. 또한 대응하는 공통 backend에 대해 그 remote agent용으로 저장된 remote identifier마다 SB API의 **`unloadMD`** 를 호출해 resource를 해제한다.

이 단계 이후 local agent는 metadata가 다시 로드되기 전까지 그 remote agent로의 전송을 개시할 수 없다. 이 두 method를 통해 NIXL agent의 **dynamicity** 요구사항이 달성된다.

한계적(marginal) 업데이트에 관해서는, agent 내부의 약간의 추가 bookkeeping만 필요하다. 그리고 중앙 KV service 모드는 agent와 중앙 metadata server 사이의 인터페이스일 뿐, SB API가 사용되는 방식을 바꾸지 않는다.

### Create transfer request

이 API는 agent 쪽 준비 작업을 수행하며 **backend SB API를 호출하지 않는다.** 다만 어느 backend를 선택할지 결정한다(사용자가 선택적으로 지정하지 않은 경우). backend가 지정되지 않았다면 agent는 request 양쪽의 memory type, 양쪽에서 사용 가능한 backend engine, 그리고 그 특정 memory type에 대해 각 backend에 등록된 memory 범위를 살펴본다. 보통 이 모든 요소를 고려하면 단 하나의 backend만이 그 transfer request를 수행할 수 있으며, 그렇지 않은 경우에는 첫 번째로 일치하는 것을 선택하거나 preference list를 사용한다.

최적 backend를 찾는 것 외에도 이 API는 여러 검사를 수행한다. 예컨대 request의 크기가 적절한지, 선택적으로 전달된 backend에서 그 memory region이 사용 가능한지 등이다. 그런 다음 양쪽 리스트의 각 descriptor에 backend로부터 받은 관련 metadata 객체 key를 채워 넣는데, 이 key는 그 원소가 전송의 initiator 쪽인지 target 쪽인지에 따라 달라질 수 있으며 이는 agent 내부 전송에서도 마찬가지다. 모든 검사와 준비가 끝나면 사용자에게 handle이 반환되며, 여기에는 backend engine이 전송을 수행하는 데 필요한 모든 정보가 담겨 있다. **이 단계에서 전송은 아직 개시되지 않는다.**

### Post transfer request

post transfer는 준비된 transfer handle에 대해 호출된다. 이미 transfer backend가 결정되어 있고 필요한 모든 metadata key(pointer)가 request에 붙어 있다. 따라서 이 함수의 주 목적은 해당 backend에 대해 SB API의 **`postXferReq`** 를 한 번 호출하는 것이다. 이 호출의 결과로 backend engine 고유의 transfer handle이 생기며, 이후 agent가 이를 사용해 전송 상태를 물을 수 있다.

이 method는 전송이 끝난 뒤 **같은 agent transfer handle에 대해 다시 호출될 수 있으며**, 새 전송이 시작될 때마다 새 transfer handle을 만들지 기존 것을 재사용할지는 backend에 달려 있다. 또한 진행 중인 전송이 있는데 그에 대해 post가 제출되면 **error를 유발하고 잠재적으로 전송을 abort**시킨다.

전송 내부에서 backend는 network resiliency나 최적화를 위한 방법을 제공할 수 있다. 예컨대 단일 전송의 동일 descriptor list 내에서, 또는 서로 다른 전송들 사이에서 부하를 분산하는 것 등이다. 이는 backend plugin이 처리하고 구현한다.

### Get transfer status

agent는 agent transfer handle 안에 저장된 backend 고유의 transfer handle을 호출하여 전송 상태를 확인한다. 이는 SB API의 **`checkXfer`** 호출로 이루어진다. backend 내부적으로는 전송의 최신 상태를 얻기 위해 필요하다면 자신의 내부 progress method를 호출할 수 있다.

### Invalidate transfer request

agent는 backend 고유의 transfer handle에 대해 SB API의 **`releaseReqH`** 를 호출해 이를 해제하고, 진행 중이며 backend에 그런 능력이 있다면 전송을 abort할 수도 있다. 그런 다음 agent는 agent 레벨 transfer handle 내의 나머지 resource를 해제해 완전히 반환한다.

### Get notifications

agent는 notification을 지원하는 모든 backend를 순회하며 SB API의 **`getNotifs`** 를 호출하고, 이는 이 method의 이전 호출 시점부터 지금까지 각 remote node로부터 받은 notification 리스트를 반환한다. 그런 다음 agent는 그런 모든 backend의 결과를 병합하여 사용자가 제공한 map에 덧붙인다. get transfer status와 마찬가지로, backend 내부적으로는 다른 agent들이 자신을 향해 개시한 전송으로부터 받은 최신 notification을 얻기 위해 필요하다면 자신의 내부 progress method를 호출할 수 있다.

### Generate notification

사용자가 backend를 지정했다면 agent는 그 backend engine의 SB API **`genNotif`** 를 호출한다. 그렇지 않으면 local과 remote 양쪽에서 사용 가능하면서 notification도 지원하는 backend를 찾는다. 후보가 둘 이상이면 첫 번째를 선택하거나 preference list를 사용한다.

### Destructor

애플리케이션 종료 시 agent가 파괴될 때, agent는 **application**이 deregister하지 않고 남긴 모든 memory를 deregister한다(나쁜 관행이지만 agent가 처리해 준다). 그런 다음 각 backend에 대해 SB API의 **destructor** 를 호출하고, 마지막으로 나머지 내부 정리를 수행한다.

---

## 읽을 때 표시해 둘 세 곳

PR #1717 코드를 읽는 동안 계속 되돌아오게 되는 지점들이다.

| 절 | 문장 | 코드에서 왜 필요한가 |
|---|---|---|
| **Transfer Operations** | *"prep는 단 한 번만, post는 여러 번"* | `postXfer()`가 매번 `totalTasks_`·`submittedTasks_`·`completedTasks_`를 리셋하는 이유 (`doca_memos_progress_engine.cpp:408`) |
| **Descriptor List Abstraction** | OBJ 행: `addr = offset`, `devID = key` | PR #1717이 remote descriptor의 `addr`과 `len`을 **읽지 않는** 지점. 값 길이는 local 쪽에서만 온다 |
| **두 plugin 비교** | 6개 method 목록 | DOCA_MEMOS의 `connect()`/`loadLocalMD()`가 한 줄짜리인 것이 게으름이 아니라 **문서화된 storage backend 패턴**임을 보증 |

## 원문과 헤더의 차이

이 가이드는 산문 해설이고, 실제 계약은 `src/api/cpp/backend/backend_engine.h`의 virtual 29개다.
가이드가 헤더보다 뒤처진 부분이 있다:

- `queryMem()` — 헤더의 **optional** 섹션에 있으나 가이드에 언급 없음. DOCA_MEMOS가 구현하는 유일한 선택적 method다
- `prepMemView()` / `releaseMemView()` — 헤더에 있으나 가이드에 없음. DOCA_MEMOS는 기본 구현(`NIXL_ERR_NOT_SUPPORTED`)을 그대로 둔다

---

관련 문서: [study_guide.md](./study_guide.md) · [analysis/README.md](./analysis/README.md) · [analysis/request-flow.md](./analysis/request-flow.md)
