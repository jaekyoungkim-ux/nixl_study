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
// 스터디용 주석본. 원본: PR #1717 src/plugins/doca_memos/doca_memos_progress_engine.cpp
// 코드는 원본 그대로이며, "▶" 로 시작하는 줄만 추가된 한글 주석이다.
// 원본 행 번호와 어긋나므로, 원본을 인용할 때는 원본 파일을 볼 것.
//
// 읽는 순서 (파일 순서대로 읽지 말 것):
//   ① 베이스 생성자        DOCA 자원을 만들고 켜는 과정
//   ② trySubmitRequest()   ★ 실제 KV 명령이 조립되는 곳
//   ③ 콜백 2개             완료/에러가 카운터로 접히는 지점
//   ④ nixlNoThread*        단순한 쪽 먼저
//   ⑤ nixlThreaded*        더블버퍼 큐. 마지막에
// ==========================================================================
#include "doca_memos_progress_engine.h"
#include "doca_memos_backend.h" // For nixlDocaMemosBackendReqH
#include "common/nixl_log.h"

#include <algorithm>
#include <thread>
#include <chrono>

// DOCA includes
#include <doca_pe.h>
#include <doca_kvdev.h>//libdoca_kv
#include <doca_kvdev_io.h>//libdoca_kv
#include <doca_nvme_kernel_kvdev.h>//libdoca_kv
#include <doca_nvme_kernel_kvdev_io.h>//libdoca_kv
#include <doca_ctx.h>
#include <doca_error.h>

namespace {

// doca_pe_progress() reaps at most one completion per call, so reaping once per
// checkXfer() costs a full poll-loop round trip per completed task. Drain a
// bounded burst instead; the bound keeps a saturating device from starving the
// caller (and, in the no-thread engine, from holding lock_ indefinitely).
// ▶ doca_pe_progress() 는 한 번에 완료 1건만 거둔다. 매번 한 번씩만 부르면
//   완료 하나당 폴링 왕복이 생기므로, 한 번에 최대 64건까지 몰아서 거둔다.
//   상한을 두는 이유는 장치가 완료를 쏟아낼 때 호출자를 굶기지 않기 위함.
constexpr int kProgressBurst = 64;

} // anonymous namespace

void

// ▶▶ 제출 도중 복구 불가 에러가 났을 때. 남은 descriptor 는 영영 콜백을 만들지
//    않으므로, totalTasks_ 를 "이미 제출된 수 + 1" 로 줄여 완료 조건을 맞춰준다.
//    이 보정이 없으면 checkXfer 가 영원히 NIXL_IN_PROG 를 반환한다.
nixlDocaMemosProgressEngine::handleSubmissionFailure(nixlDocaMemosBackendReqH *req_h,
                                                     nixl_status_t status) {
    if (req_h->overallStatus_ == NIXL_IN_PROG) {
        req_h->overallStatus_ = status;
    }

    // Tasks beyond this point were never submitted and will never generate
    // callbacks. Adjust totalTasks_ so the in-flight tasks plus this failure
    // can reach the completion threshold.
    req_h->totalTasks_ = req_h->submittedTasks_ + 1;

    if (++req_h->completedTasks_ >= req_h->totalTasks_) {
        req_h->allTasksCompleted_.store(true, std::memory_order_release);
    }
}

void

// ▶▶▶ 성공 완료 콜백. 하는 일은 카운터 증가와 완료 표시뿐이다.
//     retrieve 는 실제로 받은 길이가 기대와 다르면 경고만 남기고 성공으로 친다.
nixlDocaMemosProgressEngine::taskCompletionCallback(struct doca_task *task,
                                                    union doca_data task_user_data,
                                                    union doca_data ctx_user_data) {
    (void)ctx_user_data;

    auto *task_ctx = static_cast<docaMemosTaskContext *>(task_user_data.ptr);
    if (task_ctx && task_ctx->reqH) {
        auto *req_h = task_ctx->reqH;
        int task_index = task_ctx->taskIndex;
        // Relaxed load: missing a recent cancel is harmless because
        // processCancellations reconciles totalTasks_ afterwards. Anything
        // that changes how cancellations are reaped must preserve that.
        bool is_cancelled = req_h->cancelled_.load(std::memory_order_relaxed);

        if (!is_cancelled) {
            if (req_h->isExistQuery_) {
                req_h->taskResult_.store(static_cast<int>(DOCA_SUCCESS), std::memory_order_release);
                NIXL_DEBUG << "EXIST query completed - key exists (task " << task_index << ")";
            } else if (task_ctx->isRetrieve) {
                const struct doca_kvdev_io_task_retrieve *retrieve_task =
                    doca_kvdev_io_task_retrieve_from_task(task);
                uint32_t result_len =
                    doca_kvdev_io_task_retrieve_get_result_value_len(retrieve_task);
                if (result_len != task_ctx->expectedValueLen) {
                    NIXL_WARN << "Task " << task_index << " retrieved " << result_len
                              << " bytes, expected " << task_ctx->expectedValueLen;
                }
                NIXL_DEBUG << "Task " << task_index << " completed successfully (" << result_len
                           << " bytes)";
            } else {
                NIXL_DEBUG << "Task " << task_index << " completed successfully";
            }
        }

        if (++req_h->completedTasks_ >= req_h->totalTasks_) {
            if (!is_cancelled && req_h->overallStatus_ == NIXL_IN_PROG) {
                req_h->overallStatus_ = NIXL_SUCCESS;
            }
            req_h->allTasksCompleted_.store(true, std::memory_order_release);
        }
    }
    doca_task_free(task);
}

void

