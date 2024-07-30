/*
 * Copyright 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef BTIF_RC_H
#define BTIF_RC_H

#include "bta_av_api.h"

class RawAddress;

void btif_rc_handler(tBTA_AV_EVT event, tBTA_AV* p_data);
uint8_t btif_rc_get_connected_peer_handle(const RawAddress& peer_addr);
void btif_rc_check_handle_pending_play(const RawAddress& peer_addr,
                                       bool bSendToApp);
bool btif_rc_is_connected_peer(const RawAddress& peer_addr);

extern bool btif_av_is_source_enabled(void);
extern bool btif_av_both_enable(void);
extern bool btif_av_src_sink_coexist_enabled(void);
extern bool btif_av_is_sink_enabled(void);
extern bool btif_av_is_connected_addr(const RawAddress& peer_address);
extern bool btif_av_peer_is_connected_sink(const RawAddress& peer_address);
extern bool btif_av_peer_is_connected_source(const RawAddress& peer_address);
extern bool btif_av_peer_is_sink(const RawAddress& peer_address);
extern bool btif_av_peer_is_source(const RawAddress& peer_address);


#endif  // BTIF_RC_H
