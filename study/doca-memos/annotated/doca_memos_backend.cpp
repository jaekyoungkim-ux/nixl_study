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
// 스터디용 주석본. 원본: PR #1717 src/plugins/doca_memos/doca_memos_backend.cpp
// 코드는 원본 그대로이며, "▶" 로 시작하는 줄만 추가된 한글 주석이다.
// 원본 행 번호와 어긋나므로, 원본을 인용할 때는 원본 파일을 볼 것.
//
// 읽는 순서 (파일 순서대로 읽지 말 것):
//   ① initDocaDevice()      장치를 잡아 쓸 수 있게 만드는 전 과정  ← 시작점
//   ② parseInitParams()     위에서 쓰는 파라미터가 어디서 오는지
//   ③ convertToMemosKey / resolveMemosKey    키 해석 3단계
//   ④ registerMem()         키가 확정되는 지점
//   ⑤ prepXfer()            descriptor 쌍 → iovec + 키
//   ⑥ postXfer()            progress engine 에 위임
//   ⑦ checkXfer / releaseReqH
// ============================================================================

#include "doca_memos_backend.h"

#include "common/nixl_log.h"
#include "nixl_types.h"

#include <absl/strings/str_format.h>
#include <charconv>
#include <iomanip>
#include <limits>
#include <memory>
#include <algorithm>
#include <chrono>
#include <cstring>

// DOCA headers
// ▶ doca_kvdev* 는 DOCA MEMOS(비공개, pkg-config: doca-kv),
//   doca_pe/ctx/error 는 DOCA Core(doca-common). 둘은 다른 패키지다.
#include <doca_kvdev.h>
#include <doca_nvme_kernel_kvdev.h>
#include <doca_pe.h>
#include <doca_ctx.h>
#include <doca_error.h>

namespace {

// Metadata for OBJ_SEG memory
// ▶▶ registerMem() 이 만드는 "상자". agent 가 이 객체의 주소를 보관했다가
//    전송 시 descriptor 의 metadataP 로 되돌려준다.
//    실질적 알맹이는 objKey 하나뿐이다.
class nixlDocaMemosMetadata : public nixlBackendMD {
public:
    nixlDocaMemosMetadata(nixl_mem_t nixl_mem, uint64_t dev_id, const docaMemosKey &key)
        : nixlBackendMD(true),
          nixlMem(nixl_mem),
          devId(dev_id),
          objKey(key) {}

    ~nixlDocaMemosMetadata() = default;

    nixl_mem_t nixlMem;
    uint64_t devId;
    docaMemosKey objKey;
};

// ▶▶ prepXfer 의 입력 검증. 이 백엔드가 받을 수 있는 조합인지 본다.
[[nodiscard]] bool
isValidPrepXferParams(const nixl_xfer_op_t &operation,
                      const nixl_meta_dlist_t &local,
                      const nixl_meta_dlist_t &remote,
                      const std::string &remote_agent,
                      const std::string &local_agent) {
    if (operation != NIXL_WRITE && operation != NIXL_READ) {
        NIXL_ERROR << absl::StrFormat("Invalid operation type: %d", operation);
        return false;
    }

    // ▶ 에러가 아니라 경고다. 스토리지 백엔드는 모든 전송이 자기 자신으로의 loopback 이라
    //   remote_agent 가 자기 이름이어야 정상. 달라도 막지는 않는다.
    if (remote_agent != local_agent) {
        NIXL_WARN << absl::StrFormat("Remote agent doesn't match the requesting agent (%s). Got %s",
                                     local_agent,
                                     remote_agent);
    }

    // ▶▶ 여기가 "GPU 메모리 직행 불가" 가 강제되는 지점.
    //    local 은 반드시 DRAM_SEG. VRAM_SEG 를 넣으면 거부된다.
    if (local.getType() != DRAM_SEG) {
        NIXL_ERROR << absl::StrFormat("Local memory type must be DRAM_SEG, got %d",
                                      local.getType());
        return false;
    }

    if (remote.getType() != OBJ_SEG) {
        NIXL_ERROR << absl::StrFormat("Remote memory type must be OBJ_SEG, got %d",
                                      remote.getType());
        return false;
    }

    return true;
}

} // anonymous namespace

// ▶ 로그에 키를 hex 로 찍기 위한 출력 연산자.
std::ostream &
operator<<(std::ostream &os, const docaMemosKey &k) {
    NIXL_ASSERT(k.keyLen <= DOCA_MEMOS_MAX_OBJECT_KEY_LEN);
    std::ios::fmtflags flags(os.flags());
    char fill = os.fill();
    os << std::hex << std::setfill('0');
    for (uint32_t i = 0; i < k.keyLen; ++i) {
        os << std::setw(2) << static_cast<unsigned>(k.key[i]);
    }
    os.flags(flags);   // ▶ 스트림 상태를 원복. 안 하면 이후 출력이 전부 hex 가 된다
    os.fill(fill);
    return os;
}

