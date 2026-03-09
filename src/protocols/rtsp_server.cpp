#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "audio/audio.hpp"
#include "core/core.hpp"
#include "core/log.hpp"
#include "crypto/crypto.hpp"
#include "io/io.hpp"
#include "media/media.hpp"
#include "protocols/protocols.hpp"
#include "render/render.hpp"
namespace mirage::protocols {
namespace bplist {
struct plist_value;
using PlistDict = std::vector<std::pair<std::string, std::shared_ptr<plist_value>>>;
using PlistArray = std::vector<std::shared_ptr<plist_value>>;
struct plist_value {
    enum class type { uint, string, dict, array } type;
    uint64_t uint_val = 0;
    std::string str_val;
    PlistDict dict_val;
    PlistArray array_val;
    static std::shared_ptr<plist_value> make_uint(uint64_t v) {
        auto p = std::make_shared<plist_value>();
        p->type = type::uint;
        p->uint_val = v;
        return p;
    }
    static std::shared_ptr<plist_value> make_string(std::string s) {
        auto p = std::make_shared<plist_value>();
        p->type = type::string;
        p->str_val = std::move(s);
        return p;
    }
    static std::shared_ptr<plist_value> make_dict() {
        auto p = std::make_shared<plist_value>();
        p->type = type::dict;
        return p;
    }
    static std::shared_ptr<plist_value> make_array() {
        auto p = std::make_shared<plist_value>();
        p->type = type::array;
        return p;
    }
};
static std::vector<std::byte> encode(const std::shared_ptr<plist_value>& root) {
    std::vector<std::shared_ptr<plist_value>> all_objects;
    std::function<size_t(const std::shared_ptr<plist_value>&)> collect;
    collect = [&](const std::shared_ptr<plist_value>& val) -> size_t {
        size_t my_index = all_objects.size();
        all_objects.push_back(val);
        if (val->type == plist_value::type::dict) {
            for (const auto& [key, value] : val->dict_val) {
                auto key_obj = plist_value::make_string(key);
                collect(key_obj);
                collect(value);
            }
        } else if (val->type == plist_value::type::array) {
            for (const auto& item : val->array_val) {
                collect(item);
            }
        }
        return my_index;
    };
    collect(root);
    std::vector<std::byte> objects;
    std::vector<size_t> offsets;
    size_t obj_index = 0;
    std::function<void(const std::shared_ptr<plist_value>&)> write_object;
    write_object = [&](const std::shared_ptr<plist_value>& val) {
        offsets.push_back(objects.size() + 8);
        switch (val->type) {
            case plist_value::type::uint: {
                uint64_t v = val->uint_val;
                if (v <= 0xFF) {
                    objects.push_back(std::byte{0x10});
                    objects.push_back(std::byte{static_cast<uint8_t>(v)});
                } else if (v <= 0xFFFF) {
                    objects.push_back(std::byte{0x11});
                    objects.push_back(std::byte{static_cast<uint8_t>((v >> 8) & 0xFF)});
                    objects.push_back(std::byte{static_cast<uint8_t>(v & 0xFF)});
                } else if (v <= 0xFFFFFFFF) {
                    objects.push_back(std::byte{0x12});
                    objects.push_back(std::byte{static_cast<uint8_t>((v >> 24) & 0xFF)});
                    objects.push_back(std::byte{static_cast<uint8_t>((v >> 16) & 0xFF)});
                    objects.push_back(std::byte{static_cast<uint8_t>((v >> 8) & 0xFF)});
                    objects.push_back(std::byte{static_cast<uint8_t>(v & 0xFF)});
                } else {
                    objects.push_back(std::byte{0x13});
                    for (size_t i = 7; i < 8; --i) {
                        objects.push_back(std::byte{static_cast<uint8_t>((v >> (i * 8)) & 0xFF)});
                    }
                }
                break;
            }
            case plist_value::type::string: {
                size_t len = val->str_val.size();
                if (len < 15) {
                    objects.push_back(std::byte{static_cast<uint8_t>(0x50 | len)});
                } else {
                    objects.push_back(std::byte{0x5F});
                    objects.push_back(std::byte{0x10});
                    objects.push_back(std::byte{static_cast<uint8_t>(len)});
                }
                for (char c : val->str_val) {
                    objects.push_back(std::byte{static_cast<uint8_t>(c)});
                }
                break;
            }
            case plist_value::type::array: {
                size_t count = val->array_val.size();
                if (count < 15) {
                    objects.push_back(std::byte{static_cast<uint8_t>(0xA0 | count)});
                } else {
                    objects.push_back(std::byte{0xAF});
                    objects.push_back(std::byte{0x10});
                    objects.push_back(std::byte{static_cast<uint8_t>(count)});
                }
                size_t child_start = obj_index + 1;
                for (size_t i = 0; i < count; ++i) {
                    objects.push_back(std::byte{static_cast<uint8_t>(child_start + i)});
                }
                break;
            }
            case plist_value::type::dict: {
                size_t count = val->dict_val.size();
                if (count < 15) {
                    objects.push_back(std::byte{static_cast<uint8_t>(0xD0 | count)});
                } else {
                    objects.push_back(std::byte{0xDF});
                    objects.push_back(std::byte{0x10});
                    objects.push_back(std::byte{static_cast<uint8_t>(count)});
                }
                size_t child_start = obj_index + 1;
                for (size_t i = 0; i < count; ++i) {
                    objects.push_back(std::byte{static_cast<uint8_t>(child_start + i * 2)});
                }
                for (size_t i = 0; i < count; ++i) {
                    objects.push_back(std::byte{static_cast<uint8_t>(child_start + i * 2 + 1)});
                }
                break;
            }
        }
        ++obj_index;
    };
    for (const auto& obj : all_objects) {
        write_object(obj);
    }
    std::vector<std::byte> result;
    const char* hdr = "bplist00";
    result.reserve(8);
    for (int i = 0; i < 8; ++i) {
        result.push_back(std::byte{static_cast<uint8_t>(hdr[i])});
    }
    result.insert(result.end(), objects.begin(), objects.end());
    size_t offset_table_start = result.size();
    for (size_t off : offsets) {
        result.push_back(std::byte{static_cast<uint8_t>(off)});
    }
    for (int i = 0; i < 6; ++i) {
        result.push_back(std::byte{0});
    }
    result.push_back(std::byte{1});
    result.push_back(std::byte{1});
    for (int i = 0; i < 7; ++i) {
        result.push_back(std::byte{0});
    }
    result.push_back(std::byte{static_cast<uint8_t>(offsets.size())});
    for (int i = 0; i < 7; ++i) {
        result.push_back(std::byte{0});
    }
    result.push_back(std::byte{0});
    for (int i = 0; i < 7; ++i) {
        result.push_back(std::byte{0});
    }
    result.push_back(std::byte{static_cast<uint8_t>(offset_table_start)});
    return result;
}
static std::optional<uint64_t> parse_uint(std::span<const std::byte> data, size_t offset) {
    if (offset >= data.size()) {
        return std::nullopt;
    }
    uint8_t marker = static_cast<uint8_t>(data[offset]);
    if ((marker & 0xF0) == 0x10) {
        size_t nbytes = static_cast<size_t>(1 << (marker & 0x0F));
        uint64_t val = 0;
        for (size_t i = 0; i < nbytes && offset + 1 + i < data.size(); ++i) {
            val = (val << 8) | static_cast<uint64_t>(static_cast<uint8_t>(data[offset + 1 + i]));
        }
        return val;
    }
    return std::nullopt;
}
static std::optional<size_t> find_key(std::span<const std::byte> data, std::string_view key) {
    for (size_t i = 8; i + key.size() < data.size(); ++i) {
        auto b = static_cast<uint8_t>(data[i]);
        bool is_short_str = (b & 0xF0) == 0x50 && (b & 0x0F) == key.size();
        bool is_long_str = b == 0x5F && i + 2 + key.size() < data.size() &&
                           static_cast<uint8_t>(data[i + 1]) == 0x10 &&
                           static_cast<uint8_t>(data[i + 2]) == key.size();
        size_t str_start = is_short_str ? i + 1 : (is_long_str ? i + 3 : 0);
        if (str_start == 0) {
            continue;
        }
        bool match = true;
        for (size_t j = 0; j < key.size() && match; ++j) {
            if (static_cast<char>(data[str_start + j]) != key[j]) {
                match = false;
            }
        }
        if (match) {
            return str_start + key.size();
        }
    }
    return std::nullopt;
}
static bool has_streams(std::span<const std::byte> data) {
    return find_key(data, "streams").has_value();
}
static std::optional<uint64_t> find_8byte_int(std::span<const std::byte> data) {
    if (data.size() < 40) {
        return std::nullopt;
    }
    for (size_t i = 10; i + 9 < data.size() - 32; ++i) {
        if (static_cast<uint8_t>(data[i]) != 0x13) {
            continue;
        }
        if (i >= 2 && (static_cast<uint8_t>(data[i - 2]) & 0xF0) == 0x10 &&
            (static_cast<uint8_t>(data[i - 1]) & 0xF0) == 0x10) {
            // pattern: 10 xx 10 yy 13 <8 bytes> two 1-byte ints then 8-byte int
        } else if (i >= 1 && static_cast<uint8_t>(data[i - 1]) == 0x6e) {
            // pattern: type=110 (0x6e) then 13 <8 bytes>
        } else {
            continue;
        }
        uint64_t val = 0;
        for (int j = 0; j < 8; ++j) {
            val = (val << 8) | static_cast<uint8_t>(data[i + 1 + static_cast<size_t>(j)]);
        }
        if (val > 0) {
            return val;
        }
    }
    return std::nullopt;
}
static std::optional<uint64_t> get_uint(std::span<const std::byte> data, std::string_view key) {
    auto pos = find_key(data, key);
    if (!pos) {
        return std::nullopt;
    }
    for (size_t i = *pos; i < std::min(*pos + 100, data.size()); ++i) {
        uint8_t b = static_cast<uint8_t>(data[i]);
        if ((b & 0xF0) == 0x10) {
            return parse_uint(data, i);
        }
        if ((b & 0xF0) == 0x50 || (b & 0xF0) == 0xD0 || (b & 0xF0) == 0xA0) {
            break;
        }
    }
    return std::nullopt;
}
}  // namespace bplist
rtsp_session::rtsp_session(io::tcp_stream socket, crypto::fairplay_pairing pairing)
    : socket_(std::move(socket)), pairing_(std::move(pairing)) {}
auto rtsp_session::create(io::tcp_stream socket, crypto::fairplay_pairing pairing)
    -> std::shared_ptr<rtsp_session> {
    return std::shared_ptr<rtsp_session>(new rtsp_session(std::move(socket), std::move(pairing)));
}
io::task<result<void>> rtsp_session::run() {
    mirage::log::info("New RTSP session from {}", socket_.remote_endpoint().addr.to_string());
    auto cleanup_sockets = [this]() {
        state_ = rtsp_session_state::teardown;
        if (timing_socket_) {
            timing_socket_->close();
        }
        if (control_socket_) {
            control_socket_->close();
        }
        if (mirror_acceptor_) {
            mirror_acceptor_->close();
        }
        if (audio_data_socket_) {
            audio_data_socket_->close();
        }
        if (audio_control_socket_) {
            audio_control_socket_->close();
        }
    };
    while (state_ != rtsp_session_state::teardown) {
        auto request = co_await read_request();
        if (!request) {
            mirage::log::warn("Failed to read RTSP request: {}", request.error().message);
            cleanup_sockets();
            co_return std::unexpected(request.error());
        }
        mirage::log::debug("RTSP {} {}", request->method, request->uri);
        auto response = co_await handle_request(*request);
        if (!response) {
            mirage::log::warn("Failed to handle RTSP request: {}", response.error().message);
            rtsp_response err_resp{
                .status_code = 500,
                .status_text = "Internal Server Error",
                .headers = {{"CSeq", std::to_string(cseq_)}},
                .body = {},
            };
            co_await send_response(err_resp);
            continue;
        }
        auto send_result = co_await send_response(*response);
        if (!send_result) {
            cleanup_sockets();
            co_return std::unexpected(send_result.error());
        }
    }
    cleanup_sockets();
    io::steady_timer timer(socket_.context());
    timer.expires_after(std::chrono::milliseconds(100));
    co_await timer.async_wait();
    co_return result<void>{};
}
io::task<result<rtsp_request>> rtsp_session::read_request() {
    rtsp_request req;
    try {
        auto header_str = co_await socket_.async_read_until("\r\n\r\n");
        std::istringstream stream(header_str);
        std::string line;
        std::getline(stream, line);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::istringstream request_line(line);
        request_line >> req.method >> req.uri >> req.version;
        mirage::log::debug("Request: {} {} {}", req.method, req.uri, req.version);
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                break;
            }
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                auto key = line.substr(0, colon);
                auto value = line.substr(colon + 1);
                auto start = value.find_first_not_of(" \t");
                if (start != std::string::npos) {
                    value = value.substr(start);
                }
                req.headers[key] = value;
            }
        }
        std::string cseq_val;
        std::string cl_val;
        for (const auto& [k, v] : req.headers) {
            std::string lk = k;
            std::transform(lk.begin(), lk.end(), lk.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (lk == "cseq") {
                cseq_val = v;
            } else if (lk == "content-length") {
                cl_val = v;
            }
        }
        if (!cseq_val.empty()) {
            cseq_ = static_cast<uint32_t>(std::stoul(cseq_val));
        }
        if (!cl_val.empty()) {
            size_t content_length = std::stoull(cl_val);
            if (content_length > 0) {
                auto& leftover = socket_.buffer();
                size_t from_buffer = std::min(leftover.size(), content_length);
                if (from_buffer > 0) {
                    req.body.resize(from_buffer);
                    std::memcpy(req.body.data(), leftover.data(), from_buffer);
                    leftover.erase(0, from_buffer);
                }
                if (req.body.size() < content_length) {
                    auto to_read = content_length - req.body.size();
                    auto offset = req.body.size();
                    req.body.resize(content_length);
                    co_await socket_.async_read_exactly(
                        std::span<std::byte>(req.body.data() + offset, to_read));
                }
            }
        }
    } catch (const std::exception& e) {
        co_return std::unexpected(mirage_error::network(std::format("read failed: {}", e.what())));
    }
    co_return req;
}
io::task<result<void>> rtsp_session::send_response(const rtsp_response& resp) {
    try {
        std::ostringstream ss;
        ss << "RTSP/1.0 " << resp.status_code << " " << resp.status_text << "\r\n";
        if (!resp.headers.contains("Server")) {
            ss << "Server: AirTunes/220.68\r\n";
        }
        for (const auto& [key, value] : resp.headers) {
            ss << key << ": " << value << "\r\n";
        }
        ss << "Content-Length: " << resp.body.size() << "\r\n";
        ss << "\r\n";
        auto header = ss.str();
        co_await socket_.async_write(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(header.data()), header.size()));
        if (!resp.body.empty()) {
            co_await socket_.async_write(std::span<const std::byte>(resp.body));
        }
    } catch (const std::exception& e) {
        co_return std::unexpected(mirage_error::network(std::format("send failed: {}", e.what())));
    }
    co_return result<void>{};
}
io::task<result<rtsp_response>> rtsp_session::handle_request(const rtsp_request& req) {
    if (req.method == "OPTIONS") {
        co_return handle_options(req);
    }
    if (req.uri == "/info" && req.method == "GET") {
        co_return handle_info(req);
    }
    if (req.uri == "/pair-setup" && req.method == "POST") {
        co_return co_await handle_pair_setup(req);
    }
    if (req.uri == "/pair-verify" && req.method == "POST") {
        co_return co_await handle_pair_verify(req);
    }
    if (req.uri == "/fp-setup" && req.method == "POST") {
        co_return handle_fp_setup(req);
    }
    if (req.method == "SETUP") {
        co_return co_await handle_setup(req);
    }
    if (req.method == "RECORD") {
        co_return handle_record(req);
    }
    if (req.method == "TEARDOWN") {
        co_return handle_teardown(req);
    }
    if (req.method == "GET_PARAMETER") {
        co_return handle_get_parameter(req);
    }
    if (req.method == "SET_PARAMETER") {
        co_return handle_set_parameter(req);
    }
    co_return rtsp_response{
        .status_code = 501,
        .status_text = "Not Implemented",
        .headers = {{"CSeq", std::to_string(cseq_)}},
        .body = {},
    };
}
result<rtsp_response> rtsp_session::handle_options(const rtsp_request& /* req */) const {
    return rtsp_response{
        .status_code = 200,
        .status_text = "OK",
        .headers =
            {
                {"CSeq", std::to_string(cseq_)},
                {"Public",
                 "ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, "
                 "OPTIONS, GET_PARAMETER, SET_PARAMETER, POST, GET"},
            },
        .body = {},
    };
}
result<rtsp_response> rtsp_session::handle_info(const rtsp_request& /* req */) {
    auto pk = pairing_.ed25519_public_key();
    auto pk_b64 = base64_encode(pk);
    std::string pk_hex;
    for (auto& i : pk) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02x ", static_cast<uint8_t>(i));
        pk_hex += buf;
    }
    mirage::log::debug("Ed25519 pubkey in /info: {}", pk_hex);
    std::string plist = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-
