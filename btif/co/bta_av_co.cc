/******************************************************************************
 *
 *  Copyright 2004-2012 Broadcom Corporation
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

/******************************************************************************
 *
 *  This is the advanced audio/video call-out function implementation for
 *  BTIF.
 *
 ******************************************************************************/
#define ADDRESS_TO_LOGGABLE_CSTR(addr) addr.ToString().c_str()

#include <mutex>

#include "btif/include/bta_av_co.h"
#include <base/bind.h>
#include <base/logging.h>
#include <string.h>

#include "bt_target.h"

#include "a2dp_api.h"
#include "a2dp_sbc.h"
#include "bta_av_api.h"
#include "bta_av_ci.h"
#include "bta_av_co.h"
#include "bta_sys.h"

#include "btif_av.h"
#include "btif_av_co.h"
#include "btif_util.h"
#include "osi/include/osi.h"
#include "osi/include/properties.h"

// SCMS-T protect info
const uint8_t bta_av_co_cp_scmst[AVDT_CP_INFO_LEN] = {0x02, 0x02, 0x00};

// Control block instance
#if (BTA_AV_CO_CP_SCMS_T == TRUE)
static const bool kContentProtectEnabled = true;
#else
static const bool kContentProtectEnabled = false;
#endif
static BtaAvCo bta_av_co_cb(kContentProtectEnabled, new BtaAvCoPeerCache());

void BtaAvCoState::setActivePeer(BtaAvCoPeer* peer) { active_peer_ = peer; }

BtaAvCoPeer* BtaAvCoState::getActivePeer() const { return active_peer_; }

uint8_t* BtaAvCoState::getCodecConfig() { return codec_config_; }

void BtaAvCoState::setCodecConfig(const uint8_t* codec_config) {
  memcpy(codec_config_, codec_config, AVDT_CODEC_SIZE);
}

void BtaAvCoState::clearCodecConfig() { memset(codec_config_, 0, AVDT_CODEC_SIZE); }

void BtaAvCoState::Reset() {
  active_peer_ = nullptr;
  // TODO: b/339264791. Remove the method & usage.
  //  Pre-submit complains about codec_config not initialized.
  clearCodecConfig();
}

void BtaAvCo::Init(
    const std::vector<btav_a2dp_codec_config_t>& codec_priorities) {
  APPL_TRACE_DEBUG("%s", __func__);

  std::lock_guard<std::recursive_mutex> lock(peer_cache_->codec_lock_);

  // Reset the control block
  Reset();
  peer_cache_->Init(codec_priorities/*, supported_codecs*/);

  // Gather the supported codecs from the first peer context;
  // all contexes should be identical.
  /*supported_codecs->clear();
  for (auto* codec_config : peer_cache_->peers_[0].GetCodecs()->orderedSourceCodecs()) {
    auto& codec_info = supported_codecs->emplace_back();
    codec_info.codec_type = codec_config->codecIndex();
    codec_info.codec_id = codec_config->codecId();
    codec_info.codec_name = codec_config->name();
  }*/
}

void BtaAvCo::Reset() {
  bta_av_legacy_state_.Reset();
  bta_av_source_state_.Reset();
  bta_av_sink_state_.Reset();

  content_protect_flag_ = 0;

  if (ContentProtectEnabled()) {
    SetContentProtectFlag(AVDT_CP_SCMS_COPY_NEVER);
  } else {
    SetContentProtectFlag(AVDT_CP_SCMS_COPY_FREE);
  }
  peer_cache_->Reset();
}

bool BtaAvCo::IsSupportedCodec(btav_a2dp_codec_index_t codec_index) {
  // All peer state is initialized with the same local codec config,
  // hence we check only the first peer.
  A2dpCodecs* codecs = peer_cache_->peers_[0].GetCodecs();
  CHECK(codecs != nullptr);
  return codecs->isSupportedCodec(codec_index);
}

A2dpCodecConfig* BtaAvCo::GetActivePeerCurrentCodec() {
  std::lock_guard<std::recursive_mutex> lock(peer_cache_->codec_lock_);

  BtaAvCoState* reference_state = nullptr;
  //if (IS_FLAG_ENABLED(a2dp_concurrent_source_sink)) {
  if(1) {
    reference_state = &bta_av_source_state_;
  } else {
    reference_state = &bta_av_legacy_state_;
  }
  BtaAvCoPeer* active_peer = reference_state->getActivePeer();
  if (active_peer == nullptr || active_peer->GetCodecs() == nullptr) {
    return nullptr;
  }
  return active_peer->GetCodecs()->getCurrentCodecConfig();
}

A2dpCodecConfig* BtaAvCo::GetPeerCurrentCodec(const RawAddress& peer_address) {
  std::lock_guard<std::recursive_mutex> lock(peer_cache_->codec_lock_);

  BtaAvCoPeer* peer = peer_cache_->FindPeer(peer_address);
  if (peer == nullptr || peer->GetCodecs() == nullptr) {
    return nullptr;
  }
  return peer->GetCodecs()->getCurrentCodecConfig();
}

void BtaAvCo::ProcessDiscoveryResult(tBTA_AV_HNDL bta_av_handle,
                                     const RawAddress& peer_address,
                                     uint8_t num_seps, uint8_t num_sinks,
                                     uint8_t num_sources, uint16_t uuid_local) {
  APPL_TRACE_DEBUG(
      "%s: peer %s bta_av_handle:0x%x num_seps:%d num_sinks:%d num_sources:%d",
      __func__, peer_address.ToString().c_str(), bta_av_handle, num_seps,
      num_sinks, num_sources);

  // Find the peer
  BtaAvCoPeer* p_peer = peer_cache_->FindPeerAndUpdate(bta_av_handle, peer_address);
  if (p_peer == nullptr) {
    APPL_TRACE_ERROR(
        "%s: could not find peer entry for bta_av_handle 0x%x peer %s",
        __func__, bta_av_handle, peer_address.ToString().c_str());
    return;
  }

  /* Sanity check : this should never happen */
  if (p_peer->opened) {
    APPL_TRACE_ERROR("%s: peer %s already opened", __func__,
                     peer_address.ToString().c_str());
  }

  /* Copy the discovery results */
  p_peer->addr = peer_address;
  p_peer->num_sinks = num_sinks;
  p_peer->num_sources = num_sources;
  p_peer->num_seps = num_seps;
  p_peer->num_rx_sinks = 0;
  p_peer->num_rx_sources = 0;
  p_peer->num_sup_sinks = 0;
  p_peer->num_sup_sources = 0;
  if (uuid_local == UUID_SERVCLASS_AUDIO_SINK) {
    p_peer->uuid_to_connect = UUID_SERVCLASS_AUDIO_SOURCE;
  } else if (uuid_local == UUID_SERVCLASS_AUDIO_SOURCE) {
    p_peer->uuid_to_connect = UUID_SERVCLASS_AUDIO_SINK;
  }
}