// ▶▶ RAII 소멸자. unique_ptr 이 파괴될 때 자동 호출된다.
void
nixlDocaMemosEngine::NvmeKvdevDeleter::operator()(doca_nvme_kernel_kvdev *dev) const noexcept {
    if (!dev) {
        return;
    }
    // doca_kvdev_stop() is only valid on a started device. _set_nguid() /
    // _set_path() failures during init leave the device in the unstarted
    // state, so guard with a started check rather than blindly stopping.
    // ▶ init 중간에 실패하면 장치가 "시작 안 된" 상태로 남는다.
    //   그 상태에 stop() 을 부르면 안 되므로 반드시 확인 후 호출.
    doca_kvdev *kv = doca_nvme_kernel_kvdev_as_kvdev(dev);
    if (kv) {
        uint8_t started = 0;
        if (doca_kvdev_is_started(kv, &started) == DOCA_SUCCESS && started) {
            doca_error_t result = doca_kvdev_stop(kv);
            if (result != DOCA_SUCCESS) {
                NIXL_WARN << "Failed to stop DOCA KV device: " << doca_error_get_descr(result);
            }
        }
    }
    doca_error_t result = doca_nvme_kernel_kvdev_destroy(dev);
    if (result != DOCA_SUCCESS) {
        NIXL_WARN << "Failed to destroy NVMe kernel KV device: " << doca_error_get_descr(result);
    }
}

void
nixlDocaMemosEngine::cleanupDocaResources() {
    // ▶ kvdev_ 는 비소유 별칭이므로 그냥 지우고, 실소유자인 nvmeKvdev_ 만 reset.
    //   reset 이 위 NvmeKvdevDeleter 를 부른다.
    kvdev_ = nullptr;
    nvmeKvdev_.reset();
}

// ============================================================================
// ② parseInitParams — 설정 문자열을 멤버 변수로
// ============================================================================
// ▶▶ NIXL 의 파라미터는 전부 문자열(string→string map)이다.
//    숫자든 불리언이든 여기서 직접 파싱해야 한다.
// ▶ 참고: NIXL 은 공용 헬퍼(src/utils/common/backend.h 의 getBackendParamDefaulted)를
//   제공하는데 이 플러그인은 쓰지 않고 손으로 짰다. 동작엔 문제없으나 리뷰 코멘트감.
//   헬퍼를 썼다면 "true"/"yes"/"1" 을 모두 받고 "8192abc" 같은 오타도 잡힌다.
nixl_status_t
nixlDocaMemosEngine::parseInitParams(const nixl_b_params_t *params) {
    if (!params) {
        return NIXL_SUCCESS;
    }

    // ▶ 필수 파라미터지만 여기서는 검사하지 않는다. constructor 가 빈 문자열을 보고 throw 한다.
    auto it = params->find("device_name");
    if (it != params->end()) {
        deviceName_ = it->second;
    }

    it = params->find("num_tasks");
    if (it != params->end()) {
        try {
            const unsigned long long parsed = std::stoull(it->second);
            if (parsed == 0 || parsed > std::numeric_limits<uint32_t>::max()) {
                NIXL_ERROR << "num_tasks '" << it->second << "' out of range (1.."
                           << std::numeric_limits<uint32_t>::max() << ")";
                return NIXL_ERR_INVALID_PARAM;
            }
            numTasks_ = static_cast<uint32_t>(parsed);
        }
        catch (...) {
            NIXL_ERROR << "Failed to parse num_tasks parameter";
            return NIXL_ERR_INVALID_PARAM;
        }
    }

    it = params->find("nguid");
    if (it != params->end()) {
        nguid_ = it->second;
    }

    // ▶ 문자열 "true"/"false" 만 받는다. "True", "1", "yes" 는 거부.
    it = params->find("ignore_read_not_found");
    if (it != params->end()) {
        if (it->second == "true") {
            ignoreReadNotFound_ = true;
        } else if (it->second == "false") {
            ignoreReadNotFound_ = false;
        } else {
            NIXL_ERROR << "Invalid ignore_read_not_found '" << it->second
                       << "', expected 'true' or 'false'";
            return NIXL_ERR_INVALID_PARAM;
        }
    }

    // ▶▶ 기본값이 assume_success 라는 점에 주의.
    //    GTC 발표의 "retrieve 전에 exist 하지 마라" 요구를 코드로 구현한 것이지만,
    //    벤치마킹 시 실제 비용을 숨기므로 QUERY 측정에는 actual 을 명시해야 한다.
    it = params->find("query_mem_mode");
    if (it != params->end()) {
        if (it->second == "actual") {
            queryMemAssumeSuccess_ = false;
        } else if (it->second == "assume_success") {
            queryMemAssumeSuccess_ = true;
        } else {
            NIXL_ERROR << "Invalid query_mem_mode '" << it->second
                       << "', expected 'assume_success' or 'actual'";
            return NIXL_ERR_INVALID_PARAM;
        }
    }

    if (!params->count("nguid")) {
        NIXL_WARN << "Using default nguid (all zeros); set 'nguid' to override";
    }

    return NIXL_SUCCESS;
}

