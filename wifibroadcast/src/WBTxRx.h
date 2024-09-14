//
// Created by consti10 on 27.06.23.
//

#ifndef WIFIBROADCAST_WBTXRX_H
#define WIFIBROADCAST_WBTXRX_H

#include <pcap/pcap.h>
#include <sys/poll.h>

#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "../dummy_link/DummyLink.h"
#include "../encryption/Decryptor.h"
#include "../encryption/Encryption.h"
#include "../encryption/Encryptor.h"
#include "../radiotap/RSSIAccumulator.hpp"
#include "../radiotap/RadiotapHeaderTx.hpp"
#include "../radiotap/RadiotapHeaderTxHolder.hpp"
#include "../radiotap/RadiotapRxRfAggregator.h"
#include "../radiotap/SignalQualityAccumulator.hpp"
#include "Ieee80211Header.hpp"
#include "UINT16SeqNrHelper.hpp"
#include "UINT64SeqNrHelper.hpp"
#include "WiFiCard.h"

/**
 * This class exists to provide a clean, working interface to create a
 * broadcast-like bidirectional wifi link between an fpv air and (one or more)
 * ground unit(s). It hides away some nasty driver quirks, and offers 1) A lot
 * of usefully stats like packet loss,pollution, dbm, ... 2) Multiplexing
 * (stream_index) - multiple streams from air to ground / ground to air are
 * possible 3) Packet validation / encryption (selectable per packet) 4)
 * Multiple RX-cards (only one active tx at a time though) Packets sent by an
 * "air unit" are received by any listening ground unit (broadcast) that uses
 * the same (encryption/validation) key-pair Packets sent by an "ground unit"
 * are received by any listening air unit (broadcast) that uses the same
 * (encryption/validation) key-pair Packets sent by an "air unit" are never
 * received by another air unit (and reverse for ground unit) (This is necessary
 * due to AR9271 driver quirk - it gives injected packets back on the cb for
 * received packets)
 *
 * It adds a minimal overhead of 16 bytes per data packet for validation /
 * encryption And - configurable - a couple of packets per second for the
 * session key.
 *
 * See executables/example_hello.cpp for a simple demonstration how to use this
 * class.
 *
 * NOTE: Receiving of data is not started until startReceiving() is called !
 * (To give the user time to register all the receive handlers)
 *
 * NOTE2: You won't find any FEC or similar here - this class intentionally
 * represents a lower level where FEC or similar can be added on top
 */
