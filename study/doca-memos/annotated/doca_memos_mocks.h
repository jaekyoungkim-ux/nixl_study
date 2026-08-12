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
// ì¤í°ëì© ì£¼ìë³¸. ìë³¸: PR #1717 src/plugins/doca_memos/doca_memos_mocks.h
// ì½ëë ìë³¸ ê·¸ëë¡ì´ë©°, "▶" ë¡ ììíë ì¤ë§ ì¶ê°ë íê¸ ì£¼ìì´ë¤.
// ìë³¸ í ë²í¸ì ì´ê¸ëë¯ë¡, ìë³¸ì ì¸ì©í  ëë ìë³¸ íì¼ì ë³¼ ê².
//
// 가짜 KV 장치의 자료구조. DOCA 4.4 가 비공개인 지금
// doca_kvdev API 의 모양을 읽을 수 있는 사실상 유일한 공개 근거다.
//
// 주의: 시그니처는 진짜 헤더에서 오므로 믿을 수 있지만,
//       동작은 저자(benlwalker)가 만든 모델이다. 실제 장치 명세가 아니라
//       "플러그인이 장치에게 기대하는 것" 으로 읽어야 한다.
// ==========================================================================
#ifndef NIXL_TEST_UNIT_PLUGINS_DOCA_MEMOS_DOCA_MEMOS_MOCKS_H
#define NIXL_TEST_UNIT_PLUGINS_DOCA_MEMOS_DOCA_MEMOS_MOCKS_H

// This header is only consumed from C++ translation units. Including the C++
// standard library headers up-front keeps the mock state definitions below
// well-formed regardless of which DOCA header pulls them in.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <sys/uio.h>
#include <vector>

#include <doca_compat.h>
#include <doca_ctx.h>
#include <doca_error.h>
#include <doca_kvdev.h>
#include <doca_kvdev_io.h>
#include <doca_nvme_kernel_kvdev.h>
#include <doca_nvme_kernel_kvdev_io.h>
#include <doca_pe.h>
#include <doca_types.h>

// Concrete definitions for opaque mock types. These structs are forward
// declared in the public DOCA headers above; defining them here gives the
// mocks something to allocate, while still keeping them opaque to the plugin.

// ▶▶ 진짜 헤더에서 이 타입들은 이름만 선언돼 있다(불완전 타입).
//    목이 여기서 빈 구조체로 정의해 줘야 new 로 할당할 수 있다.
//    플러그인 입장에서는 여전히 내부가 보이지 않는다.
struct doca_kvdev {};

struct doca_kvdev_io {};

struct doca_pe {};

struct doca_ctx {};

struct doca_nvme_kernel_kvdev {};

struct doca_nvme_kernel_kvdev_io {};

// All typed KV tasks are unified into a single mock task carrying the per-op
// state the implementation needs to observe (key, value buffer, user data,
// completion status, store/retrieve options). The typed structs in the public
// headers are forward declarations only; the mocks derive them from doca_task
// so the *_as_task / *_from_task helpers are simple pointer casts.

// ▶▶▶ 세 종류 태스크(STORE/RETRIEVE/EXIST)를 구조체 하나로 합쳤다.
//     진짜 헤더에서는 타입이 따로지만 전부 doca_task 를 상속하므로
//     *_as_task / *_from_task 변환이 단순 포인터 캐스트가 된다.
//
//     do_not_overwrite / must_exist 는 **플러그인이 쓰지 않는 필드**다.
//     doca_kvdev STORE 에 조건부 쓰기 옵션이 존재한다는 증거이며,
//     플러그인이 쓰는 API 표면보다 실제 API 가 넓다는 뜻이다.
struct doca_task {
    enum Type { STORE, RETRIEVE, EXIST } type;

    std::string key;
    void *buffer = nullptr;
    size_t buffer_size = 0;
    uint32_t value_len = 0;
    union doca_data user_data{};
    doca_error_t status = DOCA_SUCCESS;
    uint8_t do_not_overwrite = 0;
    uint8_t must_exist = 0;
    uint32_t result_value_len = 0;
};

struct doca_kvdev_io_task_store : doca_task {};

struct doca_kvdev_io_task_retrieve : doca_task {};

struct doca_kvdev_io_task_exist : doca_task {};

// Mock control singleton. Tests set fields here to control how the mock
// implementation responds to plugin API calls.

// ▶▶▶ 목 제어 싱글턴. 테스트가 이 필드들을 조작해 장치의 반응을 지어낸다.
//     "어떤 에러가 어디서 날 수 있는가" 의 목록이기도 하다.
class DocaMockControl {
public:
    static DocaMockControl &
    instance() {
        static DocaMockControl inst;
        return inst;
    }

    // Serialises every singleton access. Recursive so callbacks fired from
    // doca_pe_progress() can re-enter mocks (e.g. doca_task_free) without
    // self-deadlock. Tests should hold this lock around any field they read
    // or mutate when a threaded progress engine is alive.
    // ▶ recursive 인 이유: doca_pe_progress() 안에서 콜백이 불리고,
    //   그 콜백이 doca_task_free() 같은 목 함수를 다시 부르기 때문이다.
    //   일반 mutex 면 자기 자신에게 막힌다.
    static std::unique_lock<std::recursive_mutex>
    lock() {
        return std::unique_lock<std::recursive_mutex>(instance().mutex_);
    }