// ============================================================================
// ① initDocaDevice — ★ 여기서부터 읽을 것 //!!!!constructor 과정임!!!!
// ============================================================================
// ▶▶▶ 장치를 잡아 쓸 수 있게 만드는 전 과정.
//     지금까지 이야기한 /dev/nvme0n1, NGUID, task pool 이 전부 여기서 만난다.
//     순서에 의미가 있다: 경로 → 생성 → 경로 설정 → 별칭 획득 → NGUID → start → 능력 조회
nixl_status_t//반환 형식 : 0 -> 성공, 1 -> 진행 중, 음수 -> 실패
nixlDocaMemosEngine::initDocaDevice() {//nixlDocaMemosEngine class : 본체. 
    // ▶ [1] 장치가 허용하는 최대 경로 길이를 먼저 물어본다. 하드코딩하지 않는다.
    uint32_t max_path_len = 0;
    doca_error_t result = doca_nvme_kernel_kvdev_cap_get_max_path_len(&max_path_len);
    if (result != DOCA_SUCCESS) {
        NIXL_ERROR << "doca_nvme_kernel_kvdev_cap_get_max_path_len failed: "
                   << doca_error_get_descr(result);
        return NIXL_ERR_BACKEND;
    }
    // ▶ +1 은 널 종단 문자 자리.
    if (deviceName_.size() + 1 > max_path_len) {
        NIXL_ERROR << "device_name length " << deviceName_.size() << " exceeds device-reported max "
                   << (max_path_len - 1);
        return NIXL_ERR_INVALID_PARAM;
    }

    // ▶ [2] NGUID 문자열(32자 hex)을 16바이트로 변환.
    //   DOCA_KVDEV_NGUID_LEN = 16 이므로 hex 문자는 그 두 배여야 한다.
    uint8_t nguid_bytes[DOCA_KVDEV_NGUID_LEN] = {0};
    if (!nguid_.empty()) {
        if (nguid_.length() != DOCA_KVDEV_NGUID_LEN * 2) {
            NIXL_ERROR << "Invalid nguid '" << nguid_ << "' (expected 32 hex chars)";
            return NIXL_ERR_INVALID_PARAM;
        }
        for (size_t i = 0; i < DOCA_KVDEV_NGUID_LEN; i++) {
            unsigned val = 0;
            const char *first = nguid_.data() + i * 2;
            // ▶ 두 글자씩 끊어 16진수로. ptr 검사로 "잡문자 섞임" 까지 잡는다.
            auto [ptr, ec] = std::from_chars(first, first + 2, val, 16);
            if (ec != std::errc{} || ptr != first + 2) {
                NIXL_ERROR << "Invalid nguid '" << nguid_ << "' (expected 32 hex chars)";
                return NIXL_ERR_INVALID_PARAM;
            }
            nguid_bytes[i] = static_cast<uint8_t>(val);
        }
    }

    // ▶ [3] 장치 객체(장치를 다루기 위한 handler) 생성. 
    // 아직 어느 장치인지 모르는 빈 껍데기.(NVMe controller Emulation 중인 Blufield에 연결될 예정)
    doca_nvme_kernel_kvdev *raw_nvme_kvdev = nullptr;
    result = doca_nvme_kernel_kvdev_create(&raw_nvme_kvdev);
    if (result != DOCA_SUCCESS) {
        NIXL_ERROR << "Failed to create DOCA NVMe kernel KV device: "
                   << doca_error_get_descr(result);
        return NIXL_ERR_BACKEND;
    }
    // ▶ 여기서 unique_ptr 로 옮겨 담는다. 이후 실패 경로는 cleanupDocaResources() 만 부르면 된다.
    nvmeKvdev_.reset(raw_nvme_kvdev);

    //만든 디바이스를 /dev/nvme0n1에 연결한다. (device_name이 /dev/nvme0n1임)
    result = doca_nvme_kernel_kvdev_set_path(nvmeKvdev_.get(), deviceName_.c_str());
    if (result != DOCA_SUCCESS) {
        NIXL_ERROR << "Failed to set NVMe kernel KV device path: " << doca_error_get_descr(result);
        cleanupDocaResources();
        return NIXL_ERR_BACKEND;
    }

    
    kvdev_ = doca_nvme_kernel_kvdev_as_kvdev(nvmeKvdev_.get());
    if (!kvdev_) {
        NIXL_ERROR << "Failed to convert NVMe kernel KV device to generic KV device";
        cleanupDocaResources();
        return NIXL_ERR_BACKEND;
    }

    // ▶ [6] namespace 선택. 이미 장치 이름이 붙었는데 왜....??
    result = doca_kvdev_set_nguid(kvdev_, nguid_bytes);
    if (result != DOCA_SUCCESS) {
        NIXL_ERROR << "Failed to set DOCA KV device NGUID: " << doca_error_get_descr(result);
        cleanupDocaResources();
        return NIXL_ERR_BACKEND;
    }

    // ▶ [7] 가동. 이 시점부터 장치가 명령을 받을 수 있다.
    result = doca_kvdev_start(kvdev_);
    if (result != DOCA_SUCCESS) {
        NIXL_ERROR << "Failed to start DOCA KV device: " << doca_error_get_descr(result);
        cleanupDocaResources();
        return NIXL_ERR_BACKEND;
    }

    NIXL_INFO << "DOCA KV device initialized successfully";

    // ▶▶ [8] 여기부터 세 번의 "능력 조회". start 이후에만 물어볼 수 있다.

    //사용자가 제시한 동시 task 실행 수를, 장치가 실제로 할 수 있는 만큼으로 조정한다.
    uint32_t dev_max_tasks = 0;
    result = doca_kvdev_get_max_tasks(kvdev_, &dev_max_tasks);
    if (result != DOCA_SUCCESS) {
        NIXL_ERROR << "doca_kvdev_get_max_tasks failed: " << doca_error_get_descr(result);
        cleanupDocaResources();
        return NIXL_ERR_BACKEND;
    }
    if (dev_max_tasks > 0 && numTasks_ > dev_max_tasks) {
        NIXL_INFO << "Clamping num_tasks from " << numTasks_ << " to device max " << dev_max_tasks;
        numTasks_ = dev_max_tasks;
    }

    // Plugin stores keys inline in a fixed-size array sized to the current
    // DOCA spec (DOCA_MEMOS_MAX_OBJECT_KEY_LEN). If the device ever reports a
    // larger maximum, that capacity must be revisited.
    // ▶▶ 이 플러그인은 16바이트 키를 사용. 
    //        장치가 사용하는 키 길이가 16바이트보다 길다? -> 장치의 일부 영역에 접근하지 못한다는 뜻. -> 에러 반환 결정.
    uint16_t dev_max_key_len = 0;
    result = doca_kvdev_get_max_key_len(kvdev_, &dev_max_key_len);
    if (result != DOCA_SUCCESS) {
        NIXL_ERROR << "doca_kvdev_get_max_key_len failed: " << doca_error_get_descr(result);
        cleanupDocaResources();
        return NIXL_ERR_BACKEND;
    }
    if (dev_max_key_len > DOCA_MEMOS_MAX_OBJECT_KEY_LEN) {
        NIXL_ERROR << "Device max key length " << dev_max_key_len << " exceeds plugin capacity "
                   << DOCA_MEMOS_MAX_OBJECT_KEY_LEN;
        cleanupDocaResources();
        return NIXL_ERR_BACKEND;
    }

    // CMX 쪽 BLUEFIELD에서 돌고 있는 DOCA MEMOS가, CMX의 SSD들을 미리 일정 크기의 볼륨으로 쪼개 두었다.
    //why? -> 어짜피 특정 모델이 K,V저장을 위해 쓰는 블록의 크기는 일정. 그 블록이 들어갈 정도로 미리 쪼개둬도 됨.
    //즉, hash(tokens) 키 하나당 최대 저장 가능한 value 크기는 볼륨 한 칸 크기. 그 한 칸 크기를 알아오는 것.
    result = doca_kvdev_get_max_value_len(kvdev_, &maxValueLen_);
    if (result != DOCA_SUCCESS) {
        NIXL_ERROR << "doca_kvdev_get_max_value_len failed: " << doca_error_get_descr(result);
        cleanupDocaResources();
        return NIXL_ERR_BACKEND;
    }
    NIXL_INFO << "Device max value length: " << maxValueLen_ << " bytes";

    return NIXL_SUCCESS;
}