tA2DP_STATUS BtaAvCo::ProcessSourceGetConfig(
    tBTA_AV_HNDL bta_av_handle, const RawAddress& peer_address,
    uint8_t* p_codec_info, uint8_t* p_sep_info_idx, uint8_t seid,
    uint8_t* p_num_protect, uint8_t* p_protect_info) {
  APPL_TRACE_DEBUG("%s: peer %s bta_av_handle:0x%x codec:%s seid:%d", __func__,
                   peer_address.ToString().c_str(), bta_av_handle,
                   A2DP_CodecName(p_codec_info), seid);
  APPL_TRACE_DEBUG("%s: num_protect:0x%02x protect_info:0x%02x%02x%02x",
                   __func__, *p_num_protect, p_protect_info[0],
                   p_protect_info[1], p_protect_info[2]);
  APPL_TRACE_DEBUG("%s: codec: %s", __func__,
                   A2DP_CodecInfoString(p_codec_info).c_str());

  // Find the peer
  BtaAvCoPeer* p_peer = peer_cache_->FindPeerAndUpdate(bta_av_handle, peer_address);
  if (p_peer == nullptr) {
    APPL_TRACE_ERROR(
        "%s: could not find peer entry for bta_av_handle 0x%x peer %s",
        __func__, bta_av_handle, peer_address.ToString().c_str());
    return A2DP_FAIL;
  }
  APPL_TRACE_DEBUG("%s: peer(o=%d, n_sinks=%d, n_rx_sinks=%d, n_sup_sinks=%d)",
                   __func__, p_peer->opened, p_peer->num_sinks,
                   p_peer->num_rx_sinks, p_peer->num_sup_sinks);

  p_peer->num_rx_sinks++;

  // Check the peer's Sink codec
  if (A2DP_IsPeerSinkCodecValid(p_codec_info)) {
    // If there is room for a new one
    if (p_peer->num_sup_sinks < BTA_AV_CO_NUM_ELEMENTS(p_peer->sinks)) {
      BtaAvCoSep* p_sink = &p_peer->sinks[p_peer->num_sup_sinks++];

      APPL_TRACE_DEBUG("%s: saved caps[%x:%x:%x:%x:%x:%x]", __func__,
                       p_codec_info[1], p_codec_info[2], p_codec_info[3],
                       p_codec_info[4], p_codec_info[5], p_codec_info[6]);

      memcpy(p_sink->codec_caps, p_codec_info, AVDT_CODEC_SIZE);
      p_sink->sep_info_idx = *p_sep_info_idx;
      p_sink->seid = seid;
      p_sink->num_protect = *p_num_protect;
      memcpy(p_sink->protect_info, p_protect_info, AVDT_CP_INFO_LEN);
    } else {
      APPL_TRACE_ERROR("%s: peer %s : no more room for Sink info", __func__,
                       p_peer->addr.ToString().c_str());
    }
  }

  // Check if this is the last Sink get capabilities or all supported codec
  // capabilities are retrieved.
  if ((p_peer->num_rx_sinks != p_peer->num_sinks) &&
      (p_peer->num_sup_sinks != BTA_AV_CO_NUM_ELEMENTS(p_peer->sinks))) {
    return A2DP_FAIL;
  }
  APPL_TRACE_DEBUG("%s: last Sink codec reached for peer %s (local %s)",
                   __func__, p_peer->addr.ToString().c_str(),
                   p_peer->acceptor ? "acceptor" : "initiator");

  // Select the Source codec
  const BtaAvCoSep* p_sink = nullptr;
  if (p_peer->acceptor) {
    UpdateAllSelectableSourceCodecs(p_peer);
    if (p_peer->p_sink == nullptr) {
      // Update the selected codec
      p_peer->p_sink = 
          peer_cache_->FindPeerSink(p_peer, A2DP_SourceCodecIndex(p_peer->codec_config), ContentProtectFlag());
    }
    p_sink = p_peer->p_sink;
    if (p_sink == nullptr) {
      APPL_TRACE_ERROR("%s: cannot find the selected codec for peer %s",
                       __func__, p_peer->addr.ToString().c_str());
      return A2DP_FAIL;
    }
  } else {
    if (btif_av_peer_prefers_mandatory_codec(p_peer->addr, A2dpType::kSource)) {
      // Apply user preferred codec directly before first codec selected.
      p_sink = peer_cache_->FindPeerSink(p_peer, BTAV_A2DP_CODEC_INDEX_SOURCE_SBC, ContentProtectFlag());
      if (p_sink != nullptr) {
        APPL_TRACE_API("%s: mandatory codec preferred for peer %s", __func__,
                       p_peer->addr.ToString().c_str());
        btav_a2dp_codec_config_t high_priority_mandatory{
            .codec_type = BTAV_A2DP_CODEC_INDEX_SOURCE_SBC,
            .codec_priority = BTAV_A2DP_CODEC_PRIORITY_HIGHEST,
            // Using default settings for those untouched fields
        };
        uint8_t result_codec_config[AVDT_CODEC_SIZE];
        bool restart_input = false;
        bool restart_output = false;
        bool config_updated = false;
        tA2DP_ENCODER_INIT_PEER_PARAMS peer_params;
        GetPeerEncoderParameters(p_peer->addr, &peer_params);
        p_peer->GetCodecs()->setCodecUserConfig(
            high_priority_mandatory, &peer_params, p_sink->codec_caps,
            result_codec_config, &restart_input, &restart_output,
            &config_updated);
      } else {
        APPL_TRACE_WARNING("%s: mandatory codec not found for peer %s",
                           __func__, p_peer->addr.ToString().c_str());
      }
    }
    p_sink = SelectSourceCodec(p_peer);
    if (p_sink == nullptr) {
      APPL_TRACE_ERROR("%s: cannot set up codec for peer %s", __func__,
                       p_peer->addr.ToString().c_str());
      return A2DP_FAIL;
    }
  }

  // By default, no content protection
  *p_num_protect = 0;
  if (ContentProtectEnabled() && p_peer->ContentProtectActive()) {
    *p_num_protect = AVDT_CP_INFO_LEN;
    memcpy(p_protect_info, bta_av_co_cp_scmst, AVDT_CP_INFO_LEN);
  }

  // If acceptor -> reconfig otherwise reply for configuration
  *p_sep_info_idx = p_sink->sep_info_idx;
  APPL_TRACE_EVENT("%s: peer %s acceptor:%s reconfig_needed:%s", __func__,
                   p_peer->addr.ToString().c_str(),
                   (p_peer->acceptor) ? "true" : "false",
                   (p_peer->reconfig_needed) ? "true" : "false");
  if (p_peer->acceptor) {
    if (p_peer->reconfig_needed) {
      APPL_TRACE_DEBUG("%s: call BTA_AvReconfig(0x%x) for peer %s", __func__,
                       bta_av_handle, p_peer->addr.ToString().c_str());
      BTA_AvReconfig(bta_av_handle, true, p_sink->sep_info_idx,
                     p_peer->codec_config, *p_num_protect, bta_av_co_cp_scmst);
    }
  } else {
    memcpy(p_codec_info, p_peer->codec_config, AVDT_CODEC_SIZE);
  }

  // report this peer selectable codecs after retrieved all its capabilities.
  LOG(INFO) << __func__ << ": retrieved " << +p_peer->num_rx_sinks
            << " capabilities from peer " << p_peer->addr;
  ReportSourceCodecState(p_peer);

  return A2DP_SUCCESS;
}

tA2DP_STATUS BtaAvCo::ProcessSinkGetConfig(tBTA_AV_HNDL bta_av_handle,
                                           const RawAddress& peer_address,
                                           uint8_t* p_codec_info,
                                           uint8_t* p_sep_info_idx,
                                           uint8_t seid, uint8_t* p_num_protect,
                                           uint8_t* p_protect_info) {
  std::lock_guard<std::recursive_mutex> lock(peer_cache_->codec_lock_);

  APPL_TRACE_DEBUG("%s: peer %s bta_av_handle:0x%x codec:%s seid:%d", __func__,
                   peer_address.ToString().c_str(), bta_av_handle,
                   A2DP_CodecName(p_codec_info), seid);
  APPL_TRACE_DEBUG("%s: num_protect:0x%02x protect_info:0x%02x%02x%02x",
                   __func__, *p_num_protect, p_protect_info[0],
                   p_protect_info[1], p_protect_info[2]);
  APPL_TRACE_DEBUG("%s: codec: %s", __func__,
                   A2DP_CodecInfoString(p_codec_info).c_str());

  // Find the peer
  BtaAvCoPeer* p_peer = peer_cache_->FindPeerAndUpdate(bta_av_handle, peer_address);
  if (p_peer == nullptr) {
    APPL_TRACE_ERROR(
        "%s: could not find peer entry for bta_av_handle 0x%x peer %s",
        __func__, bta_av_handle, peer_address.ToString().c_str());
    return A2DP_FAIL;
  }
  APPL_TRACE_DEBUG(
      "%s: peer %s found (o=%d, n_sources=%d, n_rx_sources=%d, "
      "n_sup_sources=%d)",
      __func__, p_peer->addr.ToString().c_str(), p_peer->opened,
      p_peer->num_sources, p_peer->num_rx_sources, p_peer->num_sup_sources);

  p_peer->num_rx_sources++;

  // Check the peer's Source codec
  if (A2DP_IsPeerSourceCodecValid(p_codec_info)) {
    // If there is room for a new one
    if (p_peer->num_sup_sources < BTA_AV_CO_NUM_ELEMENTS(p_peer->sources)) {
      BtaAvCoSep* p_source = &p_peer->sources[p_peer->num_sup_sources++];

      APPL_TRACE_DEBUG("%s: saved caps[%x:%x:%x:%x:%x:%x]", __func__,
                       p_codec_info[1], p_codec_info[2], p_codec_info[3],
                       p_codec_info[4], p_codec_info[5], p_codec_info[6]);

      memcpy(p_source->codec_caps, p_codec_info, AVDT_CODEC_SIZE);
      p_source->sep_info_idx = *p_sep_info_idx;
      p_source->seid = seid;
      p_source->num_protect = *p_num_protect;
      memcpy(p_source->protect_info, p_protect_info, AVDT_CP_INFO_LEN);
    } else {
      APPL_TRACE_ERROR("%s: peer %s : no more room for Source info", __func__,
                       p_peer->addr.ToString().c_str());
    }
  }

  // Check if this is the last Source get capabilities or all supported codec
  // capabilities are retrieved.
  if ((p_peer->num_rx_sources != p_peer->num_sources) &&
      (p_peer->num_sup_sources != BTA_AV_CO_NUM_ELEMENTS(p_peer->sources))) {
    return A2DP_FAIL;
  }
  APPL_TRACE_DEBUG("%s: last Source codec reached for peer %s", __func__,
                   p_peer->addr.ToString().c_str());

  // Select the Sink codec
  const BtaAvCoSep* p_source = nullptr;
  if (p_peer->acceptor) {
    UpdateAllSelectableSinkCodecs(p_peer);
    if (p_peer->p_source == nullptr) {
      // Update the selected codec
      p_peer->p_source =
          peer_cache_->FindPeerSource(p_peer, A2DP_SinkCodecIndex(p_peer->codec_config),ContentProtectFlag());
    }
    p_source = p_peer->p_source;
    if (p_source == nullptr) {
      APPL_TRACE_ERROR("%s: cannot find the selected codec for peer %s",
                       __func__, p_peer->addr.ToString().c_str());
      return A2DP_FAIL;
    }
  } else {
    p_source = SelectSinkCodec(p_peer);
    if (p_source == nullptr) {
      APPL_TRACE_ERROR("%s: cannot set up codec for the peer %s", __func__,
                       p_peer->addr.ToString().c_str());
      return A2DP_FAIL;
    }
  }

  // By default, no content protection
  *p_num_protect = 0;
  if (ContentProtectEnabled() && p_peer->ContentProtectActive()) {
    *p_num_protect = AVDT_CP_INFO_LEN;
    memcpy(p_protect_info, bta_av_co_cp_scmst, AVDT_CP_INFO_LEN);
  }

  // If acceptor -> reconfig otherwise reply for configuration
  *p_sep_info_idx = p_source->sep_info_idx;
  APPL_TRACE_EVENT("%s: peer %s acceptor:%s reconfig_needed:%s", __func__,
                   p_peer->addr.ToString().c_str(),
                   (p_peer->acceptor) ? "true" : "false",
                   (p_peer->reconfig_needed) ? "true" : "false");
  if (p_peer->acceptor) {
    if (p_peer->reconfig_needed) {
      APPL_TRACE_DEBUG("%s: call BTA_AvReconfig(0x%x) for peer %s", __func__,
                       bta_av_handle, p_peer->addr.ToString().c_str());
      BTA_AvReconfig(bta_av_handle, true, p_source->sep_info_idx,
                     p_peer->codec_config, *p_num_protect, bta_av_co_cp_scmst);
    }
  } else {
    memcpy(p_codec_info, p_peer->codec_config, AVDT_CODEC_SIZE);
  }

  return A2DP_SUCCESS;
}