    // Configuration for mock behavior - all DOCA API return values
    // ▶▶ 여기부터가 에러 주입 손잡이. 각 DOCA 호출의 반환값을 테스트가 지정한다.
    doca_error_t nvme_kvdev_create_result = DOCA_SUCCESS;
    doca_error_t kvdev_start_result = DOCA_SUCCESS;
    doca_error_t kvdev_stop_result = DOCA_SUCCESS;
    doca_error_t pe_create_result = DOCA_SUCCESS;
    doca_error_t pe_destroy_result = DOCA_SUCCESS;
    doca_error_t kv_io_create_result = DOCA_SUCCESS;
    doca_error_t kv_io_destroy_result = DOCA_SUCCESS;
    doca_error_t ctx_start_result = DOCA_SUCCESS;
    doca_error_t ctx_stop_result = DOCA_SUCCESS;
    doca_error_t pe_connect_ctx_result = DOCA_SUCCESS;
    doca_error_t kv_task_alloc_result = DOCA_SUCCESS;
    doca_error_t task_submit_result = DOCA_SUCCESS;
    doca_error_t pe_get_notification_handle_result = DOCA_SUCCESS;
    doca_error_t pe_request_notification_result = DOCA_SUCCESS;
    doca_error_t get_max_value_len_result = DOCA_SUCCESS;
    uint32_t max_value_len = 1u << 20; // 1 MiB default

    // Advanced error injection - fail after N successful calls.
    // task_alloc_fail_after_n: -1 disables; 0 fails first call; N fails the
    // (N+1)th call onwards. Applies uniformly to store / retrieve / exist.
    // ▶ "N번째 호출부터 실패" 형태의 주입. task pool 고갈 재시도 경로를
    //   재현하는 데 쓴다 (DOCA_ERROR_FULL 이 기본 주입 에러).
    int task_alloc_fail_after_n = -1;
    int task_alloc_call_count = 0;
    doca_error_t task_alloc_fail_error = DOCA_ERROR_FULL;
    int task_submit_fail_after_n = -1;
    int task_submit_call_count = 0;
    doca_error_t task_submit_fail_error = DOCA_ERROR_FULL;

    // Force specific task to fail with specific error
    bool force_task_error = false;
    doca_error_t forced_task_error_code = DOCA_ERROR_NOT_FOUND;

    // Callbacks installed by doca_kvdev_io_set_task_{completion,error}_cb.
    // Signatures match the public typedef.
    using TaskCallback = std::function<void(struct doca_task *, union doca_data, union doca_data)>;
    TaskCallback task_completion_cb = nullptr;
    TaskCallback task_error_cb = nullptr;

    // Simulated KV storage
    // ▶▶▶ 가짜 장치의 전부. KV 장치를 호스트 관점에서 모델링하면
    //     결국 map<key, 바이트열> 하나라는 것을 보여준다.
    //     key 는 std::string 이지만 이진 바이트를 담는다(16바이트 이하).
    std::map<std::string, std::vector<uint8_t>> kv_store;

    // Submitted tasks tracking
    std::vector<struct doca_task *> submitted_tasks;
    std::vector<union doca_data> submitted_task_user_data;

    // Progress tracking
    int pe_progress_return = 0;
    // ▶ true 면 doca_pe_progress() 가 제출된 태스크를 즉시 처리하고 콜백을 부른다.
    //   false 면 아무 일도 일어나지 않는다 — "제출했지만 아직 안 끝남" 상태 재현용.

    bool auto_complete_tasks = false;

    // Notification handle
    doca_notification_handle_t notification_handle = 100;

    // Reset all state. Acquires the mock lock so callers don't have to;
    // since the lock is recursive this is safe to call from a context that
    // already holds it.
    void
    reset() {
        std::unique_lock<std::recursive_mutex> guard(mutex_);
        nvme_kvdev_create_result = DOCA_SUCCESS;
        kvdev_start_result = DOCA_SUCCESS;
        kvdev_stop_result = DOCA_SUCCESS;
        pe_create_result = DOCA_SUCCESS;
        pe_destroy_result = DOCA_SUCCESS;
        kv_io_create_result = DOCA_SUCCESS;
        kv_io_destroy_result = DOCA_SUCCESS;
        ctx_start_result = DOCA_SUCCESS;
        ctx_stop_result = DOCA_SUCCESS;
        pe_connect_ctx_result = DOCA_SUCCESS;
        kv_task_alloc_result = DOCA_SUCCESS;
        task_submit_result = DOCA_SUCCESS;
        pe_get_notification_handle_result = DOCA_SUCCESS;
        pe_request_notification_result = DOCA_SUCCESS;
        get_max_value_len_result = DOCA_SUCCESS;
        max_value_len = 1u << 20;

        task_alloc_fail_after_n = -1;
        task_alloc_call_count = 0;
        task_alloc_fail_error = DOCA_ERROR_FULL;
        task_submit_fail_after_n = -1;
        task_submit_call_count = 0;
        task_submit_fail_error = DOCA_ERROR_FULL;
        force_task_error = false;
        forced_task_error_code = DOCA_ERROR_NOT_FOUND;

        task_completion_cb = nullptr;
        task_error_cb = nullptr;

        kv_store.clear();
        submitted_tasks.clear();
        submitted_task_user_data.clear();

        pe_progress_return = 0;
        auto_complete_tasks = false;
        notification_handle = 100;
    }

private:
    DocaMockControl() = default;

    std::recursive_mutex mutex_;
};

#endif // NIXL_TEST_UNIT_PLUGINS_DOCA_MEMOS_DOCA_MEMOS_MOCKS_H
