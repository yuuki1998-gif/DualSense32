/******************************************************************************
 *
 *  Copyright (C) 1999-2012 Broadcom Corporation
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

/* Modified 2026 by yuuki1998 for DualSense32: trimmed to the declarations
 * this library calls. Not the complete upstream header. */

#ifndef HCIDEFS_H
#define HCIDEFS_H

#include "stack/bt_types.h"

/* Define the extended flow specification fields used by AMP */
typedef struct {
  uint8_t id;
  uint8_t stype;
  uint16_t max_sdu_size;
  uint32_t sdu_inter_time;
  uint32_t access_latency;
  uint32_t flush_timeout;
} tHCI_EXT_FLOW_SPEC;

#endif