void BtaAvCo::ProcessSetConfig(tBTA_AV_HNDL bta_av_handle,
                               UNUSED_ATTR const RawAddress& peer_address,
                               const uint8_t* p_codec_info,
                               UNUSED_ATTR uint8_t seid, uint8_t num_protect,
                               const uint8_t* p_protect_info,
                               uint8_t t_local_sep, uint8_t avdt_handle) {
  tA2DP_STATUS status = A2DP_SUCCESS;
  uint8_t category = A2DP_SUCCESS;
  bool reconfig_needed = false;

  APPL_TRACE_DEBUG(
      "%s: bta_av_handle=0x%x peer_address=%s seid=%d "
      "num_protect=%d t_local_sep=%d avdt_handle=%d",
      __func__, bta_av_handle, peer_address.ToString().c_str(), seid,
      num_protect, t_local_sep, avdt_handle);
  APPL_TRACE_DEBUG("%s: p_codec_info[%x:%x:%x:%x:%x:%x]", __func__,
                   p_codec_info[1], p_codec_info[2], p_codec_info[3],
                   p_codec_info[4], p_codec_info[5], p_codec_info[6]);
  APPL_TRACE_DEBUG("%s: num_protect:0x%02x protect_info:0x%02x%02x%02x",
                   __func__, num_protect, p_protect_info[0], p_protect_info[1],
                   p_protect_info[2]);
  APPL_TRACE_DEBUG("%s: codec: %s", __func__,
                   A2DP_CodecInfoString(p_codec_info).c_str());

  // Find the peer
  BtaAvCoPeer* p_peer = peer_cache_->FindPeerAndUpdate(bta_av_handle, peer_address);
  if (p_peer == nullptr) {
    APPL_TRACE_ERROR(
        "%s: could not find peer entry for bta_av_handle 0x%x peer %s",
        __func__, bta_av_handle, peer_address.ToString().c_str());
    // Call call-in rejecting the configuration
    bta_av_ci_setconfig(bta_av_handle, A2DP_BUSY, AVDT_ASC_CODEC, 0, nullptr,
                        false, avdt_handle);
    return;
  }

  APPL_TRACE_DEBUG(
      "%s: peer %s found (o=%d, n_sinks=%d, n_rx_sinks=%d, "
      "n_sup_sinks=%d)",
      __func__, p_peer->addr.ToString().c_str(), p_peer->opened,
      p_peer->num_sinks, p_peer->num_rx_sinks, p_peer->num_sup_sinks);

  // Sanity check: should not be opened at this point
  if (p_peer->opened) {
    APPL_TRACE_ERROR("%s: peer %s already in use", __func__,
                     p_peer->addr.ToString().c_str());
  }

  if (num_protect != 0) {
    if (ContentProtectEnabled()) {
      if ((num_protect != 1) ||
          !ContentProtectIsScmst(p_protect_info)) {
        APPL_TRACE_ERROR("%s: wrong CP configuration for peer %s", __func__,
                         p_peer->addr.ToString().c_str());
        status = A2DP_BAD_CP_TYPE;
        category = AVDT_ASC_PROTECT;
      }
    } else {
      // Do not support content protection for the time being
      APPL_TRACE_ERROR("%s: wrong CP configuration for peer %s", __func__,
                       p_peer->addr.ToString().c_str());
      status = A2DP_BAD_CP_TYPE;
      category = AVDT_ASC_PROTECT;
    }
  }

  if (status == A2DP_SUCCESS) {
    bool codec_config_supported = false;

    if (t_local_sep == AVDT_TSEP_SNK) {
      APPL_TRACE_DEBUG("%s: peer %s is A2DP Source", __func__,
                       p_peer->addr.ToString().c_str());
      codec_config_supported = A2DP_IsSinkCodecSupported(p_codec_info);
      if (codec_config_supported) {
        // If Peer is Source, and our config subset matches with what is
        // requested by peer, then just accept what peer wants.
        SaveNewCodecConfig(p_peer, p_codec_info, num_protect, p_protect_info, t_local_sep);
      }
    }
    if (t_local_sep == AVDT_TSEP_SRC) {
      APPL_TRACE_DEBUG("%s: peer %s is A2DP SINK", __func__,
                       p_peer->addr.ToString().c_str());
      // Ignore the restart_output flag: accepting the remote device's
      // codec selection should not trigger codec reconfiguration.
      bool dummy_restart_output = false;
      if ((p_peer->GetCodecs() == nullptr) ||
          !SetCodecOtaConfig(p_peer, p_codec_info, num_protect, p_protect_info,
                             &dummy_restart_output, t_local_sep)) {
        APPL_TRACE_ERROR("%s: cannot set source codec %s for peer %s", __func__,
                         A2DP_CodecName(p_codec_info),
                         p_peer->addr.ToString().c_str());
      } else {
        codec_config_supported = true;
        // Check if reconfiguration is needed
        if (((num_protect == 1) && !p_peer->ContentProtectActive())) {
          reconfig_needed = true;
        }
      }
    }

    // Check if codec configuration is supported
    if (!codec_config_supported) {
      category = AVDT_ASC_CODEC;
      status = A2DP_WRONG_CODEC;
    }
  }

  if (status != A2DP_SUCCESS) {
    APPL_TRACE_DEBUG("%s: peer %s reject s=%d c=%d", __func__,
                     p_peer->addr.ToString().c_str(), status, category);
    // Call call-in rejecting the configuration
    bta_av_ci_setconfig(bta_av_handle, status, category, 0, nullptr, false,
                        avdt_handle);
    return;
  }

  // Mark that this is an acceptor peer
  p_peer->acceptor = true;
  p_peer->reconfig_needed = reconfig_needed;
  APPL_TRACE_DEBUG("%s: peer %s accept reconf=%d", __func__,
                   p_peer->addr.ToString().c_str(), reconfig_needed);
  // Call call-in accepting the configuration
  bta_av_ci_setconfig(bta_av_handle, A2DP_SUCCESS, A2DP_SUCCESS, 0, nullptr,
                      reconfig_needed, avdt_handle);
}

