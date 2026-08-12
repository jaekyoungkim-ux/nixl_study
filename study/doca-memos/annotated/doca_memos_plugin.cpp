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
// 스터디용 주석본. 원본: PR #1717 src/plugins/doca_memos/doca_memos_plugin.cpp
// 코드는 원본 그대로이며, "▶" 로 시작하는 줄만 추가된 한글 주석이다.
// 원본 행 번호와 어긋나므로, 원본을 인용할 때는 원본 파일을 볼 것.
//
// 51줄짜리 등록 보일러플레이트. plugin manager 를 위한 자기소개가 전부다.
// ==========================================================================
#include "nixl_types.h"
#include "doca_memos_backend.h"
#include "backend/backend_plugin.h"


// ▶ 템플릿에 엔진 타입을 끼우면 plugin manager 용 함수들이 자동 생성된다.
using doca_memos_plugin_t = nixlBackendPluginCreator<nixlDocaMemosEngine>;

namespace {
constexpr const char *kPluginName = "DOCA_MEMOS";
constexpr const char *kPluginVersion = "0.1.0";
} // namespace

// Custom backend parameters accepted by this plugin:
//   device_name           - Path to the NVMe KV device (e.g. "/dev/nvme0n1"). Required.
//   num_tasks             - Max in-flight DOCA tasks (default 8192, clamped to device max).
//   nguid                 - 32 hex-char namespace GUID for key scoping (default all-zeros).
//   ignore_read_not_found - "true" to treat key-not-found on retrieve as success (default false).
//   query_mem_mode        - "assume_success" (default) or "actual" (issues EXIST tasks).


// ▶▶ 정적/동적 두 갈래. 정적이면 NIXL 라이브러리에 내장되고,
//    동적이면 libplugin_DOCA_MEMOS.so 로 빌드돼 실행 중 로드된다.
//    create() 인자 5개: API 버전 / 이름 / 버전 / **백엔드 옵션** / 지원 memory type
//
//    네 번째 인자가 {} — get_backend_options() 가 빈 map 을 반환한다는 뜻이다.
//    device_name 이 필수인데도 런타임에 그 사실을 조회할 방법이 없다.
//    (UCX/GUSLI 등은 여기를 채운다)
#ifdef STATIC_PLUGIN_DOCA_MEMOS
nixlBackendPlugin *
createStaticDOCAMemosPlugin() {
    return doca_memos_plugin_t::create(
        NIXL_PLUGIN_API_VERSION, kPluginName, kPluginVersion, {}, {DRAM_SEG, OBJ_SEG});
}
#else
extern "C" NIXL_PLUGIN_EXPORT nixlBackendPlugin *
nixl_plugin_init() {
    return doca_memos_plugin_t::create(
        NIXL_PLUGIN_API_VERSION, kPluginName, kPluginVersion, {}, {DRAM_SEG, OBJ_SEG});
}

extern "C" NIXL_PLUGIN_EXPORT void
nixl_plugin_fini() {}
#endif