<plist version="1.0">
<dict>
    <key>audioFormats</key>
    <array>
        <dict>
            <key>audioInputFormats</key>
            <integer>67108860</integer>
            <key>audioOutputFormats</key>
            <integer>67108860</integer>
            <key>type</key>
            <integer>100</integer>
        </dict>
        <dict>
            <key>audioInputFormats</key>
            <integer>67108860</integer>
            <key>audioOutputFormats</key>
            <integer>67108860</integer>
            <key>type</key>
            <integer>101</integer>
        </dict>
    </array>
    <key>audioLatencies</key>
    <array>
        <dict>
            <key>audioType</key>
            <string>default</string>
            <key>inputLatencyMicros</key>
            <integer>0</integer>
            <key>outputLatencyMicros</key>
            <integer>0</integer>
            <key>type</key>
            <integer>100</integer>
        </dict>
        <dict>
            <key>audioType</key>
            <string>default</string>
            <key>inputLatencyMicros</key>
            <integer>0</integer>
            <key>outputLatencyMicros</key>
            <integer>0</integer>
            <key>type</key>
            <integer>101</integer>
        </dict>
    </array>
    <key>deviceID</key>
    <string>DC:46:28:54:D9:0E</string>
    <key>displays</key>
    <array>
        <dict>
            <key>features</key>
            <integer>14</integer>
            <key>height</key>
            <integer>1080</integer>
            <key>heightPhysical</key>
            <integer>0</integer>
            <key>heightPixels</key>
            <integer>1080</integer>
            <key>maxFPS</key>
            <integer>60</integer>
            <key>overscanned</key>
            <false/>
            <key>refreshRate</key>
            <real>0.016666666666666666</real>
            <key>rotation</key>
            <false/>
            <key>uuid</key>
            <string>e0ff8a27-6738-3d56-8a16-cc53aacee925</string>
            <key>width</key>
            <integer>1920</integer>
            <key>widthPhysical</key>
            <integer>0</integer>
            <key>widthPixels</key>
            <integer>1920</integer>
        </dict>
    </array>
    <key>features</key>
    <integer>1518337766</integer>
    <key>initialVolume</key>
    <real>-20.0</real>
    <key>keepAliveLowPower</key>
    <integer>1</integer>
    <key>keepAliveSendStatsAsBody</key>
    <true/>
    <key>macAddress</key>
    <string>DC:46:28:54:D9:0E</string>
    <key>model</key>
    <string>AppleTV6,2</string>
    <key>name</key>
    <string>mirage</string>
    <key>pi</key>
    <string>b08f5a79-db29-4384-b456-a4784d9e6055</string>
    <key>pk</key>
    <data>)" + pk_b64 + R"(</data>
    <key>sourceVersion</key>
    <string>770.8.1</string>
    <key>statusFlags</key>
    <integer>68</integer>
    <key>vv</key>
    <integer>2</integer>
</dict>
</plist>
)";
    std::vector<std::byte> body;
    body.reserve(plist.size());
    for (char c : plist) {
        body.push_back(static_cast<std::byte>(c));
    }
    return rtsp_response{
        .status_code = 200,
        .status_text = "OK",
        .headers =
            {
                {"CSeq", std::to_string(cseq_)},
                {"Content-Type", "text/x-apple-plist+xml"},
            },
        .body = std::move(body),
    };
}
auto rtsp_session::handle_pair_setup(const rtsp_request& req) -> io::task<result<rtsp_response>> {
    mirage::log::debug("Pair-setup request, {} bytes", req.body.size());
    state_ = rtsp_session_state::pair_setup;
    if (req.body.size() == 32) {
        mirage::log::info("Transient pairing: returning Ed25519 public key");
        pairing_.set_setup_complete();
        auto server_pk = pairing_.ed25519_public_key();
        mirage::log::info("Transient pairing: sending server Ed25519 public key");
        co_return rtsp_response{
            .status_code = 200,
            .status_text = "OK",
            .headers =
                {
                    {"CSeq", std::to_string(cseq_)},
                    {"Content-Type", "application/octet-stream"},
                },
            .body = std::vector<std::byte>(server_pk.begin(), server_pk.end()),
        };
    }
    auto items = crypto::tlv8::parse(req.body);
    if (!items) {
        co_return std::unexpected(items.error());
    }
    auto state_data = crypto::tlv8::find(*items, crypto::tlv8::type::state);
    if (state_data.empty()) {
        co_return std::unexpected(
            mirage_error::protocol_err(protocol::airplay, "missing state TLV"));
    }
    auto msg_state = static_cast<uint8_t>(state_data[0]);
    mirage::log::debug("Pair-setup state: M{}", msg_state);
    result<std::vector<std::byte>> response;
    if (msg_state == 1) {
        response = pairing_.handle_m1(req.body);
    } else if (msg_state == 3) {
        response = pairing_.handle_m3(req.body);
    } else if (msg_state == 5) {
        response = pairing_.handle_m5(req.body);
    } else {
        co_return std::unexpected(
            mirage_error::protocol_err(protocol::airplay, "unexpected pair-setup state"));
    }
    if (!response) {
        mirage::log::warn("Pair-setup M{} failed: {}", msg_state, response.error().message);
        co_return std::unexpected(response.error());
    }
    co_return rtsp_response{
        .status_code = 200,
        .status_text = "OK",
        .headers =
            {
                {"CSeq", std::to_string(cseq_)},
                {"Content-Type", "application/octet-stream"},
            },
        .body = std::move(*response),
    };
}
auto rtsp_session::handle_pair_verify(const rtsp_request& req) -> io::task<result<rtsp_response>> {
    mirage::log::debug("Pair-verify request, {} bytes", req.body.size());
    std::string hex;
    for (size_t i = 0; i < std::min(req.body.size(), size_t{80}); ++i) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02x ", static_cast<uint8_t>(req.body[i]));
        hex += buf;
    }
    mirage::log::debug("Pair-verify data: {}", hex);
    state_ = rtsp_session_state::pair_verify;
    if (req.body.size() == 68 && static_cast<uint8_t>(req.body[0]) == 0x01) {
        mirage::log::info("Transient pair-verify step 1");
        std::array<std::byte, 32> client_verify_pk{};
        std::array<std::byte, 32> client_auth_tag{};
        std::copy(req.body.begin() + 4, req.body.begin() + 36, client_verify_pk.begin());
        std::copy(req.body.begin() + 36, req.body.begin() + 68, client_auth_tag.begin());
        auto verify_response = pairing_.handle_transient_verify1(client_verify_pk, client_auth_tag);
        if (!verify_response) {
            mirage::log::warn("Transient pair-verify failed: {}", verify_response.error().message);
            co_return std::unexpected(verify_response.error());
        }
        std::vector<std::byte> response = std::move(*verify_response);
        std::string resp_hex;
        for (auto& i : response) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02x ", static_cast<uint8_t>(i));
            resp_hex += buf;
        }
        mirage::log::debug("Pair-verify response: {}", resp_hex);
        mirage::log::info("Transient pair-verify: sending response ({} bytes)", response.size());
        co_return rtsp_response{
            .status_code = 200,
            .status_text = "OK",
            .headers =
                {
                    {"CSeq", std::to_string(cseq_)},
                    {"Content-Type", "application/octet-stream"},
                },
            .body = std::move(response),
        };
    }
    if (req.body.size() == 68 && static_cast<uint8_t>(req.body[0]) == 0x00) {
        mirage::log::info("Transient pair-verify step 2: verifying client signature");
        std::array<std::byte, 64> client_signature{};
        std::copy(req.body.begin() + 4, req.body.begin() + 68, client_signature.begin());
        auto verify_result = pairing_.handle_transient_verify2(client_signature);
        if (!verify_result) {
            mirage::log::warn("Transient pair-verify step 2 failed: {}",
                              verify_result.error().message);
            co_return std::unexpected(verify_result.error());
        }
        mirage::log::info("Transient pair-verify: signature verified, pairing complete!");
        mirage::log::user("{} connected", socket_.remote_endpoint().addr.to_string());
        co_return rtsp_response{
            .status_code = 200,
            .status_text = "OK",
            .headers =
                {
                    {"CSeq", std::to_string(cseq_)},
                    {"Content-Type", "application/octet-stream"},
                },
            .body = {},
        };
    }
    auto items = crypto::tlv8::parse(req.body);
    if (!items) {
        co_return std::unexpected(items.error());
    }
    auto state_data = crypto::tlv8::find(*items, crypto::tlv8::type::state);
    if (state_data.empty()) {
        co_return std::unexpected(
            mirage_error::protocol_err(protocol::airplay, "missing state TLV"));
    }
    uint8_t msg_state = static_cast<uint8_t>(state_data[0]);
    mirage::log::debug("Pair-verify state: M{}", msg_state);
    if (msg_state == 1) {
        auto response = pairing_.handle_verify_m1(req.body);
        if (!response) {
            mirage::log::warn("Pair-verify M1 failed: {}", response.error().message);
            co_return std::unexpected(response.error());
        }
        co_return rtsp_response{
            .status_code = 200,
            .status_text = "OK",
            .headers =
                {
                    {"CSeq", std::to_string(cseq_)},
                    {"Content-Type", "application/octet-stream"},
                },
            .body = std::move(*response),
        };
    } else if (msg_state == 3) {
        auto session_key = pairing_.handle_verify_m3(req.body);
        if (!session_key) {
            mirage::log::warn("Pair-verify M3 failed: {}", session_key.error().message);
            co_return std::unexpected(session_key.error());
        }
        mirage::log::info("Pair-verify complete, session key established");
        mirage::log::user("{} connected", socket_.remote_endpoint().addr.to_string());
        state_ = rtsp_session_state::announced;
        std::vector<crypto::tlv8::item> response_items = {
            {crypto::tlv8::type::state, {std::byte{4}}},
        };
        co_return rtsp_response{
            .status_code = 200,
            .status_text = "OK",
            .headers =
                {
                    {"CSeq", std::to_string(cseq_)},
                    {"Content-Type", "application/octet-stream"},
                },
            .body = crypto::tlv8::encode(response_items),
        };
    }
    co_return std::unexpected(
        mirage_error::protocol_err(protocol::airplay, "unexpected pair-verify state"));
}
static std::optional<uint64_t> extract_bplist_uint(std::span<const std::byte> data,
                                                   const char* key) {
    std::string key_str(key);
    size_t key_pos = std::string::npos;
    for (size_t i = 0; i + key_str.size() < data.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < key_str.size(); ++j) {
            if (static_cast<char>(data[i + j]) != key_str[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            key_pos = i;
            mirage::log::debug("Found key '{}' at offset {}", key_str, key_pos);
            break;
        }
    }
    if (key_pos == std::string::npos) {
        return std::nullopt;
    }
    std::vector<std::pair<size_t, uint16_t>> candidates;
    for (size_t k = 8; k + 2 < data.size(); ++k) {
        auto marker = static_cast<uint8_t>(data[k]);
        if (marker == 0x11 && k + 3 <= data.size()) {
            uint16_t value = static_cast<uint16_t>((static_cast<uint16_t>(data[k + 1]) << 8) |
                                                   static_cast<uint16_t>(data[k + 2]));
            if (value >= 1024) {
                candidates.emplace_back(k, value);
                mirage::log::debug("Found 2-byte int {} at offset {}", value, k);
            }
        }
        if (marker == 0x12 && k + 5 <= data.size()) {
            uint32_t value = (static_cast<uint32_t>(data[k + 1]) << 24) |
                             (static_cast<uint32_t>(data[k + 2]) << 16) |
                             (static_cast<uint32_t>(data[k + 3]) << 8) |
                             static_cast<uint32_t>(data[k + 4]);
            if (value >= 1024 && value <= 65535) {
                candidates.emplace_back(k, static_cast<uint16_t>(value));
                mirage::log::debug("Found 4-byte int {} at offset {}", value, k);
            }
        }
    }
    for (const auto& [offset, value] : candidates) {
        if (value >= 50000) {
            mirage::log::debug("Selecting timing port {} from offset {}", value, offset);
            return value;
        }
    }
    if (!candidates.empty()) {
        mirage::log::debug("Falling back to first port candidate: {}", candidates[0].second);
        return candidates[0].second;
    }
    return std::nullopt;
}
io::task<result<rtsp_response>> rtsp_session::handle_setup(const rtsp_request& req) {
    mirage::log::debug("SETUP request for {}", req.uri);
    std::string body_hex;
    size_t dump_size = std::min(req.body.size(), size_t{700});
    for (size_t i = 0; i < dump_size; ++i) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02x ", static_cast<uint8_t>(req.body[i]));
        body_hex += buf;
        if ((i + 1) % 32 == 0) {
            body_hex += "\n";
        }
    }
    mirage::log::debug("SETUP body ({} bytes):\n{}", req.body.size(), body_hex);
    state_ = rtsp_session_state::ready;
    auto client_timing_port = extract_bplist_uint(req.body, "timingPort");
    if (client_timing_port) {
        mirage::log::info("Client timing port: {}", *client_timing_port);
        client_timing_endpoint_ = io::endpoint{socket_.remote_endpoint().addr,
                                               static_cast<uint16_t>(*client_timing_port)};
    } else {
        mirage::log::warn("Could not extract client timing port from SETUP request");
    }
    try {
        if (!timing_socket_ || !timing_socket_->is_open()) {
            timing_socket_ =
                std::make_unique<io::udp_socket>(io::udp_socket::bind(socket_.context(), 0));
        }
        if (!control_socket_ || !control_socket_->is_open()) {
            control_socket_ =
                std::make_unique<io::udp_socket>(io::udp_socket::bind(socket_.context(), 0));
        }
        if (!mirror_acceptor_ || !mirror_acceptor_->is_open()) {
            mirror_acceptor_ =
                std::make_unique<io::tcp_acceptor>(io::tcp_acceptor::bind(socket_.context(), 0));
            mirror_acceptor_->listen(1);
        }
        if (!audio_data_socket_ || !audio_data_socket_->is_open()) {
            audio_data_socket_ =
                std::make_unique<io::udp_socket>(io::udp_socket::bind(socket_.context(), 0));
        }
        if (!audio_control_socket_ || !audio_control_socket_->is_open()) {
            audio_control_socket_ =
                std::make_unique<io::udp_socket>(io::udp_socket::bind(socket_.context(), 0));
        }
    } catch (const std::exception& e) {
        mirage::log::error("Failed to bind sockets: {}", e.what());
        co_return std::unexpected(
            mirage_error::network(std::format("failed to bind sockets: {}", e.what())));
    }
    uint16_t timing_port = timing_socket_->local_endpoint().port;
    uint16_t mirror_port = mirror_acceptor_->local_endpoint().port;
    uint16_t control_port = control_socket_->local_endpoint().port;
    uint16_t audio_port = audio_data_socket_->local_endpoint().port;
    uint16_t audio_control_port = audio_control_socket_->local_endpoint().port;
    mirage::log::info("SETUP ports: timing={}, mirror={}, control={}, audio_data={}, audio_ctrl={}",
                      timing_port, mirror_port, control_port, audio_port, audio_control_port);
    auto self = shared_from_this();
    if (!base_receivers_started_) {
        io::co_spawn(socket_.context(), [self]() -> io::task<void> {
            co_await self->run_udp_receiver(*self->timing_socket_, "timing");
        });
        io::co_spawn(socket_.context(), [self]() -> io::task<void> {
            co_await self->run_udp_receiver(*self->control_socket_, "control");
        });
        base_receivers_started_ = true;
        if (client_timing_port) {
            io::co_spawn(socket_.context(),
                         [self]() -> io::task<void> { co_await self->run_ntp_timing_sender(); });
        }
    }
    if (!mirror_receiver_started_ && mirror_acceptor_) {
        io::co_spawn(socket_.context(),
                     [self]() -> io::task<void> { co_await self->run_mirror_receiver(); });
        mirror_receiver_started_ = true;
    }
    if (!audio_receiver_started_ && audio_data_socket_) {
        io::co_spawn(socket_.context(),
                     [self]() -> io::task<void> { co_await self->run_audio_receiver(); });
        io::co_spawn(socket_.context(),
                     [self]() -> io::task<void> { co_await self->run_audio_control_receiver(); });
        audio_receiver_started_ = true;
    }
    bool is_streams_setup = bplist::has_streams(req.body);
    auto res_root = bplist::plist_value::make_dict();
    std::vector<std::byte> body;
    if (is_streams_setup) {
        uint64_t type = 110;
        bool has_audio_format = bplist::find_key(req.body, "audioFormat").has_value();
        if (has_audio_format) {
            type = 96;
        } else {
            auto type_val = bplist::get_uint(req.body, "type");
            type = type_val.value_or(110);
        }
        mirage::log::debug("SETUP streams: processing type {}", type);
        if (type == 110) {
            auto stream_id = bplist::find_8byte_int(req.body);
            if (stream_id) {
                stream_connection_id_ = *stream_id;
                mirage::log::info("streamConnectionID: {}", stream_connection_id_);
            }
        }
        auto res_streams = bplist::plist_value::make_array();
        if (type == 110) {
            mirage::log::debug("SETUP type 110: video mirroring (TCP), dataPort={}", mirror_port);
            auto res_stream = bplist::plist_value::make_dict();
            res_stream->dict_val.emplace_back("dataPort",
                                              bplist::plist_value::make_uint(mirror_port));
            res_stream->dict_val.emplace_back("type", bplist::plist_value::make_uint(110));
            res_streams->array_val.push_back(res_stream);
        } else if (type == 96) {
            auto ct_val = bplist::get_uint(req.body, "ct");
            if (ct_val) {
                audio_ct_ = static_cast<uint8_t>(*ct_val);
            }
            if (audio_ct_ == 0) {
                for (size_t i = 0; i + 4 < req.body.size(); ++i) {
                    if (static_cast<uint8_t>(req.body[i]) == 0x10 &&
                        static_cast<uint8_t>(req.body[i + 1]) == 0x02 &&
                        static_cast<uint8_t>(req.body[i + 2]) == 0x10) {
                        audio_ct_ = static_cast<uint8_t>(req.body[i + 1]);
                        break;
                    }
                }
            }
            if (audio_ct_ == 0) {
                for (size_t i = 0; i + 1 < req.body.size(); ++i) {
                    uint8_t b = static_cast<uint8_t>(req.body[i]);
                    if (b == 0x10) {
                        uint8_t val = static_cast<uint8_t>(req.body[i + 1]);
                        if (val == 2 || val == 8) {
                            audio_ct_ = val;
                            break;
                        }
                    }
                }
            }
            auto spf_val = bplist::get_uint(req.body, "spf");
            if (spf_val) {
                audio_spf_ = static_cast<int>(*spf_val);
            }
            auto sr = bplist::get_uint(req.body, "sr");
            if (sr) {
                audio_sample_rate_ = static_cast<int>(*sr);
            }
            mirage::log::info("SETUP type 96: audio data={}, ctrl={}, ct={}, spf={}, sr={}",
                              audio_port, audio_control_port, audio_ct_, audio_spf_,
                              audio_sample_rate_);
            if (audio_ct_ == 2) {
                uint32_t alac_spf = static_cast<uint32_t>(audio_spf_);
                uint32_t alac_sr = static_cast<uint32_t>(audio_sample_rate_);
                std::array<std::byte, 36> alac_config{};
                alac_config[0] = std::byte{0x00};
                alac_config[1] = std::byte{0x00};
                alac_config[2] = std::byte{0x00};
                alac_config[3] = std::byte{0x24};
                alac_config[4] = std::byte{'a'};
                alac_config[5] = std::byte{'l'};
                alac_config[6] = std::byte{'a'};
                alac_config[7] = std::byte{'c'};
                alac_config[8] = std::byte{0x00};
                alac_config[9] = std::byte{0x00};
                alac_config[10] = std::byte{0x00};
                alac_config[11] = std::byte{0x00};
                alac_config[12] = std::byte{static_cast<uint8_t>((alac_spf >> 24) & 0xFF)};
                alac_config[13] = std::byte{static_cast<uint8_t>((alac_spf >> 16) & 0xFF)};
                alac_config[14] = std::byte{static_cast<uint8_t>((alac_spf >> 8) & 0xFF)};
                alac_config[15] = std::byte{static_cast<uint8_t>(alac_spf & 0xFF)};
                alac_config[16] = std::byte{0x00};
                alac_config[17] = std::byte{0x10};
                alac_config[18] = std::byte{0x28};
                alac_config[19] = std::byte{0x0A};
                alac_config[20] = std::byte{0x0E};
                alac_config[21] = std::byte{0x02};
                alac_config[22] = std::byte{0x00};
                alac_config[23] = std::byte{0xFF};
                alac_config[24] = std::byte{0x00};
                alac_config[25] = std::byte{0x00};
                alac_config[26] = std::byte{0x00};
                alac_config[27] = std::byte{0x00};
                alac_config[28] = std::byte{0x00};
                alac_config[29] = std::byte{0x00};
                alac_config[30] = std::byte{0x00};
                alac_config[31] = std::byte{0x00};
                alac_config[32] = std::byte{static_cast<uint8_t>((alac_sr >> 24) & 0xFF)};
                alac_config[33] = std::byte{static_cast<uint8_t>((alac_sr >> 16) & 0xFF)};
                alac_config[34] = std::byte{static_cast<uint8_t>((alac_sr >> 8) & 0xFF)};
                alac_config[35] = std::byte{static_cast<uint8_t>(alac_sr & 0xFF)};
                audio_decoder_ = audio::audio_decoder::create_alac(audio_sample_rate_,
                                                                   audio_channels_, alac_config);
            } else if (audio_ct_ == 8) {
                audio_decoder_ =
                    audio::audio_decoder::create_aac(audio_sample_rate_, audio_channels_);
            } else {
                audio_decoder_ =
                    audio::audio_decoder::create_aac(audio_sample_rate_, audio_channels_);
            }
            auto res_stream = bplist::plist_value::make_dict();
            res_stream->dict_val.emplace_back("dataPort",
                                              bplist::plist_value::make_uint(audio_port));
            res_stream->dict_val.emplace_back("controlPort",
                                              bplist::plist_value::make_uint(audio_control_port));
            res_stream->dict_val.emplace_back("type", bplist::plist_value::make_uint(96));
            res_streams->array_val.push_back(res_stream);
        } else {
            mirage::log::warn("SETUP: unknown stream type {}", type);
        }
        res_root->dict_val.emplace_back("streams", res_streams);
    } else {
        mirage::log::debug("SETUP type: general (timing/event ports), timingPort={}", timing_port);
        for (size_t i = 0; i + 2 < req.body.size(); ++i) {
            if (static_cast<uint8_t>(req.body[i]) == 0x4F &&
                static_cast<uint8_t>(req.body[i + 1]) == 0x10 &&
                static_cast<uint8_t>(req.body[i + 2]) == 0x10 && i + 3 + 16 <= req.body.size()) {
                std::copy_n(req.body.begin() + static_cast<std::ptrdiff_t>(i + 3), 16,
                            audio_aes_iv_.begin());
                mirage::log::info("Extracted 16-byte eiv for audio decryption");
                break;
            }
        }
        for (size_t i = 0; i + 72 <= req.body.size(); ++i) {
            if (static_cast<char>(req.body[i]) == 'F' &&
                static_cast<char>(req.body[i + 1]) == 'P' &&
                static_cast<char>(req.body[i + 2]) == 'L' &&
                static_cast<char>(req.body[i + 3]) == 'Y') {
                std::vector<std::byte> ekey(req.body.begin() + static_cast<std::ptrdiff_t>(i),
                                            req.body.begin() + static_cast<std::ptrdiff_t>(i + 72));
                mirage::log::debug("Found ekey ({} bytes)", ekey.size());
                if (ekey.size() >= 72) {
                    fp_ekey_.assign(ekey.begin(), ekey.begin() + 72);
                    mirage::log::info("Stored 72-byte ekey");
                }
                break;
            }
        }
        if (!audio_keys_ready_ && fp_keymsg_.size() == 164 && fp_ekey_.size() == 72) {
            std::array<std::byte, 164> keymsg_arr{};
            std::array<std::byte, 72> ekey_arr{};
            std::copy(fp_keymsg_.begin(), fp_keymsg_.end(), keymsg_arr.begin());
            std::copy(fp_ekey_.begin(), fp_ekey_.end(), ekey_arr.begin());
            auto aeskey = crypto::fairplay_decrypt_key(keymsg_arr, ekey_arr);
            auto shared_secret = pairing_.transient_shared_secret();
            std::array<std::byte, 48> combined{};
            std::copy_n(aeskey.begin(), 16, combined.begin());
            std::copy_n(shared_secret.begin(), 32, combined.begin() + 16);
            auto hashed_key = crypto::sha512(combined);
            std::copy_n(hashed_key.begin(), 16, aeskey.begin());
            audio_aes_key_ = aeskey;
            audio_keys_ready_ = true;
            mirage::log::info("Audio AES-CBC key derived (with ECDH hashing)");
        }
        res_root->dict_val.emplace_back("eventPort", bplist::plist_value::make_uint(0));
        res_root->dict_val.emplace_back("timingPort", bplist::plist_value::make_uint(timing_port));
    }
    body = bplist::encode(res_root);
    std::string resp_hex;
    for (size_t i = 0; i < body.size(); ++i) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02x ", static_cast<uint8_t>(body[i]));
        resp_hex += buf;
        if ((i + 1) % 16 == 0) {
            resp_hex += "\n";
        }
    }
    mirage::log::debug("SETUP response binary plist ({} bytes):\n{}", body.size(), resp_hex);
    co_return rtsp_response{
        .status_code = 200,
        .status_text = "OK",
        .headers =
            {
                {"CSeq", std::to_string(cseq_)},
                {"Content-Type", "application/x-apple-binary-plist"},
            },
        .body = std::move(body),
    };
}
auto rtsp_session::run_udp_receiver(io::udp_socket& sock, const char* name) -> io::task<void> {
    std::array<std::byte, 2048> buffer;
    io::endpoint sender;
    bool is_timing = (std::string_view(name) == "timing");
    mirage::log::debug("UDP {} receiver started on port {}", name, sock.local_endpoint().port);
    try {
        while (sock.is_open()) {
            auto n = co_await sock.async_recv_from(std::span<std::byte>(buffer), sender);
            mirage::log::debug("UDP {}: received {} bytes from {}:{}", name, n,
                               sender.addr.to_string(), sender.port);
            if (is_timing && n >= 32) {
                uint8_t header0 = static_cast<uint8_t>(buffer[0]);
                uint8_t header1 = static_cast<uint8_t>(buffer[1]);
                mirage::log::debug("Timing packet type: 0x{:02x} 0x{:02x}", header0, header1);
                if (header0 == 0x80 && header1 == 0xd2) {
                    mirage::log::info("Timing REQUEST received from iOS, sending response");
                    std::array<std::byte, 32> response{};
                    response[0] = std::byte{0x80};
                    response[1] = std::byte{0xd3};
                    response[2] = buffer[2];
                    response[3] = buffer[3];
                    auto now = std::chrono::system_clock::now();
                    auto nanos =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch())
                            .count();
                    constexpr uint64_t ntp_unix_diff = 2208988800ULL;
                    uint64_t secs = (static_cast<uint64_t>(nanos) / 1000000000ULL) + ntp_unix_diff;
                    uint64_t frac =
                        ((static_cast<uint64_t>(nanos) % 1000000000ULL) << 32) / 1000000000ULL;
                    uint64_t ntp_time = (secs << 32) | frac;
                    std::copy(buffer.begin() + 24, buffer.begin() + 32, response.begin() + 8);
                    for (size_t i = 0; i < 8; ++i) {
                        response[16 + i] = std::byte{static_cast<uint8_t>(
                            (ntp_time >> (56 - static_cast<int>(i) * 8)) & 0xFF)};
                    }
                    for (size_t i = 0; i < 8; ++i) {
                        response[24U + i] = std::byte{static_cast<uint8_t>(
                            (ntp_time >> (56 - static_cast<int>(i) * 8)) & 0xFF)};
                    }
                    co_await sock.async_send_to(std::span<const std::byte>(response), sender);
                    mirage::log::debug("Timing response sent to {}:{}", sender.addr.to_string(),
                                       sender.port);
                }
            }
        }
    } catch (const std::system_error& e) {
        if (e.code() != std::errc::operation_canceled) {
            mirage::log::debug("UDP {} receiver ended: {}", name, e.what());
        }
    }
}
io::task<void> rtsp_session::run_ntp_timing_sender() {
    if (!timing_socket_ || client_timing_endpoint_.port == 0) {
        mirage::log::warn("NTP timing sender: no timing socket or client endpoint");
        co_return;
    }
    mirage::log::info("NTP timing sender started, sending to {}:{}",
                      client_timing_endpoint_.addr.to_string(), client_timing_endpoint_.port);
    std::array<std::byte, 32> request{};
    request[0] = std::byte{0x80};
    request[1] = std::byte{0xd2};
    uint16_t seq = 1;
    try {
        io::steady_timer timer(timing_socket_->context());
        while (timing_socket_ && timing_socket_->is_open() &&
               state_ != rtsp_session_state::teardown) {
            request[2] = std::byte{static_cast<uint8_t>((seq >> 8) & 0xFF)};
            request[3] = std::byte{static_cast<uint8_t>(seq & 0xFF)};
            auto now = std::chrono::steady_clock::now();
            auto nanos =
                std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch())
                    .count();
            uint64_t send_time = static_cast<uint64_t>(nanos);
            for (size_t i = 0; i < 8; ++i) {
                request[24U + i] = std::byte{
                    static_cast<uint8_t>((send_time >> (56 - static_cast<int>(i) * 8)) & 0xFF)};
            }
            auto sent = co_await timing_socket_->async_send_to(std::span<const std::byte>(request),
                                                               client_timing_endpoint_);
            mirage::log::debug("NTP timing: sent {} bytes (seq={})", sent, seq);
            seq++;
            timer.expires_after(std::chrono::milliseconds(500));
            co_await timer.async_wait();
        }
    } catch (const std::system_error& e) {
        if (e.code() != std::errc::operation_canceled) {
            mirage::log::debug("NTP timing sender ended: {}", e.what());
        }
    }
    mirage::log::debug("NTP timing sender stopped");
}
io::task<void> rtsp_session::run_mirror_receiver() {
    if (!mirror_acceptor_) {
        mirage::log::warn("Mirror receiver: no acceptor");
        co_return;
    }
    mirage::log::info("Mirror receiver started, listening on TCP port {}",
                      mirror_acceptor_->local_endpoint().port);
    try {
        while (mirror_acceptor_->is_open() && state_ != rtsp_session_state::teardown) {
            auto mirror_socket = co_await mirror_acceptor_->async_accept();
            mirage::log::info("Mirror connection accepted from {}:{}",
                              mirror_socket.remote_endpoint().addr.to_string(),
                              mirror_socket.remote_endpoint().port);
            std::vector<std::byte> sps_pps_data;
            bool is_h265 = false;
            uint64_t frame_count = 0;
            uint64_t keyframe_count = 0;
            bool sps_pps_sent = false;
            std::optional<media::unified_decoder> video_decoder;
            auto decoder_result = media::unified_decoder::create(video_codec::h264, true);
            if (decoder_result) {
                video_decoder = std::move(*decoder_result);
                mirage::log::info("Video decoder initialized: {}", video_decoder->backend_name());
            } else {
                mirage::log::warn("Failed to create video decoder: {}",
                                  decoder_result.error().message);
            }
            auto video_renderer = render::render_window::create("Mirage - AirPlay", 1280, 720);
            std::optional<crypto::aes_ctr_decryptor> video_decryptor;
            if (stream_connection_id_ != 0) {
                if (fp_keymsg_.size() == 164 && fp_ekey_.size() == 72) {
                    {
                        std::string keymsg_hex;
                        std::string ekey_hex;
                        for (size_t i = 0; i < 32 && i < fp_keymsg_.size(); ++i) {
                            char buf[4];
                            snprintf(buf, sizeof(buf), "%02x ",
                                     static_cast<uint8_t>(fp_keymsg_[i]));
                            keymsg_hex += buf;
                        }
                        for (size_t i = 0; i < fp_ekey_.size(); ++i) {
                            char buf[4];
                            snprintf(buf, sizeof(buf), "%02x ", static_cast<uint8_t>(fp_ekey_[i]));
                            ekey_hex += buf;
                            if ((i + 1) % 16 == 0) {
                                ekey_hex += "\n";
                            }
                        }
                        mirage::log::debug("keymsg (first 32): {}", keymsg_hex);
                        mirage::log::debug("ekey (full 72 bytes):\n{}", ekey_hex);
                        std::string chunk1_hex;
                        std::string chunk2_hex;
                        for (size_t i = 16; i < 32 && i < fp_ekey_.size(); ++i) {
                            char buf[4];
                            snprintf(buf, sizeof(buf), "%02x ", static_cast<uint8_t>(fp_ekey_[i]));
                            chunk1_hex += buf;
                        }
                        for (size_t i = 56; i < 72 && i < fp_ekey_.size(); ++i) {
                            char buf[4];
                            snprintf(buf, sizeof(buf), "%02x ", static_cast<uint8_t>(fp_ekey_[i]));
                            chunk2_hex += buf;
                        }
                        mirage::log::debug("playfair chunk1 [16:32]: {}", chunk1_hex);
                        mirage::log::debug("playfair chunk2 [56:72]: {}", chunk2_hex);
                    }
                    std::array<std::byte, 164> keymsg_arr;
                    std::array<std::byte, 72> ekey_arr;
                    std::copy(fp_keymsg_.begin(), fp_keymsg_.end(), keymsg_arr.begin());
                    std::copy(fp_ekey_.begin(), fp_ekey_.end(), ekey_arr.begin());
                    auto aeskey = crypto::fairplay_decrypt_key(keymsg_arr, ekey_arr);
                    {
                        std::string aeskey_hex;
                        for (int i = 0; i < 16; ++i) {
                            char buf[4];
                            snprintf(buf, sizeof(buf), "%02x ",
                                     static_cast<uint8_t>(aeskey[static_cast<size_t>(i)]));
                            aeskey_hex += buf;
                        }
                        mirage::log::info("FairPlay decrypted aeskey (raw): {}", aeskey_hex);
                    }
                    auto shared_secret = pairing_.transient_shared_secret();
                    {
                        std::string secret_hex;
                        for (int i = 0; i < 32; ++i) {
                            char buf[4];
                            snprintf(buf, sizeof(buf), "%02x ",
                                     static_cast<uint8_t>(shared_secret[static_cast<size_t>(i)]));
                            secret_hex += buf;
                        }
                        mirage::log::debug("ECDH shared secret: {}", secret_hex);
                    }
                    std::array<std::byte, 48> combined;
                    std::copy_n(aeskey.begin(), 16, combined.begin());
                    std::copy_n(shared_secret.begin(), 32, combined.begin() + 16);
                    auto hashed_key = crypto::sha512(combined);
                    std::copy_n(hashed_key.begin(), 16, aeskey.begin());
                    {
                        std::string aeskey_hex;
                        for (int i = 0; i < 16; ++i) {
                            char buf[4];
                            snprintf(buf, sizeof(buf), "%02x ",
                                     static_cast<uint8_t>(aeskey[static_cast<size_t>(i)]));
                            aeskey_hex += buf;
                        }
                        mirage::log::info("FairPlay aeskey (after SHA-512 with ecdh): {}",
                                          aeskey_hex);
                    }
                    mirage::log::debug("SHA-512 input: 'AirPlayStreamKey{}' + {} bytes aeskey",
                                       stream_connection_id_, aeskey.size());
                    auto key_hash =
                        crypto::sha512_concat("AirPlayStreamKey", stream_connection_id_, aeskey);
                    auto iv_hash =
                        crypto::sha512_concat("AirPlayStreamIV", stream_connection_id_, aeskey);
                    std::array<std::byte, 16> video_key;
                    std::array<std::byte, 16> video_iv;
                    std::copy_n(key_hash.begin(), 16, video_key.begin());
                    std::copy_n(iv_hash.begin(), 16, video_iv.begin());
                    {
                        std::string hash_hex;
                        for (size_t i = 0; i < 64; ++i) {
                            char buf[4];
                            snprintf(buf, sizeof(buf), "%02x", static_cast<uint8_t>(key_hash[i]));
                            hash_hex += buf;
                        }
                        mirage::log::debug("SHA-512(key) full hash: {}", hash_hex);
                    }
                    {
                        std::string key_hex;
                        std::string iv_hex;
                        for (int i = 0; i < 16; ++i) {
                            char buf[4];
                            snprintf(buf, sizeof(buf), "%02x ",
                                     static_cast<uint8_t>(video_key[static_cast<size_t>(i)]));
                            key_hex += buf;
                            snprintf(buf, sizeof(buf), "%02x ",
                                     static_cast<uint8_t>(video_iv[static_cast<size_t>(i)]));
                            iv_hex += buf;
                        }
                        mirage::log::info("Video key: {}", key_hex);
                        mirage::log::info("Video IV: {}", iv_hex);
                    }
                    auto decryptor = crypto::aes_ctr_decryptor::create(video_key, video_iv);
                    if (decryptor) {
                        video_decryptor = std::move(*decryptor);
                        mirage::log::info("Video decryption initialized for streamConnectionID={}",
                                          stream_connection_id_);
                    } else {
                        mirage::log::warn("Failed to initialize video decryption: {}",
                                          decryptor.error().message);
                    }
                    audio_aes_key_ = aeskey;
                    audio_keys_ready_ = true;
                    mirage::log::info("Audio AES-CBC key ready");
                } else {
                    mirage::log::warn(
                        "Cannot initialize video decryption: keymsg={} bytes, ekey={} bytes",
                        fp_keymsg_.size(), fp_ekey_.size());
                }
            } else {
                mirage::log::warn("Cannot initialize video decryption: streamID={}",
                                  stream_connection_id_);
            }
            std::array<std::byte, 128> header;
            while (mirror_socket.is_open() && state_ != rtsp_session_state::teardown &&
                   (!video_renderer || video_renderer->is_open())) {
                try {
                    co_await mirror_socket.async_read_exactly(std::span<std::byte>(header));
                } catch (const std::system_error& e) {
                    if (e.code() != std::errc::connection_reset &&
                        e.code() != std::errc::operation_canceled) {
                        mirage::log::debug("Mirror header read error: {}", e.what());
                    }
                    break;
                } catch (...) {
                    break;
                }
                uint32_t payload_size = static_cast<uint32_t>(header[0]) |
                                        (static_cast<uint32_t>(header[1]) << 8) |
                                        (static_cast<uint32_t>(header[2]) << 16) |
                                        (static_cast<uint32_t>(header[3]) << 24);
                uint8_t payload_type = static_cast<uint8_t>(header[4]);
                uint8_t payload_flag = static_cast<uint8_t>(header[5]);
                uint8_t option0 = static_cast<uint8_t>(header[6]);
                (void)header[7];
                if (option0 == 0x1e || option0 == 0x5e) {
                    is_h265 = true;
                }
                std::vector<std::byte> payload(payload_size);
                try {
                    co_await mirror_socket.async_read_exactly(
                        std::span<std::byte>(payload.data(), payload_size));
                } catch (const std::system_error& e) {
                    if (e.code() != std::errc::connection_reset &&
                        e.code() != std::errc::operation_canceled) {
                        mirage::log::debug("Mirror payload read error: {}", e.what());
                    }
                    break;
                } catch (...) {
                    break;
                }
                switch (payload_type) {
                    case 0x00: {
                        bool is_keyframe = (payload_flag == 0x10);
                        frame_count++;
                        if (is_keyframe) {
                            keyframe_count++;
                        }
                        if (frame_count % 60 == 1) {
                            mirage::log::info(
                                "Video: {} frames ({} keyframes), {} codec, payload {} bytes{}",
                                frame_count, keyframe_count, is_h265 ? "H.265" : "H.264",
                                payload_size, is_keyframe ? " [KEYFRAME]" : "");
                        }
                        if (video_decryptor && payload_size > 0) {
                            if (frame_count <= 3) {
                                std::string hex;
                                for (size_t i = 0; i < std::min(size_t{32}, payload.size()); ++i) {
                                    char buf[4];
                                    snprintf(buf, sizeof(buf), "%02x ",
                                             static_cast<uint8_t>(payload[i]));
                                    hex += buf;
                                }
                                mirage::log::debug("Frame {} encrypted ({} bytes): {}", frame_count,
                                                   payload_size, hex);
                            }
                            std::vector<std::byte> decrypted(payload_size);
                            auto result = video_decryptor->decrypt(payload, decrypted);
                            if (result) {
                                if (frame_count <= 3) {
                                    std::string hex;
                                    for (size_t i = 0; i < std::min(size_t{32}, decrypted.size());
                                         ++i) {
                                        char buf[4];
                                        snprintf(buf, sizeof(buf), "%02x ",
                                                 static_cast<uint8_t>(decrypted[i]));
                                        hex += buf;
                                    }
                                    mirage::log::debug("Frame {} decrypted ({} bytes): {}",
                                                       frame_count, payload_size, hex);
                                }
                                size_t offset = 0;
                                int nal_count = 0;
                                bool valid = true;
                                while (offset + 4 < decrypted.size() && valid) {
                                    uint32_t nal_len =
                                        (static_cast<uint32_t>(decrypted[offset]) << 24) |
                                        (static_cast<uint32_t>(decrypted[offset + 1]) << 16) |
                                        (static_cast<uint32_t>(decrypted[offset + 2]) << 8) |
                                        static_cast<uint32_t>(decrypted[offset + 3]);
                                    if (nal_len == 0 || offset + 4 + nal_len > decrypted.size()) {
                                        valid = false;
                                        break;
                                    }
                                    uint8_t nal_header =
                                        static_cast<uint8_t>(decrypted[offset + 4]);
                                    if (nal_header & 0x80) {
                                        valid = false;
                                        break;
                                    }
                                    (void)(is_h265 ? ((nal_header >> 1) & 0x3F)
                                                   : (nal_header & 0x1F));
                                    nal_count++;
                                    offset += 4 + nal_len;
                                }
                                if (frame_count <= 5 || (frame_count % 300 == 1)) {
                                    if (valid && offset == decrypted.size()) {
                                        mirage::log::info(
                                            "Frame {} decrypted successfully: {} NAL unit(s)",
                                            frame_count, nal_count);
                                    } else {
                                        mirage::log::warn(
                                            "Frame {} decryption check failed at offset {}/{}",
                                            frame_count, offset, decrypted.size());
                                    }
                                }
                                if (video_decoder && video_renderer && video_renderer->is_open()) {
                                    std::vector<std::byte> annex_b;
                                    constexpr std::array<std::byte, 4> start_code{
                                        std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                                        std::byte{0x01}};
                                    if (is_keyframe && !sps_pps_sent && !sps_pps_data.empty()) {
                                        annex_b.insert(annex_b.end(), sps_pps_data.begin(),
                                                       sps_pps_data.end());
                                        sps_pps_sent = true;
                                        mirage::log::info(
                                            "Prepended SPS/PPS ({} bytes) to keyframe",
                                            sps_pps_data.size());
                                    }
                                    size_t nal_offset = 0;
                                    while (nal_offset + 4 < decrypted.size()) {
                                        uint32_t nal_len =
                                            (static_cast<uint32_t>(decrypted[nal_offset]) << 24) |
                                            (static_cast<uint32_t>(decrypted[nal_offset + 1])
                                             << 16) |
                                            (static_cast<uint32_t>(decrypted[nal_offset + 2])
                                             << 8) |
                                            static_cast<uint32_t>(decrypted[nal_offset + 3]);
                                        if (nal_len == 0 ||
                                            nal_offset + 4 + nal_len > decrypted.size()) {
                                            break;
                                        }
                                        annex_b.insert(annex_b.end(), start_code.begin(),
                                                       start_code.end());
                                        annex_b.insert(
                                            annex_b.end(),
                                            decrypted.begin() +
                                                static_cast<std::ptrdiff_t>(nal_offset + 4),
                                            decrypted.begin() + static_cast<std::ptrdiff_t>(
                                                                    nal_offset + 4 + nal_len));
                                        nal_offset += 4 + nal_len;
                                    }
                                    auto decode_result = video_decoder->decode(annex_b);
                                    if (decode_result && decode_result->has_value()) {
                                        video_renderer->submit_frame(
                                            std::move(decode_result->value()));
                                    } else if (!decode_result) {
                                        if (frame_count % 60 == 1) {
                                            mirage::log::debug("Decode: {}",
                                                               decode_result.error().message);
                                        }
                                    }
                                }
                            }
                        }
                        break;
                    }
                    case 0x01: {
                        if (payload_size < 11) {
                            mirage::log::warn("SPS/PPS packet too small: {} bytes", payload_size);
                            break;
                        }
                        {
                            std::string hex;
                            for (size_t i = 0; i < std::min(size_t{48}, payload.size()); ++i) {
                                char buf[4];
                                snprintf(buf, sizeof(buf), "%02x ",
                                         static_cast<uint8_t>(payload[i]));
                                hex += buf;
                            }
                            mirage::log::debug("SPS/PPS raw ({} bytes): {}", payload_size, hex);
                        }
                        auto sps_size =
                            static_cast<size_t>((static_cast<uint16_t>(payload[6]) << 8) |
                                                static_cast<uint16_t>(payload[7]));
                        if (8 + sps_size + 3 > payload_size) {
                            mirage::log::warn("SPS/PPS: invalid sps_size {} for payload {}",
                                              sps_size, payload_size);
                            break;
                        }
                        auto pps_size = static_cast<size_t>(
                            (static_cast<uint16_t>(payload[8 + sps_size + 1]) << 8) |
                            static_cast<uint16_t>(payload[8 + sps_size + 2]));
                        if (8 + sps_size + 3 + pps_size > payload_size) {
                            mirage::log::warn("SPS/PPS: invalid pps_size {} for payload {}",
                                              pps_size, payload_size);
                            break;
                        }
                        mirage::log::info("Received SPS ({} bytes) + PPS ({} bytes), {} codec",
                                          sps_size, pps_size, is_h265 ? "H.265" : "H.264");
                        constexpr std::array<std::byte, 4> start_code{
                            std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}};
                        sps_pps_data.clear();
                        sps_pps_data.reserve(sps_size + pps_size + 8);
                        sps_pps_data.insert(sps_pps_data.end(), start_code.begin(),
                                            start_code.end());
                        sps_pps_data.insert(
                            sps_pps_data.end(), payload.begin() + 8,
                            payload.begin() + static_cast<std::ptrdiff_t>(8 + sps_size));
                        sps_pps_data.insert(sps_pps_data.end(), start_code.begin(),
                                            start_code.end());
                        sps_pps_data.insert(
                            sps_pps_data.end(),
                            payload.begin() + static_cast<std::ptrdiff_t>(8 + sps_size + 3),
                            payload.begin() +
                                static_cast<std::ptrdiff_t>(8 + sps_size + 3 + pps_size));
                        sps_pps_sent = false;
                        {
                            std::string hex;
                            for (auto& i : sps_pps_data) {
                                char buf[4];
                                snprintf(buf, sizeof(buf), "%02x ", static_cast<uint8_t>(i));
                                hex += buf;
                            }
                            mirage::log::debug("Parsed SPS/PPS Annex-B ({} bytes): {}",
                                               sps_pps_data.size(), hex);
                        }
                        break;
                    }
                    case 0x02: {
                        mirage::log::debug("Mirror heartbeat packet");
                        break;
                    }
                    case 0x05: {
                        mirage::log::debug("Streaming report ({} bytes)", payload_size);
                        break;
                    }
                    default:
                        mirage::log::debug("Unknown mirror packet type 0x{:02x} 0x{:02x}, {} bytes",
                                           payload_type, payload_flag, payload_size);
                        break;
                }
            }
            mirage::log::info("Mirror connection closed after {} frames ({} keyframes)",
                              frame_count, keyframe_count);
            if (video_renderer && !video_renderer->is_open()) {
                mirage::log::info("Window closed, tearing down session");
                state_ = rtsp_session_state::teardown;
                socket_.close();
            }
        }
    } catch (const std::system_error& e) {
        if (e.code() != std::errc::operation_canceled) {
            mirage::log::debug("Mirror receiver ended: {}", e.what());
        }
    }
    mirage::log::debug("Mirror receiver stopped");
}
void rtsp_session::process_audio_packet(std::span<const std::byte> rtp_packet, size_t len) {
    if (len <= 12 || !audio_decoder_) {
        return;
    }

    auto payload_start = rtp_packet.data() + 12;
    auto payload_len = len - 12;

    if (payload_len == 0) {
        return;
    }

    constexpr std::array<std::byte, 4> no_data_marker{std::byte{0x00}, std::byte{0x68},
                                                      std::byte{0x34}, std::byte{0x00}};
    if (payload_len == 4 && std::equal(payload_start, payload_start + 4, no_data_marker.begin())) {
        return;
    }

    if (audio_ct_ == 2 && payload_len == 44) {
        return;
    }

    std::vector<std::byte> decrypted(payload_len);
    if (audio_keys_ready_ && payload_len > 0) {
        auto encrypted_len = (payload_len / 16) * 16;
        if (encrypted_len > 0) {
            crypto::aes_cbc_decrypt(audio_aes_key_, audio_aes_iv_,
                                    std::span<const std::byte>(payload_start, encrypted_len),
                                    std::span<std::byte>(decrypted.data(), encrypted_len));
        }
        if (payload_len > encrypted_len) {
            std::copy_n(payload_start + encrypted_len, payload_len - encrypted_len,
                        decrypted.data() + encrypted_len);
        }
    } else {
        std::copy_n(payload_start, payload_len, decrypted.data());
    }

    auto pcm = audio_decoder_->decode(decrypted);
    if (!pcm.empty() && audio_player_) {
        audio_player_->push_pcm(pcm);
    }

    ++audio_packet_count_;
    if (audio_packet_count_ == 1 || audio_packet_count_ % 500 == 0) {
        mirage::log::info("Audio: {} packets received", audio_packet_count_);
    }
}
io::task<void> rtsp_session::run_audio_receiver() {
    if (!audio_data_socket_) {
        co_return;
    }
    mirage::log::info("Audio receiver started on UDP port {}",
                      audio_data_socket_->local_endpoint().port);

    if (!audio_decoder_) {
        audio_decoder_ = audio::audio_decoder::create_aac(audio_sample_rate_, audio_channels_);
    }
    if (!audio_decoder_) {
        mirage::log::error("Failed to create audio decoder");
        co_return;
    }

    audio_player_ = audio::audio_player::create(44100, 2);
    if (!audio_player_) {
        mirage::log::error("Failed to create audio player");
        co_return;
    }

    mirage::log::info("Audio decoder and player initialized");

    std::array<std::byte, 2048> buf;

    try {
        while (audio_data_socket_->is_open() && state_ != rtsp_session_state::teardown) {
            io::endpoint sender;
            auto n =
                co_await audio_data_socket_->async_recv_from(std::span<std::byte>(buf), sender);

            if (n <= 12) {
                continue;
            }

            auto seqnum =
                static_cast<uint16_t>((static_cast<uint16_t>(static_cast<uint8_t>(buf[2])) << 8) |
                                      static_cast<uint16_t>(static_cast<uint8_t>(buf[3])));

            if (audio_packet_count_ > 0) {
                auto expected = static_cast<uint16_t>(last_audio_seqnum_ + 1);
                auto gap = static_cast<uint16_t>(seqnum - expected);
                if (gap > 0 && gap < 1000) {
                    mirage::log::debug(
                        "Audio gap detected: expected seq {}, got {}, missing {} packets", expected,
                        seqnum, gap);
                    if (audio_control_socket_ && audio_control_socket_->is_open() &&
                        audio_control_remote_.port != 0) {
                        std::array<std::byte, 8> resend_req{};
                        resend_req[0] = std::byte{0x80};
                        resend_req[1] = std::byte{0xD5};
                        resend_req[2] =
                            std::byte{static_cast<uint8_t>((audio_resend_seqnum_ >> 8) & 0xFF)};
                        resend_req[3] =
                            std::byte{static_cast<uint8_t>(audio_resend_seqnum_ & 0xFF)};
                        resend_req[4] = std::byte{static_cast<uint8_t>((expected >> 8) & 0xFF)};
                        resend_req[5] = std::byte{static_cast<uint8_t>(expected & 0xFF)};
                        resend_req[6] = std::byte{static_cast<uint8_t>((gap >> 8) & 0xFF)};
                        resend_req[7] = std::byte{static_cast<uint8_t>(gap & 0xFF)};
                        ++audio_resend_seqnum_;
                        co_await audio_control_socket_->async_send_to(
                            std::span<const std::byte>(resend_req), audio_control_remote_);
                        mirage::log::debug("Sent resend request: seq_start={}, count={}", expected,
                                           gap);
                    }
                }
            }

            last_audio_seqnum_ = seqnum;
            process_audio_packet(std::span<const std::byte>(buf.data(), n), n);
        }
    } catch (const std::system_error& e) {
        if (e.code() != std::errc::operation_canceled) {
            mirage::log::debug("Audio receiver ended: {}", e.what());
        }
    }

    if (audio_player_) {
        audio_player_->stop();
    }
    mirage::log::info("Audio receiver stopped after {} packets", audio_packet_count_);
}
io::task<void> rtsp_session::run_audio_control_receiver() {
    if (!audio_control_socket_) {
        co_return;
    }
    mirage::log::debug("Audio control receiver started on port {}",
                       audio_control_socket_->local_endpoint().port);
    std::array<std::byte, 2048> buf;
    try {
        while (audio_control_socket_->is_open() && state_ != rtsp_session_state::teardown) {
            io::endpoint sender;
            auto n =
                co_await audio_control_socket_->async_recv_from(std::span<std::byte>(buf), sender);

            audio_control_remote_ = sender;

            if (n < 4) {
                continue;
            }

            auto ptype = static_cast<uint8_t>(buf[1]);

            if (ptype == 0x56 && n >= 16) {
                auto rtp_data = std::span<const std::byte>(buf.data() + 4, n - 4);
                mirage::log::debug("Audio control: retransmitted packet ({} bytes)",
                                   rtp_data.size());
                process_audio_packet(rtp_data, rtp_data.size());
            } else if (ptype == 0x54) {
                mirage::log::debug("Audio control: sync packet ({} bytes)", n);
            } else {
                mirage::log::debug("Audio control: type=0x{:02x}, {} bytes from {}:{}", ptype, n,
                                   sender.addr.to_string(), sender.port);
            }
        }
    } catch (const std::system_error& e) {
        if (e.code() != std::errc::operation_canceled) {
            mirage::log::debug("Audio control receiver ended: {}", e.what());
        }
    }
}
result<rtsp_response> rtsp_session::handle_record(const rtsp_request& /* req */) {
    mirage::log::debug("RECORD request");
    state_ = rtsp_session_state::playing;
    return rtsp_response{
        .status_code = 200,
        .status_text = "OK",
        .headers =
            {
                {"CSeq", std::to_string(cseq_)},
                {"Audio-Latency", "11025"},
                {"Audio-Jack-Status", "connected; type=analog"},
            },
        .body = {},
    };
}
result<rtsp_response> rtsp_session::handle_teardown(const rtsp_request& req) {
    bool teardown_audio = false;
    bool teardown_video = false;
    bool has_streams = false;
    if (!req.body.empty() && bplist::has_streams(req.body)) {
        has_streams = true;
        auto type_val = bplist::get_uint(req.body, "type");
        if (type_val) {
            if (*type_val == 96) {
                teardown_audio = true;
            } else if (*type_val == 110) {
                teardown_video = true;
            }
        }
    }
    mirage::log::debug("TEARDOWN request: audio={}, video={}, full={}", teardown_audio,
                       teardown_video, !has_streams);
    if (teardown_audio) {
        if (audio_data_socket_) {
            audio_data_socket_->close();
        }
        if (audio_control_socket_) {
            audio_control_socket_->close();
        }
        if (audio_player_) {
            audio_player_->stop();
        }
        audio_receiver_started_ = false;
        mirage::log::info("Audio stream torn down");
    }
    if (teardown_video) {
        if (mirror_acceptor_) {
            mirror_acceptor_->close();
        }
        mirror_receiver_started_ = false;
        mirage::log::info("Video stream torn down");
    }
    if (!has_streams) {
        mirage::log::user("{} disconnected", socket_.remote_endpoint().addr.to_string());
        state_ = rtsp_session_state::teardown;
        if (timing_socket_) {
            timing_socket_->close();
        }
        if (control_socket_) {
            control_socket_->close();
        }
        if (mirror_acceptor_) {
            mirror_acceptor_->close();
        }
        if (audio_data_socket_) {
            audio_data_socket_->close();
        }
        if (audio_control_socket_) {
            audio_control_socket_->close();
        }
        if (audio_player_) {
            audio_player_->stop();
        }
        base_receivers_started_ = false;
        mirror_receiver_started_ = false;
        audio_receiver_started_ = false;
    }
    return rtsp_response{
        .status_code = 200,
        .status_text = "OK",
        .headers = {{"CSeq", std::to_string(cseq_)}},
        .body = {},
    };
}
static constexpr std::array<std::array<std::byte, 142>, 4> fp_reply_messages = {{
    {std::byte{0x46}, std::byte{0x50}, std::byte{0x4c}, std::byte{0x59}, std::byte{0x03},
     std::byte{0x01}, std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
     std::byte{0x00}, std::byte{0x82}, std::byte{0x02}, std::byte{0x00}, std::byte{0x0f},
     std::byte{0x9f}, std::byte{0x3f}, std::byte{0x9e}, std::byte{0x0a}, std::byte{0x25},
     std::byte{0x21}, std::byte{0xdb}, std::byte{0xdf}, std::byte{0x31}, std::byte{0x2a},
     std::byte{0xb2}, std::byte{0xbf}, std::byte{0xb2}, std::byte{0x9e}, std::byte{0x8d},
     std::byte{0x23}, std::byte{0x2b}, std::byte{0x63}, std::byte{0x76}, std::byte{0xa8},
     std::byte{0xc8}, std::byte{0x18}, std::byte{0x70}, std::byte{0x1d}, std::byte{0x22},
     std::byte{0xae}, std::byte{0x93}, std::byte{0xd8}, std::byte{0x27}, std::byte{0x37},
     std::byte{0xfe}, std::byte{0xaf}, std::byte{0x9d}, std::byte{0xb4}, std::byte{0xfd},
     std::byte{0xf4}, std::byte{0x1c}, std::byte{0x2d}, std::byte{0xba}, std::byte{0x9d},
     std::byte{0x1f}, std::byte{0x49}, std::byte{0xca}, std::byte{0xaa}, std::byte{0xbf},
     std::byte{0x65}, std::byte{0x91}, std::byte{0xac}, std::byte{0x1f}, std::byte{0x7b},
     std::byte{0xc6}, std::byte{0xf7}, std::byte{0xe0}, std::byte{0x66}, std::byte{0x3d},
     std::byte{0x21}, std::byte{0xaf}, std::byte{0xe0}, std::byte{0x15}, std::byte{0x65},
     std::byte{0x95}, std::byte{0x3e}, std::byte{0xab}, std::byte{0x81}, std::byte{0xf4},
     std::byte{0x18}, std::byte{0xce}, std::byte{0xed}, std::byte{0x09}, std::byte{0x5a},
     std::byte{0xdb}, std::byte{0x7c}, std::byte{0x3d}, std::byte{0x0e}, std::byte{0x25},
     std::byte{0x49}, std::byte{0x09}, std::byte{0xa7}, std::byte{0x98}, std::byte{0x31},
     std::byte{0xd4}, std::byte{0x9c}, std::byte{0x39}, std::byte{0x82}, std::byte{0x97},
     std::byte{0x34}, std::byte{0x34}, std::byte{0xfa}, std::byte{0xcb}, std::byte{0x42},
     std::byte{0xc6}, std::byte{0x3a}, std::byte{0x1c}, std::byte{0xd9}, std::byte{0x11},
     std::byte{0xa6}, std::byte{0xfe}, std::byte{0x94}, std::byte{0x1a}, std::byte{0x8a},
     std::byte{0x6d}, std::byte{0x4a}, std::byte{0x74}, std::byte{0x3b}, std::byte{0x46},
     std::byte{0xc3}, std::byte{0xa7}, std::byte{0x64}, std::byte{0x9e}, std::byte{0x44},
     std::byte{0xc7}, std::byte{0x89}, std::byte{0x55}, std::byte{0xe4}, std::byte{0x9d},
     std::byte{0x81}, std::byte{0x55}, std::byte{0x00}, std::byte{0x95}, std::byte{0x49},
     std::byte{0xc4}, std::byte{0xe2}, std::byte{0xf7}, std::byte{0xa3}, std::byte{0xf6},
     std::byte{0xd5}, std::byte{0xba}},
    {std::byte{0x46}, std::byte{0x50}, std::byte{0x4c}, std::byte{0x59}, std::byte{0x03},
     std::byte{0x01}, std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
     std::byte{0x00}, std::byte{0x82}, std::byte{0x02}, std::byte{0x01}, std::byte{0xcf},
     std::byte{0x32}, std::byte{0xa2}, std::byte{0x57}, std::byte{0x14}, std::byte{0xb2},
     std::byte{0x52}, std::byte{0x4f}, std::byte{0x8a}, std::byte{0xa0}, std::byte{0xad},
     std::byte{0x7a}, std::byte{0xf1}, std::byte{0x64}, std::byte{0xe3}, std::byte{0x7b},
     std::byte{0xcf}, std::byte{0x44}, std::byte{0x24}, std::byte{0xe2}, std::byte{0x00},
     std::byte{0x04}, std::byte{0x7e}, std::byte{0xfc}, std::byte{0x0a}, std::byte{0xd6},
     std::byte{0x7a}, std::byte{0xfc}, std::byte{0xd9}, std::byte{0x5d}, std::byte{0xed},
     std::byte{0x1c}, std::byte{0x27}, std::byte{0x30}, std::byte{0xbb}, std::byte{0x59},
     std::byte{0x1b}, std::byte{0x96}, std::byte{0x2e}, std::byte{0xd6}, std::byte{0x3a},
     std::byte{0x9c}, std::byte{0x4d}, std::byte{0xed}, std::byte{0x88}, std::byte{0xba},
     std::byte{0x8f}, std::byte{0xc7}, std::byte{0x8d}, std::byte{0xe6}, std::byte{0x4d},
     std::byte{0x91}, std::byte{0xcc}, std::byte{0xfd}, std::byte{0x5c}, std::byte{0x7b},
     std::byte{0x56}, std::byte{0xda}, std::byte{0x88}, std::byte{0xe3}, std::byte{0x1f},
     std::byte{0x5c}, std::byte{0xce}, std::byte{0xaf}, std::byte{0xc7}, std::byte{0x43},
     std::byte{0x19}, std::byte{0x95}, std::byte{0xa0}, std::byte{0x16}, std::byte{0x65},
     std::byte{0xa5}, std::byte{0x4e}, std::byte{0x19}, std::byte{0x39}, std::byte{0xd2},
     std::byte{0x5b}, std::byte{0x94}, std::byte{0xdb}, std::byte{0x64}, std::byte{0xb9},
     std::byte{0xe4}, std::byte{0x5d}, std::byte{0x8d}, std::byte{0x06}, std::byte{0x3e},
     std::byte{0x1e}, std::byte{0x6a}, std::byte{0xf0}, std::byte{0x7e}, std::byte{0x96},
     std::byte{0x56}, std::byte{0x16}, std::byte{0x2b}, std::byte{0x0e}, std::byte{0xfa},
     std::byte{0x40}, std::byte{0x42}, std::byte{0x75}, std::byte{0xea}, std::byte{0x5a},
     std::byte{0x44}, std::byte{0xd9}, std::byte{0x59}, std::byte{0x1c}, std::byte{0x72},
     std::byte{0x56}, std::byte{0xb9}, std::byte{0xfb}, std::byte{0xe6}, std::byte{0x51},
     std::byte{0x38}, std::byte{0x98}, std::byte{0xb8}, std::byte{0x02}, std::byte{0x27},
     std::byte{0x72}, std::byte{0x19}, std::byte{0x88}, std::byte{0x57}, std::byte{0x16},
     std::byte{0x50}, std::byte{0x94}, std::byte{0x2a}, std::byte{0xd9}, std::byte{0x46},
     std::byte{0x68}, std::byte{0x8a}},
    {std::byte{0x46}, std::byte{0x50}, std::byte{0x4c}, std::byte{0x59}, std::byte{0x03},
     std::byte{0x01}, std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
     std::byte{0x00}, std::byte{0x82}, std::byte{0x02}, std::byte{0x02}, std::byte{0xc1},
     std::byte{0x69}, std::byte{0xa3}, std::byte{0x52}, std::byte{0xee}, std::byte{0xed},
     std::byte{0x35}, std::byte{0xb1}, std::byte{0x8c}, std::byte{0xdd}, std::byte{0x9c},
     std::byte{0x58}, std::byte{0xd6}, std::byte{0x4f}, std::byte{0x16}, std::byte{0xc1},
     std::byte{0x51}, std::byte{0x9a}, std::byte{0x89}, std::byte{0xeb}, std::byte{0x53},
     std::byte{0x17}, std::byte{0xbd}, std::byte{0x0d}, std::byte{0x43}, std::byte{0x36},
     std::byte{0xcd}, std::byte{0x68}, std::byte{0xf6}, std::byte{0x38}, std::byte{0xff},
     std::byte{0x9d}, std::byte{0x01}, std::byte{0x6a}, std::byte{0x5b}, std::byte{0x52},
     std::byte{0xb7}, std::byte{0xfa}, std::byte{0x92}, std::byte{0x16}, std::byte{0xb2},
     std::byte{0xb6}, std::byte{0x54}, std::byte{0x82}, std::byte{0xc7}, std::byte{0x84},
     std::byte{0x44}, std::byte{0x11}, std::byte{0x81}, std::byte{0x21}, std::byte{0xa2},
     std::byte{0xc7}, std::byte{0xfe}, std::byte{0xd8}, std::byte{0x3d}, std::byte{0xb7},
     std::byte{0x11}, std::byte{0x9e}, std::byte{0x91}, std::byte{0x82}, std::byte{0xaa},
     std::byte{0xd7}, std::byte{0xd1}, std::byte{0x8c}, std::byte{0x70}, std::byte{0x63},
     std::byte{0xe2}, std::byte{0xa4}, std::byte{0x57}, std::byte{0x55}, std::byte{0x59},
     std::byte{0x10}, std::byte{0xaf}, std::byte{0x9e}, std::byte{0x0e}, std::byte{0xfc},
     std::byte{0x76}, std::byte{0x34}, std::byte{0x7d}, std::byte{0x16}, std::byte{0x40},
     std::byte{0x43}, std::byte{0x80}, std::byte{0x7f}, std::byte{0x58}, std::byte{0x1e},
     std::byte{0xe4}, std::byte{0xfb}, std::byte{0xe4}, std::byte{0x2c}, std::byte{0xa9},
     std::byte{0xde}, std::byte{0xdc}, std::byte{0x1b}, std::byte{0x5e}, std::byte{0xb2},
     std::byte{0xa3}, std::byte{0xaa}, std::byte{0x3d}, std::byte{0x2e}, std::byte{0xcd},
     std::byte{0x59}, std::byte{0xe7}, std::byte{0xee}, std::byte{0xe7}, std::byte{0x0b},
     std::byte{0x36}, std::byte{0x29}, std::byte{0xf2}, std::byte{0x2a}, std::byte{0xfd},
     std::byte{0x16}, std::byte{0x1d}, std::byte{0x87}, std::byte{0x73}, std::byte{0x53},
     std::byte{0xdd}, std::byte{0xb9}, std::byte{0x9a}, std::byte{0xdc}, std::byte{0x8e},
     std::byte{0x07}, std::byte{0x00}, std::byte{0x6e}, std::byte{0x56}, std::byte{0xf8},
     std::byte{0x50}, std::byte{0xce}},
    {std::byte{0x46}, std::byte{0x50}, std::byte{0x4c}, std::byte{0x59}, std::byte{0x03},
     std::byte{0x01}, std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
     std::byte{0x00}, std::byte{0x82}, std::byte{0x02}, std::byte{0x03}, std::byte{0x90},
     std::byte{0x01}, std::byte{0xe1}, std::byte{0x72}, std::byte{0x7e}, std::byte{0x0f},
     std::byte{0x57}, std::byte{0xf9}, std::byte{0xf5}, std::byte{0x88}, std::byte{0x0d},
     std::byte{0xb1}, std::byte{0x04}, std::byte{0xa6}, std::byte{0x25}, std::byte{0x7a},
     std::byte{0x23}, std::byte{0xf5}, std::byte{0xcf}, std::byte{0xff}, std::byte{0x1a},
     std::byte{0xbb}, std::byte{0xe1}, std::byte{0xe9}, std::byte{0x30}, std::byte{0x45},
     std::byte{0x25}, std::byte{0x1a}, std::byte{0xfb}, std::byte{0x97}, std::byte{0xeb},
     std::byte{0x9f}, std::byte{0xc0}, std::byte{0x01}, std::byte{0x1e}, std::byte{0xbe},
     std::byte{0x0f}, std::byte{0x3a}, std::byte{0x81}, std::byte{0xdf}, std::byte{0x5b},
     std::byte{0x69}, std::byte{0x1d}, std::byte{0x76}, std::byte{0xac}, std::byte{0xb2},
     std::byte{0xf7}, std::byte{0xa5}, std::byte{0xc7}, std::byte{0x08}, std::byte{0xe3},
     std::byte{0xd3}, std::byte{0x28}, std::byte{0xf5}, std::byte{0x6b}, std::byte{0xb3},
     std::byte{0x9d}, std::byte{0xbd}, std::byte{0xe5}, std::byte{0xf2}, std::byte{0x9c},
     std::byte{0x8a}, std::byte{0x17}, std::byte{0xf4}, std::byte{0x81}, std::byte{0x48},
     std::byte{0x7e}, std::byte{0x3a}, std::byte{0xe8}, std::byte{0x63}, std::byte{0xc6},
     std::byte{0x78}, std::byte{0x32}, std::byte{0x54}, std::byte{0x22}, std::byte{0xe6},
     std::byte{0xf7}, std::byte{0x8e}, std::byte{0x16}, std::byte{0x6d}, std::byte{0x18},
     std::byte{0xaa}, std::byte{0x7f}, std::byte{0xd6}, std::byte{0x36}, std::byte{0x25},
     std::byte{0x8b}, std::byte{0xce}, std::byte{0x28}, std::byte{0x72}, std::byte{0x6f},
     std::byte{0x66}, std::byte{0x1f}, std::byte{0x73}, std::byte{0x88}, std::byte{0x93},
     std::byte{0xce}, std::byte{0x44}, std::byte{0x31}, std::byte{0x1e}, std::byte{0x4b},
     std::byte{0xe6}, std::byte{0xc0}, std::byte{0x53}, std::byte{0x51}, std::byte{0x93},
     std::byte{0xe5}, std::byte{0xef}, std::byte{0x72}, std::byte{0xe8}, std::byte{0x68},
     std::byte{0x62}, std::byte{0x33}, std::byte{0x72}, std::byte{0x9c}, std::byte{0x22},
     std::byte{0x7d}, std::byte{0x82}, std::byte{0x0c}, std::byte{0x99}, std::byte{0x94},
     std::byte{0x45}, std::byte{0xd8}, std::byte{0x92}, std::byte{0x46}, std::byte{0xc8},
     std::byte{0xc3}, std::byte{0x59}},
}};
static constexpr std::array<std::byte, 12> fp_header = {
    std::byte{0x46}, std::byte{0x50}, std::byte{0x4c}, std::byte{0x59},
    std::byte{0x03}, std::byte{0x01}, std::byte{0x04}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x14}};
