/******************************************************************************
 *
 *  Copyright 2014 Google, Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#pragma once

#include "btif_uid.h"

#include <hardware/bt_sock.h>

#define BTIF_SOCK_CONNECTION_EVT 0    /* CONNECTION Event */

const btsock_interface_t* btif_sock_get_interface(void);

bt_status_t btif_sock_init(uid_set_t* uid_set);

/*******************************************************************************
 *
 * Function         btif_dm_upstreams_cback
 *
 * Description      Executes UPSTREAMS events in btif context
 *
 * Returns          void
 *
 ******************************************************************************/
void btif_obex_upstreams_evt(uint16_t event, char* p_param);
void btif_sock_cleanup(void);
