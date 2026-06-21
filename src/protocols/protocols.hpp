#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "audio/audio.hpp"
#include "core/core.hpp"
#include "crypto/crypto.hpp"
#include "io/io.hpp"
#include "protocols/airplay_protocol.hpp"
namespace mirage::protocols {
enum class rtsp_session_state : uint8_t {
    init,
    pair_setup,
    pair_verify,
    announced,
    ready,
    playing,
    paused,
    teardown
};
struct rtsp_request {
    std::string method;
    std::string uri;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::vector<std::byte> body;
};
struct rtsp_response {
    int status_code;
    std::string status_text;
    std::unordered_map<std::string, std::string> headers;
    std::vector<std::byte> body;
};
class rtsp_session : public std::enable_shared_from_this<rtsp_session> {
public:
    static std::shared_ptr<rtsp_session> create(io::tcp_stream socket,
                                                crypto::fairplay_pairing pairing);
    io::task<result<void>> run();
    void stop();
    [[nodiscard]] rtsp_session_state state() const { return state_; }

private:
    io::task<result<rtsp_request>> read_request();
    io::task<result<void>> send_response(const rtsp_response& resp);
    io::task<result<rtsp_response>> handle_request(const rtsp_request& req);
    result<rtsp_response> handle_options(const rtsp_request& req) const;
    result<rtsp_response> handle_info(const rtsp_request& req);
    io::task<result<rtsp_response>> handle_pair_setup(const rtsp_request& req);
    io::task<result<rtsp_response>> handle_pair_verify(const rtsp_request& req);
    result<rtsp_response> handle_announce(const rtsp_request& req);
    io::task<result<rtsp_response>> handle_setup(const rtsp_request& req);
    result<rtsp_response> handle_record(const rtsp_request& req);
    result<rtsp_response> handle_teardown(const rtsp_request& req);
    result<rtsp_response> handle_get_parameter(const rtsp_request& req) const;
    result<rtsp_response> handle_set_parameter(const rtsp_request& req);
    result<rtsp_response> handle_pause(const rtsp_request& req);
    result<rtsp_response> handle_flush(const rtsp_request& req);
    result<rtsp_response> handle_fp_setup(const rtsp_request& req);
    io::task<void> run_udp_receiver(io::udp_socket& sock, const char* name);
    io::task<void> run_ntp_timing_sender();
    io::task<void> run_mirror_receiver();
    io::task<void> run_audio_receiver();
    io::task<void> run_audio_control_receiver();
    void process_audio_packet(std::span<const std::byte> rtp_packet, size_t len);
    bool configure_audio_decoder();
    bool queue_audio_packet(std::span<const std::byte> rtp_packet, uint16_t seqnum,
                            bool retransmitted);
    void drain_audio_packets();
    io::task<void> send_audio_resend_request(uint16_t start_seqnum, uint16_t count);
    void reset_audio_packet_state();
    void close_audio_stream();
    void close_video_stream();
    void close_stream_sockets();
    rtsp_session(io::tcp_stream socket, crypto::fairplay_pairing pairing);
    struct buffered_audio_packet {
        uint16_t seqnum = 0;
        std::vector<std::byte> packet;
        bool retransmitted = false;
    };
    io::tcp_stream socket_;
    crypto::fairplay_pairing pairing_;
    rtsp_session_state state_ = rtsp_session_state::init;
    uint32_t cseq_ = 0;
    std::vector<std::byte> fp_keymsg_;
    std::vector<std::byte> fp_ekey_;
    std::array<std::byte, 16> audio_aes_key_{};
    std::array<std::byte, 16> audio_aes_iv_{};
    bool audio_keys_ready_ = false;
    std::unique_ptr<io::udp_socket> timing_socket_;
    std::unique_ptr<io::udp_socket> control_socket_;
    std::unique_ptr<io::tcp_acceptor> mirror_acceptor_;
    std::unique_ptr<io::udp_socket> audio_data_socket_;
    std::unique_ptr<io::udp_socket> audio_control_socket_;
    std::unique_ptr<audio::audio_player> audio_player_;
    std::unique_ptr<audio::audio_decoder> audio_decoder_;
    io::endpoint client_timing_endpoint_;
    bool base_receivers_started_ = false;
    bool mirror_receiver_started_ = false;
    bool audio_receiver_started_ = false;
    uint64_t stream_connection_id_ = 0;
    uint8_t audio_ct_ = 0;
    int audio_sample_rate_ = airplay::default_sample_rate;
    int audio_channels_ = airplay::default_channels;
    int audio_spf_ = 352;
    bool audio_sequence_started_ = false;
    uint16_t next_audio_seqnum_ = 0;
    uint64_t audio_packet_count_ = 0;
    uint64_t audio_silent_marker_count_ = 0;
    uint64_t audio_invalid_payload_count_ = 0;
    uint64_t audio_gap_count_ = 0;
    uint64_t audio_duplicate_packet_count_ = 0;
    uint64_t audio_stale_packet_count_ = 0;
    uint64_t audio_resend_request_count_ = 0;
    uint16_t audio_resend_seqnum_ = 0;
    bool audio_resend_pending_ = false;
    uint16_t audio_resend_pending_start_ = 0;
    uint16_t audio_resend_pending_count_ = 0;
    float audio_volume_db_ = 0.0F;
    float audio_linear_volume_ = 1.0F;
    io::endpoint audio_control_remote_;
    std::vector<buffered_audio_packet> audio_pending_packets_;
};
class rtsp_server {
public:
    static result<rtsp_server> bind(io::io_context& ctx, uint16_t port,
                                    crypto::ed25519_keypair keypair);
    io::task<void> run();
    void stop();

private:
    struct session_store;
    rtsp_server(io::tcp_acceptor acceptor, crypto::ed25519_keypair keypair);
    io::tcp_acceptor acceptor_;
    crypto::ed25519_keypair keypair_;
    std::shared_ptr<session_store> sessions_;
    bool running_ = false;
};
enum class cast_message_type : uint8_t { connect, close, heartbeat, get_status, launch_app, media };
class cast_receiver {
public:
    static result<cast_receiver> bind(io::io_context& ctx, uint16_t port);
    io::task<void> run();
    void stop();
    ~cast_receiver();
    cast_receiver(cast_receiver&&) noexcept;
    cast_receiver& operator=(cast_receiver&&) noexcept;

private:
    struct impl;
    explicit cast_receiver(std::unique_ptr<impl> impl_ptr);
    std::unique_ptr<impl> impl_;
};
class wfd_session {
public:
    static result<wfd_session> create(io::io_context& ctx);
    io::task<void> run();
    void stop();
    ~wfd_session();
    wfd_session(wfd_session&&) noexcept;
    wfd_session& operator=(wfd_session&&) noexcept;

private:
    struct impl;
    explicit wfd_session(std::unique_ptr<impl> impl_ptr);
    std::unique_ptr<impl> impl_;
};
}  // namespace mirage::protocols