class WBTxRx {
 public:
  struct Options {
    // Bidirectional, so we need 2 keys. If not specified, keys generated from
    // default bind phrase are used
    std::optional<wb::KeyPairTxRx> secure_keypair = std::nullopt;
    // on the rx pcap rx fd, set direction PCAP_D_IN (aka only packets received
    // by the card) - doesn't work on AR9271
    bool pcap_rx_set_direction = true;
    bool set_tx_sock_qdisc_bypass = false;
    // thy spam the console, but usefully for debugging
    // log all received packets (regardless where they are from)
    bool log_all_received_packets = false;
    bool log_all_received_validated_packets = false;
    // more verbose rx logging
    bool advanced_debugging_rx = false;
    bool debug_tx_injection_time = false;
    // advanced latency related debugging
    bool advanced_latency_debugging_rx = false;
    // set sched_param = max realtime on the thread that pulls out the packets
    bool receive_thread_max_realtime = true;
    // enable / disable switching on which card to send packets in case there
    // are multiple cards given if this option is disabled, card 0 is always
    // used for sending
    bool enable_auto_switch_tx_card = true;
    // interval in which the session key packet is sent out - if no data is fed
    // to the TX, no session key is sent until data is fed.
    std::chrono::milliseconds session_key_packet_interval =
        std::chrono::seconds(1);
    // You need to set this to air / gnd on the air / gnd unit since AR9271 has
    // a bug where it reports injected packets as received packets
    bool use_gnd_identifier = false;
    // RSSI can be tricky
    int debug_rssi = 0;  // 0 - do not debug, 1=print min,max,avg and log
                         // invalid 2=print every packet
    // Debug encrypt / calculate checksum time
    bool debug_encrypt_time = false;
    // Debug decrypt / validate checksum time
    bool debug_decrypt_time = false;
    // Debug packet gaps
    bool debug_packet_gaps = false;
    // Debug multi rx packets variance
    bool debug_multi_rx_packets_variance = false;
    // This is only for debugging / testing, inject packets with a fixed MAC -
    // won't be received as valid packets by another rx instance
    bool enable_non_openhd_mode = false;
    // tmp
    bool tx_without_pcap = false;
    // a tx error hint is thrown if injecting the packet takes longer than
    // max_sane_injection_time
    std::chrono::milliseconds max_sane_injection_time =
        std::chrono::milliseconds(5);
    // debugging of rx radiotap header(s)
    int rx_radiotap_debug_level = 0;
  };
  /**
   * @param wifi_cards card(s) used for tx / rx
   * @param options1 see documentation in options string
   * @param session_key_radiotap_header radiotap header used when injecting
   * session key packets
   */
  explicit WBTxRx(
      std::vector<wifibroadcast::WifiCard> wifi_cards, Options options1,
      std::shared_ptr<RadiotapHeaderTxHolder> session_key_radiotap_header);
  WBTxRx(const WBTxRx&) = delete;
  WBTxRx& operator=(const WBTxRx&) = delete;
  ~WBTxRx();
  /**
   * Creates a valid injection packet which has the layout:
   * radiotap_header,ieee_80211_header, data (encrypted or not encrypted),
   * encryption/validation suffix A increasing nonce is used for each packet,
   * and is used for packet validation on the receiving side. NOTE: Encryption
   * and/or validation adds a fixed amount of overhead to each injected packet !
   * @param stream_index used to multiplex more than one data stream, written
   * into the IEE80211 header uint8_t but needs to be in range of [MIN,MAX]
   * stream index
   * @param data the packet payload
   * @param data_len the packet payload length
   * @param tx_radiotap_header can be used to modify injected packet(s)
   * properties
   * @param encrypt: Optionally encrypt the packet, if not encrypted, only a
   * (secure) validation checksum is calculated & checked on rx Encryption
   * results in more CPU load and is therefore not wanted in all cases (e.g. by
   * default, openhd does not encrypt video)
   */
  void tx_inject_packet(uint8_t stream_index, const uint8_t* data, int data_len,
                        const RadiotapHeaderTx& tx_radiotap_header,
                        bool encrypt);
  /**
   * A typical stream RX (aka the receiver for a specific multiplexed stream)
   * needs to react to events during streaming. For lowest latency, we do this
   * via callback(s) that are called directly. You can register listening on
   * these events and also deregister them here.
   * @param nonce: the nonce of the received packet (can be used for sequence
   * numbering)
   * @param wlan_index: the card on which the packet was received (in case there
   * are multiple cards used for wb)
   * @param radio_port: the multiplex index used to separate streams during
   * injection
   */
  typedef std::function<void(uint64_t nonce, int wlan_index,
                             const uint8_t* data, const int data_len)>
      SPECIFIC_OUTPUT_DATA_CB;
  typedef std::function<void()> NEW_SESSION_CB;
  struct StreamRxHandler {
    uint8_t radio_port;  // For which multiplexed stream this handles events
    SPECIFIC_OUTPUT_DATA_CB
    cb_packet;  // called every time a packet for this stream is received
    NEW_SESSION_CB cb_session;  // called every time a new session is detected
    StreamRxHandler(uint8_t radio_port1, SPECIFIC_OUTPUT_DATA_CB cb_packet1,
                    NEW_SESSION_CB cb_session1)
        : radio_port(radio_port1),
          cb_packet(std::move(cb_packet1)),
          cb_session(std::move(cb_session1)) {}
  };
  void rx_register_stream_handler(std::shared_ptr<StreamRxHandler> handler);
  void rx_unregister_stream_handler(uint8_t radio_port);
  typedef std::function<void(uint64_t nonce, int wlan_index,
                             const uint8_t radioPort, const uint8_t* data,
                             const int data_len)>
      OUTPUT_DATA_CALLBACK;
  // register callback that is called each time a valid packet is received (any
  // multiplexed stream)
  void rx_register_callback(OUTPUT_DATA_CALLBACK cb);
  // register callback that is called when the wifi card (probably)
  // disconneccted
  typedef std::function<void(int error)> DEVICE_FATAL_ERROR_CALLBACK;
  DEVICE_FATAL_ERROR_CALLBACK m_fatal_error_cb = nullptr;

  void register_fatal_error_callback(const DEVICE_FATAL_ERROR_CALLBACK& cb);
  /**
   * Receiving packets happens in the background in another thread.
   */
  void start_receiving();
  void stop_receiving();