// ▶▶▶ 에러 콜백. 이 플러그인에서 가장 특이한 규약이 여기 있다.
//
//     EXIST 태스크는 성공하든 실패하든 **항상 이 에러 콜백으로** 결과가 온다:
//        DOCA_ERROR_ALREADY_EXIST → key 있음
//        DOCA_ERROR_NOT_FOUND     → key 없음
//     다른 태스크와 규약이 다르므로 이 API 를 다룰 때 주의해야 한다.
//
//     retrieve 의 NOT_FOUND 는 ignore_read_not_found 설정에 따라 정상 처리되기도
//     한다 — "내구성 100% 포기" 설계가 코드에 반영된 지점.
nixlDocaMemosProgressEngine::taskErrorCallback(struct doca_task *task,
                                               union doca_data task_user_data,
                                               union doca_data ctx_user_data) {
    (void)ctx_user_data;

    auto *task_ctx = static_cast<docaMemosTaskContext *>(task_user_data.ptr);
    if (task_ctx && task_ctx->reqH) {
        auto *req_h = task_ctx->reqH;
        int task_index = task_ctx->taskIndex;
        // See taskCompletionCallback for why relaxed is sufficient here.
        bool is_cancelled = req_h->cancelled_.load(std::memory_order_relaxed);

        if (!is_cancelled) {
            doca_error_t task_err = doca_task_get_status(task);

            if (req_h->isExistQuery_) {
                // The DOCA EXIST task always completes via the error callback:
                // ALREADY_EXIST = key found, NOT_FOUND = key absent. Any other
                // status is a real failure.
                if (task_err == DOCA_ERROR_ALREADY_EXIST) {
                    req_h->taskResult_.store(static_cast<int>(DOCA_SUCCESS),
                                             std::memory_order_release);
                    NIXL_DEBUG << "EXIST query completed - key exists (task " << task_index << ")";
                } else if (task_err == DOCA_ERROR_NOT_FOUND) {
                    req_h->taskResult_.store(static_cast<int>(DOCA_ERROR_NOT_FOUND),
                                             std::memory_order_release);
                    NIXL_DEBUG << "EXIST query completed - key does not exist (task " << task_index
                               << ")";
                } else {
                    NIXL_ERROR << "EXIST task " << task_index
                               << " failed: " << doca_error_get_descr(task_err) << " ("
                               << static_cast<int>(task_err) << ")";
                    if (req_h->overallStatus_ == NIXL_IN_PROG) {
                        req_h->overallStatus_ = NIXL_ERR_BACKEND;
                    }
                }
            } else if (task_err == DOCA_ERROR_NOT_FOUND && req_h->ignoreNotFound_) {
                NIXL_DEBUG << "Task " << task_index
                           << " got key-not-found on retrieve, ignoring per configuration";
            } else {
                NIXL_ERROR << "Task " << task_index
                           << " failed (non-EXIST operation): " << doca_error_get_descr(task_err)
                           << " (" << static_cast<int>(task_err) << ")";

                if (req_h->overallStatus_ == NIXL_IN_PROG) {
                    req_h->overallStatus_ = NIXL_ERR_BACKEND;
                }
            }
        }

        if (++req_h->completedTasks_ >= req_h->totalTasks_) {
            if (!is_cancelled && req_h->overallStatus_ == NIXL_IN_PROG) {
                req_h->overallStatus_ = NIXL_SUCCESS;
            }
            req_h->allTasksCompleted_.store(true, std::memory_order_release);
        }
    }
    doca_task_free(task);
}


// ▶▶ !!constructor!! 베이스 생성자. initDocaDevice() 와 같은 패턴이다 — 만들고 → 설정하고 → 켠다.
//
//      pe 생성 → IO 컨텍스트 생성 → 일반 IO 별칭 → task 수 설정
//      → 콜백 2개 등록 → ctx 별칭 → pe 에 ctx 연결 → ctx start
//
//    실패를 예외가 아니라 initErr_ 플래그로 알린다. 그래서 호출자
//    (createProgressEngine)가 hasInitError() 를 따로 확인해야 한다.
nixlDocaMemosProgressEngine::nixlDocaMemosProgressEngine(struct doca_nvme_kernel_kvdev *nvme_kvdev,
                                                         uint32_t num_tasks,
                                                         uint32_t max_value_len)
    : maxValueLen_(max_value_len) {
    doca_error_t result;

    result = doca_pe_create(&pe_);//progress engine 생성, 장치와의 소통 담당
    if (result != DOCA_SUCCESS) {
        NIXL_ERROR << "Failed to create DOCA progress engine: " << doca_error_get_descr(result);
        initErr_ = true;
        return;
    }

    result = doca_nvme_kernel_kvdev_io_create(nvme_kvdev, &kkvIo_);//장치에 명령 넣는 큐(kkvIo) 생성 
    if (result != DOCA_SUCCESS) {
        NIXL_ERROR << "Failed to create DOCA NVMe kernel KV IO context: "
                   << doca_error_get_descr(result);
        cleanupDocaResources();
        initErr_ = true;
        return;
    }

    kvIo_ = doca_nvme_kernel_kvdev_io_as_kvdev_io(kkvIo_);//kkvIo형변환
    if (!kvIo_) {
        NIXL_ERROR << "Failed to convert NVMe kernel KV IO context to generic KV IO context";
        cleanupDocaResources();
        initErr_ = true;
        return;
    }

    result = doca_kvdev_io_set_num_tasks(kvIo_, num_tasks);//큐에 확보할 명령서 수. 기본 8192, 사용자 지정 가능, 장치 상한으로 깎임
    if (result != DOCA_SUCCESS) {
        NIXL_ERROR << "Failed to set DOCA KV IO task count: " << doca_error_get_descr(result);
        cleanupDocaResources();
        initErr_ = true;
        return;
    }

    result = doca_kvdev_io_set_task_completion_cb(kvIo_, taskCompletionCallback);//성공시 이 함수 불러라
    if (result != DOCA_SUCCESS) {
        NIXL_ERROR << "Failed to set DOCA KV IO completion callback: "
                   << doca_error_get_descr(result);
        cleanupDocaResources();
        initErr_ = true;
        return;
    }

    result = doca_kvdev_io_set_task_error_cb(kvIo_, taskErrorCallback);//실패시 이 함수 불러라
    if (result != DOCA_SUCCESS) {
        NIXL_ERROR << "Failed to set DOCA KV IO error callback: " << doca_error_get_descr(result);
        cleanupDocaResources();
        initErr_ = true;
        return;
    }

    ctx_ = doca_kvdev_io_as_ctx(kvIo_);//kvIo 형변환
    if (!ctx_) {
        NIXL_ERROR << "Failed to convert KV IO context to DOCA context";
        cleanupDocaResources();
        initErr_ = true;
        return;
    }

    result = doca_pe_connect_ctx(pe_, ctx_);//pe_가 ctx_(kkvIO형변환된 것)를 감시하도록 함.
    if (result != DOCA_SUCCESS) {
        NIXL_ERROR << "Failed to connect context to progress engine: "
                   << doca_error_get_descr(result);
        cleanupDocaResources();
        initErr_ = true;
        return;
    }

    result = doca_ctx_start(ctx_);//큐 가동.
    if (result != DOCA_SUCCESS) {
        NIXL_ERROR << "Failed to start DOCA context: " << doca_error_get_descr(result);
        cleanupDocaResources();
        initErr_ = true;
        return;
    }

    NIXL_DEBUG << "Progress engine base initialization complete";
}