void BtaAvCo::ProcessOpen(tBTA_AV_HNDL bta_av_handle,
                          const RawAddress& peer_address, uint16_t mtu) {
  APPL_TRACE_DEBUG("%s: peer %s bta_av_handle: 0x%x mtu:%d", __func__,
                   peer_address.ToString().c_str(), bta_av_handle, mtu);

  // Find the peer
  BtaAvCoPeer* p_peer = peer_cache_->FindPeerAndUpdate(bta_av_handle, peer_address);
  if (p_peer == nullptr) {
    APPL_TRACE_ERROR(
        "%s: could not find peer entry for bta_av_handle 0x%x peer %s",
        __func__, bta_av_handle, peer_address.ToString().c_str());
    return;
  }
  p_peer->opened = true;
  p_peer->mtu = mtu;

  BtaAvCoState* reference_state = getStateFromPeer(p_peer);
  if (reference_state == nullptr) {
    BTIF_TRACE_WARNING("Invalid bta av state");
    return;
  }
  BtaAvCoPeer* active_peer = reference_state->getActivePeer();
  if (active_peer == nullptr) {
    reference_state->setActivePeer(p_peer);
  }
}

void BtaAvCo::ProcessClose(tBTA_AV_HNDL bta_av_handle,
                           const RawAddress& peer_address) {
  APPL_TRACE_DEBUG("%s: peer %s bta_av_handle: 0x%x", __func__,
                   peer_address.ToString().c_str(), bta_av_handle);
  btif_av_reset_audio_delay();

  // Find the peer
  BtaAvCoPeer* p_peer = peer_cache_->FindPeerAndUpdate(bta_av_handle, peer_address);
  if (p_peer == nullptr) {
    APPL_TRACE_ERROR(
        "%s: could not find peer entry for bta_av_handle 0x%x peer %s",
        __func__, bta_av_handle, peer_address.ToString().c_str());
    return;
  }
  // Reset the active peer
  BtaAvCoState* reference_state = getStateFromPeer(p_peer);
  if (reference_state == nullptr) {
    BTIF_TRACE_WARNING("Invalid bta av state");
    return;
  }
  BtaAvCoPeer* active_peer = reference_state->getActivePeer();
  if (active_peer == p_peer) {
    reference_state->setActivePeer(nullptr);
  }
  // Mark the peer closed and clean the peer info
  p_peer->Init(peer_cache_->codec_priorities_);
}

void BtaAvCo::ProcessStart(tBTA_AV_HNDL bta_av_handle,
                           const RawAddress& peer_address,
                           const uint8_t* p_codec_info, bool* p_no_rtp_header) {
  APPL_TRACE_DEBUG("%s: peer %s bta_av_handle: 0x%x", __func__,
                   peer_address.ToString().c_str(), bta_av_handle);

  // Find the peer
  BtaAvCoPeer* p_peer = peer_cache_->FindPeerAndUpdate(bta_av_handle, peer_address);
  if (p_peer == nullptr) {
    APPL_TRACE_ERROR(
        "%s: could not find peer entry for bta_av_handle 0x%x peer %s",
        __func__, bta_av_handle, peer_address.ToString().c_str());
    return;
  }

  bool add_rtp_header =
      A2DP_UsesRtpHeader(p_peer->ContentProtectActive(), p_codec_info);

  APPL_TRACE_DEBUG("%s: bta_av_handle: 0x%x add_rtp_header: %s", __func__,
                   bta_av_handle, add_rtp_header ? "true" : "false");
  *p_no_rtp_header = !add_rtp_header;
}

void BtaAvCo::ProcessStop(tBTA_AV_HNDL bta_av_handle,
                          const RawAddress& peer_address) {
  APPL_TRACE_DEBUG("%s: peer %s bta_av_handle: 0x%x", __func__,
                   peer_address.ToString().c_str(), bta_av_handle);
  // Nothing to do
}

BT_HDR* BtaAvCo::GetNextSourceDataPacket(const uint8_t* p_codec_info,
                                         uint32_t* p_timestamp) {
  BT_HDR* p_buf;

  APPL_TRACE_DEBUG("%s: codec: %s", __func__, A2DP_CodecName(p_codec_info));

  p_buf = btif_a2dp_source_audio_readbuf();
  if (p_buf == nullptr) return nullptr;

  /*
   * Retrieve the timestamp information from the media packet,
   * and set up the packet header.
   *
   * In media packet, the following information is available:
   * p_buf->layer_specific : number of audio frames in the packet
   * p_buf->word[0] : timestamp
   */
  if (!A2DP_GetPacketTimestamp(p_codec_info, (const uint8_t*)(p_buf + 1),
                               p_timestamp) ||
      !A2DP_BuildCodecHeader(p_codec_info, p_buf, p_buf->layer_specific)) {
    APPL_TRACE_ERROR("%s: unsupported codec type (%d)", __func__,
                     A2DP_GetCodecType(p_codec_info));
  }

  BtaAvCoState* reference_state = nullptr;
  //if (IS_FLAG_ENABLED(a2dp_concurrent_source_sink)) {
  if (1) {
    reference_state = &bta_av_source_state_;
  } else {
    reference_state = &bta_av_legacy_state_;
  }
  BtaAvCoPeer* active_peer = reference_state->getActivePeer();

  if (ContentProtectEnabled() && (active_peer != nullptr) &&
      active_peer->ContentProtectActive()) {
    p_buf->len++;
    p_buf->offset--;
    uint8_t* p = (uint8_t*)(p_buf + 1) + p_buf->offset;
    *p = ContentProtectFlag();
  }

  return p_buf;
}

void BtaAvCo::DataPacketWasDropped(tBTA_AV_HNDL bta_av_handle,
                                   const RawAddress& peer_address) {
  APPL_TRACE_ERROR("%s: peer %s dropped audio packet on handle 0x%x", __func__,
                   peer_address.ToString().c_str(), bta_av_handle);
}

void BtaAvCo::ProcessAudioDelay(tBTA_AV_HNDL bta_av_handle,
                                const RawAddress& peer_address,
                                uint16_t delay) {
  APPL_TRACE_DEBUG("%s: peer %s bta_av_handle: 0x%x delay:0x%x", __func__,
                   peer_address.ToString().c_str(), bta_av_handle, delay);

  btif_av_set_audio_delay(peer_address, delay, A2dpType::kSource);
}

void BtaAvCo::UpdateMtu(tBTA_AV_HNDL bta_av_handle,
                        const RawAddress& peer_address, uint16_t mtu) {
  LOG(INFO) << __func__ << ": peer " << peer_address
            << " bta_av_handle: " << loghex(bta_av_handle) << " mtu: " << mtu;

  // Find the peer
  BtaAvCoPeer* p_peer = peer_cache_->FindPeerAndUpdate(bta_av_handle, peer_address);
  if (p_peer == nullptr) {
    LOG(ERROR) << __func__ << ": could not find peer entry for bta_av_handle "
               << loghex(bta_av_handle) << " peer " << peer_address;
    return;
  }
  p_peer->mtu = mtu;
}

bool BtaAvCo::SetActivePeer(const RawAddress& peer_address, const uint8_t t_local_sep) {
  APPL_TRACE_DEBUG("peer_address={}", peer_address);

  std::lock_guard<std::recursive_mutex> lock(peer_cache_->codec_lock_);

  BtaAvCoState* reference_state = getStateFromLocalProfile(t_local_sep);
  if (reference_state == nullptr) {
    APPL_TRACE_DEBUG("Invalid bta av state for peer_address : {} with local sep as :{}", peer_address,
              t_local_sep);
    return false;
  }
  if (peer_address.IsEmpty()) {
    // Reset the active peer;
    reference_state->setActivePeer(nullptr);
    uint8_t* codec_config = reference_state->getCodecConfig();
    memset(codec_config, 0, AVDT_CODEC_SIZE);
    return true;
  }

  // Find the peer
  BtaAvCoPeer* p_peer = peer_cache_->FindPeer(peer_address);
  if (p_peer == nullptr) {
    return false;
  }

  reference_state->setActivePeer(p_peer);
  uint8_t* codec_config = reference_state->getCodecConfig();
  memcpy(codec_config, p_peer->codec_config, AVDT_CODEC_SIZE);
  BTIF_TRACE_WARNING("codec = %s", A2DP_CodecInfoString(codec_config).c_str());
  // report the selected codec configuration of this new active peer.
  ReportSourceCodecState(p_peer);
  return true;
}