  // Statistics
  struct TxStats {
    int64_t n_injected_packets = 0;
    // excluding wifi / radiotap / encryption overhead
    int64_t n_injected_bytes_excluding_overhead = 0;
    // including wifi / radiotap / encryption overhead, as well as session key
    // packets
    int64_t n_injected_bytes_including_overhead = 0;
    // recalculated in X second intervals
    int curr_packets_per_second = -1;
    int curr_bits_per_second_excluding_overhead = -1;
    int curr_bits_per_second_including_overhead = -1;
    // tx error hint, first sign the tx can't keep up with the provided bitrate
    int32_t count_tx_injections_error_hint = 0;
    // actual tx errors - e.g. packets dropped during injection.
    // Usually, this shouldn't increase, since "injecting a frame" should be a
    // blocking operation (until there is space available in the tx queue, aka
    // either linux network or driver packet queue) and openhd does automatic
    // bitrate adjust at the tx.
    int32_t count_tx_dropped_packets = 0;
  };
  struct RxStats {
    // Total count of received packets / bytes - can be from another wb tx, but
    // also from someone else using wifi
    int64_t count_p_any = 0;
    int64_t count_bytes_any = 0;
    // Total count of valid received packets / bytes (decrypted)
    int64_t count_p_valid = 0;
    int64_t count_bytes_valid = 0;
    // Those values are recalculated in X second intervals.
    // If no data arrives for a long time, they report -1 instead of 0
    // Current packet loss on whatever card reports the lowest packet loss (or
    // card0 if there are not multiple RX cards)
    int32_t curr_lowest_packet_loss = -1;
    int32_t curr_packets_per_second = -1;
    int32_t curr_bits_per_second = -1;
    // n received valid session key packets
    int n_received_valid_session_key_packets = 0;
    // Percentage of non openhd packets over total n of packets
    int curr_link_pollution_perc = 0;
    // N of non openhd packets in the last second
    int curr_n_foreign_packets_pps = 0;
    // Usefully for channel scan - n packets that are quite likely coming from
    // an openhd air / ground unit (respective depending on if air/gnd mode) But
    // not validated - e.g. on a channel scan, session key packet(s) have not
    // been received yet
    int curr_n_likely_openhd_packets = 0;
    // Usefully for telling the user that he is probably using incompatible bind
    // phrases / encryption keys on air and ground
    bool likely_mismatching_encryption_key = false;
  };
  struct RxStatsPerCard {
    int card_index = 0;  // 0 for first card, 1 for second, ...
    int64_t count_p_any = 0;
    int64_t count_p_valid = 0;
    int32_t curr_packet_loss = -1;
  };
  TxStats get_tx_stats();
  RxStats get_rx_stats();
  RxStatsPerCard get_rx_stats_for_card(int card_index);
  RadiotapRxRfAggregator::CardKeyRfIndicators get_rx_rf_stats_for_card(
      int card_index);
  // used by openhd during frequency scan
  void rx_reset_stats();
  // used by the rate adjustment test executable
  void tx_reset_stats();
  // OpenHD displays whatever card is currently transmitting in the OSD
  int get_curr_active_tx_card_idx();
  // Used by OpenHD to do "passive mode" on a GCS
  void set_passive_mode(bool passive);
  // Used by OpenHD on the ground to notify the user of disconnecting card(s)
  // (Hints at power issues)
  bool get_card_has_disconnected(int card_idx);
  // For development only
  std::shared_ptr<DummyLink> get_dummy_link();

 public:
  struct SessionExtraData {
    // OpenHD uses different carrier bandwidths for the session key data (20Mhz,
    // such that it can be always received on 20Mhz BW) and - if enabled - 40Mhz
    // for the rest of the (video, telemetry) data. NOTE: To receive 40Mhz data,
    // the RX needs to be configured for 40Mhz receive (and this comes at a
    // slight dBm penalty)
    uint8_t tx_data_channel_width;
  } __attribute__((__packed__));
  // Session key used for encrypting outgoing packets
  struct SessionKeyPacket {
    std::array<uint8_t, crypto_box_NONCEBYTES>
        sessionKeyNonce{};  // random data
    std::array<uint8_t,
               crypto_aead_chacha20poly1305_KEYBYTES + crypto_box_MACBYTES>
        sessionKeyData{};  // encrypted session key
  };
  static_assert(sizeof(SessionKeyPacket) == 72);
  // The final packet size ( radiotap header + iee80211 header + payload ) is
  // never bigger than that the reasoning behind this value:
  // https://github.com/svpcom/wifibroadcast/issues/69
  static constexpr const auto PCAP_MAX_PACKET_SIZE = 1510;
  // This is the max number of bytes usable when injecting
  static constexpr const auto RAW_WIFI_FRAME_MAX_PAYLOAD_SIZE =
      (PCAP_MAX_PACKET_SIZE - RadiotapHeaderTx::SIZE_BYTES -
       IEEE80211_HEADER_SIZE_BYTES);
  static_assert(RAW_WIFI_FRAME_MAX_PAYLOAD_SIZE == 1473);
  // and we use some bytes of that for encryption / packet validation
  static constexpr const auto MAX_PACKET_PAYLOAD_SIZE =
      RAW_WIFI_FRAME_MAX_PAYLOAD_SIZE - crypto_aead_chacha20poly1305_ABYTES;
  static_assert(MAX_PACKET_PAYLOAD_SIZE == 1457);
  static std::string tx_stats_to_string(const TxStats& data);
  static std::string rx_stats_to_string(const RxStats& data);
  static std::string rx_stats_per_card_to_string(const RxStatsPerCard& data);