void
nixlDocaMemosProgressEngine::cleanupDocaResources() {
    doca_error_t result;
    if (ctx_) {
        result = doca_ctx_stop(ctx_);
        if (result != DOCA_SUCCESS) {
            NIXL_WARN << "Failed to stop DOCA context: " << doca_error_get_descr(result);
        }
        ctx_ = nullptr;
    }

    if (kkvIo_) {
        result = doca_nvme_kernel_kvdev_io_destroy(kkvIo_);
        if (result != DOCA_SUCCESS) {
            NIXL_WARN << "Failed to destroy DOCA NVMe kernel KV IO context: "
                      << doca_error_get_descr(result);
        }
        kkvIo_ = nullptr;
        kvIo_ = nullptr;
    }

    if (pe_) {
        result = doca_pe_destroy(pe_);
        if (result != DOCA_SUCCESS) {
            NIXL_WARN << "Failed to destroy DOCA progress engine: " << doca_error_get_descr(result);
        }
        pe_ = nullptr;
    }
}

nixl_status_t

// ▶ 베이스의 기본 구현. 원자적으로 게시된 완료 플래그만 읽으므로 락이 필요 없다.
nixlDocaMemosProgressEngine::checkXfer(nixlDocaMemosBackendReqH *req_h) const {
    if (!req_h) {
        return NIXL_ERR_INVALID_PARAM;
    }

    if (req_h->allTasksCompleted()) {
        return req_h->getOverallStatus();
    }

    return NIXL_IN_PROG;
}

bool

// ▶▶▶ 이 플러그인에서 가장 중요한 함수. descriptor 쌍을 task들로 바꿔 제출한다.
//
//     descriptor 쌍 하나 → DOCA task 한 장. 인덱스 i 로 양쪽을 함께 쓴다.
//        objectKeys_[i]   ← remote[i] 에서 온 16바이트 key
//        valueIovecs_[i]  ← local[i] 에서 온 (주소, 길이)
//
//     반환값의 뜻이 특이하다:
//        true  = 이 요청에 더 할 일이 없다 (전부 제출했거나, 실패로 종결)
//        false = task pool 이 부족해 중간에 멈췄다 → 재시도 큐로
//
//     nextDescriptorIndex_ 가 "어디까지 제출했나" 커서 역할을 한다.
//     iovec 개수 자리에 1 이 고정돼 있다 — API 는 배열을 받지만 SGL 을 쓰지 않는다.
nixlDocaMemosProgressEngine::trySubmitRequest(nixlDocaMemosBackendReqH *req_h,
                                              const nixl_xfer_op_t &operation,
                                              const nixl_meta_dlist_t &local,
                                              const nixl_meta_dlist_t &remote) const {
    if (!pendingRequests_.empty() && !req_h->isPending_) {
        NIXL_DEBUG << "Pending queue not empty - queuing new request without submission";
        req_h->nextDescriptorIndex_ = 0;
        return false;
    }

    for (int i = req_h->nextDescriptorIndex_; i < local.descCount(); i++) {
        const docaMemosKey &object_key = req_h->objectKeys_[i];//원하는 key 주소(backend에서 옴)

        auto *task_ctx = &req_h->taskContexts_[i];//뭐였지
        task_ctx->reqH = req_h;//어느 요청의
        task_ctx->taskIndex = i;//몇번째 discriptor
        task_ctx->isRetrieve = (operation == NIXL_READ);//읽기 ? 쓰기 ?
        task_ctx->expectedValueLen = req_h->valueIovecs_[i].iov_len;//예상 길이 -> 나중에 읽어온 값 길이가 다르면 경고.
        union doca_data task_user_data = {.ptr = task_ctx};//DOCA에게 pointer로 전달..?

        struct doca_task *doca_task = nullptr;
        doca_error_t result;

        const size_t iov_len = req_h->valueIovecs_[i].iov_len;
        if (iov_len > UINT32_MAX) {//예상 데이터 크기가 DOCA 한계 초과
            NIXL_ERROR << "Buffer length " << iov_len << " exceeds DOCA KV max (UINT32_MAX)";
            handleSubmissionFailure(req_h, NIXL_ERR_INVALID_PARAM);
            return true;
        }
        const uint32_t value_len = static_cast<uint32_t>(iov_len);
        if (maxValueLen_ > 0 && value_len > maxValueLen_) {//예상 데이터 크기가 볼륨 크기 초과
            NIXL_ERROR << "Buffer length " << value_len << " exceeds device max value length "
                       << maxValueLen_;
            handleSubmissionFailure(req_h, NIXL_ERR_INVALID_PARAM);
            return true;
        }

        if (operation == NIXL_WRITE) {//STORE 명령 제작 단계(빈 명령서 가져와서 채우기)
            struct doca_kvdev_io_task_store *store_task = nullptr;
            result = doca_kvdev_io_task_store_alloc_init(kvIo_, task_user_data, &store_task);//빈 명령서 형식 하나 가져와서 우리 task에 할당.
            if (result != DOCA_SUCCESS) {
                if (result == DOCA_ERROR_FULL || result == DOCA_ERROR_NO_MEMORY) {
                    req_h->nextDescriptorIndex_ = i;
                    NIXL_DEBUG << "Task pool exhausted at descriptor " << i
                               << ", queueing for retry";
                    return false;
                }
                NIXL_ERROR << "Failed to allocate DOCA KV store task: "
                           << doca_error_get_descr(result);
                handleSubmissionFailure(req_h, NIXL_ERR_BACKEND);
                return true;
            }
            doca_kvdev_io_task_store_set_key_value_conf(store_task,
                                                        object_key.key,//키 주소
                                                        object_key.keyLen,//키 길이
                                                        &req_h->valueIovecs_[i],//host DDR의 가상 주소
                                                        1,//조각 개수(1이라는 것은 벨류 하나가 메모리 상에서 연속적이어야 한다는 뜻)
                                                        value_len);//host DDR의 길이
            doca_task = doca_kvdev_io_task_store_as_task(store_task);//task 형 변환
        } else { //RETRIEVE 명령 제작 단계(빈 명령서 가져와서 채우기)
            struct doca_kvdev_io_task_retrieve *retrieve_task = nullptr;
            result = doca_kvdev_io_task_retrieve_alloc_init(kvIo_, task_user_data, &retrieve_task);
            if (result != DOCA_SUCCESS) {
                if (result == DOCA_ERROR_FULL || result == DOCA_ERROR_NO_MEMORY) {
                    req_h->nextDescriptorIndex_ = i;
                    NIXL_DEBUG << "Task pool exhausted at descriptor " << i
                               << ", queueing for retry";
                    return false;
                }
                NIXL_ERROR << "Failed to allocate DOCA KV retrieve task: "
                           << doca_error_get_descr(result);
                handleSubmissionFailure(req_h, NIXL_ERR_BACKEND);
                return true;
            }
            doca_kvdev_io_task_retrieve_set_key_value_conf(retrieve_task,
                                                           object_key.key,
                                                           object_key.keyLen,
                                                           &req_h->valueIovecs_[i],
                                                           1,
                                                           value_len);
            doca_task = doca_kvdev_io_task_retrieve_as_task(retrieve_task);
        }

        // Bump submittedTasks_ only after a successful submit, so the count
        // stays authoritative even if doca_task_submit invokes the callback
        // synchronously on failure.
        result = doca_task_submit(doca_task);//                 채워진 명령서 가져가라 신호 queue에 발사 ->이후는 알 수 없음
        if (result != DOCA_SUCCESS) {
            if (result == DOCA_ERROR_FULL) {
                doca_task_free(doca_task);
                req_h->nextDescriptorIndex_ = i;
                NIXL_DEBUG << "Task queue full at descriptor " << i << ", queueing for retry";
                return false;
            }
            NIXL_ERROR << "Failed to submit task: " << doca_error_get_descr(result);
            doca_task_free(doca_task);
            handleSubmissionFailure(req_h, NIXL_ERR_BACKEND);
            return true;
        }
        req_h->submittedTasks_++;
        req_h->nextDescriptorIndex_ = i + 1;
    }

    return true;
}