// ▶▶ enableProgTh(agent 생성 시 설정)에 따라 두 엔진 중 하나를 만든다.
//    이 선택이 이후 postXfer / checkXfer 의 동작을 완전히 바꾼다.
nixl_status_t
nixlDocaMemosEngine::createProgressEngine(const nixlBackendInitParams *init_params) {
    try {
        if (init_params->enableProgTh) {
            if (init_params->pthrDelay == 0) {
                NIXL_WARN << "Progress-thread delay is 0us; thread will busy-spin";
            }
            // ▶ 전용 스레드가 DOCA API 의 유일한 호출자가 된다. 호출자 쪽은 사실상 lock-free.
            progressEngine_ = std::make_unique<nixlThreadedProgressEngine>(
                nvmeKvdev_.get(),
                numTasks_,
                maxValueLen_,
                std::chrono::microseconds(init_params->pthrDelay));
        } else {
            // ▶ 스레드 없음. checkXfer() 를 부르는 호출자가 직접 진행시킨다.
            progressEngine_ = std::make_unique<nixlNoThreadProgressEngine>(
                nvmeKvdev_.get(), numTasks_, maxValueLen_);
        }
    }
    catch (const std::exception &e) {
        NIXL_ERROR << "Failed to create progress engine: " << e.what();
        return NIXL_ERR_BACKEND;
    }

    // ▶ 생성자에서 던지지 않고 플래그로 실패를 알리는 방식이라 여기서 확인해야 한다.
    if (progressEngine_->hasInitError()) {
        NIXL_ERROR << "Progress engine initialization failed";
        return NIXL_ERR_BACKEND;
    }

    return NIXL_SUCCESS;
}

