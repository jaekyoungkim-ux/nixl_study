/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// ============================================================================
// 스터디용 주석본. 원본: PR #1717 src/plugins/doca_memos/doca_memos_backend.h
// 코드는 원본 그대로이며, "▶" 로 시작하는 줄만 추가된 한글 주석이다.
// 원본 행 번호와 어긋나므로, 원본을 인용할 때는 원본 파일을 볼 것.
// ============================================================================

#ifndef NIXL_SRC_PLUGINS_DOCA_MEMOS_DOCA_MEMOS_BACKEND_H
#define NIXL_SRC_PLUGINS_DOCA_MEMOS_DOCA_MEMOS_BACKEND_H

#include <ostream>
#include <string>
#include <atomic>
#include <vector>
#include <memory>
#include <sys/uio.h> // for struct iovec
// ▶ iovec 은 POSIX 표준 구조체다. DOCA 가 만든 게 아니라 리눅스 readv/writev 의 그것.
//   { void *iov_base; size_t iov_len; } — 유저 공간 가상 주소를 담는다.
#include "backend/backend_engine.h"
#include "doca_memos_progress_engine.h"

// ▶▶ 여기가 "우리 몫의 경계선"이다.
//    아래 두 타입은 이름만 선언하고 정의는 DOCA SDK 안에 있다.
//    즉 이 플러그인은 DOCA 자료구조의 내부를 들여다보지 않고 포인터로만 다룬다.
// Forward declarations for DOCA types
struct doca_kvdev;
struct doca_nvme_kernel_kvdev;

// Static cap on object-key size. DOCA does not expose a compile-time maximum,
// so this is set to the current spec value (128-bit) and verified at runtime
// via doca_kvdev_get_max_key_len() during engine init.
// ▶ 16바이트 = 128비트. NVMe KV Command Set 의 최대 키 길이와 일치하고,
//   GTC 발표의 "128-bit keys" 와도 정확히 맞는다.
//   컴파일 타임 상수라 런타임에 장치가 더 큰 값을 보고하면 init 에서 거부한다.
inline constexpr size_t DOCA_MEMOS_MAX_OBJECT_KEY_LEN = 16;

// Forward declaration for backend init params
class nixlBackendInitParams;

// Default-constructed instances have keyLen = 0, which DOCA will reject at
// submit time. Callers must populate via nixlDocaMemosEngine::resolveMemosKey()
// (or convertToMemosKey()) before handing the key to any DOCA API.
// ▶▶ 지금까지 "16바이트 상자" 라고 부른 것의 실체.
//    registerMem() 이 이걸 만들어 metadata 객체 안에 넣고, prepXfer() 가 꺼내 쓴다.
//    기본 생성 시 keyLen=0 이라 그대로 쓰면 DOCA 가 거부한다 (안전장치).
struct docaMemosKey {
    uint8_t key[DOCA_MEMOS_MAX_OBJECT_KEY_LEN] = {};
    uint16_t keyLen = 0;
};

std::ostream &
operator<<(std::ostream &os, const docaMemosKey &k);

// Per-task scratch passed to DOCA via task_user_data and handed back to the
// completion / error callbacks. Lifetime invariants:
//   - reqH is a non-owning pointer; the engine guarantees the request handle
//     outlives every task it submitted (see nixlDocaMemosBackendReqH and the
//     destructors in nixlNoThreadProgressEngine / nixlThreadedProgressEngine).
//   - Instances live inside reqH->taskContexts_, so the address published to
//     DOCA stays valid as long as that vector is not resized after submit.
// ▶▶ DOCA task 한 장마다 붙는 표식.
//    task 를 제출할 때 이 구조체의 주소를 DOCA 에 함께 넘기고(task_user_data),
//    나중에 완료 콜백이 그 주소를 그대로 돌려준다. "이 task 가 어느 요청 소속인가" 를
//    콜백이 알아내는 유일한 수단이다.
// ▶ 주의: 이 인스턴스들은 reqH->taskContexts_ 벡터 안에 산다.
//   제출 후 그 벡터를 resize 하면 DOCA 에 넘긴 주소가 무효화된다.
//   주석에 불변식으로 적혀 있을 뿐 컴파일러가 강제하지 못하는 위험 지점.
struct docaMemosTaskContext {
    class nixlDocaMemosBackendReqH *reqH;
    int taskIndex;
    bool isRetrieve = false;
    size_t expectedValueLen = 0;
};