nixl_status_t

// ▶▶ 무스레드형 제출. 엔진 mutex 를 잡고 그 자리에서 제출한다.
//    카운터를 여기서 리셋하는 이유는 "prep 1회 / post 다회" 규약 때문 —
//    같은 핸들이 다시 post 될 수 있다.
nixlNoThreadProgressEngine::postXfer(nixlDocaMemosBackendReqH *req_h, // 핸들
                                     const nixl_xfer_op_t &operation, // read/write
                                     const nixl_meta_dlist_t &local, // local descriptors
                                     const nixl_meta_dlist_t &remote) const {// remote descriptors
    const std::lock_guard<std::mutex> guard(lock_); //mutex lock -> critical section start

    req_h->totalTasks_ = local.descCount(); // 이 요청의 descriptor(task) 수
    req_h->submittedTasks_ = 0;       //초
    req_h->completedTasks_ = 0;       //기
    req_h->nextDescriptorIndex_ = 0;  //화
    req_h->allTasksCompleted_.store(false, std::memory_order_release); //완료 신호 초기화
    req_h->overallStatus_ = NIXL_IN_PROG; //상태 : 진행 중

    NIXL_DEBUG << "Posting transfer with " << req_h->totalTasks_ << " tasks";
    bool fully_submitted = trySubmitRequest(req_h, operation, local, remote); //submit

    if (!fully_submitted) {//descriptor 다 안 들어갔으면 나중에 이어서 제출
        req_h->storedOperation_ = operation;
        req_h->storedLocal_ = std::make_unique<nixl_meta_dlist_t>(local);
        req_h->storedRemote_ = std::make_unique<nixl_meta_dlist_t>(remote);
        req_h->isPending_ = true;
        pendingRequests_.push_back(req_h);

        NIXL_DEBUG << "Request partially submitted (" << req_h->submittedTasks_ << "/"
                   << req_h->totalTasks_ << "), queued for retry";
    }

    // Only reachable when trySubmitRequest hit a synchronous failure and
    // called handleSubmissionFailure, which caps totalTasks_ to the in-flight
    // count plus one. In-flight callbacks cannot race here because lock_ is
    // held and doca_pe_progress runs only under the same lock.
    if (req_h->allTasksCompleted()) {
        return req_h->getOverallStatus();
    }

    NIXL_DEBUG << "Transfer posted (" << req_h->submittedTasks_ << " tasks submitted)";
    return NIXL_IN_PROG;//descriptor가 모두 제출되지 않았어도, in progress 반환됨.
}

void

// ▶ 재시도 큐를 앞에서부터 비운다. 하나라도 여전히 막히면 즉시 멈춘다(FIFO 보장).
nixlNoThreadProgressEngine::tryResumePendingRequests() const {
    const std::lock_guard<std::mutex> guard(lock_);

    while (!pendingRequests_.empty()) {
        auto *req_h = pendingRequests_.front();

        bool completed;
        if (req_h->isExistQuery_) {
            completed = trySubmitExistTask(req_h);
        } else {
            completed = trySubmitRequest(
                req_h, req_h->storedOperation_, *req_h->storedLocal_, *req_h->storedRemote_);
        }

        if (!completed) {
            break;
        }

        pendingRequests_.pop_front();
        req_h->isPending_ = false;
    }
}

void

// ▶▶ releaseReqH 가 부르는 것. 즉시 반환하는 것이 규약이다.
//    · 아직 제출 전이면 큐에서 빼고 바로 delete
//    · 이미 in-flight 면 cancelled_ 만 세우고 cancelledRequests_ 로 미룬다
//      → 실제 delete 는 progress() 안에서 콜백이 다 온 뒤에
nixlNoThreadProgressEngine::cancelRequest(nixlDocaMemosBackendReqH *req_h) const {
    const std::lock_guard<std::mutex> guard(lock_);

    if (req_h->isPending_) {
        auto it = std::find(pendingRequests_.begin(), pendingRequests_.end(), req_h);
        if (it != pendingRequests_.end()) {
            pendingRequests_.erase(it);
        }
        req_h->isPending_ = false;
        req_h->totalTasks_ = req_h->submittedTasks_;
    }

    if (req_h->completedTasks_ >= req_h->totalTasks_) {
        delete req_h;
        return;
    }

    req_h->cancelled_.store(true, std::memory_order_release);
    cancelledRequests_.push_back(req_h);
}