// ============================================================================
// Constructor — 백엔드가 태어나는 곳
// ============================================================================
// ▶▶▶ 실패하면 throw 한다. constructor 에는 반환값이 없기 때문.
//     NIXL 이 이 예외를 잡아 nullptr 로 바꿔 사용자에게 "생성 실패" 로 알린다
//     (backend_plugin.h 의 createEngine 참고).
//     덕분에 "반쯤 초기화된 백엔드" 가 존재할 수 없다.
nixlDocaMemosEngine::nixlDocaMemosEngine(const nixlBackendInitParams *init_params)
    : nixlBackendEngine(init_params) {

    NIXL_INFO << "Initializing DOCA KV backend";

    if (parseInitParams(init_params->customParams) != NIXL_SUCCESS) {
        throw std::runtime_error("DOCA_MEMOS: failed to parse init params");
    }

    // ▶ device_name 이 필수인 것이 강제되는 유일한 지점.
    if (deviceName_.empty()) {
        NIXL_ERROR << "DOCA KV backend requires 'device_name' parameter to be set";
        NIXL_ERROR << "Example: params[\"device_name\"] = \"/dev/nvme0n1\"";
        throw std::runtime_error("DOCA_MEMOS: device_name is required");
    }

    NIXL_INFO << "Initializing DOCA KV with device_name=" << deviceName_
              << ", num_tasks=" << numTasks_ << ", nguid=" << nguid_
              << ", query_mem_mode=" << (queryMemAssumeSuccess_ ? "assume_success" : "actual")
              << ", ignore_read_not_found=" << (ignoreReadNotFound_ ? "true" : "false");

    // ▶ 순서가 중요: initDocaDevice 가 numTasks_/maxValueLen_ 을 확정해야
    //   createProgressEngine 이 그 값으로 task pool 을 만들 수 있다.
    if (initDocaDevice() != NIXL_SUCCESS) {
        throw std::runtime_error("DOCA_MEMOS: failed to initialize DOCA device");
    }

    NIXL_INFO << "Creating progress engine";
    if (createProgressEngine(init_params) != NIXL_SUCCESS) {
        throw std::runtime_error("DOCA_MEMOS: failed to create progress engine");
    }

    NIXL_INFO << "DOCA KV backend initialized successfully";
}

// ▶▶ 정확히 생성의 역순. progress engine 이 장치 위에 얹혀 있으므로 먼저 내린다.
nixlDocaMemosEngine::~nixlDocaMemosEngine() {
    NIXL_INFO << "Destroying DOCA KV backend";

    progressEngine_.reset();   // ▶ 스레드 정지 + 남은 작업 drain

    cleanupDocaResources();    // ▶ 장치 stop → destroy

    NIXL_INFO << "DOCA KV backend destroyed";
}