// ▶▶▶ 전송 핸들. prepXfer() 가 만들어 NIXL 코어에 넘기고,
//     postXfer / checkXfer / releaseReqH 가 이걸 받아 쓴다.
//     핵심 역할: DOCA task N 장의 상태를 모아 NIXL 이 묻는 1 개의 답으로 접는 것.
class nixlDocaMemosBackendReqH : public nixlBackendReqH {
public:
    explicit nixlDocaMemosBackendReqH(int num_tasks) : totalTasks_(num_tasks) {}

    ~nixlDocaMemosBackendReqH() = default;

    // Move construction is needed so this type satisfies MoveInsertable for
    // std::vector (callers reserve() then emplace_back()). Move assignment is
    // deleted because reassigning an in-flight request handle is never valid.
    // ▶ 이동 생성만 허용. queryMem(actual) 이 핸들을 vector 에 담기 때문에 필요하다.
    //   진행 중인 핸들을 덮어쓰는 것은 언제나 버그이므로 이동 대입은 삭제.
    nixlDocaMemosBackendReqH(nixlDocaMemosBackendReqH &&other) noexcept
        : totalTasks_(other.totalTasks_),
          submittedTasks_(other.submittedTasks_),
          completedTasks_(other.completedTasks_),
          allTasksCompleted_(other.allTasksCompleted_.load(std::memory_order_relaxed)),
          cancelled_(other.cancelled_.load(std::memory_order_relaxed)),
          overallStatus_(other.overallStatus_),
          taskResult_(other.taskResult_.load(std::memory_order_relaxed)),
          isExistQuery_(other.isExistQuery_),
          ignoreNotFound_(other.ignoreNotFound_),
          nextDescriptorIndex_(other.nextDescriptorIndex_),
          isPending_(other.isPending_),
          storedOperation_(other.storedOperation_),
          storedLocal_(std::move(other.storedLocal_)),
          storedRemote_(std::move(other.storedRemote_)),
          valueIovecs_(std::move(other.valueIovecs_)),
          objectKeys_(std::move(other.objectKeys_)),
          taskContexts_(std::move(other.taskContexts_)) {}

    nixlDocaMemosBackendReqH(const nixlDocaMemosBackendReqH &) = delete;
    nixlDocaMemosBackendReqH &
    operator=(const nixlDocaMemosBackendReqH &) = delete;
    nixlDocaMemosBackendReqH &
    operator=(nixlDocaMemosBackendReqH &&) = delete;

    // ▶ checkXfer() 가 이 두 함수만 본다. 락 없이 원자적으로 읽을 수 있어서
    //   폴링이 엔진 mutex 를 건드리지 않는다.
    [[nodiscard]] bool
    allTasksCompleted() const noexcept {
        return allTasksCompleted_.load(std::memory_order_acquire);
    }

    [[nodiscard]] nixl_status_t
    getOverallStatus() const noexcept {
        return overallStatus_;
    }

private:
    // Engines and their static helpers are the sole owners of the bookkeeping
    // below. Keeping these private prevents accidental external mutation while
    // letting the engines manipulate request state directly.
    // ▶ 아래 상태를 만질 수 있는 것은 엔진들뿐. friend 로 열어두고 외부에는 닫아둔다.
    friend class nixlDocaMemosEngine;
    friend class nixlDocaMemosProgressEngine;
    friend class nixlNoThreadProgressEngine;
    friend class nixlThreadedProgressEngine;

    // ▶▶ 카운터 3인방. "N 장을 1 개로 접는" 장치의 핵심.
    //    completedTasks_ >= totalTasks_ 가 되면 allTasksCompleted_ 를 세운다.
    int totalTasks_ = 0;      // ▶ 이 요청이 만들 task 총 개수 (= descriptor 쌍 개수)
    int submittedTasks_ = 0;  // ▶ 실제로 DOCA 에 제출된 수
    int completedTasks_ = 0;  // ▶ 콜백이 돌아온 수
    std::atomic<bool> allTasksCompleted_{false};
    std::atomic<bool> cancelled_{false};   // ▶ releaseReqH 가 세움. 콜백이 보고 뒷정리 건너뜀
    nixl_status_t overallStatus_ = NIXL_IN_PROG;
    std::atomic<int> taskResult_{0};       // ▶ EXIST 질의 전용 결과 칸
    bool isExistQuery_ = false;            // ▶ 전송이 아니라 queryMem(actual) 용 핸들인가
    bool ignoreNotFound_ = false;          // ▶ ignore_read_not_found 설정의 사본