int

// ▶ 폴링 + 취소된 요청 회수. 두 가지가 한 락 안에서 일어난다.
nixlNoThreadProgressEngine::progress() const {
    const std::lock_guard<std::mutex> guard(lock_);
    if (!pe_) {
        return 0;
    }
    int ret = 0;
    while (ret < kProgressBurst && doca_pe_progress(pe_) != 0) {//한 번에 최대 64개 회수
        ret++;
    }//완료된 요청 회수

    for (auto it = cancelledRequests_.begin(); it != cancelledRequests_.end();) {
        if ((*it)->allTasksCompleted_.load(std::memory_order_acquire)) {
            delete *it;
            it = cancelledRequests_.erase(it);
        } else {
            ++it;
        }
    }//취소된 요청 회수

    return ret;
}

void

// ▶▶ queryMem(actual) 전용. descriptor 마다 임시 핸들을 하나씩 만든다.
//    reserve() 가 반드시 필요하다 — 이 핸들들의 주소를 DOCA 에 넘기므로
//    벡터가 재할당되면 넘긴 주소가 무효화된다.
nixlDocaMemosProgressEngine::prepareQueryHandles(
    const nixl_reg_dlist_t &descs,
    std::vector<nixlDocaMemosBackendReqH> &query_handles) const {
    size_t count = static_cast<size_t>(descs.descCount());
    // reserve() is load-bearing: callers stash &query_handles[i] in DOCA's
    // task_user_data, so the vector must not reallocate during emplace_back.
    query_handles.reserve(count);

    for (size_t i = 0; i < count; i++) {
        query_handles.emplace_back(1);
        auto &req_h = query_handles.back();
        req_h.isExistQuery_ = true;
        req_h.taskContexts_.resize(1);
        req_h.objectKeys_.resize(1);

        const auto &desc = descs[i];
        if (!nixlDocaMemosEngine::resolveMemosKey(
                desc.devId, desc.metaInfo, req_h.objectKeys_[0])) {
            NIXL_ERROR << "Failed to resolve key for descriptor " << i;
            handleSubmissionFailure(&req_h, NIXL_ERR_INVALID_PARAM);
        }
    }
}

void

// ▶▶ 모든 질의가 끝날 때까지 busy-poll 한다(yield 로 양보).
//    타임아웃이 없다 — 넣으면 use-after-free 가 된다(아래 queryMem 주석 참조).
nixlDocaMemosProgressEngine::waitForQueryCompletion(
    const std::vector<nixlDocaMemosBackendReqH> &query_handles,
    const std::function<void()> &poll) const {
    while (true) {
        if (poll) {
            poll();
        }

        bool all_completed = true;
        for (const auto &req_h : query_handles) {
            if (!req_h.allTasksCompleted()) {
                all_completed = false;
                break;
            }
        }
        if (all_completed) {
            break;
        }

        std::this_thread::yield();
    }
}

size_t

// ▶ 결과 취합. taskResult_ 를 NIXL 응답으로 번역한다.
//     DOCA_SUCCESS → 있음 (빈 params 를 넣어 has_value()=true)
//     NOT_FOUND    → 없음 (nullopt) — 이것도 "성공한 질의"로 센다
//     그 외        → 실패 (nullopt) — 이건 실패로 센다
nixlDocaMemosProgressEngine::collectQueryResults(
    const std::vector<nixlDocaMemosBackendReqH> &query_handles,
    std::vector<nixl_query_resp_t> &resp) const {
    size_t successful_queries = 0;

    for (const auto &req_h : query_handles) {
        if (req_h.getOverallStatus() == NIXL_SUCCESS) {
            doca_error_t task_result =
                static_cast<doca_error_t>(req_h.taskResult_.load(std::memory_order_acquire));
            if (task_result == DOCA_SUCCESS) {
                resp.emplace_back(nixl_query_resp_t{nixl_b_params_t{}});
                successful_queries++;
                NIXL_DEBUG << "Key exists";
            } else if (task_result == DOCA_ERROR_NOT_FOUND) {
                resp.emplace_back(std::nullopt);
                successful_queries++;
                NIXL_DEBUG << "Key does not exist";
            } else {
                resp.emplace_back(std::nullopt);
                NIXL_DEBUG << "Unexpected task result: " << task_result;
            }
        } else {
            resp.emplace_back(std::nullopt);
            NIXL_DEBUG << "Query failed with error";
        }
    }

    return successful_queries;
}

//무스레드형 엔진 constructor - 뭐 없음.
nixlNoThreadProgressEngine::nixlNoThreadProgressEngine(struct doca_nvme_kernel_kvdev *nvme_kvdev,
                                                       uint32_t num_tasks,
                                                       uint32_t max_value_len)
    : nixlDocaMemosProgressEngine(nvme_kvdev, num_tasks, max_value_len) {
    if (initErr_) {
        return;
    }
    NIXL_DEBUG << "Created no-thread progress engine";
}

namespace {

constexpr auto kDestructorDrainBudget = std::chrono::seconds(10);

} // anonymous namespace


// ▶▶ 소멸자. ctx 를 멈추기 전에 in-flight task 를 최대 10초까지 비운다.
//    시간 안에 안 끝나면 경고만 남긴다.
nixlNoThreadProgressEngine::~nixlNoThreadProgressEngine() {
    // Drain in-flight tasks so DOCA doesn't see them still owned by ctx_
    // when cleanupDocaResources stops the context.
    auto deadline = std::chrono::steady_clock::now() + kDestructorDrainBudget;
    while (pe_ && std::chrono::steady_clock::now() < deadline) {
        int did = doca_pe_progress(pe_);
        for (auto it = cancelledRequests_.begin(); it != cancelledRequests_.end();) {
            if ((*it)->allTasksCompleted_.load(std::memory_order_acquire)) {
                delete *it;
                it = cancelledRequests_.erase(it);
            } else {
                ++it;
            }
        }
        if (did == 0 && cancelledRequests_.empty()) {
            break;
        }
    }
    if (!cancelledRequests_.empty()) {
        NIXL_WARN << "no-thread engine destructor: " << cancelledRequests_.size()
                  << " cancelled request(s) did not drain within " << kDestructorDrainBudget.count()
                  << "s";
    }
    cleanupDocaResources();
}

