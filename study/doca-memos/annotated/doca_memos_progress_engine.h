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


// ==========================================================================
// ì¤í°ëì© ì£¼ìë³¸. ìë³¸: PR #1717 src/plugins/doca_memos/doca_memos_progress_engine.h
// ì½ëë ìë³¸ ê·¸ëë¡ì´ë©°, "▶" ë¡ ììíë ì¤ë§ ì¶ê°ë íê¸ ì£¼ìì´ë¤.
// ìë³¸ í ë²í¸ì ì´ê¸ëë¯ë¡, ìë³¸ì ì¸ì©í  ëë ìë³¸ íì¼ì ë³¼ ê².
//
// 이 파일은 선언만 있다. 두 엔진의 설계 의도가 클래스 주석에 상세히 적혀 있어
// .cpp 를 읽기 전에 먼저 훑는 것이 좋다.
// ==========================================================================
#ifndef NIXL_SRC_PLUGINS_DOCA_MEMOS_DOCA_MEMOS_PROGRESS_ENGINE_H
#define NIXL_SRC_PLUGINS_DOCA_MEMOS_DOCA_MEMOS_PROGRESS_ENGINE_H

#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include "nixl_types.h"
#include "backend/backend_aux.h" // For nixl_meta_dlist_t

// Forward declaration suffices for the static callback declarations below
// (function declarations only need an incomplete type for value parameters).
// The full definition is pulled in by the .cpp via the real DOCA headers (or
// by the mock headers in unit tests).
union doca_data;

// Forward declarations for DOCA types
struct doca_pe;
struct doca_kvdev;
struct doca_kvdev_io;
struct doca_nvme_kernel_kvdev;
struct doca_nvme_kernel_kvdev_io;
struct doca_ctx;
struct doca_task;
// Forward declaration for request handle
class nixlDocaMemosBackendReqH;

/**
 * @brief Base class for DOCA KV progress engine management.
 *
 * Owns the DOCA progress-engine, KV-IO context, and the shared submission
 * helpers (trySubmitRequest, trySubmitExistTask, collectQueryResults).
 *
 * pendingRequests_ is the only piece of mutable state that lives in the base
 * because both subclasses share the helpers that consult / mutate it.
 * Synchronisation around it is the subclass's responsibility:
 *   - nixlNoThreadProgressEngine guards every access with its own lock_.
 *   - nixlThreadedProgressEngine touches it only from the progress thread.
 *
 * Per-subclass state (cancellation queues, mutexes, double-buffers, etc.)
 * lives in the subclasses, not here.
 */

// ▶▶▶ 베이스 클래스. 두 엔진이 공유하는 것을 담는다.
//     · DOCA 자원 4개 (pe_, kkvIo_, kvIo_, ctx_)
//     · 제출 헬퍼 (trySubmitRequest / trySubmitExistTask)
//     · 완료·에러 콜백 (static)
//     · pendingRequests_ — task pool 이 가득 차 밀린 요청들의 재시도 큐
//
//     동기화는 서브클래스 책임이다. 무스레드형은 lock_ 으로 감싸고,
//     스레드형은 progress thread 에서만 건드려 락 자체를 없앤다.
class nixlDocaMemosProgressEngine {
public:
    virtual ~nixlDocaMemosProgressEngine() = default;

    bool
    hasInitError() const {
        return initErr_;
    }

    virtual nixl_status_t
    checkXfer(nixlDocaMemosBackendReqH *req_h) const;
    virtual nixl_status_t
    postXfer(nixlDocaMemosBackendReqH *req_h,
             const nixl_xfer_op_t &operation,
             const nixl_meta_dlist_t &local,
             const nixl_meta_dlist_t &remote) const = 0;
    virtual void
    cancelRequest(nixlDocaMemosBackendReqH *req_h) const = 0;
    virtual nixl_status_t
    queryMem(const nixl_reg_dlist_t &descs, std::vector<nixl_query_resp_t> &resp) const = 0;

protected:
    nixlDocaMemosProgressEngine(struct doca_nvme_kernel_kvdev *nvme_kvdev,
                                uint32_t num_tasks,
                                uint32_t max_value_len);
    void
    cleanupDocaResources();
    bool
    trySubmitRequest(nixlDocaMemosBackendReqH *req_h,
                     const nixl_xfer_op_t &operation,
                     const nixl_meta_dlist_t &local,
                     const nixl_meta_dlist_t &remote) const;
    bool
    trySubmitExistTask(nixlDocaMemosBackendReqH *req_h) const;
    void
    prepareQueryHandles(const nixl_reg_dlist_t &descs,
                        std::vector<nixlDocaMemosBackendReqH> &query_handles) const;
    void
    waitForQueryCompletion(const std::vector<nixlDocaMemosBackendReqH> &query_handles,
                           const std::function<void()> &poll) const;
    size_t
    collectQueryResults(const std::vector<nixlDocaMemosBackendReqH> &query_handles,
                        std::vector<nixl_query_resp_t> &resp) const;

    // DOCA task callbacks and submission-failure helper. They live as static
    // members so they can touch nixlDocaMemosBackendReqH's private bookkeeping
    // through the friendship granted to nixlDocaMemosProgressEngine.
    static void
    taskCompletionCallback(struct doca_task *task,
                           union doca_data task_user_data,
                           union doca_data ctx_user_data);
    static void
    taskErrorCallback(struct doca_task *task,
                      union doca_data task_user_data,
                      union doca_data ctx_user_data);
    static void
    handleSubmissionFailure(nixlDocaMemosBackendReqH *req_h, nixl_status_t status);

    struct doca_pe *pe_ = nullptr;
    struct doca_nvme_kernel_kvdev_io *kkvIo_ = nullptr;
    struct doca_kvdev_io *kvIo_ = nullptr;
    struct doca_ctx *ctx_ = nullptr;

