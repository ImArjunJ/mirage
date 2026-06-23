#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/core.hpp"
#include "core/log.hpp"
#include "crypto/crypto.hpp"
#include "io/io.hpp"
#include "media/media.hpp"
#include "media/pipeline.hpp"
#include "protocols/airplay/media_source.hpp"
#include "protocols/airplay_protocol.hpp"
#include "protocols/protocols.hpp"
#include "protocols/rtsp_message.hpp"
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
        if (nbytes == 0 || nbytes > 8 || offset + 1 + nbytes > data.size()) {
            return std::nullopt;
        }
        uint64_t val = 0;
        for (size_t i = 0; i < nbytes; ++i) {
            val = (val << 8) | static_cast<uint64_t>(static_cast<uint8_t>(data[offset + 1 + i]));
        }
        return val;
    }
    return std::nullopt;
}
static std::optional<uint64_t> read_be(std::span<const std::byte> data, size_t offset,
                                       size_t nbytes) {
    if (nbytes == 0 || nbytes > 8 || offset + nbytes > data.size()) {
        return std::nullopt;
    }
    uint64_t val = 0;
    for (size_t i = 0; i < nbytes; ++i) {
        val = (val << 8) | static_cast<uint64_t>(static_cast<uint8_t>(data[offset + i]));
    }
    return val;
}
static std::optional<std::pair<size_t, uint64_t>> parse_count(std::span<const std::byte> data,
                                                              size_t offset, uint8_t marker) {
    uint64_t count = marker & 0x0F;
    size_t cursor = offset + 1;
    if (count != 0x0F) {
        return std::pair{cursor, count};
    }
    auto parsed = parse_uint(data, cursor);
    if (!parsed) {
        return std::nullopt;
    }
    auto int_marker = static_cast<uint8_t>(data[cursor]);
    size_t nbytes = static_cast<size_t>(1) << (int_marker & 0x0F);
    return std::pair{cursor + 1 + nbytes, *parsed};
}
struct parse_context {
    std::span<const std::byte> data;
    std::vector<size_t> offsets;
    size_t ref_size = 0;
};
static std::shared_ptr<plist_value> parse_object(const parse_context& ctx, size_t index,
                                                 size_t depth) {
    if (depth > 64 || index >= ctx.offsets.size()) {
        return nullptr;
    }
    size_t offset = ctx.offsets[index];
    if (offset >= ctx.data.size()) {
        return nullptr;
    }
    uint8_t marker = static_cast<uint8_t>(ctx.data[offset]);
    uint8_t kind = marker & 0xF0;
    if (marker == 0x08 || marker == 0x09) {
        return plist_value::make_uint(marker == 0x09 ? 1 : 0);
    }
    if (kind == 0x10) {
        auto parsed = parse_uint(ctx.data, offset);
        if (!parsed) {
            return nullptr;
        }
        return plist_value::make_uint(*parsed);
    }
    if (kind == 0x50 || kind == 0x60) {
        auto count = parse_count(ctx.data, offset, marker);
        if (!count) {
            return nullptr;
        }
        auto [cursor, chars] = *count;
        if (chars > ctx.data.size()) {
            return nullptr;
        }
        std::string value;
        if (kind == 0x50) {
            if (cursor + chars > ctx.data.size()) {
                return nullptr;
            }
            value.reserve(static_cast<size_t>(chars));
            for (size_t i = 0; i < chars; ++i) {
                value.push_back(static_cast<char>(ctx.data[cursor + i]));
            }
        } else {
            if (chars > (ctx.data.size() - cursor) / 2) {
                return nullptr;
            }
            value.reserve(static_cast<size_t>(chars));
            for (size_t i = 0; i < chars; ++i) {
                auto high = static_cast<uint8_t>(ctx.data[cursor + i * 2]);
                auto low = static_cast<uint8_t>(ctx.data[cursor + i * 2 + 1]);
                value.push_back(high == 0 ? static_cast<char>(low) : '?');
            }
        }
        return plist_value::make_string(std::move(value));
    }
    if (kind == 0xA0) {
        auto count = parse_count(ctx.data, offset, marker);
        if (!count) {
            return nullptr;
        }
        auto [cursor, items] = *count;
        if (ctx.ref_size == 0 || items > (ctx.data.size() - cursor) / ctx.ref_size) {
            return nullptr;
        }
        auto array = plist_value::make_array();
        for (size_t i = 0; i < items; ++i) {
            auto ref = read_be(ctx.data, cursor + i * ctx.ref_size, ctx.ref_size);
            if (!ref) {
                return nullptr;
            }
            auto child = parse_object(ctx, static_cast<size_t>(*ref), depth + 1);
            if (child) {
                array->array_val.push_back(std::move(child));
            }
        }
        return array;
    }
    if (kind == 0xD0) {
        auto count = parse_count(ctx.data, offset, marker);
        if (!count) {
            return nullptr;
        }
        auto [cursor, items] = *count;
        if (ctx.ref_size == 0 || items > (ctx.data.size() - cursor) / (ctx.ref_size * 2)) {
            return nullptr;
        }
        auto dict = plist_value::make_dict();
        size_t values_offset = cursor + static_cast<size_t>(items) * ctx.ref_size;
        for (size_t i = 0; i < items; ++i) {
            auto key_ref = read_be(ctx.data, cursor + i * ctx.ref_size, ctx.ref_size);
            auto value_ref = read_be(ctx.data, values_offset + i * ctx.ref_size, ctx.ref_size);
            if (!key_ref || !value_ref) {
                return nullptr;
            }
            auto key = parse_object(ctx, static_cast<size_t>(*key_ref), depth + 1);
            auto value = parse_object(ctx, static_cast<size_t>(*value_ref), depth + 1);
            if (key && value && key->type == plist_value::type::string) {
                dict->dict_val.emplace_back(key->str_val, std::move(value));
            }
        }
        return dict;
    }
    return nullptr;
}
static std::shared_ptr<plist_value> decode(std::span<const std::byte> data) {
    constexpr size_t trailer_size = 32;
    if (data.size() < 8 + trailer_size) {
        return nullptr;
    }
    const char* hdr = "bplist00";
    for (size_t i = 0; i < 8; ++i) {
        if (static_cast<char>(data[i]) != hdr[i]) {
            return nullptr;
        }
    }
    size_t trailer = data.size() - trailer_size;
    auto offset_size = static_cast<size_t>(static_cast<uint8_t>(data[trailer + 6]));
    auto ref_size = static_cast<size_t>(static_cast<uint8_t>(data[trailer + 7]));
    auto num_objects = read_be(data, trailer + 8, 8);
    auto top_object = read_be(data, trailer + 16, 8);
    auto offset_table = read_be(data, trailer + 24, 8);
    if (!num_objects || !top_object || !offset_table || offset_size == 0 || ref_size == 0 ||
        *num_objects > data.size() || *top_object >= *num_objects || *offset_table > data.size() ||
        *num_objects > (data.size() - static_cast<size_t>(*offset_table)) / offset_size) {
        return nullptr;
    }
    parse_context ctx{};
    ctx.data = data;
    ctx.ref_size = ref_size;
    ctx.offsets.reserve(static_cast<size_t>(*num_objects));
    for (size_t i = 0; i < *num_objects; ++i) {
        auto offset =
            read_be(data, static_cast<size_t>(*offset_table) + i * offset_size, offset_size);
        if (!offset || *offset >= data.size()) {
            return nullptr;
        }
        ctx.offsets.push_back(static_cast<size_t>(*offset));
    }
    return parse_object(ctx, static_cast<size_t>(*top_object), 0);
}
static std::shared_ptr<plist_value> find_value(const std::shared_ptr<plist_value>& root,
                                               std::string_view key) {
    if (!root) {
        return nullptr;
    }
    if (root->type == plist_value::type::dict) {
        for (const auto& [dict_key, value] : root->dict_val) {
            if (dict_key == key) {
                return value;
            }
        }
        for (const auto& [_, value] : root->dict_val) {
            if (auto found = find_value(value, key)) {
                return found;
            }
        }
    } else if (root->type == plist_value::type::array) {
        for (const auto& value : root->array_val) {
            if (auto found = find_value(value, key)) {
                return found;
            }
        }
    }
    return nullptr;
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
    if (auto root = decode(data)) {
        if (auto value = find_value(root, key); value && value->type == plist_value::type::uint) {
            return value->uint_val;
        }
    }
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
static std::optional<std::string_view> header_value(const rtsp_request& req,
                                                    std::string_view name) {
    for (const auto& [key, value] : req.headers) {
        if (key.size() != name.size()) {
            continue;
        }
        bool match = true;
        for (size_t i = 0; i < key.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(key[i])) !=
                std::tolower(static_cast<unsigned char>(name[i]))) {
                match = false;
                break;
            }
        }
        if (match) {
            return value;
        }
    }
    return std::nullopt;
}
static std::optional<uint16_t> parse_transport_port(std::string_view transport,
                                                    std::string_view key) {
    auto pos = transport.find(key);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos += key.size();
    if (pos >= transport.size() || transport[pos] != '=') {
        return std::nullopt;
    }
    ++pos;
    uint32_t value = 0;
    bool any = false;
    while (pos < transport.size() && std::isdigit(static_cast<unsigned char>(transport[pos]))) {
        any = true;
        value = value * 10 + static_cast<uint32_t>(transport[pos] - '0');
        if (value > 65535) {
            return std::nullopt;
        }
        ++pos;
    }
    if (!any) {
        return std::nullopt;
    }
    return static_cast<uint16_t>(value);
}
static std::vector<int> parse_sdp_ints(std::string_view text) {
    std::vector<int> values;
    std::istringstream stream(std::string{text});
    int value = 0;
    while (stream >> value) {
        values.push_back(value);
    }
    return values;
}
rtsp_session::rtsp_session(io::tcp_stream socket, crypto::fairplay_pairing pairing,
                           receiver_source_descriptor source)
    : socket_(std::move(socket)),
      pairing_(std::move(pairing)),
      media_sink_(media::make_local_media_sink()),
      source_(source),
      airplay_media_(*media_sink_) {}