nixl_status_t

// ▶▶ 무스레드형이 override 하는 지점. 상태를 묻기 전에
//    ① 폴링하고 ② 밀린 요청을 재시도한다.(이 요청을 check하기 전에, 다른 걸 먼저 하는 것.)
nixlNoThreadProgressEngine::checkXfer(nixlDocaMemosBackendReqH *req_h) const {
    if (!req_h) {
        return NIXL_ERR_INVALID_PARAM;
    }

    progress(); // 완료된 것 몰아서 거두기
    tryResumePendingRequests(); // 대기 중인 descriptor들 넣기
    return nixlDocaMemosProgressEngine::checkXfer(req_h);
}

nixl_status_t

// ▶▶ 동기 질의. 제출하고, 다 끝날 때까지 폴링하며 기다린 뒤 결과를 모은다.
//    호출자 입장에서 blocking 이므로 지연 전량이 그대로 노출된다.
nixlNoThreadProgressEngine::queryMem(const nixl_reg_dlist_t &descs,
                                     std::vector<nixl_query_resp_t> &resp) const {
    resp.reserve(descs.descCount());

    std::vector<nixlDocaMemosBackendReqH> query_handles;
    prepareQueryHandles(descs, query_handles);

    // See nixlThreadedProgressEngine::queryMem for the lifetime contract:
    // &query_handles[i] is published into pendingRequests_ and into DOCA's
    // task_user_data, so query_handles must not reallocate (prepareQueryHandles
    // reserves) and waitForQueryCompletion must drain every callback before
    // we return.
    {
        const std::lock_guard<std::mutex> guard(lock_);
        for (auto &req_h : query_handles) {
            if (req_h.allTasksCompleted()) {
                continue;
            }
            if (!trySubmitExistTask(&req_h)) {
                req_h.isPending_ = true;
                pendingRequests_.push_back(&req_h);
            }
        }
    }

    waitForQueryCompletion(query_handles, [this] {
        progress();
        tryResumePendingRequests();
    });

    size_t successful_queries = collectQueryResults(query_handles, resp);
    size_t total = query_handles.size();
    if (successful_queries < total) {
        NIXL_ERROR << (total - successful_queries) << " of " << total << " query task(s) failed";
        return NIXL_ERR_BACKEND;
    }

    return NIXL_SUCCESS;
}

nixl_status_t

// ▶▶ 스레드형 제출. 큐에 넣고 즉시 NIXL_IN_PROG 를 반환한다.
//    실제 제출은 progress thread 가 한다. dlist 를 통째로 복사해 두는 이유는
//    제출 시점에 호출자의 목록이 살아 있으리라는 보장이 없기 때문.
nixlThreadedProgressEngine::postXfer(nixlDocaMemosBackendReqH *req_h,
                                     const nixl_xfer_op_t &operation,
                                     const nixl_meta_dlist_t &local,
                                     const nixl_meta_dlist_t &remote) const {
    req_h->totalTasks_ = local.descCount();
    req_h->submittedTasks_ = 0;
    req_h->completedTasks_ = 0;
    req_h->nextDescriptorIndex_ = 0;
    req_h->allTasksCompleted_.store(false, std::memory_order_release);
    req_h->overallStatus_ = NIXL_IN_PROG;

    NIXL_DEBUG << "Posting transfer with " << req_h->totalTasks_ << " tasks (queued)";

    {
        const std::lock_guard<std::mutex> g(queueMutex_);
        producerVec_->push_back({req_h,
                                 operation,
                                 std::make_unique<nixl_meta_dlist_t>(local),
                                 std::make_unique<nixl_meta_dlist_t>(remote)});
        hasNewWork_.store(true, std::memory_order_release);
    }
    wakeup_.notify_one();

    return NIXL_IN_PROG;
}

void

// ▶▶ 아직 큐에 있으면 빼서 바로 delete. 이미 progress thread 가 가져갔으면
//    cancelled_ 만 세우고 넘긴다. 플래그를 여기서(나중이 아니라) 세우는 이유는,
//    스왑 직후 도착하는 콜백이 취소 사실을 보게 하기 위함.
nixlThreadedProgressEngine::cancelRequest(nixlDocaMemosBackendReqH *req_h) const {
    // If the request is still in the producer queue it can be deleted
    // immediately. Otherwise mark it cancelled here (not in
    // processCancellations) so any callbacks firing in the window before the
    // progress thread observes the cancel see cancelled=true and skip their
    // status bookkeeping.
    {
        const std::lock_guard<std::mutex> g(queueMutex_);

        auto it = std::find_if(producerVec_->begin(),
                               producerVec_->end(),
                               [req_h](const pendingEntry &e) { return e.reqH == req_h; });
        if (it != producerVec_->end()) {
            producerVec_->erase(it);
            delete req_h;
            return;
        }

        req_h->cancelled_.store(true, std::memory_order_release);
        cancelledRequests_.push_back(req_h);
        hasNewWork_.store(true, std::memory_order_release);
    }
    wakeup_.notify_one();
}


// ▶ !!스레드형 constructor!! 베이스 초기화가 성공했을 때만 스레드를 띄운다.
nixlThreadedProgressEngine::nixlThreadedProgressEngine(struct doca_nvme_kernel_kvdev *nvme_kvdev,
                                                       uint32_t num_tasks,
                                                       uint32_t max_value_len,
                                                       std::chrono::microseconds thread_delay)
    : nixlDocaMemosProgressEngine(nvme_kvdev, num_tasks, max_value_len),
      threadDelay_(thread_delay) {
    if (initErr_) {
        return;
    }

    NIXL_INFO << "Starting threaded progress engine with delay " << threadDelay_.count() << "us";

    // std::thread's constructor throws std::system_error on pthread_create
    // failure; nothing else to check.
    progressThread_ = std::thread(&nixlThreadedProgressEngine::progressThreadFunc, this);//스레드 생성
}


