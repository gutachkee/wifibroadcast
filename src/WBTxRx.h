//
// Created by consti10 on 27.06.23.
//

#ifndef WIFIBROADCAST_WBTXRX_H
#define WIFIBROADCAST_WBTXRX_H

#include <sys/poll.h>

#include <atomic>
#include <map>
#include <utility>
#include <thread>

#include "Encryption.hpp"
#include "Ieee80211Header.hpp"
#include "RSSIForWifiCard.hpp"
#include "RadiotapHeader.hpp"
#include "SeqNrHelper.hpp"
#include "TimeHelper.hpp"

/**
 * This class exists to provide a clean, working interface to create a broadcast-like
 * bidirectional wifi link between an fpv air and (one or more) ground unit(s).
 * It hides away some nasty driver quirks, and offers
 * 1) A lot of usefully stats like packet loss, dbm, ...
 * 2) Multiplexing (radio_port) - multiple streams from air to ground / ground to air are possible
 * 3) Packet validation / encryption (selectable per packet)
 * 4) Multiple RX-cards (only one active tx at a time though)
 * Packets sent by an "air unit" are received by any listening ground unit (broadcast) that uses the same (encryption/validation) key-pair
 * Packets sent by an "ground unit" are received by any listening air unit (broadcast) that uses the same (encryption/validation) key-pair
 * Packets sent by an "air unit" are never received by another air unit (and reverse for ground unit)
 * (This is necessary due to AR9271 driver quirk - it gives injected packets back on the cb for received packets)
 *
 * It adds a minimal overhead of 16 bytes per data packet for validation / encryption
 * And - configurable - a couple of packets per second for the session key.
 *
 * See example_hello for how to use this class.
 *
 * NOTE: Receiving of data is not started until startReceiving() is called !
 */
class WBTxRx {
 public:
  struct Options{
    // file for encryptor
    // make optional for ease of use - with no keypair given the default "seed" is used
    std::optional<std::string> encryption_key = std::nullopt;
    // dirty, rssi on rtl8812au is "bugged", this discards the first rssi value reported by the card.
    bool rtl8812au_rssi_fixup=false;
    // on the rx pcap fd, set direction PCAP_D_IN (aka only packets received by the card) - doesn't work on AR9271
    bool set_direction= true;
    // thy spam the console, but usefully for debugging
    // log all received packets (regardless where they are from)
    bool log_all_received_packets= false;
    bool log_all_received_validated_packets= false;
    // more verbose tx logging
    bool advanced_debugging_tx = false;
    // more verbose rx logging
    bool advanced_debugging_rx = false;
    // advanced latency related debugging
    bool advanced_latency_debugging_rx= false;
    // set sched_param = max realtime on the thread that pulls out the packets
    bool receive_thread_max_realtime= true;
    // enable / disable switching on which card to send packets in case there are multiple cards given
    // if this option is disabled, card 0 is always used for sending
    bool enable_auto_switch_tx_card= true;
    // interval in which the session key packet is sent out - if no data is fed to the TX,
    // no session key is sent until data is fed.
    std::chrono::milliseconds session_key_packet_interval=std::chrono::seconds(1);
    // You need to set this to air / gnd on the air / gnd unit since AR9271 has a bug where it reports injected packets as received packets
    bool use_gnd_identifier= false;
    // RSSI can be tricky
    bool debug_rssi= false;
  };
  explicit WBTxRx(std::vector<std::string> wifi_cards,Options options1);
  WBTxRx(const WBTxRx &) = delete;
  WBTxRx &operator=(const WBTxRx &) = delete;
  ~WBTxRx();
  /**
   * Creates a valid injection packet which has the layout:
   * radiotap_header,ieee_80211_header, data (encrypted or not encrypted), encryption/validation suffix
   * A increasing nonce is used for each packet, and is used for packet validation
   * on the receiving side.
   * NOTE: Encryption and/or validation adds a fixed amount of overhead to each injected packet !
   * @param stream_index used to multiplex more than one data stream, written into the IEE80211 header
   * uint8_t but needs to be in range of [MIN,MAX] stream index
   * @param data the packet payload
   * @param data_len the packet payload length
   * @param encrypt: Optionally encrypt the packet, if not encrypted, it is only validated in a secure way
   * Encryption results in more CPU load and is therefore not wanted in all cases (e.g. by default, openhd does not encrypt video)
   */
  void tx_inject_packet(uint8_t stream_index,const uint8_t* data,int data_len,bool encrypt=false);

