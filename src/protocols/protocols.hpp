#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/core.hpp"
#include "core/receiver_client.hpp"
#include "core/receiver_source.hpp"
#include "crypto/crypto.hpp"
#include "io/io.hpp"
#include "media/pipeline.hpp"
#include "protocols/airplay/media_source.hpp"
#include "protocols/airplay/rtsp_state.hpp"
#include "protocols/airplay_protocol.hpp"
namespace mirage::protocols {
using rtsp_session_state = airplay::rtsp_session_state;
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
                                                crypto::fairplay_pairing pairing,
                                                receiver_source_descriptor source,
                                                receiver_session_observer* observer = nullptr,
                                                uint64_t client_status_id = 0);
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
    result<rtsp_response> handle_control_post(const rtsp_request& req) const;
    result<rtsp_response> handle_pause(const rtsp_request& req);
    result<rtsp_response> handle_flush(const rtsp_request& req);
    result<rtsp_response> handle_fp_setup(const rtsp_request& req);
    io::task<void> run_udp_receiver(io::udp_socket& sock, const char* name);
    io::task<void> run_ntp_timing_sender();
    io::task<void> run_mirror_receiver();
    io::task<void> run_audio_receiver();
    io::task<void> run_audio_control_receiver();
    bool configure_audio_decoder();
    io::task<void> send_audio_resend_request(uint16_t start_seqnum, uint16_t count);
    void reset_audio_packet_state();
    void close_audio_stream();
    void close_video_stream();
    void close_stream_sockets();
    void publish_media_status();
    rtsp_session(io::tcp_stream socket, crypto::fairplay_pairing pairing,
                 receiver_source_descriptor source, receiver_session_observer* observer,
                 uint64_t client_status_id);
    io::tcp_stream socket_;
    crypto::fairplay_pairing pairing_;
    rtsp_session_state state_ = rtsp_session_state::init;
    uint32_t cseq_ = 0;
    std::vector<std::byte> fp_keymsg_;
    std::vector<std::byte> fp_ekey_;
    std::unique_ptr<io::udp_socket> timing_socket_;
    std::unique_ptr<io::udp_socket> control_socket_;
    std::unique_ptr<io::tcp_acceptor> mirror_acceptor_;
    std::unique_ptr<io::udp_socket> audio_data_socket_;
    std::unique_ptr<io::udp_socket> audio_control_socket_;
    std::unique_ptr<media::media_sink> media_sink_;
    receiver_source_descriptor source_;
    receiver_session_observer* observer_ = nullptr;
    uint64_t client_status_id_ = 0;
    airplay::media_source airplay_media_;
    io::endpoint client_timing_endpoint_;
    bool base_receivers_started_ = false;
    bool mirror_receiver_started_ = false;
    bool audio_receiver_started_ = false;
    uint64_t stream_connection_id_ = 0;
    uint8_t audio_ct_ = 0;
    int audio_sample_rate_ = airplay::default_sample_rate;
    int audio_channels_ = airplay::default_channels;
    int audio_spf_ = 352;
    float audio_volume_db_ = 0.0F;
    float audio_linear_volume_ = 1.0F;
    receiver_client_media_status media_status_;
    uint16_t audio_resend_control_seqnum_ = 0;
    io::endpoint audio_control_remote_;
};
class rtsp_server {
public:
    static result<rtsp_server> bind(io::io_context& ctx, receiver_source_descriptor source,
                                    crypto::ed25519_keypair keypair,
                                    receiver_session_observer* observer = nullptr);
    io::task<void> run();
    void stop();

private:
    struct session_store;
    rtsp_server(io::tcp_acceptor acceptor, receiver_source_descriptor source,
                crypto::ed25519_keypair keypair, receiver_session_observer* observer);
    io::tcp_acceptor acceptor_;
    receiver_source_descriptor source_;
    crypto::ed25519_keypair keypair_;
    std::shared_ptr<session_store> sessions_;
    bool running_ = false;
};
enum class cast_message_type : uint8_t { connect, close, heartbeat, get_status, launch_app, media };
class cast_receiver {
public:
    static result<cast_receiver> bind(io::io_context& ctx, uint16_t port, std::string device_name,
                                      receiver_session_observer* observer = nullptr);
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
    static result<wfd_session> bind(io::io_context& ctx, uint16_t port,
                                    receiver_session_observer* observer = nullptr);
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