// ▶▶▶ 소멸 순서가 까다롭다. 스레드를 세우고 → 남은 완료를 최대 10초 비우고
//     → 큐에 남은 것들을 정리한다.
//
//     마지막에 pendingRequests_ 가 남아 있으면 **의도적으로 누수시킨다.**
//     이미 DOCA 에 제출된 task 가 있어, 지우면 콜백이 죽은 포인터를 건드리기
//     때문이다. 누수와 use-after-free 중 누수를 택했다.
nixlThreadedProgressEngine::~nixlThreadedProgressEngine() {
    NIXL_INFO << "Stopping threaded progress engine";

    threadStop_.store(true, std::memory_order_release);
    wakeup_.notify_all();

    if (progressThread_.joinable()) {
        progressThread_.join();
    }

    auto deadline = std::chrono::steady_clock::now() + kDestructorDrainBudget;
    while (pe_ && std::chrono::steady_clock::now() < deadline) {
        if (doca_pe_progress(pe_) == 0) {
            break;
        }
    }
    if (pe_ && std::chrono::steady_clock::now() >= deadline) {
        NIXL_WARN << "threaded engine destructor: doca_pe drain timed out after "
                  << kDestructorDrainBudget.count() << "s";
    }

    for (auto *req_h : pendingDeletes_) {
        delete req_h;
    }
    pendingDeletes_.clear();

    // Producer queues may still hold handles the progress thread never popped.
    // No DOCA submit ever happened for these, so deleting them is safe.
    for (auto &entry : vecA_) {
        delete entry.reqH;
    }
    vecA_.clear();
    for (auto &entry : vecB_) {
        delete entry.reqH;
    }
    vecB_.clear();

    for (auto *req_h : cancelledRequests_) {
        delete req_h;
    }
    cancelledRequests_.clear();

    // Anything still in pendingRequests_ has tasks that were submitted to DOCA
    // but did not complete within the drain budget; deleting them now would
    // race with any callback DOCA might still fire. Leak with a warning.
    if (!pendingRequests_.empty()) {
        NIXL_WARN << "threaded engine destructor: " << pendingRequests_.size()
                  << " request(s) with in-flight tasks did not drain; leaking to avoid UAF";
    }

    cleanupDocaResources();
}

bool

// ▶▶ EXIST 태스크 제출. 전송용 trySubmitRequest 와 달리 항상 key 1개만 다룬다
//    (질의 핸들 하나당 descriptor 하나).
nixlDocaMemosProgressEngine::trySubmitExistTask(nixlDocaMemosBackendReqH *req_h) const {
    if (!pendingRequests_.empty() && !req_h->isPending_) {
        return false;
    }

    const docaMemosKey &key = req_h->objectKeys_[0];

    auto *task_ctx = &req_h->taskContexts_[0];
    task_ctx->reqH = req_h;
    task_ctx->taskIndex = 0;
    union doca_data task_user_data = {.ptr = task_ctx};

    struct doca_kvdev_io_task_exist *exist_task = nullptr;
    doca_error_t result = doca_kvdev_io_task_exist_alloc_init(kvIo_, task_user_data, &exist_task);
    if (result != DOCA_SUCCESS) {
        if (result == DOCA_ERROR_FULL || result == DOCA_ERROR_NO_MEMORY) {
            return false;
        }
        NIXL_ERROR << "Failed to allocate DOCA KV exist task: " << doca_error_get_descr(result);
        handleSubmissionFailure(req_h, NIXL_ERR_BACKEND);
        return true;
    }

    doca_kvdev_io_task_exist_set_key_conf(exist_task, key.key, key.keyLen);
    struct doca_task *doca_task = doca_kvdev_io_task_exist_as_task(exist_task);

    result = doca_task_submit(doca_task);
    if (result != DOCA_SUCCESS) {
        if (result == DOCA_ERROR_FULL) {
            doca_task_free(doca_task);
            return false;
        }
        NIXL_ERROR << "Failed to submit EXIST task: " << doca_error_get_descr(result);
        doca_task_free(doca_task);
        handleSubmissionFailure(req_h, NIXL_ERR_BACKEND);
        return true;
    }
    req_h->submittedTasks_++;

    return true;
}

void

// ▶▶ 취소 정리. 제출된 것이 없으면 즉시 delete, 남아 있으면 pendingDeletes_ 로.
//    totalTasks_ 를 submittedTasks_ 로 낮춰 "이미 완료" 상태를 만드는 것이 요령.
nixlThreadedProgressEngine::processCancellations(
    std::vector<nixlDocaMemosBackendReqH *> &cancels) const {
    // cancelled flag is already set by cancelRequest; here we reconcile queue
    // state and schedule deferred deletion once in-flight callbacks drain.
    for (auto *req_h : cancels) {
        if (req_h->isPending_) {
            auto it = std::find(pendingRequests_.begin(), pendingRequests_.end(), req_h);
            if (it != pendingRequests_.end()) {
                pendingRequests_.erase(it);
            }
            req_h->totalTasks_ = req_h->submittedTasks_;
        }
        req_h->isPending_ = false;
        if (req_h->completedTasks_ >= req_h->totalTasks_) {
            delete req_h;
        } else {
            pendingDeletes_.push_back(req_h);
        }
    }
    cancels.clear();
}

void