    // Synchronisation owned by the subclass; see class doc.
    // ▶ task pool 고갈로 중간에 끊긴 요청들. 앞에서부터 순서대로 재시도(FIFO).
    mutable std::deque<nixlDocaMemosBackendReqH *> pendingRequests_;
    bool initErr_ = false;
    uint32_t maxValueLen_ = 0;
};

/**
 * @brief Synchronous progress engine. The caller's thread drives the DOCA PE
 * via checkXfer()/queryMem(). All shared state is serialised by lock_.
 */

// ▶▶▶ 무스레드형. 호출자의 스레드가 checkXfer()/queryMem() 안에서 직접
//     doca_pe_progress() 를 돌린다. mutex 하나가 제출·폴링·취소를 전부 직렬화한다.
//     즉 이 모드에서는 checkXfer 를 부르지 않으면 전송이 진행되지 않는다.
class nixlNoThreadProgressEngine : public nixlDocaMemosProgressEngine {
public:
    nixlNoThreadProgressEngine(struct doca_nvme_kernel_kvdev *nvme_kvdev,
                               uint32_t num_tasks,
                               uint32_t max_value_len);
    ~nixlNoThreadProgressEngine() override;

    nixl_status_t
    postXfer(nixlDocaMemosBackendReqH *req_h,
             const nixl_xfer_op_t &operation,
             const nixl_meta_dlist_t &local,
             const nixl_meta_dlist_t &remote) const override;
    void
    cancelRequest(nixlDocaMemosBackendReqH *req_h) const override;
    nixl_status_t
    checkXfer(nixlDocaMemosBackendReqH *req_h) const override;
    nixl_status_t
    queryMem(const nixl_reg_dlist_t &descs, std::vector<nixl_query_resp_t> &resp) const override;

private:
    int
    progress() const;
    void
    tryResumePendingRequests() const;

    mutable std::vector<nixlDocaMemosBackendReqH *> cancelledRequests_;
    mutable std::mutex lock_;
};

/**
 * @brief Threaded progress engine — lock-free hot path.
 *
 * The progress thread is the sole caller of all DOCA APIs (doca_pe_progress,
 * doca_kvdev_io_task_*_alloc_init, doca_task_submit, etc.).
 *
 * Producers (postXfer, cancelRequest) append to *producerVec_ and to
 * cancelledRequests_ under queueMutex_ and set hasNewWork_. The progress thread,
 * when it observes hasNewWork_, briefly takes queueMutex_ to swap producerVec_
 * between vecA_ and vecB_ and to swap-out cancelledRequests_, then drains the
 * inactive vector outside the lock. This keeps producers' critical sections
 * to a single push + pointer swap, regardless of how much work the consumer
 * has queued up.
 */

// ▶▶▶ 스레드형. progress thread 가 DOCA API 의 유일한 호출자다.
//     생산자(postXfer/cancelRequest)는 큐에 넣기만 하고 즉시 돌아온다.
//
//     더블버퍼가 핵심이다 — 생산자는 producerVec_ 에 push 하고,
//     소비자는 락을 잠깐 잡아 producerVec_ 를 vecA_↔vecB_ 로 스왑한 뒤
//     락 밖에서 비운다. 생산자의 임계구역이 "push 1회 + 포인터 스왑" 으로 고정된다.
class nixlThreadedProgressEngine : public nixlDocaMemosProgressEngine {
public:
    nixlThreadedProgressEngine(struct doca_nvme_kernel_kvdev *nvme_kvdev,
                               uint32_t num_tasks,
                               uint32_t max_value_len,
                               std::chrono::microseconds thread_delay);
    ~nixlThreadedProgressEngine() override;

    nixl_status_t
    postXfer(nixlDocaMemosBackendReqH *req_h,
             const nixl_xfer_op_t &operation,
             const nixl_meta_dlist_t &local,
             const nixl_meta_dlist_t &remote) const override;
    void
    cancelRequest(nixlDocaMemosBackendReqH *req_h) const override;
    nixl_status_t
    queryMem(const nixl_reg_dlist_t &descs, std::vector<nixl_query_resp_t> &resp) const override;

private:
    // ▶ 큐에 실리는 단위. dlist 를 통째로 복사해 두는 이유는
    //   실제 제출이 나중에(progress thread 에서) 일어나기 때문이다.
    struct pendingEntry {
        nixlDocaMemosBackendReqH *reqH;
        nixl_xfer_op_t operation;
        std::unique_ptr<nixl_meta_dlist_t> local;
        std::unique_ptr<nixl_meta_dlist_t> remote;
    };

    void
    progressThreadFunc();
    void
    processCancellations(std::vector<nixlDocaMemosBackendReqH *> &cancels) const;

    std::chrono::microseconds threadDelay_;
    std::atomic<bool> threadStop_{false};
    std::thread progressThread_;

    mutable std::mutex queueMutex_;
    mutable std::atomic<bool> hasNewWork_{false};
    mutable std::vector<pendingEntry> vecA_;
    mutable std::vector<pendingEntry> vecB_;
    mutable std::vector<pendingEntry> *producerVec_ = &vecA_;
    mutable std::vector<nixlDocaMemosBackendReqH *> cancelledRequests_;
    // ▶ 취소됐으나 아직 in-flight task 가 남은 핸들. 콜백이 다 온 뒤 지운다.

    mutable std::vector<nixlDocaMemosBackendReqH *> pendingDeletes_;

    mutable std::mutex wakeupMutex_;
    mutable std::condition_variable wakeup_;
};

#endif // NIXL_SRC_PLUGINS_DOCA_MEMOS_DOCA_MEMOS_PROGRESS_ENGINE_H