 private:
  const Options m_options;
  std::shared_ptr<spdlog::logger> m_console;
  std::shared_ptr<RadiotapHeaderTxHolder> m_session_key_radiotap_header;
  const std::vector<wifibroadcast::WifiCard> m_wifi_cards;
  std::chrono::steady_clock::time_point m_session_key_next_announce_ts{};
  Ieee80211HeaderOpenHD m_tx_ieee80211_hdr_openhd{};
  std::array<uint8_t, PCAP_MAX_PACKET_SIZE> m_tx_packet_buff{};
  uint16_t m_ieee80211_seq = 0;
  struct RadioPort {
    uint8_t encrypted : 1;  // 1 bit encryption enabled / disabled
    uint8_t
        multiplex_index : 7;  // 7 bit multiplex / stream index (2^7=128 => 126
                              // possible multiplexed streams since one is
                              // reserved for session keys and we count from 0)
  } __attribute__((packed));
  static_assert(sizeof(RadioPort) == 1);
  static uint8_t radio_port_to_uint8_t(const RadioPort& radio_port) {
    uint8_t ret;
    memcpy(&ret, (void*)&radio_port, 1);
    return ret;
  }
  static constexpr auto STREAM_INDEX_MIN = 0;
  static constexpr auto STREAM_INDEX_MAX = 126;
  // Not available as a valid stream index, since used for the session packets
  static constexpr auto STREAM_INDEX_SESSION_KEY_PACKETS = 127;
  uint64_t m_nonce = 0;
  // For multiple RX cards the card with the highest rx rssi is used to inject
  // packets on
  std::atomic<int> m_curr_tx_card = 0;
  struct ActiveCardCalculationData {
    int64_t last_received_n_valid_packets = 0;
  };
  std::vector<ActiveCardCalculationData> m_active_tx_card_data;
  SessionKeyPacket m_tx_sess_key_packet;
  std::unique_ptr<wb::Encryptor> m_encryptor;
  std::unique_ptr<wb::Decryptor> m_decryptor;
  struct PcapTxRx {
    pcap_t* tx = nullptr;
    pcap_t* rx = nullptr;
    int tx_sockfd = -1;
  };
  std::vector<PcapTxRx> m_pcap_handles;
  // temporary
  std::mutex m_tx_mutex;
  bool keep_receiving = true;
  int m_n_receiver_errors = 0;
  std::unique_ptr<std::thread> m_receive_thread;
  std::vector<pollfd> m_receive_pollfds;
  std::chrono::steady_clock::time_point m_last_receiver_error_log =
      std::chrono::steady_clock::now();
  UINT16SeqNrHelper m_seq_nr_helper_iee80211;
  // for calculating the loss and more per rx card (when multiple rx cards are
  // used)
  struct PerCardCalculators {
    UINT64SeqNrHelper seq_nr{};
    RadiotapRxRfAggregator rf_aggregator;
    void reset_all();
  };
  std::vector<std::shared_ptr<PerCardCalculators>> m_per_card_calc;
  OUTPUT_DATA_CALLBACK m_output_cb = nullptr;
  RxStats m_rx_stats{};
  TxStats m_tx_stats{};
  std::vector<RxStatsPerCard> m_rx_stats_per_card;
  std::map<int, std::shared_ptr<StreamRxHandler>> m_rx_handlers;
  // If each iteration pulls too many packets out your CPU is most likely too
  // slow
  AvgCalculatorSize m_n_packets_polled_pcap;
  AvgCalculator m_packet_host_latency;
  // We adjust the TX card in 1 second intervals
  std::chrono::steady_clock::time_point m_last_highest_rssi_adjustment_tp =
      std::chrono::steady_clock::now();
  static constexpr auto HIGHEST_RSSI_ADJUSTMENT_INTERVAL =
      std::chrono::seconds(1);
  std::atomic_bool m_disable_all_transmissions = false;
  std::vector<bool> m_card_is_disconnected;
  BitrateCalculator m_tx_bitrate_calculator_excluding_overhead{};
  BitrateCalculator m_tx_bitrate_calculator_including_overhead{};
  PacketsPerSecondCalculator m_tx_packets_per_second_calculator{};
  BitrateCalculator m_rx_bitrate_calculator{};
  PacketsPerSecondCalculator m_rx_packets_per_second_calculator{};
  AvgCalculator m_packet_encrypt_time;
  AvgCalculator m_packet_decrypt_time;
  AvgCalculator m_tx_inject_time;