void BtaAvCo::SaveCodec(const uint8_t* new_codec_config) {
  //if (IS_FLAG_ENABLED(a2dp_concurrent_source_sink)) {
  if(1) {  
    uint8_t* codec_config = bta_av_sink_state_.getCodecConfig();
    memcpy(codec_config, new_codec_config, AVDT_CODEC_SIZE);
  } else {
    uint8_t* codec_config = bta_av_legacy_state_.getCodecConfig();
    memcpy(codec_config, new_codec_config, AVDT_CODEC_SIZE);
  }
}

void BtaAvCo::GetPeerEncoderParameters(
    const RawAddress& peer_address,
    tA2DP_ENCODER_INIT_PEER_PARAMS* p_peer_params) {
  uint16_t min_mtu = 0xFFFF;
  //CHECK(p_peer_params != nullptr) << "Peer address " << peer_address;

  std::lock_guard<std::recursive_mutex> lock(peer_cache_->codec_lock_);

  // Compute the MTU
  for (size_t i = 0; i < BTA_AV_CO_NUM_ELEMENTS(peer_cache_->peers_); i++) {
    const BtaAvCoPeer* p_peer = &peer_cache_->peers_[i];
    if (!p_peer->opened) continue;
    if (p_peer->addr != peer_address) continue;
    if (p_peer->mtu < min_mtu) min_mtu = p_peer->mtu;
  }
  p_peer_params->peer_mtu = min_mtu;
  p_peer_params->is_peer_edr = btif_av_is_peer_edr(peer_address, A2dpType::kSource);
  p_peer_params->peer_supports_3mbps =
      btif_av_peer_supports_3mbps(peer_address, A2dpType::kSource);
  APPL_TRACE_DEBUG(
      "%s: peer_address=%s peer_mtu=%d is_peer_edr=%s peer_supports_3mbps=%s",
      __func__, peer_address.ToString().c_str(), p_peer_params->peer_mtu,
      logbool(p_peer_params->is_peer_edr).c_str(),
      logbool(p_peer_params->peer_supports_3mbps).c_str());
}

const tA2DP_ENCODER_INTERFACE* BtaAvCo::GetSourceEncoderInterface() {
  std::lock_guard<std::recursive_mutex> lock(peer_cache_->codec_lock_);
  //if (IS_FLAG_ENABLED(a2dp_concurrent_source_sink)) {
  if(1) {
    return A2DP_GetEncoderInterface(bta_av_source_state_.getCodecConfig());
  }
  return A2DP_GetEncoderInterface(bta_av_legacy_state_.getCodecConfig());
}

const tA2DP_DECODER_INTERFACE* BtaAvCo::GetSinkDecoderInterface() {
  std::lock_guard<std::recursive_mutex> lock(peer_cache_->codec_lock_);
  return A2DP_GetDecoderInterface(bta_av_sink_state_.getCodecConfig());
  //return A2DP_GetDecoderInterface(codec_config_);
}

bool BtaAvCo::SetCodecUserConfig(
    const RawAddress& peer_address,
    const btav_a2dp_codec_config_t& codec_user_config, bool* p_restart_output) {
  uint8_t result_codec_config[AVDT_CODEC_SIZE];
  const BtaAvCoSep* p_sink = nullptr;
  bool restart_input = false;
  bool restart_output = false;
  bool config_updated = false;
  bool success = true;

  LOG(ERROR) << __func__ << ": peer_address=" << peer_address
          << " codec_user_config={" << codec_user_config.ToString() << "}";

  *p_restart_output = false;

  BtaAvCoPeer* p_peer = peer_cache_->FindPeer(peer_address);
  if (p_peer == nullptr) {
    LOG(ERROR) << __func__ << ": cannot find peer " << peer_address
               << " to configure";
    success = false;
    goto done;
  }

  // Don't call BTA_AvReconfig() prior to retrieving all peer's capabilities
  if ((p_peer->num_rx_sinks != p_peer->num_sinks) &&
      (p_peer->num_sup_sinks != BTA_AV_CO_NUM_ELEMENTS(p_peer->sinks))) {
    LOG(WARNING) << __func__ << ": peer " << p_peer->addr
                 << " : not all peer's capabilities have been retrieved";
    success = false;
    goto done;
  }

  // Find the peer SEP codec to use
  if (codec_user_config.codec_type < BTAV_A2DP_CODEC_INDEX_MAX) {
    p_sink = peer_cache_->FindPeerSink(p_peer, codec_user_config.codec_type,
                    ContentProtectFlag());
  } else {
    // Use the current sink codec
    p_sink = p_peer->p_sink;
  }
  if (p_sink == nullptr) {
    LOG(ERROR) << __func__ << ": peer " << p_peer->addr
               << " : cannot find peer SEP to configure for codec type "
               << codec_user_config.codec_type;
    success = false;
    goto done;
  }

  tA2DP_ENCODER_INIT_PEER_PARAMS peer_params;
  GetPeerEncoderParameters(p_peer->addr, &peer_params);
  if (!p_peer->GetCodecs()->setCodecUserConfig(
          codec_user_config, &peer_params, p_sink->codec_caps,
          result_codec_config, &restart_input, &restart_output,
          &config_updated)) {
    success = false;
    goto done;
  }

  if (restart_output) {
    uint8_t num_protect = 0;
    if (ContentProtectEnabled() && p_peer->ContentProtectActive()) {
      num_protect = AVDT_CP_INFO_LEN;
    }

    p_sink = SelectSourceCodec(p_peer);
    if (p_sink == nullptr) {
      LOG(ERROR) << __func__ << ": peer " << p_peer->addr
                 << " : cannot set up codec for the peer SINK";
      success = false;
      goto done;
    }

    p_peer->acceptor = false;
    LOG(ERROR) << __func__ << ": call BTA_AvReconfig("
            << loghex(p_peer->BtaAvHandle()) << ")";
    BTA_AvReconfig(p_peer->BtaAvHandle(), true, p_sink->sep_info_idx,
                   p_peer->codec_config, num_protect, bta_av_co_cp_scmst);
    *p_restart_output = true;
  }

done:
  // We send the upcall if there is no change or the user config failed for
  // current active peer, so the caller would know it failed. If there is no
  // error, the new selected codec configuration would be sent after we are
  // ready to start a new session with the audio HAL.
  // For none active peer, we unconditionally send the upcall, so the caller
  // would always know the result.
  // NOTE: Currently, the input is restarted by sending an upcall
  // and informing the Media Framework about the change.

  // Find the peer that is currently open
  BtaAvCoPeer* active_peer;
  //if (IS_FLAG_ENABLED(a2dp_concurrent_source_sink)) {
  if(1) {
    active_peer = bta_av_source_state_.getActivePeer();
  } else {
    active_peer = bta_av_legacy_state_.getActivePeer();
  }

  if (p_peer != nullptr &&
      (!restart_output || !success || p_peer != active_peer)) {
    return ReportSourceCodecState(p_peer);
  }

  return success;
}

bool BtaAvCo::SetCodecAudioConfig(
    const btav_a2dp_codec_config_t& codec_audio_config) {
  uint8_t result_codec_config[AVDT_CODEC_SIZE];
  bool restart_output = false;
  bool config_updated = false;

  LOG(ERROR) << __func__
          << ": codec_audio_config: " << codec_audio_config.ToString();

  // Find the peer that is currently open
  BtaAvCoPeer* p_peer;
  //if (IS_FLAG_ENABLED(a2dp_concurrent_source_sink)) {
  if(1) {
    p_peer = bta_av_source_state_.getActivePeer();
  } else {
    p_peer = bta_av_legacy_state_.getActivePeer();
  }

  if (p_peer == nullptr) {
    LOG(ERROR) << __func__ << ": no active peer to configure";
    return false;
  }

  // Don't call BTA_AvReconfig() prior to retrieving all peer's capabilities
  if ((p_peer->num_rx_sinks != p_peer->num_sinks) &&
      (p_peer->num_sup_sinks != BTA_AV_CO_NUM_ELEMENTS(p_peer->sinks))) {
    LOG(WARNING) << __func__ << ": peer " << p_peer->addr
                 << " : not all peer's capabilities have been retrieved";
    return false;
  }

  // Use the current sink codec
  const BtaAvCoSep* p_sink = p_peer->p_sink;
  if (p_sink == nullptr) {
    LOG(ERROR) << __func__ << ": peer " << p_peer->addr
               << " : cannot find peer SEP to configure";
    return false;
  }

  tA2DP_ENCODER_INIT_PEER_PARAMS peer_params;
  GetPeerEncoderParameters(p_peer->addr, &peer_params);
  if (!p_peer->GetCodecs()->setCodecAudioConfig(
          codec_audio_config, &peer_params, p_sink->codec_caps,
          result_codec_config, &restart_output, &config_updated)) {
    return false;
  }

  if (restart_output) {
    uint8_t num_protect = 0;
    if (ContentProtectEnabled() && p_peer->ContentProtectActive()) {
      num_protect = AVDT_CP_INFO_LEN;
    }

    SaveNewCodecConfig(p_peer, result_codec_config, p_sink->num_protect,
                       p_sink->protect_info, AVDT_TSEP_SRC);

    p_peer->acceptor = false;
    LOG(ERROR) << __func__ << ": call BTA_AvReconfig("
            << loghex(p_peer->BtaAvHandle()) << ")";
    BTA_AvReconfig(p_peer->BtaAvHandle(), true, p_sink->sep_info_idx,
                   p_peer->codec_config, num_protect, bta_av_co_cp_scmst);
  }

  if (config_updated) {
    // NOTE: Currently, the input is restarted by sending an upcall
    // and informing the Media Framework about the change of selected codec.
    return ReportSourceCodecState(p_peer);
  }

  return true;
}