// ============================================================================
// ③ 키 해석 — hex 우선, raw 차선, devId 최후
// ============================================================================
// ▶▶ hex 문자열 해석 전용. 실패하면 false 를 내고 호출자가 다음 방법을 시도한다.
//    조건: 비어있지 않고 / 32자 이하 / 짝수 길이 / 전부 유효한 hex
bool
nixlDocaMemosEngine::convertToMemosKey(const std::string &meta_info, docaMemosKey &key) {
    if (meta_info.empty()) {
        return false;
    }
    if (meta_info.size() > DOCA_MEMOS_MAX_OBJECT_KEY_LEN * 2) {
        return false;
    }
    if (meta_info.size() & 1) {   // ▶ 홀수 길이면 hex 로 볼 수 없다
        return false;
    }
    auto minfo = meta_info.data();
    key.keyLen = meta_info.size() / 2;
    for (uint32_t i = 0; i < key.keyLen; i++) {
        unsigned val = 0;
        auto [ptr, ec] = std::from_chars(minfo + i * 2, minfo + i * 2 + 2, val, 16);
        if (ec != std::errc{} || ptr != minfo + i * 2 + 2) {
            return false;   // ▶ hex 가 아니면 실패. keyLen 은 이미 써놨지만 호출자가 덮어쓴다
        }
        key.key[i] = static_cast<uint8_t>(val);
    }
    return true;
}

// Resolves meta_info to a device key via one of three paths. The hex-first /
// raw-fallback behaviour means the same meta_info string may take different
// paths depending on its contents, so log the chosen path at DEBUG level.
// ▶▶▶ 키 해석 3단계. README 의 규칙이 그대로 코드가 된 것.
//     주의: 가이드의 OBJ descriptor 표는 devID 를 키로, str 을 확장 키로 규정하는데
//     여기서는 우선순위가 뒤집혀 있다(metaInfo 우선, 비었을 때만 devId).
//     OBJ 용 공용 헬퍼가 없어 백엔드마다 자기 규약을 만든 결과다.
bool
nixlDocaMemosEngine::resolveMemosKey(uint64_t dev_id,
                                     const std::string &meta_info,
                                     docaMemosKey &key) {
    // ▶ [3단계] 비어 있으면 devId 8바이트를 그대로 키로 쓴다.
    if (meta_info.empty()) {
        key.keyLen = sizeof(dev_id);
        memcpy(key.key, &dev_id, key.keyLen);
        NIXL_DEBUG << "resolveMemosKey: empty meta_info, using dev_id bytes";
        return true;
    }
    // ▶ [1단계] hex 로 해석되면 디코드해서 사용. "a3f9c2..." 같은 프리픽스 해시가 이 경로.
    if (convertToMemosKey(meta_info, key)) {
        NIXL_DEBUG << "resolveMemosKey: parsed meta_info as hex (" << key.keyLen << " bytes)";
        return true;
    }
    // ▶ [2단계] hex 가 아니면 문자열 바이트를 그대로 키로. "ckpt_0001" 같은 것.
    if (meta_info.size() <= DOCA_MEMOS_MAX_OBJECT_KEY_LEN) {
        key.keyLen = meta_info.size();
        memcpy(key.key, meta_info.data(), key.keyLen);
        NIXL_DEBUG << "resolveMemosKey: using meta_info as raw bytes (" << key.keyLen << " bytes)";
        return true;
    }
    return false;   // ▶ 16바이트 초과 문자열은 실패
}

// ============================================================================
// ④ registerMem — 키가 확정되는 지점
// ============================================================================
// ▶▶▶ DOCA API 를 한 번도 부르지 않는다. 장치를 건드리지 않는다.
//     KV 캐시는 새 키가 계속 생기므로 이 함수가 워크로드 내내 반복 호출되는데,
//     장치 왕복이 없어서 값싸다. 이게 중요한 설계 선택이다.
nixl_status_t
nixlDocaMemosEngine::registerMem(const nixlBlobDesc &mem,//descriptor 하나(addr, len, devId, metaInfo(이걸로 키를 만듦))
                                 const nixl_mem_t &nixl_mem,//memory type(OBJ or DRAM)
                                 nixlBackendMD *&out) {//결과 출력 자리 
    auto supported_mems = getSupportedMems();
    if (std::find(supported_mems.begin(), supported_mems.end(), nixl_mem) == supported_mems.end()) {
        return NIXL_ERR_NOT_SUPPORTED;
    }

    if (nixl_mem == OBJ_SEG) {//키 관련 registerMem이다.
        // ▶ 키를 확정해 상자에 담는다. 이 상자의 주소가 agent 로 넘어가 보관된다.
        docaMemosKey key; //16바이트 키, 현재 keyLen =0
        if (!resolveMemosKey(mem.devId, mem.metaInfo, key)) {
            NIXL_ERROR << "Failed to convert metaInfo to docaMemosKey: " << mem.metaInfo;
            return NIXL_ERR_INVALID_PARAM;
        }//resolveMemosKey()가, 재료를 가지고 키를 만든다. keylen은 재료의 길이(Byte)
        //1. metainfo가 비어있다면 -> devId가 키가 된다. 
        //2. metainfo가 hex수열이라면 -> metainfo를 두 글자씩 끊어 디코드.->이게 정상 
        //3. metainfo가 문자열이라면 -> 글자를 그대로 바이트로 디코드.

        auto kv_md = std::make_unique<nixlDocaMemosMetadata>(nixl_mem, mem.devId, key);
        //키를 보관하기 위한 상자를 만듦. 
        NIXL_DEBUG << "Registered OBJ_SEG memory with devId: " << mem.devId;
        out = kv_md.release();   // ▶ out = agent가 보관하는 key 상자의 주소
    } else {//host DDR 주소, 크기는 어짜피 나중에 전송용 descriptor에 다시 실려 옴.
        out = nullptr;
    }

    return NIXL_SUCCESS;
}

