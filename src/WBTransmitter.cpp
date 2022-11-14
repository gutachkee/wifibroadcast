
// Copyright (C) 2017, 2018, 2019 Vasily Evseenko <svpcom@p2ptech.org>
// 2020 Constantin Geier
/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; version 3.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License along
 *   with this program; if not, write to the Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "WBTransmitter.h"
#include "HelperSources/SchedulingHelper.hpp"
#include "HelperSources/RTPHelper.hpp"

static FEC_VARIABLE_INPUT_TYPE convert(const TOptions &options) {
  if (options.fec_k.index() == 0)return FEC_VARIABLE_INPUT_TYPE::none;
  const std::string tmp = std::get<std::string>(options.fec_k);
  if (tmp == std::string("h264")) {
    return FEC_VARIABLE_INPUT_TYPE::h264;
  } else if (tmp == std::string("h265")) {
    return FEC_VARIABLE_INPUT_TYPE::h265;
  }
  assert(false);
}

WBTransmitter::WBTransmitter(RadiotapHeader::UserSelectableParams radioTapHeaderParams, TOptions options1,std::shared_ptr<spdlog::logger> console) :
    options(std::move(options1)),
    mPcapTransmitter(options.wlan),
    mEncryptor(options.keypair),
    _radioTapHeaderParams(radioTapHeaderParams),
    // FEC is disabled if k is integer and 0
    IS_FEC_DISABLED(options.fec_k.index() == 0 && std::get<int>(options.fec_k) == 0),
    // FEC is variable if k is an string
    IS_FEC_VARIABLE(options.fec_k.index() == 1),
    fecVariableInputType(convert(options)),
    mRadiotapHeader{RadiotapHeader{_radioTapHeaderParams}}{
  if(!console){
    m_console=m_console=wifibroadcast::log::create_or_get("wb_tx"+std::to_string(options.radio_port));
  }else{
    m_console=console;
  }
  mEncryptor.makeNewSessionKey(sessionKeyPacket.sessionKeyNonce, sessionKeyPacket.sessionKeyData);
  if (IS_FEC_DISABLED) {
    mFecDisabledEncoder = std::make_unique<FECDisabledEncoder>();
    mFecDisabledEncoder->outputDataCallback =
        notstd::bind_front(&WBTransmitter::sendFecPrimaryOrSecondaryFragment, this);
  } else {
    // variable if k is a string with video type
    const int kMax = options.fec_k.index() == 0 ? std::get<int>(options.fec_k) : MAX_N_P_FRAGMENTS_PER_BLOCK;
    mFecEncoder = std::make_unique<FECEncoder>(kMax, options.fec_percentage);
    mFecEncoder->outputDataCallback = notstd::bind_front(&WBTransmitter::sendFecPrimaryOrSecondaryFragment, this);
    sessionKeyPacket.MAX_N_FRAGMENTS_PER_BLOCK = FECEncoder::calculateN(kMax, options.fec_percentage);
  }
  m_console->info("WB-TX assigned ID {} assigned WLAN {}", options.radio_port, options.wlan.c_str());
  // the rx needs to know if FEC is enabled or disabled. Note, both variable and fixed fec counts as FEC enabled
  sessionKeyPacket.IS_FEC_ENABLED = !IS_FEC_DISABLED;
  if (options.enableLogAlive) {
    keepLogAliveThreadRunning = true;
    logAliveThread = std::make_unique<std::thread>([this]() {
      while (keepLogAliveThreadRunning) {
        logAlive();
        std::this_thread::sleep_for(LOG_INTERVAL);
      }
    });
  }
  m_console->info("Sending Session key on startup");
  for (int i = 0; i < 5; i++) {
    sendSessionKey();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // next session key in delta ms if packets are being fed
  session_key_announce_ts = std::chrono::steady_clock::now()+SESSION_KEY_ANNOUNCE_DELTA;
}

WBTransmitter::~WBTransmitter() {
  keepLogAliveThreadRunning = false;
  if (logAliveThread && logAliveThread->joinable()) {
    logAliveThread->join();
  }
}

void WBTransmitter::sendPacket(const AbstractWBPacket &abstractWbPacket) {
  count_bytes_data_injected+=abstractWbPacket.payloadSize;
  mIeee80211Header.writeParams(options.radio_port, ieee80211_seq);
  ieee80211_seq += 16;
  //mIeee80211Header.printSequenceControl();
  std::lock_guard<std::mutex> guard(radiotapHeaderMutex);
  const auto injectionTime = mPcapTransmitter.injectPacket(mRadiotapHeader, mIeee80211Header, abstractWbPacket);
  if(injectionTime>MAX_SANE_INJECTION_TIME){
    count_tx_injections_error_hint++;
    if(options.enableLogAlive){
      m_console->warn("Injecting PCAP packet took really long:",MyTimeHelper::R(injectionTime));
    }
  }
  nInjectedPackets++;
  if(options.enableLogAlive){
    pcapInjectionTime.add(injectionTime);
    pcapInjectionTime.printInIntervalls(std::chrono::seconds(1), false);
  }
}

void WBTransmitter::sendFecPrimaryOrSecondaryFragment(const uint64_t nonce,
                                                      const uint8_t *payload,
                                                      const std::size_t payloadSize) {
  //m_console->info("WBTransmitter::sendFecBlock {}",(int)payloadSize);
  const WBDataHeader wbDataHeader(nonce,m_curr_seq_nr);
  m_curr_seq_nr++;
  const auto encryptedData = mEncryptor.encryptPacket(nonce, payload, payloadSize, wbDataHeader);
  //
  sendPacket({(const uint8_t *) &wbDataHeader, sizeof(WBDataHeader), encryptedData.data(), encryptedData.size()});
#ifdef ENABLE_ADVANCED_DEBUGGING
  //LatencyTestingPacket latencyTestingPacket;
  //sendPacket((uint8_t*)&latencyTestingPacket,sizeof(latencyTestingPacket));
#endif
}

void WBTransmitter::sendSessionKey() {
  sendPacket({(uint8_t *) &sessionKeyPacket, WBSessionKeyPacket::SIZE_BYTES});
  nInjectedSessionKeypackets++;
}

std::string WBTransmitter::createDebugState() const {
  const auto runTimeSeconds =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - INIT_TIME).count();
  std::stringstream ss;
  // input packets & injected packets
  const auto nInjectedDataPackets=nInjectedPackets-nInjectedSessionKeypackets;
  //ss << runTimeSeconds << "\tTX:in:("<<nInputPackets<<")out:(" << nInjectedDataPackets << ":" << nInjectedSessionKeypackets << ")\n";
  ss <<"TX:in:("<<nInputPackets<<")out:(" << nInjectedDataPackets << ":" << nInjectedSessionKeypackets << ")\n";
  return ss.str();
}