  /**
   * A typical stream RX (aka the receiver for a specific multiplexed stream) needs to react to events during streaming.
   * For lowest latency, we do this via callback(s) that are called directly.
   * You can register listening on these events and also deregister them here.
   * @param nonce: the nonce of the received packet (can be used for sequence numbering)
   * @param wlan_index: the card on which the packet was received (in case there are multiple cards used for wb)
   * @param radio_port: the multiplex index used to separate streams during injection
   */
  typedef std::function<void(uint64_t nonce,int wlan_index,const uint8_t *data, const std::size_t data_len)> SPECIFIC_OUTPUT_DATA_CB;
  typedef std::function<void()> NEW_SESSION_CB;
  struct StreamRxHandler{
    uint8_t radio_port; // For which multiplexed stream this handles events
    SPECIFIC_OUTPUT_DATA_CB cb_packet; // called every time a packet for this stream is received
    NEW_SESSION_CB cb_session; // called every time a new session is detected
    StreamRxHandler(uint8_t radio_port1,SPECIFIC_OUTPUT_DATA_CB cb_packet1,NEW_SESSION_CB cb_session1)
    :radio_port(radio_port1),cb_packet(std::move(cb_packet1)),cb_session(std::move(cb_session1)){}
  };
  void rx_register_stream_handler(std::shared_ptr<StreamRxHandler> handler);
  void rx_unregister_stream_handler(uint8_t radio_port);
  typedef std::function<void(uint64_t nonce,int wlan_index,const uint8_t radioPort,const uint8_t *data, const std::size_t data_len)> OUTPUT_DATA_CALLBACK;
  // register callback that is called each time a valid packet is received (any multiplexed stream)
  void rx_register_callback(OUTPUT_DATA_CALLBACK cb);

  /**
   * Receiving packets happens in the background in another thread.
   */
  void start_receiving();
  void stop_receiving();

   // These are for updating injection parameters at run time. They will be applied on the next injected packet.
   // They are generally thread-safe. See RadiotapHeader for more information on what these parameters do.
   // After calling this method, the injected packets will use a different radiotap header
   // I'd like to use an atomic instead of mutex, but unfortunately some compilers don't eat atomic struct
   void tx_threadsafe_update_radiotap_header(const RadiotapHeader::UserSelectableParams& params);
   void tx_update_mcs_index(uint8_t mcs_index);
   void tx_update_channel_width(int width_mhz);
   void tx_update_stbc(int stbc);
   void tx_update_guard_interval(bool short_gi);
   void tx_update_ldpc(bool ldpc);