// ▶ 상자를 만든 것이 registerMem 이므로 부수는 것도 짝인 여기.
//   loadLocalMD 가 같은 포인터를 냈으므로 unloadMD 는 손대면 안 된다(double free).
nixl_status_t
nixlDocaMemosEngine::deregisterMem(nixlBackendMD *meta) {
    nixlDocaMemosMetadata *kv_md = static_cast<nixlDocaMemosMetadata *>(meta);
    if (kv_md) {
        NIXL_DEBUG << "Deregistered memory with devId: " << kv_md->devId;
    }
    delete kv_md;   // ▶ nullptr(DRAM_SEG) 여도 안전

    return NIXL_SUCCESS;
}

// ▶▶ 유일하게 자발적으로 구현한 optional method.
//    기본 모드에서는 장치에 묻지 않고 전부 성공을 반환한다 —
//    "retrieve 자체가 exist 검사이니 미리 묻지 말라" 는 설계 철학의 구현.
nixl_status_t
nixlDocaMemosEngine::queryMem(const nixl_reg_dlist_t &descs,
                              std::vector<nixl_query_resp_t> &resp) const {
    if (queryMemAssumeSuccess_) {
        resp.reserve(descs.descCount());
        for (int i = 0; i < descs.descCount(); i++) {
            resp.emplace_back(nixl_b_params_t{});   // ▶ 빈 값이지만 has_value() 는 true
        }
        return NIXL_SUCCESS;
    }

    // ▶ actual 모드는 진짜 EXIST 를 발행한다. 동기 호출이라 지연이 전부 노출된다.
    return progressEngine_->queryMem(descs, resp);
}

// ============================================================================
// ⑤ prepXfer — descriptor 쌍에서 재료를 뽑아 짝지어 둔다
// ============================================================================
// ▶▶▶ DOCA 를 부르지 않는다. 전송도 시작하지 않는다.
//     하는 일: 검사 → 핸들 생성 → local 에서 iovec, remote 에서 키를 뽑아 배열에 저장.
nixl_status_t
nixlDocaMemosEngine::prepXfer(const nixl_xfer_op_t &operation, //read or write
                              const nixl_meta_dlist_t &local, //local descriptor list
                              const nixl_meta_dlist_t &remote, //remote descriptor list
                              const std::string &remote_agent, //상대 agent(여기선 자신)
                              nixlBackendReqH *&handle, //결과 출력(handle 반환)
                              const nixl_opt_b_args_t *opt_args) const {
    if (!isValidPrepXferParams(operation, local, remote, remote_agent, localAgent)) {
        return NIXL_ERR_INVALID_PARAM;
    }

    int desc_count = local.descCount();
    if (desc_count == 0) {
        NIXL_ERROR << "Empty descriptor lists";
        return NIXL_ERR_INVALID_PARAM;
    }

    // ▶▶ 개수가 다르면 거부. local[i] 와 remote[i] 가 인덱스로 1:1 대응해야 하기 때문.
    if (remote.descCount() != desc_count) {
        NIXL_ERROR << "Descriptor count mismatch: local=" << desc_count
                   << " remote=" << remote.descCount();
        return NIXL_ERR_INVALID_PARAM;
    }

    // ▶ 핸들 생성. descriptor 몇개던 핸들 하나. descriptor 쌍 개수 = 앞으로 만들 DOCA task 장수. => 핸들 안의 totalTask_에 저장.
    auto req_h = std::make_unique<nixlDocaMemosBackendReqH>(desc_count);
    req_h->valueIovecs_.resize(desc_count);
    req_h->taskContexts_.resize(desc_count);
    // ▶ 설정값을 핸들에 복사해 둔다. 콜백이 엔진을 참조하지 않고 판단할 수 있게.
    if (operation == NIXL_READ) {
        req_h->ignoreNotFound_ = ignoreReadNotFound_;
    }

    // ▶▶ local 쪽: 주소와 길이를 iovec 으로. 여기 담기는 주소는 유저 공간 가상 주소이고,
    //    물리 주소 변환은 훨씬 아래 커널 NVMe 드라이버가 한다.
    for (int i = 0; i < desc_count; i++) {
        const auto &local_desc = local[i];
        req_h->valueIovecs_[i] = {reinterpret_cast<void *>(local_desc.addr), local_desc.len};
    }//valueIovecs에 담기는 값은 local_des.addr과 완전 동일. 다만 DOCA가 iovec 포인터 형식 요구. 그래서 handle 내 공간에 iovec 형식으로 저장하고, 포인터 제공.

    req_h->objectKeys_.clear();
    req_h->objectKeys_.reserve(desc_count);

    // ▶▶ remote 쪽: metadataP 포인터를 따라가 상자에서 키를 꺼낸다.
    //    주의 — remote_desc 의 addr 과 len 은 읽지 않는다.
    //    가이드 OBJ 표는 addr 을 offset 으로 규정하지만 이 플러그인은 무시한다
    //    (= KV 값의 부분 읽기 불가). 값 길이는 전적으로 local 쪽에서 온다.
    for (int i = 0; i < desc_count; i++) {
        const auto &remote_desc = remote[i];
        auto *kv_md = static_cast<nixlDocaMemosMetadata *>(remote_desc.metadataP);
        if (!kv_md) {
            NIXL_ERROR << "No metadata for remote descriptor at index " << i;
            return NIXL_ERR_INVALID_PARAM;   // ▶ req_h 는 unique_ptr 이라 자동 해제
        }
        req_h->objectKeys_.push_back(kv_md->objKey);   // ▶ 키를 값으로 복사해 둔다
    }// remote descriptor에 있는 metadataP가 아까 agent에 저장해뒀던 key 주소임.

    handle = req_h.release();   // ▶ 소유권을 NIXL 코어로

    NIXL_DEBUG << "Prepared transfer: operation=" << operation << ", " << desc_count << " tasks";

    return NIXL_SUCCESS;
}