void WBTransmitter::logAlive() const {
  std::cout << createDebugState();
}

void WBTransmitter::feedPacket(const uint8_t *buf, size_t size) {
  if (size <= 0 || size > FEC_MAX_PAYLOAD_SIZE) {
    m_console->warn("Fed packet with incompatible size:",size);
    return;
  }
  count_bytes_data_provided+=size;
  const auto cur_ts = std::chrono::steady_clock::now();
  // send session key in SESSION_KEY_ANNOUNCE_DELTA intervals
  if ((cur_ts >= session_key_announce_ts)) {
    // Announce session key
    sendSessionKey();
    session_key_announce_ts = cur_ts + SESSION_KEY_ANNOUNCE_DELTA;
  }
  // this calls a callback internally
  if (IS_FEC_DISABLED) {
    mFecDisabledEncoder->encodePacket(buf, size);
  } else {
    if (IS_FEC_VARIABLE) {
      // variable k
      bool endBlock = false;
      if (fecVariableInputType == FEC_VARIABLE_INPUT_TYPE::h264) {
        endBlock = RTPLockup::h264_end_block(buf, size);
      } else {
        endBlock = RTPLockup::h265_end_block(buf, size);
      }
      mFecEncoder->encodePacket(buf, size, endBlock);
    } else {
      // fixed k
      mFecEncoder->encodePacket(buf, size);
    }
    if (mFecEncoder->resetOnOverflow()) {
      // running out of sequence numbers should never happen during the lifetime of the TX instance, but handle it properly anyways
      mEncryptor.makeNewSessionKey(sessionKeyPacket.sessionKeyNonce, sessionKeyPacket.sessionKeyData);
      sendSessionKey();
    }
  }
  nInputPackets++;
}

void WBTransmitter::update_mcs_index(uint8_t mcs_index) {
  _radioTapHeaderParams.mcs_index=mcs_index;
  auto newRadioTapHeader=RadiotapHeader{_radioTapHeaderParams};
  std::lock_guard<std::mutex> guard(radiotapHeaderMutex);
  mRadiotapHeader=newRadioTapHeader;
}