   // Statistics
   struct TxStats{
     int64_t n_injected_packets=0;
     // excluding wifi / radiotap / encryption overhead
     int64_t n_injected_bytes_excluding_overhead =0;
     // including wifi / radiotap / encryption overhead, as well as session key packets
     int64_t n_injected_bytes_including_overhead =0;
     // recalculated in X second intervals
     int curr_packets_per_second=-1;
     int curr_bits_per_second_excluding_overhead=-1;
     int curr_bits_per_second_including_overhead=-1;
     // tx error hint, first sign the tx can't keep up with the provided bitrate
     int32_t count_tx_injections_error_hint=0;
     // actual tx errors
     int32_t count_tx_errors=0;
   };
   struct RxStats{
     // Total count of received packets / bytes - can be from another wb tx, but also from someone else using wifi
     int64_t count_p_any=0;
     int64_t count_bytes_any=0;
     // Total count of valid received packets / bytes (decrypted)
     int64_t count_p_valid=0;
     int64_t count_bytes_valid=0;
     // Those values are recalculated in X second intervals.
     // If no data arrives for a long time, they report -1 instead of 0
     int32_t curr_packet_loss=-1;
     int32_t curr_packets_per_second=-1;
     int32_t curr_bits_per_second=-1;
     // n received valid session key packets
     int n_received_valid_session_key_packets=0;
     // mcs index on the most recent okay data packet, if the card supports reporting it
     int last_received_packet_mcs_index=-1;
     // channel width (20Mhz or 40Mhz) on the most recent received okay data packet, if the card supports reporting it
     int last_received_packet_channel_width=-1;
     // complicated but important metric in our case - how many "big gaps" we had in the last 1 second
     int16_t curr_big_gaps_counter=-1;
     // Percentage of non openhd packets over total n of packets
     int curr_link_pollution_perc=0;
     // Usefully for channel scan - n packets that are quite likely coming from an openhd air / ground unit (respective depending on if air/gnd mode)
     // But not validated - e.g. on a channel scan, session key packet(s) have not been received yet
     int curr_n_likely_openhd_packets=0;
   };
   struct RxStatsPerCard{
     RSSIForWifiCard rssi_for_wifi_card{};
     int64_t count_p_any=0;
     int64_t count_p_valid=0;
     int32_t curr_packet_loss=-1;
     int signal_quality=-1;
   };
   TxStats get_tx_stats();
   RxStats get_rx_stats();
   RxStatsPerCard get_rx_stats_for_card(int card_index);
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
  public:
   // Session key used for encrypting outgoing packets
   struct SessionKeyPacket{
     std::array<uint8_t, crypto_box_NONCEBYTES> sessionKeyNonce{};  // random data
     std::array<uint8_t, crypto_aead_chacha20poly1305_KEYBYTES + crypto_box_MACBYTES> sessionKeyData{}; // encrypted session key
   };
   // The final packet size ( radiotap header + iee80211 header + payload ) is never bigger than that
   // the reasoning behind this value: https://github.com/svpcom/wifibroadcast/issues/69
   static constexpr const auto PCAP_MAX_PACKET_SIZE = 1510;
   // This is the max number of bytes usable when injecting
   static constexpr const auto RAW_WIFI_FRAME_MAX_PAYLOAD_SIZE = (PCAP_MAX_PACKET_SIZE - RadiotapHeader::SIZE_BYTES -
        IEEE80211_HEADER_SIZE_BYTES);
   static_assert(RAW_WIFI_FRAME_MAX_PAYLOAD_SIZE==1473);
   // and we use some bytes of that for encryption / packet validation
   static constexpr const auto MAX_PACKET_PAYLOAD_SIZE=RAW_WIFI_FRAME_MAX_PAYLOAD_SIZE-crypto_aead_chacha20poly1305_ABYTES;
   static_assert(MAX_PACKET_PAYLOAD_SIZE==1457);
   static std::string tx_stats_to_string(const TxStats& data);
   static std::string rx_stats_to_string(const RxStats& data);
 private:
  const Options m_options;
  std::shared_ptr<spdlog::logger> m_console;
  std::vector<std::string> m_wifi_cards;
  std::chrono::steady_clock::time_point m_session_key_next_announce_ts{};
  RadiotapHeader::UserSelectableParams m_radioTapHeaderParams{};
  RadiotapHeader m_tx_radiotap_header;
  Ieee80211HeaderOpenHD m_tx_ieee80211_hdr_openhd{};
  uint16_t m_ieee80211_seq = 0;
  struct RadioPort{
     uint8_t encrypted: 1; // 1 bit encryption enabled / disabled
     uint8_t multiplex_index: 7; // 7 bit multiplex / stream index (2^7=128 => 127 possible multiplexed streams since one is reserved for session keys)
  }__attribute__ ((packed));
  static_assert(sizeof(RadioPort)==1);
  static uint8_t radio_port_to_uint8_t(const RadioPort& radio_port){
     uint8_t ret;
     memcpy(&ret,(void*)&radio_port,1);
     return ret;
  }
  static constexpr auto STREAM_INDEX_MIN =0;
  static constexpr auto STREAM_INDEX_MAX =127;
  // Not available as a valid stream index, since used for the session packets
  static constexpr auto STREAM_INDEX_SESSION_KEY_PACKETS =128;
  uint64_t m_nonce=0;
  // For multiple RX cards the card with the highest rx rssi is used to inject packets on
  std::atomic<int> m_curr_tx_card=0;
  SessionKeyPacket m_tx_sess_key_packet;
  std::unique_ptr<Encryptor> m_encryptor;
  std::unique_ptr<Decryptor> m_decryptor;
  struct PcapTxRx{
    pcap_t *tx= nullptr;
    pcap_t *rx= nullptr;
  };
  std::vector<PcapTxRx> m_pcap_handles;
  // temporary
  std::mutex m_tx_mutex;
  bool keep_receiving= true;
  int m_n_receiver_errors=0;
  std::unique_ptr<std::thread> m_receive_thread;
  std::vector<pollfd> m_receive_pollfds;
  std::chrono::steady_clock::time_point m_last_receiver_error_log=std::chrono::steady_clock::now();
  // for calculating the packet loss on the rx side
  seq_nr::Helper m_seq_nr_helper;
  seq_nr::Helper m_seq_nr_helper_iee80211;
  // for calculating the loss per rx card (when multiple rx cards are used)
  std::vector<std::shared_ptr<seq_nr::Helper>> m_seq_nr_per_card;
  OUTPUT_DATA_CALLBACK m_output_cb= nullptr;
  RxStats m_rx_stats{};
  TxStats m_tx_stats{};
  // a tx error is thrown if injecting the packet takes longer than MAX_SANE_INJECTION_TIME,
  // which hints at an overflowing tx queue (unfortunately I don't know a way to directly get the tx queue yet)
  // However, this hint can be misleading - for example, during testing (MCS set to 3) and with about 5MBit/s video after FEC
  // I get about 5 tx error(s) per second with my atheros, but it works fine. This workaround also seems to not work at all
  // with the RTL8812au.
  static constexpr std::chrono::nanoseconds MAX_SANE_INJECTION_TIME=std::chrono::milliseconds(5);
  std::vector<RxStatsPerCard> m_rx_stats_per_card;
  std::map<int,std::shared_ptr<StreamRxHandler>> m_rx_handlers;
  // If each iteration pulls too many packets out your CPU is most likely too slow
  AvgCalculatorSize m_n_packets_polled_pcap;
  AvgCalculator m_packet_host_latency;
  // We adjust the TX card in 1 second intervals
  std::chrono::steady_clock::time_point m_last_highest_rssi_adjustment_tp=std::chrono::steady_clock::now();
  static constexpr auto HIGHEST_RSSI_ADJUSTMENT_INTERVAL=std::chrono::seconds(1);
  bool m_disable_all_transmissions= false;
  std::vector<bool> m_card_is_disconnected;
  BitrateCalculator m_tx_bitrate_calculator_excluding_overhead{};
  BitrateCalculator m_tx_bitrate_calculator_including_overhead{};
  PacketsPerSecondCalculator m_tx_packets_per_second_calculator{};
  BitrateCalculator m_rx_bitrate_calculator{};
  PacketsPerSecondCalculator m_rx_packets_per_second_calculator{};
 private:
  // we announce the session key in regular intervals if data is currently being injected (tx_ is called)
  void announce_session_key_if_needed();
  // send out the session key
  void send_session_key();
  // called by the receive thread, wait for data to become available then pull data
  void loop_receive_packets();
  // pull data from a pcap handle which has data available
  int loop_iter(int rx_index);
  // called every time we have a new (raw) data packet
  void on_new_packet(uint8_t wlan_idx, const pcap_pkthdr &hdr, const uint8_t *pkt);
  // verify and decrypt the packet if possible
  // returns true if packet could be decrypted successfully
  bool process_received_data_packet(int wlan_idx,uint8_t stream_index,bool encrypted,uint64_t nonce,const uint8_t *pkt_payload,int pkt_payload_size);
  // called avery time we have successfully decrypted a packet
  void on_valid_packet(uint64_t nonce,int wlan_index,uint8_t stream_index,const uint8_t *data, std::size_t data_len);
  static std::string options_to_string(const std::vector<std::string>& wifi_cards,const Options& options);
 private:
  // These are 'extra' for calculating some channel pollution value
  uint32_t m_pollution_total_rx_packets=0;
  uint32_t m_pollution_openhd_rx_packets=0;
  std::chrono::steady_clock::time_point m_last_pollution_calculation=std::chrono::steady_clock::now();
  void recalculate_pollution_perc();
};

static std::ostream& operator<<(std::ostream& strm, const WBTxRx::TxStats& data){
  strm<<WBTxRx::tx_stats_to_string(data);
  return strm;
}
static std::ostream& operator<<(std::ostream& strm, const WBTxRx::RxStats& data){
  strm<<WBTxRx::rx_stats_to_string(data);
  return strm;
}

#endif  // WIFIBROADCAST_WBTXRX_H