result<rtsp_response> rtsp_session::handle_fp_setup(const rtsp_request& req) {
    mirage::log::debug("FairPlay setup request, {} bytes", req.body.size());
    if (req.body.size() == 16) {
        if (req.body.size() < 15 || static_cast<uint8_t>(req.body[4]) != 0x03) {
            mirage::log::warn("Unsupported FairPlay version");
            return std::unexpected(
                mirage_error::protocol_err(protocol::airplay, "unsupported FairPlay version"));
        }
        auto mode = static_cast<size_t>(static_cast<uint8_t>(req.body[14]) & 0x03);
        mirage::log::debug("FairPlay setup mode: {}", mode);
        std::vector<std::byte> response(fp_reply_messages[mode].begin(),
                                        fp_reply_messages[mode].end());
        return rtsp_response{
            .status_code = 200,
            .status_text = "OK",
            .headers =
                {
                    {"CSeq", std::to_string(cseq_)},
                    {"Content-Type", "application/octet-stream"},
                },
            .body = std::move(response),
        };
    }
    if (req.body.size() == 164) {
        if (static_cast<uint8_t>(req.body[4]) != 0x03) {
            mirage::log::warn("Unsupported FairPlay version in handshake");
            return std::unexpected(
                mirage_error::protocol_err(protocol::airplay, "unsupported FairPlay version"));
        }
        fp_keymsg_.assign(req.body.begin(), req.body.end());
        std::vector<std::byte> response;
        response.reserve(32);
        response.insert(response.end(), fp_header.begin(), fp_header.end());
        response.insert(response.end(), req.body.begin() + 144, req.body.begin() + 164);
        mirage::log::debug("FairPlay handshake complete");
        return rtsp_response{
            .status_code = 200,
            .status_text = "OK",
            .headers =
                {
                    {"CSeq", std::to_string(cseq_)},
                    {"Content-Type", "application/octet-stream"},
                },
            .body = std::move(response),
        };
    }
    mirage::log::warn("Invalid FairPlay setup data length: {}", req.body.size());
    return std::unexpected(
        mirage_error::protocol_err(protocol::airplay, "invalid FairPlay data length"));
}
result<rtsp_response> rtsp_session::handle_get_parameter(const rtsp_request& req) const {
    std::string content_type;
    for (const auto& [k, v] : req.headers) {
        std::string lk = k;
        std::transform(lk.begin(), lk.end(), lk.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lk == "content-type") {
            content_type = v;
            break;
        }
    }
    if (content_type.empty()) {
        return rtsp_response{
            .status_code = 451,
            .status_text = "Parameter not understood",
            .headers = {{"CSeq", std::to_string(cseq_)}},
            .body = {},
        };
    }
    if (content_type == "text/parameters") {
        std::string body_str(reinterpret_cast<const char*>(req.body.data()), req.body.size());
        if (body_str.find("volume") != std::string::npos) {
            std::string volume_response = "volume: 0.000000\r\n";
            std::vector<std::byte> body;
            body.reserve(volume_response.size());
            for (char c : volume_response) {
                body.push_back(static_cast<std::byte>(c));
            }
            return rtsp_response{
                .status_code = 200,
                .status_text = "OK",
                .headers =
                    {
                        {"CSeq", std::to_string(cseq_)},
                        {"Content-Type", "text/parameters"},
                    },
                .body = std::move(body),
            };
        }
        mirage::log::debug("GET_PARAMETER unknown parameter: {}", body_str);
    }
    return rtsp_response{
        .status_code = 200,
        .status_text = "OK",
        .headers = {{"CSeq", std::to_string(cseq_)}},
        .body = {},
    };
}
result<rtsp_response> rtsp_session::handle_set_parameter(const rtsp_request& req) const {
    std::string content_type;
    for (const auto& [k, v] : req.headers) {
        std::string lk = k;
        std::transform(lk.begin(), lk.end(), lk.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lk == "content-type") {
            content_type = v;
            break;
        }
    }
    if (content_type.empty()) {
        return rtsp_response{
            .status_code = 451,
            .status_text = "Parameter not understood",
            .headers = {{"CSeq", std::to_string(cseq_)}},
            .body = {},
        };
    }
    if (content_type == "text/parameters") {
        std::string body_str(reinterpret_cast<const char*>(req.body.data()), req.body.size());
        if (body_str.find("volume:") != std::string::npos) {
            mirage::log::debug("SET_PARAMETER volume: {}", body_str);
            auto pos = body_str.find("volume:");
            if (pos != std::string::npos) {
                float db = std::stof(body_str.substr(pos + 7));
                float linear = (db <= -30.0F) ? 0.0F : std::pow(10.0F, db / 20.0F);
                if (audio_player_) {
                    audio_player_->set_volume(linear);
                }
            }
        }
        if (body_str.find("progress:") != std::string::npos) {
            mirage::log::debug("SET_PARAMETER progress: {}", body_str);
        }
    } else if (content_type == "image/jpeg" || content_type == "image/png") {
        mirage::log::debug("SET_PARAMETER received artwork ({} bytes)", req.body.size());
    } else if (content_type == "application/x-dmap-tagged") {
        mirage::log::debug("SET_PARAMETER received DMAP metadata ({} bytes)", req.body.size());
    } else {
        mirage::log::debug("SET_PARAMETER unknown content type: {}", content_type);
    }
    return rtsp_response{
        .status_code = 200,
        .status_text = "OK",
        .headers = {{"CSeq", std::to_string(cseq_)}},
        .body = {},
    };
}
rtsp_server::rtsp_server(io::tcp_acceptor acceptor, crypto::ed25519_keypair keypair)
    : acceptor_(std::move(acceptor)), keypair_(std::move(keypair)) {}