bool BtaAvCo::ReportSourceCodecState(BtaAvCoPeer* p_peer) {
  btav_a2dp_codec_config_t codec_config;
  std::vector<btav_a2dp_codec_config_t> codecs_local_capabilities;
  std::vector<btav_a2dp_codec_config_t> codecs_selectable_capabilities;

  LOG(ERROR) << __func__ << ": peer_address=" << p_peer->addr;
  A2dpCodecs* codecs = p_peer->GetCodecs();
  CHECK(codecs != nullptr);
  if (!codecs->getCodecConfigAndCapabilities(&codec_config,
                                             &codecs_local_capabilities,
                                             &codecs_selectable_capabilities)) {
    LOG(WARNING) << __func__ << ": Peer " << p_peer->addr
                 << " : error reporting audio source codec state: cannot get "
                    "codec config and capabilities";
    return false;
  }
  LOG(INFO) << __func__ << ": peer " << p_peer->addr << " codec_config={"
            << codec_config.ToString() << "}";
  btif_av_report_source_codec_state(p_peer->addr, codec_config,
                                    codecs_local_capabilities,
                                    codecs_selectable_capabilities);
  return true;
}

bool BtaAvCo::ReportSinkCodecState(BtaAvCoPeer* p_peer) {
  APPL_TRACE_DEBUG("%s: peer_address=%s", __func__,
                   p_peer->addr.ToString().c_str());
  // Nothing to do (for now)
  return true;
}

void BtaAvCo::DebugDump(int fd) {
  std::lock_guard<std::recursive_mutex> lock(peer_cache_->codec_lock_);


  //
  // Active peer codec-specific stats
  //
  if (bta_av_legacy_state_.getActivePeer() != nullptr) {
    A2dpCodecs* a2dp_codecs = bta_av_legacy_state_.getActivePeer()->GetCodecs();
    if (a2dp_codecs != nullptr) {
      a2dp_codecs->debug_codec_dump(fd);
    }
  }
  if (bta_av_source_state_.getActivePeer() != nullptr) {
    A2dpCodecs* a2dp_codecs = bta_av_source_state_.getActivePeer()->GetCodecs();
    if (a2dp_codecs != nullptr) {
      a2dp_codecs->debug_codec_dump(fd);
    }
  }
  if (bta_av_sink_state_.getActivePeer() != nullptr) {
    A2dpCodecs* a2dp_codecs = bta_av_sink_state_.getActivePeer()->GetCodecs();
    if (a2dp_codecs != nullptr) {
      a2dp_codecs->debug_codec_dump(fd);
    }
  }

  dprintf(fd, "\nA2DP Peers State:\n");
  dprintf(fd, "  Active peer: %s\n",
          (bta_av_legacy_state_.getActivePeer() != nullptr)
                  ? ADDRESS_TO_LOGGABLE_CSTR(bta_av_legacy_state_.getActivePeer()->addr)
                  : "null");
  dprintf(fd, "  Source: active peer: %s\n",
          (bta_av_source_state_.getActivePeer() != nullptr)
                  ? ADDRESS_TO_LOGGABLE_CSTR(bta_av_source_state_.getActivePeer()->addr)
                  : "null");
  dprintf(fd, "  Sink: active peer: %s\n",
          (bta_av_sink_state_.getActivePeer() != nullptr)
                  ? ADDRESS_TO_LOGGABLE_CSTR(bta_av_sink_state_.getActivePeer()->addr)
                  : "null");

  for (size_t i = 0; i < BTA_AV_CO_NUM_ELEMENTS(peer_cache_->peers_); i++) {
    const BtaAvCoPeer& peer = peer_cache_->peers_[i];
    if (peer.addr.IsEmpty()) {
      continue;
    }
    dprintf(fd, "  Peer: %s\n", peer.addr.ToString().c_str());
    dprintf(fd, "    Number of sinks: %u\n", peer.num_sinks);
    dprintf(fd, "    Number of sources: %u\n", peer.num_sources);
    dprintf(fd, "    Number of SEPs: %u\n", peer.num_seps);
    dprintf(fd, "    Number of received sinks: %u\n", peer.num_rx_sinks);
    dprintf(fd, "    Number of received sources: %u\n", peer.num_rx_sources);
    dprintf(fd, "    Number of supported sinks: %u\n", peer.num_sup_sinks);
    dprintf(fd, "    Number of supported sources: %u\n", peer.num_sup_sources);
    dprintf(fd, "    Acceptor: %s\n", (peer.acceptor) ? "true" : "false");
    dprintf(fd, "    Reconfig needed: %s\n",
            (peer.reconfig_needed) ? "true" : "false");
    dprintf(fd, "    Opened: %s\n", (peer.opened) ? "true" : "false");
    dprintf(fd, "    MTU: %u\n", peer.mtu);
    dprintf(fd, "    UUID to connect: 0x%x\n", peer.uuid_to_connect);
    dprintf(fd, "    BTA AV handle: %u\n", peer.BtaAvHandle());
  }
}

const BtaAvCoSep* BtaAvCo::SelectSourceCodec(BtaAvCoPeer* p_peer) {
  const BtaAvCoSep* p_sink = nullptr;

  // Update all selectable codecs.
  // This is needed to update the selectable parameters for each codec.
  // NOTE: The selectable codec info is used only for informational purpose.
  UpdateAllSelectableSourceCodecs(p_peer);

  // Select the codec
  for (const auto& iter : p_peer->GetCodecs()->orderedSourceCodecs()) {
    LOG(ERROR) << __func__ << ": trying codec " << iter->name();
    p_sink = AttemptSourceCodecSelection(*iter, p_peer);
    if (p_sink != nullptr) {
      LOG(ERROR) << __func__ << ": selected codec " << iter->name();
      break;
    }
    LOG(ERROR) << __func__ << ": cannot use codec " << iter->name();
  }
  return p_sink;
}

const BtaAvCoSep* BtaAvCo::SelectSinkCodec(BtaAvCoPeer* p_peer) {
  const BtaAvCoSep* p_source = nullptr;

  // Update all selectable codecs.
  // This is needed to update the selectable parameters for each codec.
  // NOTE: The selectable codec info is used only for informational purpose.
  UpdateAllSelectableSinkCodecs(p_peer);

  // Select the codec
  for (const auto& iter : p_peer->GetCodecs()->orderedSinkCodecs()) {
    APPL_TRACE_DEBUG("%s: trying codec %s", __func__, iter->name().c_str());
    p_source = AttemptSinkCodecSelection(*iter, p_peer);
    if (p_source != nullptr) {
      APPL_TRACE_DEBUG("%s: selected codec %s", __func__, iter->name().c_str());
      break;
    }
    APPL_TRACE_DEBUG("%s: cannot use codec %s", __func__, iter->name().c_str());
  }

  // NOTE: Unconditionally dispatch the event to make sure a callback with
  // the most recent codec info is generated.
  ReportSinkCodecState(p_peer);

  return p_source;
}

const BtaAvCoSep* BtaAvCo::AttemptSourceCodecSelection(
    const A2dpCodecConfig& codec_config, BtaAvCoPeer* p_peer) {
  uint8_t new_codec_config[AVDT_CODEC_SIZE];

  APPL_TRACE_DEBUG("%s", __func__);

  // Find the peer Sink for the codec
  BtaAvCoSep* p_sink = peer_cache_->FindPeerSink(p_peer, codec_config.codecIndex(), ContentProtectFlag());
  if (p_sink == nullptr) {
    APPL_TRACE_DEBUG("%s: peer Sink for codec %s not found", __func__,
                     codec_config.name().c_str());
    return nullptr;
  }
  if (!p_peer->GetCodecs()->setCodecConfig(
          p_sink->codec_caps, true /* is_capability */, new_codec_config,
          true /* select_current_codec */)) {
    APPL_TRACE_DEBUG("%s: cannot set source codec %s", __func__,
                     codec_config.name().c_str());
    return nullptr;
  }
  p_peer->p_sink = p_sink;

  SaveNewCodecConfig(p_peer, new_codec_config, p_sink->num_protect,
                     p_sink->protect_info, AVDT_TSEP_SRC);

  return p_sink;
}