auto rtsp_session::create(io::tcp_stream socket, crypto::fairplay_pairing pairing,
                          receiver_source_descriptor source) -> std::shared_ptr<rtsp_session> {
    return std::shared_ptr<rtsp_session>(
        new rtsp_session(std::move(socket), std::move(pairing), source));
}
void rtsp_session::reset_audio_packet_state() {
    audio_resend_control_seqnum_ = 0;
    airplay_media_.reset_audio_packets();
}
void rtsp_session::close_audio_stream() {
    if (audio_data_socket_) {
        audio_data_socket_->close();
    }
    if (audio_control_socket_) {
        audio_control_socket_->close();
    }
    media_sink_->on_audio_teardown();
    audio_receiver_started_ = false;
}
void rtsp_session::close_video_stream() {
    if (mirror_acceptor_) {
        mirror_acceptor_->close();
    }
    airplay_media_.stop_video();
    mirror_receiver_started_ = false;
}
void rtsp_session::close_stream_sockets() {
    if (timing_socket_) {
        timing_socket_->close();
    }
    if (control_socket_) {
        control_socket_->close();
    }
    close_video_stream();
    close_audio_stream();
    base_receivers_started_ = false;
}
void rtsp_session::stop() {
    state_ = rtsp_session_state::teardown;
    close_stream_sockets();
    if (socket_.is_open()) {
        socket_.close();
    }
}
io::task<result<void>> rtsp_session::run() {
    mirage::log::info("New RTSP session from {}", socket_.remote_endpoint().addr.to_string());
    while (state_ != rtsp_session_state::teardown) {
        auto request = co_await read_request();
        if (!request) {
            if (state_ == rtsp_session_state::teardown) {
                close_stream_sockets();
                co_return result<void>{};
            }
            mirage::log::warn("Failed to read RTSP request: {}", request.error().message);
            state_ = rtsp_session_state::teardown;
            close_stream_sockets();
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
            state_ = rtsp_session_state::teardown;
            close_stream_sockets();
            co_return std::unexpected(send_result.error());
        }
    }
    close_stream_sockets();
    io::steady_timer timer(socket_.context());
    timer.expires_after(std::chrono::milliseconds(100));
    co_await timer.async_wait();
    co_return result<void>{};
}
io::task<result<rtsp_request>> rtsp_session::read_request() {
    rtsp_request req;
    try {
        auto header_str = co_await socket_.async_read_until("\r\n\r\n");
        auto parsed_head = parse_rtsp_request_head(header_str);
        if (!parsed_head) {
            co_return std::unexpected(parsed_head.error());
        }

        size_t content_length = parsed_head->content_length;
        if (parsed_head->cseq) {
            cseq_ = *parsed_head->cseq;
        }
        req.method = std::move(parsed_head->method);
        req.uri = std::move(parsed_head->uri);
        req.version = std::move(parsed_head->version);
        req.headers = std::move(parsed_head->headers);

        mirage::log::debug("Request: {} {} {}", req.method, req.uri, req.version);

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
    auto action = airplay::classify_rtsp_action(req.method, req.uri);
    if (!airplay::rtsp_action_allowed(state_, action)) {
        mirage::log::warn("Rejecting RTSP {} while session is {}",
                          airplay::rtsp_action_name(action), airplay::rtsp_state_name(state_));
        co_return rtsp_response{
            .status_code = 455,
            .status_text = "Method Not Valid in This State",
            .headers = {{"CSeq", std::to_string(cseq_)}},
            .body = {},
        };
    }

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
    if (req.method == "ANNOUNCE") {
        co_return handle_announce(req);
    }
    if (req.method == "SETUP") {
        co_return co_await handle_setup(req);
    }
    if (req.method == "RECORD") {
        co_return handle_record(req);
    }
    if (req.method == "PAUSE") {
        co_return handle_pause(req);
    }
    if (req.method == "FLUSH") {
        co_return handle_flush(req);
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
    mirage::log::debug("Including Ed25519 public key in /info: {} bytes", pk.size());
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
	    <integer>)" + std::to_string(airplay::feature_low) +
                        R"(</integer>
	    <key>initialVolume</key>
	    <real>)" + std::format("{:.1f}", airplay::initial_volume_db) +
                        R"(</real>
    <key>keepAliveLowPower</key>
    <integer>1</integer>
    <key>keepAliveSendStatsAsBody</key>
    <true/>
    <key>macAddress</key>
    <string>DC:46:28:54:D9:0E</string>
	    <key>model</key>
	    <string>)" + std::string(airplay::compatibility_model) +
                        R"(</string>
    <key>name</key>
    <string>mirage</string>
    <key>pi</key>
    <string>b08f5a79-db29-4384-b456-a4784d9e6055</string>
    <key>pk</key>
    <data>)" + pk_b64 + R"(</data>
	    <key>sourceVersion</key>
	    <string>)" + std::string(airplay::info_source_version) +
                        R"(</string>
	    <key>statusFlags</key>
	    <integer>)" + std::to_string(airplay::status_flags) +
                        R"(</integer>
	    <key>vv</key>
	    <integer>)" + std::to_string(airplay::protocol_version) +
                        R"(</integer>
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
        state_ = rtsp_session_state::announced;
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
result<rtsp_response> rtsp_session::handle_announce(const rtsp_request& req) {
    state_ = rtsp_session_state::announced;
    std::string sdp(reinterpret_cast<const char*>(req.body.data()), req.body.size());
    if (sdp.find("AppleLossless") != std::string::npos || sdp.find("alac") != std::string::npos) {
        audio_ct_ = 2;
        auto codec_pos = sdp.find("AppleLossless/");
        if (codec_pos != std::string::npos) {
            codec_pos += std::string_view("AppleLossless/").size();
            try {
                audio_sample_rate_ = std::stoi(sdp.substr(codec_pos));
            } catch (const std::exception&) {
            }
        }
        auto fmtp_pos = sdp.find("a=fmtp:");
        if (fmtp_pos != std::string::npos) {
            auto line_end = sdp.find('\n', fmtp_pos);
            auto line = std::string_view{sdp}.substr(fmtp_pos, line_end == std::string::npos
                                                                   ? std::string_view::npos
                                                                   : line_end - fmtp_pos);
            auto values_start = line.find(' ');
            if (values_start != std::string_view::npos) {
                auto values = parse_sdp_ints(line.substr(values_start + 1));
                if (values.size() >= 11) {
                    audio_spf_ = values[0];
                    audio_channels_ = values[6];
                    audio_sample_rate_ = values[10];
                }
            }
        }
        mirage::log::info("RAOP ANNOUNCE: ALAC audio-only stream (sr={}, ch={}, spf={})",
                          audio_sample_rate_, audio_channels_, audio_spf_);
    } else if (sdp.find("mpeg4-generic") != std::string::npos ||
               sdp.find("AAC") != std::string::npos) {
        audio_ct_ = 8;
        auto codec_pos = sdp.find("mpeg4-generic/");
        if (codec_pos != std::string::npos) {
            codec_pos += std::string_view("mpeg4-generic/").size();
            try {
                audio_sample_rate_ = std::stoi(sdp.substr(codec_pos));
            } catch (const std::exception&) {
            }
            auto channels_pos = sdp.find('/', codec_pos);
            if (channels_pos != std::string::npos) {
                ++channels_pos;
                try {
                    audio_channels_ = std::stoi(sdp.substr(channels_pos));
                } catch (const std::exception&) {
                }
            }
        }
        mirage::log::info("RAOP ANNOUNCE: AAC audio-only stream (sr={}, ch={}, spf={})",
                          audio_sample_rate_, audio_channels_, audio_spf_);
    } else {
        mirage::log::info("RAOP ANNOUNCE: audio-only stream");
    }

    return rtsp_response{
        .status_code = 200,
        .status_text = "OK",
        .headers = {{"CSeq", std::to_string(cseq_)}},
        .body = {},
    };
}
bool rtsp_session::configure_audio_decoder() {
    auto configured = airplay_media_.configure_audio({
        .codec_tag = audio_ct_,
        .sample_rate = audio_sample_rate_,
        .channels = audio_channels_,
        .frames_per_packet = audio_spf_,
    });
    if (!configured) {
        mirage::log::error("Audio setup failed: {}", configured.error().message);
        return false;
    }
    media_sink_->on_audio_volume(audio_volume_db_, audio_linear_volume_);
    return true;
}
io::task<result<rtsp_response>> rtsp_session::handle_setup(const rtsp_request& req) {
    mirage::log::debug("SETUP request for {}", req.uri);
    mirage::log::debug("SETUP body: {} bytes", req.body.size());
    state_ = rtsp_session_state::ready;
    bool is_streams_setup = bplist::has_streams(req.body);
    auto transport = header_value(req, "Transport");
    bool is_raop_transport_setup =
        !is_streams_setup && transport && transport->find("RTP/AVP/UDP") != std::string_view::npos;
    if (is_raop_transport_setup) {
        if (auto control_port = parse_transport_port(*transport, "control_port")) {
            audio_control_remote_ = io::endpoint{socket_.remote_endpoint().addr, *control_port};
            mirage::log::debug("RAOP client control port: {}", *control_port);
        }
        if (auto timing_port = parse_transport_port(*transport, "timing_port")) {
            client_timing_endpoint_ = io::endpoint{socket_.remote_endpoint().addr, *timing_port};
            mirage::log::debug("RAOP client timing port: {}", *timing_port);
        }
    } else if (!is_streams_setup) {
        auto client_timing_port = bplist::get_uint(req.body, "timingPort");
        if (!client_timing_port) {
            client_timing_port = extract_bplist_uint(req.body, "timingPort");
        }
        if (client_timing_port) {
            mirage::log::info("Client timing port: {}", *client_timing_port);
            client_timing_endpoint_ = io::endpoint{socket_.remote_endpoint().addr,
                                                   static_cast<uint16_t>(*client_timing_port)};
        } else {
            mirage::log::warn("Could not extract client timing port from general SETUP request");
        }
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
        if (client_timing_endpoint_.port != 0) {
            io::co_spawn(socket_.context(),
                         [self]() -> io::task<void> { co_await self->run_ntp_timing_sender(); });
        }
    }
    if (is_raop_transport_setup) {
        reset_audio_packet_state();
        if (audio_ct_ == 0) {
            audio_ct_ = 2;
        }
        if (!configure_audio_decoder()) {
            mirage::log::error("Failed to create audio decoder for RAOP SETUP");
        }
        if (!audio_receiver_started_ && audio_data_socket_) {
            io::co_spawn(socket_.context(),
                         [self]() -> io::task<void> { co_await self->run_audio_receiver(); });
            io::co_spawn(socket_.context(), [self]() -> io::task<void> {
                co_await self->run_audio_control_receiver();
            });
            audio_receiver_started_ = true;
        }
        mirage::log::info("RAOP SETUP: audio data={}, control={}, timing={}, ct={}, sr={}, ch={}",
                          audio_port, audio_control_port, timing_port, audio_ct_,
                          audio_sample_rate_, audio_channels_);
        log_receiver_audio_setup(source_,
                                 {
                                     .codec = airplay::audio_codec_label(audio_ct_),
                                     .sample_rate = audio_sample_rate_,
                                     .channels = audio_channels_,
                                     .frames_per_packet = audio_spf_,
                                     .data_port = audio_port,
                                     .control_port = audio_control_port,
                                     .timing_port = timing_port,
                                 },
                                 "RAOP audio setup");
        co_return rtsp_response{
            .status_code = 200,
            .status_text = "OK",
            .headers =
                {
                    {"CSeq", std::to_string(cseq_)},
                    {"Transport", std::format("RTP/AVP/UDP;unicast;mode=record;server_port={};"
                                              "control_port={};timing_port={}",
                                              audio_port, audio_control_port, timing_port)},
                    {"Session", "1"},
                    {"Audio-Latency",
                     std::to_string(airplay::audio_latency_samples(audio_sample_rate_))},
                    {"Audio-Jack-Status", "connected; type=analog"},
                },
            .body = {},
        };
    }
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
            auto stream_id = bplist::get_uint(req.body, "streamConnectionID");
            if (!stream_id) {
                stream_id = bplist::find_8byte_int(req.body);
            }
            if (stream_id) {
                stream_connection_id_ = *stream_id;
                mirage::log::info("streamConnectionID: {}", stream_connection_id_);
            }
        }
        auto res_streams = bplist::plist_value::make_array();
        if (type == 110) {
            mirage::log::debug("SETUP type 110: video mirroring (TCP), dataPort={}", mirror_port);
            if (!mirror_receiver_started_ && mirror_acceptor_) {
                io::co_spawn(socket_.context(),
                             [self]() -> io::task<void> { co_await self->run_mirror_receiver(); });
                mirror_receiver_started_ = true;
            }
            auto res_stream = bplist::plist_value::make_dict();
            res_stream->dict_val.emplace_back("dataPort",
                                              bplist::plist_value::make_uint(mirror_port));
            res_stream->dict_val.emplace_back("type", bplist::plist_value::make_uint(110));
            res_streams->array_val.push_back(res_stream);
        } else if (type == 96) {
            reset_audio_packet_state();
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
            auto audio_format = bplist::get_uint(req.body, "audioFormat");
            if (audio_ct_ == 2 && audio_spf_ == 480) {
                mirage::log::warn("Audio SETUP requested ct=2 with AAC-ELD spf=480; using ct=8");
                audio_ct_ = 8;
            }
            if (audio_format) {
                mirage::log::info(
                    "SETUP type 96: audio data={}, ctrl={}, ct={}, spf={}, sr={}, "
                    "audioFormat=0x{:x}",
                    audio_port, audio_control_port, audio_ct_, audio_spf_, audio_sample_rate_,
                    *audio_format);
            } else {
                mirage::log::info("SETUP type 96: audio data={}, ctrl={}, ct={}, spf={}, sr={}",
                                  audio_port, audio_control_port, audio_ct_, audio_spf_,
                                  audio_sample_rate_);
            }
            log_receiver_audio_setup(source_, {
                                                  .codec = airplay::audio_codec_label(audio_ct_),
                                                  .sample_rate = audio_sample_rate_,
                                                  .channels = audio_channels_,
                                                  .frames_per_packet = audio_spf_,
                                                  .data_port = audio_port,
                                                  .control_port = audio_control_port,
                                                  .timing_port = std::nullopt,
                                              });
            if (!configure_audio_decoder()) {
                mirage::log::error("Failed to create audio decoder for SETUP type 96");
            }
            if (!audio_receiver_started_ && audio_data_socket_) {
                io::co_spawn(socket_.context(),
                             [self]() -> io::task<void> { co_await self->run_audio_receiver(); });
                io::co_spawn(socket_.context(), [self]() -> io::task<void> {
                    co_await self->run_audio_control_receiver();
                });
                audio_receiver_started_ = true;
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
                std::array<std::byte, 16> audio_iv{};
                std::copy_n(req.body.begin() + static_cast<std::ptrdiff_t>(i + 3), 16,
                            audio_iv.begin());
                airplay_media_.set_audio_iv(audio_iv);
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
        if (!airplay_media_.audio_keys_ready() && fp_keymsg_.size() == 164 &&
            fp_ekey_.size() == 72) {
            auto derived = airplay_media_.derive_audio_key(fp_keymsg_, fp_ekey_,
                                                           pairing_.transient_shared_secret());
            if (!derived) {
                mirage::log::warn("Audio key derivation failed: {}", derived.error().message);
            }
        }
        res_root->dict_val.emplace_back("eventPort", bplist::plist_value::make_uint(0));
        res_root->dict_val.emplace_back("timingPort", bplist::plist_value::make_uint(timing_port));
    }
    body = bplist::encode(res_root);
    mirage::log::debug("SETUP response binary plist: {} bytes", body.size());
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
            mirage::log::trace("UDP {}: received {} bytes from {}:{}", name, n,
                               sender.addr.to_string(), sender.port);
            if (is_timing && n >= 32) {
                uint8_t header0 = static_cast<uint8_t>(buffer[0]);
                uint8_t header1 = static_cast<uint8_t>(buffer[1]);
                mirage::log::trace("Timing packet type: 0x{:02x} 0x{:02x}", header0, header1);
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
                    mirage::log::trace("Timing response sent to {}:{}", sender.addr.to_string(),
                                       sender.port);
                }
            }
        }
    } catch (const std::system_error& e) {
        if (state_ != rtsp_session_state::teardown && e.code() != std::errc::operation_canceled) {
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
            mirage::log::trace("NTP timing: sent {} bytes (seq={})", sent, seq);
            seq++;
            timer.expires_after(std::chrono::milliseconds(500));
            co_await timer.async_wait();
        }
    } catch (const std::system_error& e) {
        if (state_ != rtsp_session_state::teardown && e.code() != std::errc::operation_canceled) {
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
            media::video_stream_config sink_config;
            sink_config.codec = video_codec::h264;
            sink_config.width = 1280;
            sink_config.height = 720;
            sink_config.prefer_hardware = true;
            sink_config.title = "Mirage - AirPlay";

            airplay::video_source_config source_config;
            source_config.sink_config = std::move(sink_config);
            source_config.stream_connection_id = stream_connection_id_;
            source_config.keymsg = fp_keymsg_;
            source_config.ekey = fp_ekey_;
            source_config.shared_secret = pairing_.transient_shared_secret();

            auto video_setup = airplay_media_.start_video(source_config);
            if (!video_setup) {
                mirage::log::warn("Video setup failed: {}", video_setup.error().message);
            }
            std::array<std::byte, 128> header;
            while (mirror_socket.is_open() && state_ != rtsp_session_state::teardown &&
                   airplay_media_.video_open()) {
                try {
                    co_await mirror_socket.async_read_exactly(std::span<std::byte>(header));
                } catch (const std::system_error& e) {
                    if (state_ != rtsp_session_state::teardown &&
                        e.code() != std::errc::connection_reset &&
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
                std::vector<std::byte> payload(payload_size);
                try {
                    co_await mirror_socket.async_read_exactly(
                        std::span<std::byte>(payload.data(), payload_size));
                } catch (const std::system_error& e) {
                    if (state_ != rtsp_session_state::teardown &&
                        e.code() != std::errc::connection_reset &&
                        e.code() != std::errc::operation_canceled) {
                        mirage::log::debug("Mirror payload read error: {}", e.what());
                    }
                    break;
                } catch (...) {
                    break;
                }
                auto handled = airplay_media_.receive_mirror_payload(
                    payload_type, payload_flag, option0,
                    std::span<const std::byte>(payload.data(), payload.size()));
                if (!handled && payload_type == 0x00) {
                    mirage::log::debug("Mirror frame processing failed: {}",
                                       handled.error().message);
                }
            }
            auto stats = airplay_media_.video_stats();
            log_receiver_video_summary(source_, {
                                                    .frames = stats.frames,
                                                    .keyframes = stats.keyframes,
                                                    .decrypted_frames = stats.decrypted_frames,
                                                    .decrypt_failures = stats.decrypt_failures,
                                                    .decode_failures = stats.decode_failures,
                                                });
            if (!airplay_media_.video_open()) {
                mirage::log::info("Window closed, tearing down session");
                state_ = rtsp_session_state::teardown;
                socket_.close();
            }
            airplay_media_.stop_video();
        }
    } catch (const std::system_error& e) {
        if (mirror_receiver_started_ && state_ != rtsp_session_state::teardown &&
            e.code() != std::errc::operation_canceled) {
            mirage::log::debug("Mirror receiver ended: {}", e.what());
        }
    }
    mirror_receiver_started_ = false;
    mirage::log::debug("Mirror receiver stopped");
}
io::task<void> rtsp_session::send_audio_resend_request(uint16_t start_seqnum, uint16_t count) {
    if (count == 0 || !audio_control_socket_ || !audio_control_socket_->is_open() ||
        audio_control_remote_.port == 0) {
        co_return;
    }

    count = std::min<uint16_t>(count, airplay::max_audio_resend_packets);
    std::array<std::byte, 8> resend_req{};
    resend_req[0] = std::byte{0x80};
    resend_req[1] = std::byte{0xD5};
    resend_req[2] = std::byte{static_cast<uint8_t>((audio_resend_control_seqnum_ >> 8) & 0xFF)};
    resend_req[3] = std::byte{static_cast<uint8_t>(audio_resend_control_seqnum_ & 0xFF)};
    resend_req[4] = std::byte{static_cast<uint8_t>((start_seqnum >> 8) & 0xFF)};
    resend_req[5] = std::byte{static_cast<uint8_t>(start_seqnum & 0xFF)};
    resend_req[6] = std::byte{static_cast<uint8_t>((count >> 8) & 0xFF)};
    resend_req[7] = std::byte{static_cast<uint8_t>(count & 0xFF)};
    ++audio_resend_control_seqnum_;

    try {
        co_await audio_control_socket_->async_send_to(std::span<const std::byte>(resend_req),
                                                      audio_control_remote_);
        mirage::log::debug("Sent audio resend request: seq_start={}, count={}", start_seqnum,
                           count);
    } catch (const std::system_error& e) {
        if (state_ != rtsp_session_state::teardown) {
            mirage::log::debug("Audio resend request failed: {}", e.what());
        }
    }
}
io::task<void> rtsp_session::run_audio_receiver() {
    if (!audio_data_socket_) {
        co_return;
    }
    mirage::log::info("Audio receiver started on UDP port {}",
                      audio_data_socket_->local_endpoint().port);

    std::array<std::byte, 2048> buf;

    try {
        while (audio_data_socket_->is_open() && state_ != rtsp_session_state::teardown) {
            io::endpoint sender;
            auto n =
                co_await audio_data_socket_->async_recv_from(std::span<std::byte>(buf), sender);

            if (n <= 12) {
                continue;
            }

            auto received =
                airplay_media_.receive_audio_rtp(std::span<const std::byte>(buf.data(), n), false);
            if (received.resend) {
                co_await send_audio_resend_request(received.resend->start_seqnum,
                                                   received.resend->count);
            }
        }
    } catch (const std::system_error& e) {
        if (audio_receiver_started_ && state_ != rtsp_session_state::teardown &&
            e.code() != std::errc::operation_canceled) {
            mirage::log::debug("Audio receiver ended: {}", e.what());
        }
    }

    media_sink_->on_audio_teardown();
    auto stats = airplay_media_.audio_stats();
    log_receiver_audio_summary(source_, {
                                            .received_packets = stats.received_packets,
                                            .decoded_packets = stats.decoded_packets,
                                            .silent_or_marker = stats.silent_or_marker,
                                            .gaps = stats.gaps,
                                            .resend_requests = stats.resend_requests,
                                            .stale_or_redundant = stats.stale_or_redundant,
                                            .duplicates = stats.duplicates,
                                            .invalid = stats.invalid,
                                            .pending = stats.pending,
                                        });
    audio_receiver_started_ = false;
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
                auto seqnum = static_cast<uint16_t>(
                    (static_cast<uint16_t>(static_cast<uint8_t>(rtp_data[2])) << 8) |
                    static_cast<uint16_t>(static_cast<uint8_t>(rtp_data[3])));
                mirage::log::trace("Audio control: retransmitted packet seq={} ({} bytes)", seqnum,
                                   rtp_data.size());
                (void)airplay_media_.receive_audio_rtp(rtp_data, true);
            } else if (ptype == 0x54) {
                mirage::log::trace("Audio control: sync packet ({} bytes)", n);
            } else {
                mirage::log::trace("Audio control: type=0x{:02x}, {} bytes from {}:{}", ptype, n,
                                   sender.addr.to_string(), sender.port);
            }
        }
    } catch (const std::system_error& e) {
        if (audio_receiver_started_ && state_ != rtsp_session_state::teardown &&
            e.code() != std::errc::operation_canceled) {
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
                {"Audio-Latency",
                 std::to_string(airplay::audio_latency_samples(audio_sample_rate_))},
                {"Audio-Jack-Status", "connected; type=analog"},
            },
        .body = {},
    };
}
result<rtsp_response> rtsp_session::handle_pause(const rtsp_request& /* req */) {
    mirage::log::debug("PAUSE request");
    state_ = rtsp_session_state::paused;
    media_sink_->on_audio_pause();
    return rtsp_response{
        .status_code = 200,
        .status_text = "OK",
        .headers = {{"CSeq", std::to_string(cseq_)}},
        .body = {},
    };
}
result<rtsp_response> rtsp_session::handle_flush(const rtsp_request& /* req */) {
    mirage::log::debug("FLUSH request");
    reset_audio_packet_state();
    media_sink_->on_audio_flush();
    return rtsp_response{
        .status_code = 200,
        .status_text = "OK",
        .headers = {{"CSeq", std::to_string(cseq_)}},
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
        close_audio_stream();
        mirage::log::info("Audio stream torn down");
    }
    if (teardown_video) {
        close_video_stream();
        mirage::log::info("Video stream torn down");
    }
    if (!has_streams) {
        mirage::log::user("{} disconnected", socket_.remote_endpoint().addr.to_string());
        state_ = rtsp_session_state::teardown;
        close_stream_sockets();
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
            std::string volume_response = std::format("volume: {:.6f}\r\n", audio_volume_db_);
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
result<rtsp_response> rtsp_session::handle_set_parameter(const rtsp_request& req) {
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
                try {
                    audio_volume_db_ = std::stof(body_str.substr(pos + 7));
                    audio_linear_volume_ = airplay::db_to_linear(audio_volume_db_);
                    media_sink_->on_audio_volume(audio_volume_db_, audio_linear_volume_);
                } catch (const std::exception& e) {
                    mirage::log::warn("Invalid volume parameter: {}", e.what());
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
struct rtsp_server::session_store {
    struct entry {
        uint64_t id = 0;
        std::shared_ptr<rtsp_session> session;
    };

    uint64_t add(std::shared_ptr<rtsp_session> session) {
        uint64_t id = next_id++;
        active.push_back(entry{.id = id, .session = std::move(session)});
        return id;
    }

    void remove(uint64_t id) {
        active.erase(std::remove_if(active.begin(), active.end(),
                                    [id](const entry& item) { return item.id == id; }),
                     active.end());
    }

    void stop_all() {
        stopping = true;
        for (auto& item : active) {
            if (item.session) {
                item.session->stop();
            }
        }
    }

    std::vector<entry> active;
    uint64_t next_id = 1;
    bool stopping = false;
};
rtsp_server::rtsp_server(io::tcp_acceptor acceptor, receiver_source_descriptor source,
                         crypto::ed25519_keypair keypair)
    : acceptor_(std::move(acceptor)),
      source_(source),
      keypair_(std::move(keypair)),
      sessions_(std::make_shared<session_store>()) {}
auto rtsp_server::bind(io::io_context& ctx, receiver_source_descriptor source,
                       crypto::ed25519_keypair keypair) -> result<rtsp_server> {
    try {
        auto acceptor = io::tcp_acceptor::bind(ctx, source.port);
        mirage::log::info("RTSP server bound to port {}", source.port);
        return rtsp_server{std::move(acceptor), source, std::move(keypair)};
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
            auto session = rtsp_session::create(std::move(socket), std::move(pairing), source_);
            auto sessions = sessions_;
            auto session_id = sessions->add(session);
            io::co_spawn(acceptor_.context(), [session, sessions, session_id]() -> io::task<void> {
                auto result = co_await session->run();
                sessions->remove(session_id);
                if (!result && !sessions->stopping &&
                    session->state() != rtsp_session_state::teardown) {
                    mirage::log::warn("Session ended with error: {}", result.error().message);
                }
            });
        } catch (const std::system_error& e) {
            if (running_ && e.code() != std::errc::operation_canceled) {
                mirage::log::warn("Accept error: {}", e.what());
            }
        }
    }
}
void rtsp_server::stop() {
    running_ = false;
    if (sessions_) {
        sessions_->stop_all();
    }
    acceptor_.close();
}
}  // namespace mirage::protocols