    // ▶ task pool 이 가득 차 중간에 끊겼을 때, 어디부터 다시 제출할지 기억하는 커서.
    int nextDescriptorIndex_ = 0;
    bool isPending_ = false;               // ▶ 재시도 큐에 들어가 있는가

    // ▶ 재시도하려면 원래 인자를 다시 봐야 하므로 dlist 를 통째로 복사해 둔다.
    nixl_xfer_op_t storedOperation_ = NIXL_WRITE;
    std::unique_ptr<nixl_meta_dlist_t> storedLocal_;
    std::unique_ptr<nixl_meta_dlist_t> storedRemote_;

    // ▶▶ prepXfer() 가 채우는 세 벌의 배열. 인덱스 i 로 서로 짝을 이룬다.
    //    valueIovecs_[i] ← local[i] 에서 뽑은 (주소, 길이)
    //    objectKeys_[i]  ← remote[i] 의 metadata 에서 꺼낸 16바이트 키
    //    taskContexts_[i] ← 그 둘로 만들 task 에 붙일 표식
    std::vector<struct iovec> valueIovecs_;
    std::vector<docaMemosKey> objectKeys_;
    std::vector<docaMemosTaskContext> taskContexts_;
};

// ▶▶▶ 백엔드 엔진 본체. nixlBackendEngine(SB API) 을 구현한다.
//     virtual 26 개 중 여기 나타나는 것만 구현했고, 나머지는 기본값을 쓴다.
class nixlDocaMemosEngine : public nixlBackendEngine {
public:
    nixlDocaMemosEngine(const nixlBackendInitParams *init_params);
    ~nixlDocaMemosEngine() override;

    // ▶▶ Capability Indicators — "나는 이만큼만 한다" 는 자기 신고.
    //    이 네 개의 답이 나머지 구현 의무를 결정한다.

    // ▶ false → getConnInfo / loadRemoteConnInfo / getPublicData / loadRemoteMD 면제.
    //   주의: "원격 스토리지에 못 간다" 가 아니라 "대화할 원격 NIXL agent 가 없다" 는 뜻.
    //   CMX 박스에는 agent 가 없으므로 false 가 맞다.
    bool
    supportsRemote() const override {
        return false;
    }

    // ▶ true → loadLocalMD 구현 의무. 모든 전송이 자기 자신으로의 loopback 이다.
    bool
    supportsLocal() const override {
        return true;
    }

    // ▶ false → getNotifs / genNotif 면제. 알려줄 상대가 없다.
    bool
    supportsNotif() const override {
        return false;
    }

    // ▶▶ agent 가 라우팅에 쓰는 목록. DRAM↔OBJ 요청만 이 백엔드로 온다.
    //    VRAM_SEG 가 없다 = GPU 메모리 직행 불가. 반드시 host DDR 을 경유한다.
    nixl_mem_list_t
    getSupportedMems() const override {
        return {OBJ_SEG, DRAM_SEG};
    }

    // ▶ OBJ_SEG 면 키를 확정해 metadata 객체를 만들고, DRAM_SEG 면 nullptr 을 낸다.
    //   DOCA 호출이 한 번도 없다 — 그래서 자주 불려도 싸다.
    nixl_status_t
    registerMem(const nixlBlobDesc &mem, const nixl_mem_t &nixl_mem, nixlBackendMD *&out) override;

    nixl_status_t
    deregisterMem(nixlBackendMD *meta) override;

    // ▶ 유일하게 자발적으로 구현한 optional method.
    //   query_mem_mode 에 따라 즉시 성공을 내거나 진짜 EXIST 를 발행한다.
    nixl_status_t
    queryMem(const nixl_reg_dlist_t &descs, std::vector<nixl_query_resp_t> &resp) const override;

    // ▶▶ 아래 셋은 "면제받지 못한 자리를 최소로 메운" 것들.
    //    가이드가 GDS 예로 설명한 storage backend 표준 패턴 그대로다.
    nixl_status_t
    connect(const std::string &remote_agent) override {
        return NIXL_SUCCESS;   // ▶ 자기 자신에게 연결할 것이 없다
    }

    nixl_status_t
    disconnect(const std::string &remote_agent) override {
        return NIXL_SUCCESS;
    }

    nixl_status_t
    unloadMD(nixlBackendMD *input) override {
        return NIXL_SUCCESS;   // ▶ loadLocalMD 가 같은 포인터를 냈으므로 여기서 지우면 double free
    }