const BtaAvCoSep* BtaAvCo::AttemptSinkCodecSelection(
    const A2dpCodecConfig& codec_config, BtaAvCoPeer* p_peer) {
  uint8_t new_codec_config[AVDT_CODEC_SIZE];

  APPL_TRACE_DEBUG("%s", __func__);

  // Find the peer Source for the codec
  BtaAvCoSep* p_source = 
    peer_cache_->FindPeerSource(p_peer, codec_config.codecIndex(), ContentProtectFlag());
  if (p_source == nullptr) {
    APPL_TRACE_DEBUG("%s: peer Source for codec %s not found", __func__,
                     codec_config.name().c_str());
    return nullptr;
  }
  if (!p_peer->GetCodecs()->setSinkCodecConfig(
          p_source->codec_caps, true /* is_capability */, new_codec_config,
          true /* select_current_codec */)) {
    APPL_TRACE_DEBUG("%s: cannot set sink codec %s", __func__,
                     codec_config.name().c_str());
    return nullptr;
  }
  p_peer->p_source = p_source;

  SaveNewCodecConfig(p_peer, new_codec_config, p_source->num_protect,
                     p_source->protect_info, AVDT_TSEP_SNK);

  return p_source;
}

size_t BtaAvCo::UpdateAllSelectableSourceCodecs(BtaAvCoPeer* p_peer) {
  APPL_TRACE_DEBUG("%s: peer %s", __func__, p_peer->addr.ToString().c_str());

  size_t updated_codecs = 0;
  for (const auto& iter : p_peer->GetCodecs()->orderedSourceCodecs()) {
    APPL_TRACE_DEBUG("%s: updating selectable codec %s", __func__,
                     iter->name().c_str());
    if (UpdateSelectableSourceCodec(*iter, p_peer)) {
      updated_codecs++;
    }
  }
  return updated_codecs;
}

bool BtaAvCo::UpdateSelectableSourceCodec(const A2dpCodecConfig& codec_config,
                                          BtaAvCoPeer* p_peer) {
  APPL_TRACE_DEBUG("%s: peer %s", __func__, p_peer->addr.ToString().c_str());

  // Find the peer Sink for the codec
  const BtaAvCoSep* p_sink = 
      peer_cache_->FindPeerSink(p_peer, codec_config.codecIndex(), ContentProtectFlag());
  if (p_sink == nullptr) {
    // The peer Sink device does not support this codec
    return false;
  }
  if (!p_peer->GetCodecs()->setPeerSinkCodecCapabilities(p_sink->codec_caps)) {
    APPL_TRACE_WARNING("%s: cannot update peer %s codec capabilities for %s",
                       __func__, p_peer->addr.ToString().c_str(),
                       A2DP_CodecName(p_sink->codec_caps));
    return false;
  }
  return true;
}

size_t BtaAvCo::UpdateAllSelectableSinkCodecs(BtaAvCoPeer* p_peer) {
  APPL_TRACE_DEBUG("%s: peer %s", __func__, p_peer->addr.ToString().c_str());

  size_t updated_codecs = 0;
  for (const auto& iter : p_peer->GetCodecs()->orderedSinkCodecs()) {
    APPL_TRACE_DEBUG("%s: updating selectable codec %s", __func__,
                     iter->name().c_str());
    if (UpdateSelectableSinkCodec(*iter, p_peer)) {
      updated_codecs++;
    }
  }
  return updated_codecs;
}

bool BtaAvCo::UpdateSelectableSinkCodec(const A2dpCodecConfig& codec_config,
                                        BtaAvCoPeer* p_peer) {
  APPL_TRACE_DEBUG("%s: peer %s", __func__, p_peer->addr.ToString().c_str());

  // Find the peer Source for the codec
  const BtaAvCoSep* p_source =
      peer_cache_->FindPeerSource(p_peer, codec_config.codecIndex(), ContentProtectFlag());
  if (p_source == nullptr) {
    // The peer Source device does not support this codec
    return false;
  }
  if (!p_peer->GetCodecs()->setPeerSourceCodecCapabilities(
          p_source->codec_caps)) {
    APPL_TRACE_WARNING("%s: cannot update peer %s codec capabilities for %s",
                       __func__, p_peer->addr.ToString().c_str(),
                       A2DP_CodecName(p_source->codec_caps));
    return false;
  }
  return true;
}

void BtaAvCo::SaveNewCodecConfig(BtaAvCoPeer* p_peer,
                                 const uint8_t* new_codec_config,
                                 uint8_t num_protect,
                                 const uint8_t* p_protect_info, const uint8_t t_local_sep) {
  APPL_TRACE_DEBUG("%s: peer %s", __func__, p_peer->addr.ToString().c_str());
  APPL_TRACE_DEBUG("%s: codec: %s", __func__,
                   A2DP_CodecInfoString(new_codec_config).c_str());

  std::lock_guard<std::recursive_mutex> lock(peer_cache_->codec_lock_);
  BtaAvCoState* reference_state = getStateFromLocalProfile(t_local_sep);
  if (reference_state == nullptr) {
    BTIF_TRACE_WARNING("Invalid bta av state for peer_address : {} with local sep as :{}", p_peer->addr,
              t_local_sep);
    return;
  }
  reference_state->setCodecConfig(new_codec_config);
  memcpy(p_peer->codec_config, new_codec_config, AVDT_CODEC_SIZE);

  if (ContentProtectEnabled()) {
    // Check if this Sink supports SCMS
    bool cp_active = AudioProtectHasScmst(num_protect, p_protect_info);
    p_peer->SetContentProtectActive(cp_active);
  }
}

bool BtaAvCo::SetCodecOtaConfig(BtaAvCoPeer* p_peer,
                                const uint8_t* p_ota_codec_config,
                                uint8_t num_protect,
                                const uint8_t* p_protect_info,
                                bool* p_restart_output,
                                const uint8_t t_local_sep) {
  uint8_t result_codec_config[AVDT_CODEC_SIZE];
  bool restart_input = false;
  bool restart_output = false;
  bool config_updated = false;

  LOG(INFO) << __func__ << ": peer_address=" << p_peer->addr
            << ", codec: " << A2DP_CodecInfoString(p_ota_codec_config);

  *p_restart_output = false;

  // Find the peer SEP codec to use
  const BtaAvCoSep* p_sink = 
    peer_cache_->FindPeerSink(p_peer, A2DP_SourceCodecIndex(p_ota_codec_config), ContentProtectFlag());
  if ((p_peer->num_sup_sinks > 0) && (p_sink == nullptr)) {
    // There are no peer SEPs if we didn't do the discovery procedure yet.
    // We have all the information we need from the peer, so we can
    // proceed with the OTA codec configuration.
    LOG(ERROR) << __func__ << ": peer " << p_peer->addr
               << " : cannot find peer SEP to configure";
    return false;
  }

  tA2DP_ENCODER_INIT_PEER_PARAMS peer_params;
  GetPeerEncoderParameters(p_peer->addr, &peer_params);
  if (!p_peer->GetCodecs()->setCodecOtaConfig(
          p_ota_codec_config, &peer_params, result_codec_config, &restart_input,
          &restart_output, &config_updated)) {
    LOG(ERROR) << __func__ << ": peer " << p_peer->addr
               << " : cannot set OTA config";
    return false;
  }

  if (restart_output) {
    LOG(ERROR) << __func__ << ": restart output for codec: "
            << A2DP_CodecInfoString(result_codec_config);

    *p_restart_output = true;
    p_peer->p_sink = p_sink;
    SaveNewCodecConfig(p_peer, result_codec_config, num_protect,
                       p_protect_info, t_local_sep);
  }

  if (restart_input || config_updated) {
    // NOTE: Currently, the input is restarted by sending an upcall
    // and informing the Media Framework about the change of selected codec.
    ReportSourceCodecState(p_peer);
  }

  return true;
}