auto rtsp_server::bind(io::io_context& ctx, uint16_t port, crypto::ed25519_keypair keypair)
    -> result<rtsp_server> {
    try {
        auto acceptor = io::tcp_acceptor::bind(ctx, port);
        mirage::log::info("RTSP server bound to port {}", port);
        return rtsp_server{std::move(acceptor), std::move(keypair)};
    } catch (const std::exception& e) {
        return std::unexpected(
            mirage_error::network(std::format("failed to bind RTSP server: {}", e.what())));
    }
}
io::task<void> rtsp_server::run() {
    running_ = true;
    while (running_) {
        try {
            auto socket = co_await acceptor_.async_accept();
            auto cloned_keypair = keypair_.clone();
            if (!cloned_keypair) {
                mirage::log::error("Failed to clone server keypair: {}",
                                   cloned_keypair.error().message);
                continue;
            }
            crypto::fairplay_pairing pairing{std::move(*cloned_keypair)};
            auto session = rtsp_session::create(std::move(socket), std::move(pairing));
            io::co_spawn(acceptor_.context(), [session]() -> io::task<void> {
                auto result = co_await session->run();
                if (!result) {
                    mirage::log::warn("Session ended with error: {}", result.error().message);
                }
            });
        } catch (const std::system_error& e) {
            if (e.code() != std::errc::operation_canceled) {
                mirage::log::warn("Accept error: {}", e.what());
            }
        }
    }
}
void rtsp_server::stop() {
    running_ = false;
    acceptor_.close();
}
}  // namespace mirage::protocols