 private:
  // For OpenHD rate control, this method should block until the driver accepted
  // the packet returns true if packet is now in linux kernel / driver hands,
  // false otherwise. on failure, m_tx_stats.count_tx_errors is increased by one
  // if injection takes "really long", tx error hint is increase
  bool inject_radiotap_packet(int card_index, const uint8_t* packet_buff,
                              int packet_size);
  // we announce the session key in regular intervals if data is currently being
  // injected (tx_ is called)
  void announce_session_key_if_needed();
  // send out the session key
  void send_session_key();
  // called by the receive thread, wait for data to become available then pull
  // data
  void loop_receive_packets();
  // pull data from a pcap handle which has data available
  int loop_iter_pcap(int rx_index);
  int loop_iter_raw(int rx_index);
  // returns true if the packet has a valid layout and is aimed at this receiver
  void process_session_stream_packet(
      uint8_t wlan_idx, const RadioPort& radio_port,
      const std::optional<radiotap::rx::ParsedRxRadiotapPacket>& parsedPacket,
      size_t pkt_payload_size, uint64_t nonce);
  // returns true if the packet has a valid layout and is aimed at this receiver
  void process_common_stream_packet(
      uint8_t wlan_idx, const RadioPort& radio_port, const uint8_t* pkt,
      int pkt_len,
      std::optional<radiotap::rx::ParsedRxRadiotapPacket> parsedPacket,
      const uint8_t* pkt_payload, size_t pkt_payload_size, uint64_t nonce);
  // called every time we have a new (raw) data packet
  void on_new_packet(uint8_t wlan_idx, const uint8_t* pkt, int pkt_len);
  // verify and decrypt the packet if possible
  // returns true if packet could be decrypted successfully
  bool process_received_data_packet(int wlan_idx, uint8_t stream_index,
                                    bool encrypted, uint64_t nonce,
                                    const uint8_t* pkt_payload,
                                    int pkt_payload_size);
  // called avery time we have successfully decrypted a packet
  void on_valid_data_packet(uint64_t nonce, int wlan_index,
                            uint8_t stream_index, const uint8_t* data,
                            int data_len);
  static std::string options_to_string(
      const std::vector<std::string>& wifi_cards, const Options& options);
  // Adjustment of which card is used for injecting packets in case there are
  // multiple RX card(s) (Of all cards currently receiving data, find the one
  // with the highest reported dBm)
  void switch_tx_card_if_needed();

 private:
  // These are 'extra' for calculating some channel pollution value
  uint32_t m_pollution_total_rx_packets = 0;
  uint32_t m_pollution_openhd_rx_packets = 0;
  std::chrono::steady_clock::time_point m_last_pollution_calculation =
      std::chrono::steady_clock::now();
  void recalculate_pollution_perc();
  // These are 'extra' for calculating the "likely wrong encryption keys" value
  uint32_t m_likely_wrong_encryption_valid_session_keys = 0;
  std::chrono::steady_clock::time_point m_likely_wrong_encryption_last_check =
      std::chrono::steady_clock::now();
  uint32_t m_likely_wrong_encryption_invalid_session_keys = 0;
  std::shared_ptr<DummyLink> m_optional_dummy_link = nullptr;
};

static std::ostream& operator<<(std::ostream& strm,
                                const WBTxRx::TxStats& data) {
  strm << WBTxRx::tx_stats_to_string(data);
  return strm;
}
static std::ostream& operator<<(std::ostream& strm,
                                const WBTxRx::RxStats& data) {
  strm << WBTxRx::rx_stats_to_string(data);
  return strm;
}
static std::ostream& operator<<(std::ostream& strm,
                                const WBTxRx::RxStatsPerCard& data) {
  strm << WBTxRx::rx_stats_per_card_to_string(data);
  return strm;
}

#endif  // WIFIBROADCAST_WBTXRX_H