// ============================================================================
// ⑥ postXfer — progress engine 에 위임
// ============================================================================
// ▶▶ 이 함수 자체는 거의 아무것도 하지 않는다. 실제 제출은 엔진이 한다.
//    threaded 면 큐에 넣고 즉시 반환, no-thread 면 그 자리에서 제출.
nixl_status_t
nixlDocaMemosEngine::postXfer(const nixl_xfer_op_t &operation,
                              const nixl_meta_dlist_t &local,
                              const nixl_meta_dlist_t &remote,
                              const std::string &remote_agent,
                              nixlBackendReqH *&handle,
                              //prepXfer에서 만든 handle 그대로 다시 옴.
                              const nixl_opt_b_args_t *opt_args) const {
    auto req_h = static_cast<nixlDocaMemosBackendReqH *>(handle);//handle 형 DOCAMEMOS용으로 바꿈.
    if (!req_h) {
        return NIXL_ERR_INVALID_PARAM;
    }

    if (operation != NIXL_WRITE && operation != NIXL_READ) {
        NIXL_ERROR << "Unsupported operation type: " << operation;
        return NIXL_ERR_INVALID_PARAM;
    }
    return progressEngine_->postXfer(req_h, operation, local, remote);
    //progressEngine 호출 -> post 실행
}

// ============================================================================
// ⑦ checkXfer / releaseReqH
// ============================================================================
// ▶ 엔진이 카운터를 보고 답한다. no-thread 모드에서는 이 안에서 폴링까지 한다.
nixl_status_t
nixlDocaMemosEngine::checkXfer(nixlBackendReqH *handle) const {
    auto req_h = static_cast<nixlDocaMemosBackendReqH *>(handle);//handle 형변환
    return progressEngine_->checkXfer(req_h);//progressEngine 호출 -> check 진행
}

// ▶▶ 즉시 반환한다(block 하지 않는다). 진행 중인 task 가 있으면
//    핸들에 "취소됨" 표시만 하고 실제 삭제는 마지막 콜백이 온 뒤로 미룬다.
//    가이드의 "releaseXferReq 는 non-blocking 이어야 한다" 규약을 지킨 방식.
nixl_status_t
nixlDocaMemosEngine::releaseReqH(nixlBackendReqH *handle) const {
    auto req_h = static_cast<nixlDocaMemosBackendReqH *>(handle);//handle 형변환
    if (!req_h) {
        return NIXL_ERR_INVALID_PARAM;
    }

    NIXL_DEBUG << "Releasing request handle with " << req_h->totalTasks_ << " tasks";

    progressEngine_->cancelRequest(req_h);//progressEngine 호출 -> release 호출
    return NIXL_SUCCESS;
}