// ▶▶▶ progress thread 본체. 한 바퀴에 네 가지를 순서대로 한다.
//
//     ① doca_pe_progress 를 최대 64회 돌려 완료를 거둔다
//     ② pendingDeletes_ 에서 다 끝난 것을 지운다
//     ③ pendingRequests_ (task pool 부족으로 밀린 것) 재시도
//     ④ 새 작업 스왑 — 여기가 더블버퍼의 핵심
//
//     ④의 임계구역은 "포인터 스왑 + 취소 목록 swap" 뿐이고,
//     실제 제출은 락 밖에서 일어난다. 그래서 생산자가 오래 막히지 않는다.
//
//     한 바퀴에 아무 일도 없었으면 condvar 로 잔다.
//     단 threadDelay_ 가 0 이면 자지 않고 계속 돈다(busy-spin, 사용자가 선택한 것).
nixlThreadedProgressEngine::progressThreadFunc() {
    NIXL_INFO << "Progress thread running";

    while (!threadStop_.load(std::memory_order_acquire)) {
        bool made_progress = false;

        if (pe_) {
            for (int n = 0; n < kProgressBurst && doca_pe_progress(pe_) != 0; n++) {
                made_progress = true;
            }
        }

        for (auto it = pendingDeletes_.begin(); it != pendingDeletes_.end();) {
            if ((*it)->allTasksCompleted_.load(std::memory_order_acquire)) {
                delete *it;
                it = pendingDeletes_.erase(it);
                made_progress = true;
            } else {
                ++it;
            }
        }

        while (!pendingRequests_.empty()) {
            auto *req_h = pendingRequests_.front();
            bool completed;
            if (req_h->isExistQuery_) {
                completed = trySubmitExistTask(req_h);
            } else {
                completed = trySubmitRequest(
                    req_h, req_h->storedOperation_, *req_h->storedLocal_, *req_h->storedRemote_);
            }
            if (!completed) {
                break;
            }
            pendingRequests_.pop_front();
            req_h->isPending_ = false;
            made_progress = true;
        }

        if (hasNewWork_.load(std::memory_order_acquire)) {
            std::vector<pendingEntry> *drain_vec;
            std::vector<nixlDocaMemosBackendReqH *> cancels;
            {
                const std::lock_guard<std::mutex> g(queueMutex_);
                drain_vec = producerVec_;
                producerVec_ = (producerVec_ == &vecA_) ? &vecB_ : &vecA_;
                cancels.swap(cancelledRequests_);
                hasNewWork_.store(false, std::memory_order_release);
            }

            if (!cancels.empty()) {
                processCancellations(cancels);
                made_progress = true;
            }

            for (auto &entry : *drain_vec) {
                // A cancel that lands after this vector was swapped out cannot
                // be found in the producer queue, so cancelRequest() only sets
                // the flag and defers reclamation. Honor it here so a cancelled
                // request is never submitted to DOCA. Collapsing totalTasks_ to
                // submittedTasks_ lets the deferred processCancellations() free
                // it (nothing was submitted, so it completes immediately).
                if (entry.reqH->cancelled_.load(std::memory_order_acquire)) {
                    entry.reqH->totalTasks_ = entry.reqH->submittedTasks_;
                    made_progress = true;
                    continue;
                }

                bool fully_submitted;
                if (entry.reqH->isExistQuery_) {
                    fully_submitted = trySubmitExistTask(entry.reqH);
                } else {
                    fully_submitted =
                        trySubmitRequest(entry.reqH, entry.operation, *entry.local, *entry.remote);
                }

                if (!fully_submitted) {
                    if (!entry.reqH->isExistQuery_) {
                        entry.reqH->storedOperation_ = entry.operation;
                        entry.reqH->storedLocal_ = std::move(entry.local);
                        entry.reqH->storedRemote_ = std::move(entry.remote);
                    }
                    entry.reqH->isPending_ = true;
                    pendingRequests_.push_back(entry.reqH);
                }
                made_progress = true;
            }
            drain_vec->clear();
        }

        if (made_progress) {
            if (threadDelay_.count() > 0) {
                std::this_thread::sleep_for(threadDelay_);
            }
            continue;
        }

        // delay_us = 0 means "busy-spin" (the user opted in, see the warning
        // in createProgressEngine). Skip the condvar entirely so we keep
        // polling doca_pe_progress at full rate.
        if (threadDelay_.count() == 0) {
            continue;
        }

        // Idle path: wait on the condvar to amortise the cost of polling
        // doca_pe_progress. Producers notify after setting hasNewWork_; the
        // predicate is also checked on spurious wakeups.
        std::unique_lock<std::mutex> lk(wakeupMutex_);
        wakeup_.wait_for(lk, threadDelay_, [this] {
            return threadStop_.load(std::memory_order_acquire) ||
                hasNewWork_.load(std::memory_order_acquire);
        });
    }

    NIXL_INFO << "Progress thread exiting";
}

nixl_status_t

// ▶▶ 스레드형 질의. 큐에 넣어 progress thread 에게 시키고 끝날 때까지 기다린다.
//
//    주석에 적힌 수명 계약 두 가지가 중요하다:
//      ① prepareQueryHandles 가 reserve 했으므로 벡터가 재할당되지 않는다
//      ② waitForQueryCompletion 이 모든 콜백을 기다리므로 핸들이 task 보다 오래 산다
//    ②에 타임아웃을 넣으면 use-after-free 가 된다 — 저자가 명시적으로 경고해 뒀다.

nixlThreadedProgressEngine::queryMem(const nixl_reg_dlist_t &descs,
                                     std::vector<nixl_query_resp_t> &resp) const {
    size_t count = static_cast<size_t>(descs.descCount());
    resp.reserve(count);

    std::vector<nixlDocaMemosBackendReqH> query_handles;
    prepareQueryHandles(descs, query_handles);

    // Lifetime contract: we hand &query_handles[i] to the progress thread via
    // producerVec_. Two invariants must hold for this to stay safe:
    //   1. prepareQueryHandles() reserved capacity, so query_handles never
    //      reallocates and the addresses we publish stay valid.
    //   2. waitForQueryCompletion() below blocks until every callback has
    //      fired, so query_handles outlives all in-flight DOCA tasks. Adding
    //      a timeout to the wait would silently introduce a use-after-free.
    size_t enqueued = 0;
    {
        const std::lock_guard<std::mutex> g(queueMutex_);
        for (auto &req_h : query_handles) {
            if (!req_h.allTasksCompleted()) {
                // operation/local/remote are unused for exist queries; the
                // progress thread dispatches on req_h->isExistQuery_.
                producerVec_->push_back({&req_h, {}, nullptr, nullptr});
                enqueued++;
            }
        }
        if (enqueued > 0) {
            hasNewWork_.store(true, std::memory_order_release);
        }
    }
    if (enqueued > 0) {
        wakeup_.notify_one();
        // Nothing to wait for when every handle already completed (e.g. an
        // empty batch or the assume_success fast-path); fall through to collect
        // the results that are already in place, matching the no-thread engine.
        waitForQueryCompletion(query_handles, nullptr);
    }

    size_t successful_queries = collectQueryResults(query_handles, resp);
    if (successful_queries < count) {
        NIXL_ERROR << (count - successful_queries) << " of " << count << " query task(s) failed";
        return NIXL_ERR_BACKEND;
    }

    return NIXL_SUCCESS;
}