    // ▶▶ 전송 4형제. 생애: prep(준비) → post(시작) → check(반복 확인) → release(정리)
    nixl_status_t
    prepXfer(const nixl_xfer_op_t &operation,
             const nixl_meta_dlist_t &local,
             const nixl_meta_dlist_t &remote,
             const std::string &remote_agent,
             nixlBackendReqH *&handle,
             const nixl_opt_b_args_t *opt_args = nullptr) const override;

    nixl_status_t
    postXfer(const nixl_xfer_op_t &operation,
             const nixl_meta_dlist_t &local,
             const nixl_meta_dlist_t &remote,
             const std::string &remote_agent,
             nixlBackendReqH *&handle,
             const nixl_opt_b_args_t *opt_args = nullptr) const override;

    nixl_status_t
    checkXfer(nixlBackendReqH *handle) const override;

    nixl_status_t
    releaseReqH(nixlBackendReqH *handle) const override;

    // ▶ 입력 포인터를 그대로 출력. initiator/target 신원을 구별할 이유가 없어서다.
    //   agent 는 두 포인터가 같으면 unloadMD 를 아예 호출하지 않는다.
    nixl_status_t
    loadLocalMD(nixlBackendMD *input, nixlBackendMD *&output) override {
        output = input;
        return NIXL_SUCCESS;
    }

    // Stateless helpers used by both the engine and the progress engine to turn
    // a registration's metaInfo blob into a docaMemosKey. Exposed as static
    // methods (rather than free functions) to keep them out of the plugin's
    // exported global namespace.
    // ▶▶ 키 해석기. progress engine 도 queryMem 에서 써야 해서 static 으로 뺐다.
    static bool
    convertToMemosKey(const std::string &meta_info, docaMemosKey &key);
    static bool
    resolveMemosKey(uint64_t dev_id, const std::string &meta_info, docaMemosKey &key);

private:
    // Custom deleter for nvmeKvdev_: stops the generic kvdev view, then
    // destroys the NVMe kernel handle that owns it. kvdev_ is a non-owning
    // alias derived from nvmeKvdev_ and is cleared before nvmeKvdev_.reset().
    // ▶ RAII. started 여부를 확인한 뒤 stop → destroy 한다.
    //   init 중간에 실패해 시작조차 못 한 장치에 stop() 을 부르면 안 되기 때문.
    struct NvmeKvdevDeleter {
        void
        operator()(doca_nvme_kernel_kvdev *dev) const noexcept;
    };

    // ▶ kvdev_ 는 nvmeKvdev_ 를 다른 API 로 본 것일 뿐 소유하지 않는다(별칭).
    //   그래서 정리할 때 kvdev_ = nullptr 로 지우고 nvmeKvdev_ 만 reset 한다.
    doca_kvdev *kvdev_ = nullptr;
    std::unique_ptr<doca_nvme_kernel_kvdev, NvmeKvdevDeleter> nvmeKvdev_;
    std::unique_ptr<nixlDocaMemosProgressEngine> progressEngine_;

    static constexpr const char *kDefaultNguid = "00000000000000000000000000000000";
    static constexpr uint32_t kDefaultNumTasks = 8192;

    // ▶▶ 설정 파라미터 5개가 여기 저장된다. parseInitParams() 가 채운다.
    std::string deviceName_;              // ▶ "/dev/nvme0n1" — 필수
    uint32_t numTasks_ = kDefaultNumTasks;// ▶ task pool 크기. 장치 한계로 clamp 됨
    uint32_t maxValueLen_ = 0;            // ▶ 장치가 보고한 최대 값 길이. 초과 시 제출 거부
    std::string nguid_ = kDefaultNguid;   // ▶ 32자 hex → 16바이트 namespace 식별자
    bool queryMemAssumeSuccess_ = true;   // ▶ 기본값 true = 장치에 묻지 않고 성공 반환
    bool ignoreReadNotFound_ = false;

    // ▶ constructor 가 이 순서로 부른다: parseInitParams → initDocaDevice → createProgressEngine
    nixl_status_t
    parseInitParams(const nixl_b_params_t *params);

    nixl_status_t
    initDocaDevice();

    nixl_status_t
    createProgressEngine(const nixlBackendInitParams *init_params);

    void
    cleanupDocaResources();
};

#endif // NIXL_SRC_PLUGINS_DOCA_MEMOS_DOCA_MEMOS_BACKEND_H