BtaAvCoState* BtaAvCo::getStateFromLocalProfile(const uint8_t t_local_sep) {
  //if (com::android::bluetooth::flags::a2dp_concurrent_source_sink()) {
  if(1) {
    if (t_local_sep == AVDT_TSEP_SRC) {
      return &bta_av_source_state_;
    } else if (t_local_sep == AVDT_TSEP_SNK) {
      return &bta_av_sink_state_;
    } else {
      BTIF_TRACE_WARNING("Invalid bta av state for local sep type {}", t_local_sep);
      return nullptr;
    }
  } else {
    return &bta_av_legacy_state_;
  }
}

BtaAvCoState* BtaAvCo::getStateFromPeer(const BtaAvCoPeer* p_peer) {
  //if (com::android::bluetooth::flags::a2dp_concurrent_source_sink()) {
  if(1) {
    if (p_peer->uuid_to_connect == UUID_SERVCLASS_AUDIO_SINK) {
      return &bta_av_source_state_;
    } else if (p_peer->uuid_to_connect == UUID_SERVCLASS_AUDIO_SOURCE) {
      return &bta_av_sink_state_;
    } else {
      BTIF_TRACE_WARNING("Invalid bta av state for peer_address : {} with uuid as :{}", p_peer->addr,
                p_peer->uuid_to_connect);
      return nullptr;
    }
  } else {
    return &bta_av_legacy_state_;
  }
}

void bta_av_co_init(
    const std::vector<btav_a2dp_codec_config_t>& codec_priorities) {
  bta_av_co_cb.Init(codec_priorities);
}

bool bta_av_co_is_supported_codec(btav_a2dp_codec_index_t codec_index) {
  return bta_av_co_cb.IsSupportedCodec(codec_index);
}

A2dpCodecConfig* bta_av_get_a2dp_current_codec(void) {
  return bta_av_co_cb.GetActivePeerCurrentCodec();
}

A2dpCodecConfig* bta_av_get_a2dp_peer_current_codec(
    const RawAddress& peer_address) {
  return bta_av_co_cb.GetPeerCurrentCodec(peer_address);
}

bool bta_av_co_audio_init(btav_a2dp_codec_index_t codec_index,
                          AvdtpSepConfig* p_cfg) {
  return A2DP_InitCodecConfig(codec_index, p_cfg);
}

void bta_av_co_audio_disc_res(tBTA_AV_HNDL bta_av_handle,
                              const RawAddress& peer_address, uint8_t num_seps,
                              uint8_t num_sinks, uint8_t num_sources,
                              uint16_t uuid_local) {
  bta_av_co_cb.ProcessDiscoveryResult(bta_av_handle, peer_address, num_seps,
                                      num_sinks, num_sources, uuid_local);
}

tA2DP_STATUS bta_av_co_audio_getconfig(tBTA_AV_HNDL bta_av_handle,
                                       const RawAddress& peer_address,
                                       uint8_t* p_codec_info,
                                       uint8_t* p_sep_info_idx, uint8_t seid,
                                       uint8_t* p_num_protect,
                                       uint8_t* p_protect_info) {
  uint16_t peer_uuid = bta_av_co_cb.peer_cache_->FindPeerUuid(bta_av_handle);

  APPL_TRACE_DEBUG("%s: peer %s bta_av_handle=0x%x peer_uuid=0x%x", __func__,
                   peer_address.ToString().c_str(), bta_av_handle, peer_uuid);

  switch (peer_uuid) {
    case UUID_SERVCLASS_AUDIO_SOURCE:
      return bta_av_co_cb.ProcessSinkGetConfig(
          bta_av_handle, peer_address, p_codec_info, p_sep_info_idx, seid,
          p_num_protect, p_protect_info);
    case UUID_SERVCLASS_AUDIO_SINK:
      return bta_av_co_cb.ProcessSourceGetConfig(
          bta_av_handle, peer_address, p_codec_info, p_sep_info_idx, seid,
          p_num_protect, p_protect_info);
    default:
      break;
  }
  APPL_TRACE_ERROR(
      "%s: peer %s : Invalid peer UUID: 0x%x for bta_av_handle 0x%x",
      peer_address.ToString().c_str(), peer_uuid, bta_av_handle);
  return A2DP_FAIL;
}

void bta_av_co_audio_setconfig(tBTA_AV_HNDL bta_av_handle,
                               const RawAddress& peer_address,
                               const uint8_t* p_codec_info, uint8_t seid,
                               uint8_t num_protect,
                               const uint8_t* p_protect_info,
                               uint8_t t_local_sep, uint8_t avdt_handle) {
  bta_av_co_cb.ProcessSetConfig(bta_av_handle, peer_address, p_codec_info, seid,
                                num_protect, p_protect_info, t_local_sep,
                                avdt_handle);
}

void bta_av_co_audio_open(tBTA_AV_HNDL bta_av_handle,
                          const RawAddress& peer_address, uint16_t mtu) {
  bta_av_co_cb.ProcessOpen(bta_av_handle, peer_address, mtu);
}

void bta_av_co_audio_close(tBTA_AV_HNDL bta_av_handle,
                           const RawAddress& peer_address) {
  bta_av_co_cb.ProcessClose(bta_av_handle, peer_address);
}

void bta_av_co_audio_start(tBTA_AV_HNDL bta_av_handle,
                           const RawAddress& peer_address,
                           const uint8_t* p_codec_info, bool* p_no_rtp_header) {
  bta_av_co_cb.ProcessStart(bta_av_handle, peer_address, p_codec_info,
                            p_no_rtp_header);
}

void bta_av_co_audio_stop(tBTA_AV_HNDL bta_av_handle,
                          const RawAddress& peer_address) {
  bta_av_co_cb.ProcessStop(bta_av_handle, peer_address);
}

BT_HDR* bta_av_co_audio_source_data_path(const uint8_t* p_codec_info,
                                         uint32_t* p_timestamp) {
  return bta_av_co_cb.GetNextSourceDataPacket(p_codec_info, p_timestamp);
}

void bta_av_co_audio_drop(tBTA_AV_HNDL bta_av_handle,
                          const RawAddress& peer_address) {
  bta_av_co_cb.DataPacketWasDropped(bta_av_handle, peer_address);
}

void bta_av_co_audio_delay(tBTA_AV_HNDL bta_av_handle,
                           const RawAddress& peer_address, uint16_t delay) {
  bta_av_co_cb.ProcessAudioDelay(bta_av_handle, peer_address, delay);
}

void bta_av_co_audio_update_mtu(tBTA_AV_HNDL bta_av_handle,
                                const RawAddress& peer_address, uint16_t mtu) {
  bta_av_co_cb.UpdateMtu(bta_av_handle, peer_address, mtu);
}

bool bta_av_co_set_active_peer(const RawAddress& peer_address) {
  return bta_av_co_cb.SetActivePeer(peer_address, AVDT_TSEP_INVALID);
}

bool bta_av_co_set_active_sink_peer(const RawAddress& peer_address) {
  return bta_av_co_cb.SetActivePeer(peer_address, AVDT_TSEP_SNK);
}
bool bta_av_co_set_active_source_peer(const RawAddress& peer_address) {
  return bta_av_co_cb.SetActivePeer(peer_address, AVDT_TSEP_SRC);
}

void bta_av_co_save_codec(const uint8_t* new_codec_config) {
  return bta_av_co_cb.SaveCodec(new_codec_config);
}

void bta_av_co_get_peer_params(const RawAddress& peer_address,
                               tA2DP_ENCODER_INIT_PEER_PARAMS* p_peer_params) {
  bta_av_co_cb.GetPeerEncoderParameters(peer_address, p_peer_params);
}

const tA2DP_ENCODER_INTERFACE* bta_av_co_get_encoder_interface(void) {
  return bta_av_co_cb.GetSourceEncoderInterface();
}

const tA2DP_DECODER_INTERFACE* bta_av_co_get_decoder_interface(void) {
  return bta_av_co_cb.GetSinkDecoderInterface();
}

bool bta_av_co_set_codec_user_config(
    const RawAddress& peer_address,
    const btav_a2dp_codec_config_t& codec_user_config, bool* p_restart_output) {
  return bta_av_co_cb.SetCodecUserConfig(peer_address, codec_user_config,
                                         p_restart_output);
}

bool bta_av_co_set_codec_audio_config(
    const btav_a2dp_codec_config_t& codec_audio_config) {
  return bta_av_co_cb.SetCodecAudioConfig(codec_audio_config);
}

bool bta_av_co_content_protect_is_active(const RawAddress& peer_address) {
  BtaAvCoPeer* p_peer = bta_av_co_cb.peer_cache_->FindPeer(peer_address);
  CHECK(p_peer != nullptr);
  return p_peer->ContentProtectActive();
}

void btif_a2dp_codec_debug_dump(int fd) { bta_av_co_cb.DebugDump(fd); }

