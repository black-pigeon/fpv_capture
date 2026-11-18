// Host-only UHD transport for the DDM-P1 CS16 waveform replay method.
// E100 IQTAXI register/DMA APIs are intentionally not used by this USB device.
#include "replay.hpp"
#include "tx_events.hpp"
#include "live_control.hpp"
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/types/tune_request.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <openssl/evp.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void stop_handler(int) { interrupted = 1; }
using Sample = std::complex<int16_t>;
static_assert(sizeof(Sample) == 4, "CS16 requires 4 bytes per complex sample");
struct Config {
    std::string input, metadata, args = "type=b200,serial=U220200";
    double rate = 0, dwell = 10, gain = 0, bandwidth = 16e6, settle = .2;
    std::optional<double> attenuation;
    std::string send_frames, standard, alternate_input, alternate_metadata;
    bool live_control = false;
    size_t channel = 0;
    bool transmit = false;
    std::vector<double> frequencies{5740e6, 5800e6, 5860e6};
};
double number(const std::string& text)
{
    size_t used = 0;
    double result = std::stod(text, &used);
    if (used != text.size() || !std::isfinite(result)) throw std::runtime_error("invalid number: " + text);
    return result;
}
void usage()
{
    std::cout << "U220 FPV CS16 replay (offline by default)\n"
        "  --input FILE              raw little-endian int16 I,Q (required)\n"
        "  --metadata FILE           generator signal.json; verifies SHA256, size and rate\n"
        "  --sample-rate HZ          required without metadata; must match IQ rate\n"
        "  --freqs HZ,HZ,...          sequential RF centers, default 5740e6,5800e6,5860e6\n"
        "  --dwell SEC               continuous TX per center, default 10 (0..3600 exclusive zero)\n"
        "  --settle SEC              minimum LO settling before TX, default .2 (0..10)\n"
        "  --gain DB                 UHD TX gain, default 0 (exclusive with --attenuation)\n"
        "  --attenuation DB          TX attenuation 0..89.75 dB, step .25; larger is weaker\n"
        "  --bandwidth HZ            analog TX filter, default 16e6\n"
        "  --channel N               UHD TX index 0 or 1, default 0; check physical port\n"
        "  --args STRING             default type=b200,serial=U220200\n"
        "  --dry-run                 validate/print only, never creates a UHD device (default)\n"
        "  --live-control            accept JSON runtime updates on stdin (requires both standards)\n"
        "  --alternate-input FILE    opposite-standard IQ, preloaded before TX\n"
        "  --alternate-metadata FILE opposite-standard signal.json\n"
        "  --transmit                enable bounded RF transmission\n";
}
Config parse(int argc, char** argv)
{
    Config c;
    bool saw_dry = false, saw_gain = false;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--live-control") { c.live_control = true; continue; }
        if (key == "--transmit") { c.transmit = true; continue; }
        if (key == "--dry-run") { saw_dry = true; continue; }
        if (i + 1 == argc) throw std::runtime_error("missing value for " + key);
        const std::string value = argv[++i];
        if (key == "--input") c.input = value;
        else if (key == "--alternate-input") c.alternate_input = value;
        else if (key == "--alternate-metadata") c.alternate_metadata = value;
        else if (key == "--metadata") c.metadata = value;
        else if (key == "--args") c.args = value;
        else if (key == "--sample-rate") c.rate = number(value);
        else if (key == "--dwell") c.dwell = number(value);
        else if (key == "--settle") c.settle = number(value);
        else if (key == "--gain") { c.gain = number(value); saw_gain = true; }
        else if (key == "--attenuation") c.attenuation = number(value);
        else if (key == "--bandwidth") c.bandwidth = number(value);
        else if (key == "--channel") {
            double channel = number(value);
            if (channel != 0 && channel != 1) throw std::runtime_error("channel must be 0 or 1");
            c.channel = static_cast<size_t>(channel);
        } else if (key == "--freqs") {
            c.frequencies.clear();
            std::stringstream list(value);
            std::string item;
            if (value.empty() || value.back() == ',') throw std::runtime_error("empty frequency");
            while (std::getline(list, item, ',')) c.frequencies.push_back(number(item));
        } else throw std::runtime_error("unknown option: " + key);
    }
    if (c.attenuation) {
        const double a = *c.attenuation;
        if (saw_gain) throw std::runtime_error("choose --gain or --attenuation");
        if (a < 0 || a > 89.75 || std::abs(a * 4 - std::round(a * 4)) > 1e-6)
            throw std::runtime_error("attenuation requires 0..89.75 dB in .25 dB steps");
    }
    if (c.live_control && (!c.attenuation || c.metadata.empty() || c.alternate_input.empty() || c.alternate_metadata.empty()))
        throw std::runtime_error("live control requires attenuation and metadata/IQ for both standards");
    if (saw_dry && c.transmit) throw std::runtime_error("choose --dry-run or --transmit");
    if (c.input.empty()) throw std::runtime_error("--input is required");
    if (c.dwell <= 0 || c.dwell > 3600 || c.settle < 0 || c.settle > 10 ||
        c.gain < 0 || c.gain > 89.8 || c.bandwidth < 200e3 || c.bandwidth > 56e6)
        throw std::runtime_error("dwell, settle, gain or bandwidth outside supported bounds");
    if (c.frequencies.empty()) throw std::runtime_error("empty frequency list");
    for (double f : c.frequencies)
        if (f < 50e6 || f > 6e9) throw std::runtime_error("frequency outside 50 MHz..6 GHz");
    // B2xx defaults to 16 x 8176-byte USB buffers: only about 1 ms at
    // 30.72 MSPS SC16. Keep enough queued transfers for desktop scheduling
    // jitter without changing waveform rate or suppressing underflow errors.
    uhd::device_addr_t device_args(c.args);
    if (!device_args.has_key("num_send_frames")) device_args["num_send_frames"] = "256";
    c.send_frames = device_args["num_send_frames"];
    c.args = device_args.to_string();
    return c;
}
std::string sha256(const std::vector<Sample>& data)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (!EVP_Digest(data.data(), data.size() * sizeof(Sample), digest, &length, EVP_sha256(), nullptr))
        throw std::runtime_error("SHA256 failed");
    std::ostringstream output;
    for (unsigned i = 0; i < length; ++i)
        output << std::hex << std::setfill('0') << std::setw(2) << unsigned(digest[i]);
    return output.str();
}
std::vector<Sample> load(Config& c)
{
    uint16_t endian = 1;
    if (*reinterpret_cast<char*>(&endian) != 1) throw std::runtime_error("little-endian host required");
    std::ifstream file(c.input, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("cannot open IQ input");
    const auto bytes = file.tellg();
    if (bytes <= 0 || bytes % 4 != 0 || bytes > 512 * 1024 * 1024)
        throw std::runtime_error("IQ must be nonempty, a multiple of 4 bytes and <=512 MiB");
    std::vector<Sample> iq(static_cast<size_t>(bytes) / 4);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(iq.data()), bytes)) throw std::runtime_error("short IQ read");
    if (!c.metadata.empty()) {
        boost::property_tree::ptree meta;
        boost::property_tree::read_json(c.metadata, meta);
        if (meta.get<std::string>("format") != "cs16_le" ||
            meta.get<std::string>("iq_file") != std::filesystem::path(c.input).filename() ||
            meta.get<uint64_t>("bytes") != static_cast<uint64_t>(bytes) ||
            meta.get<uint64_t>("samples") != iq.size() ||
            meta.get<std::string>("sha256") != sha256(iq))
            throw std::runtime_error("metadata does not match IQ format, name, length or SHA256");
        c.standard = meta.get<std::string>("standard", "");
        const double rate = meta.get<double>("sample_rate_hz");
        if (c.rate && c.rate != rate) throw std::runtime_error("sample-rate disagrees with metadata");
        c.rate = rate;
    }
    if (!std::isfinite(c.rate) || c.rate < 1e6 || c.rate > 61.44e6)
        throw std::runtime_error("require matching IQ sample rate 1e6..61.44e6 (or --metadata)");
    return iq;
}
void pause_until(std::chrono::steady_clock::time_point deadline)
{
    while (!interrupted && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
}
void range_check(const uhd::meta_range_t& range, double value, const char* name)
{
    if (std::abs(range.clip(value) - value) > .01)
        throw std::runtime_error(std::string(name) + " outside device range");
}
struct Events : fpv::TxEventState {
    void poll(const uhd::tx_streamer::sptr& tx, double timeout = 0)
    {
        uhd::async_metadata_t event;
        while (tx->recv_async_msg(event, timeout)) {
            timeout = 0;
            if (record(event.event_code)) {
                if (is_underflow(event.event_code))
                    std::cerr << "TX_UNDERFLOW count=" << underflows << " event=" << event.strevent()
                        << " continuing=1" << std::endl;
                else std::cerr << "TX_EVENT " << event.strevent() << std::endl;
            }
        }
    }
};
void control_reply(uint64_t id, const std::string& status, const fpv::Update& state,
                   double frequency, const std::string& error = "")
{
    boost::property_tree::ptree reply, frequencies;
    reply.put("id", id); reply.put("status", status);
    reply.put("standard", state.standard); reply.put("attenuation_db", state.attenuation);
    reply.put("dwell_s", state.dwell); reply.put("frequency_hz", frequency);
    for (double f : state.frequencies) {
        boost::property_tree::ptree item; item.put("", f); frequencies.push_back({"", item});
    }
    reply.add_child("frequencies_hz", frequencies);
    if (!error.empty()) reply.put("error", error);
    std::ostringstream line;
    boost::property_tree::write_json(line, reply, false);
    std::cout << "CONTROL " << line.str() << std::flush;
}
bool transmit_live(const Config& c, const std::vector<Sample>& iq,
                   const std::vector<Sample>& alternate, const uhd::usrp::multi_usrp::sptr& radio,
                   const uhd::tx_streamer::sptr& tx, bool has_lock)
{
    fpv::ControlInput controls(STDIN_FILENO);
    const auto gain_range = radio->get_tx_gain_range(c.channel);
    fpv::Update state{0, c.frequencies, c.standard, *c.attenuation, c.dwell};
    size_t center = 0;
    uint64_t acknowledge = 0;
    bool ready = false;
    auto set_level = [&](double attenuation) {
        const double gain = gain_range.stop() - attenuation;
        range_check(gain_range, gain, "gain");
        radio->set_tx_gain(gain, c.channel);
        if (std::abs(radio->get_tx_gain(c.channel) - gain) > .01)
            throw std::runtime_error("UHD coerced live TX attenuation");
    };
    while (!interrupted && !controls.closed && center < state.frequencies.size()) {
        const double frequency = state.frequencies[center];
        radio->set_tx_freq(uhd::tune_request_t(frequency), c.channel);
        if (std::abs(radio->get_tx_freq(c.channel) - frequency) > 1)
            throw std::runtime_error("UHD coerced live TX frequency");
        pause_until(std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(c.settle)));
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!interrupted && has_lock && !radio->get_tx_sensor("lo_locked", c.channel).to_bool()) {
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("TX LO lock timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (interrupted || controls.closed) break;
        if (!ready || acknowledge) {
            control_reply(acknowledge, ready ? "applied" : "ready", state, radio->get_tx_freq(c.channel));
            ready = true; acknowledge = 0;
        } else control_reply(0, "center", state, radio->get_tx_freq(c.channel));
        const auto& samples = state.standard == c.standard ? iq : alternate;
        Events stale; stale.poll(tx);
        Events events;
        std::optional<fpv::Update> pending;
        const uint64_t target = static_cast<uint64_t>(std::llround(c.rate * state.dwell));
        const auto start = radio->get_time_now() + uhd::time_spec_t(.2);
        auto service_control = [&] {
            if (interrupted || controls.closed) return true;
            if (auto message = controls.take()) {
                if (!message->request) {
                    control_reply(message->id, "rejected", state, frequency, message->error);
                } else {
                    auto request = *message->request;
                    std::string error;
                    try {
                        for (double f : request.frequencies)
                            range_check(radio->get_tx_freq_range(c.channel), f, "frequency");
                        range_check(gain_range, gain_range.stop() - request.attenuation, "gain");
                    } catch (const std::exception& e) { error = e.what(); }
                    if (!error.empty()) control_reply(request.id, "rejected", state, frequency, error);
                    else if (request.frequencies == state.frequencies && request.standard == state.standard &&
                             request.dwell == state.dwell) {
                        set_level(request.attenuation);
                        state = request;
                        control_reply(request.id, "applied", state, radio->get_tx_freq(c.channel));
                    } else { pending = std::move(request); return true; }
                }
            }
            return false;
        };
        std::cout << "BEGIN frequency_hz=" << frequency << " standard=" << state.standard
                  << " samples=" << target << std::endl;
        auto end_burst = [&] {
            uhd::tx_metadata_t md; md.end_of_burst = true;
            tx->send(samples.data(), 0, md, 1.0);
        };
        uint64_t sent = 0;
        try {
            sent = fpv::replay(samples.size(), target, 65536,
                [&](size_t offset, size_t count, bool first) {
                    uhd::tx_metadata_t md;
                    md.start_of_burst = first; md.has_time_spec = first; md.time_spec = start;
                    const auto accepted = tx->send(samples.data() + offset, count, md, 1.0);
                    events.poll(tx);
                    if (events.errors) throw std::runtime_error("fatal TX asynchronous event");
                    return accepted;
                }, service_control);
            if (sent) end_burst();
        } catch (...) { try { end_burst(); } catch (...) {} throw; }
        if (sent) {
            const double remaining = std::max(0.0, (start - radio->get_time_now()).get_real_secs() + sent / c.rate);
            deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(remaining + 2));
            while (!events.ack && std::chrono::steady_clock::now() < deadline) events.poll(tx, .05);
            if (!events.ack || events.errors) throw std::runtime_error("live TX burst did not finish cleanly");
        }
        std::cout << "RESULT frequency_hz=" << frequency << " accepted_samples=" << sent
                  << " async_errors=" << events.errors << " underflows=" << events.underflows
                  << " burst_ack=" << events.ack << " reconfigured=" << bool(pending)
                  << " interrupted=" << (interrupted != 0) << std::endl;
        if (interrupted || controls.closed) break;
        if (pending) {
            set_level(pending->attenuation);
            state = *pending;
            acknowledge = state.id;
            center = 0;
        } else ++center;
    }
    return !interrupted && !controls.closed;
}
bool transmit(const Config& c, const std::vector<Sample>& iq, const std::vector<Sample>& alternate)
{
    auto radio = uhd::usrp::multi_usrp::make(c.args);
    if (c.channel >= radio->get_tx_num_channels()) throw std::runtime_error("TX channel unavailable");
    radio->set_master_clock_rate(61.44e6);
    radio->set_tx_rate(c.rate, c.channel);
    if (std::abs(radio->get_tx_rate(c.channel) - c.rate) > .1)
        throw std::runtime_error("UHD coerced the sample rate; refusing wrong video timing");
    const auto gain_range = radio->get_tx_gain_range(c.channel);
    const double requested_gain = c.attenuation ? gain_range.stop() - *c.attenuation : c.gain;
    range_check(gain_range, requested_gain, "gain");
    range_check(radio->get_tx_bandwidth_range(c.channel), c.bandwidth, "bandwidth");
    for (double f : c.frequencies) range_check(radio->get_tx_freq_range(c.channel), f, "frequency");
    radio->set_tx_gain(requested_gain, c.channel);
    const double actual_gain = radio->get_tx_gain(c.channel);
    if (std::abs(actual_gain - requested_gain) > .01)
        throw std::runtime_error("UHD coerced TX gain/attenuation");
    radio->set_tx_bandwidth(c.bandwidth, c.channel);
    radio->set_tx_antenna("TX/RX", c.channel);
    std::cout << "DEVICE channel=" << c.channel << " frontend=" << radio->get_tx_subdev_name(c.channel)
        << " antenna=" << radio->get_tx_antenna(c.channel) << " rate=" << radio->get_tx_rate(c.channel)
        << " gain_db=" << actual_gain << " attenuation_db=" << gain_range.stop() - actual_gain
        << " bandwidth_hz=" << radio->get_tx_bandwidth(c.channel) << std::endl;
    const auto sensors = radio->get_tx_sensor_names(c.channel);
    const bool has_lock = std::find(sensors.begin(), sensors.end(), "lo_locked") != sensors.end();
    uhd::stream_args_t stream_args("sc16", "sc16");
    stream_args.channels = {c.channel};
    // Resume from the next packet after a gap; waiting for a new SOB would
    // leave the remaining samples of this continuous replay silent.
    stream_args.args["underflow_policy"] = "next_packet";
    auto tx = radio->get_tx_stream(stream_args);
    if (c.live_control) return transmit_live(c, iq, alternate, radio, tx, has_lock);
    bool clean = true;
    for (double frequency : c.frequencies) {
        if (interrupted) break;
        radio->set_tx_freq(uhd::tune_request_t(frequency), c.channel);
        if (std::abs(radio->get_tx_freq(c.channel) - frequency) > 1)
            throw std::runtime_error("UHD coerced TX frequency");
        pause_until(std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(c.settle)));
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!interrupted && has_lock && !radio->get_tx_sensor("lo_locked", c.channel).to_bool()) {
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("TX LO lock timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (interrupted) break;
        Events stale; stale.poll(tx);
        Events events;
        const uint64_t target = static_cast<uint64_t>(std::llround(c.rate * c.dwell));
        const auto start = radio->get_time_now() + uhd::time_spec_t(.2);
        std::cout << "BEGIN frequency_hz=" << frequency << " samples=" << target << std::endl;
        auto end_burst = [&] {
            uhd::tx_metadata_t md;
            md.end_of_burst = true;
            tx->send(iq.data(), 0, md, 1.0);
        };
        uint64_t sent = 0, accepted_samples = 0;
        const auto host_start = std::chrono::steady_clock::now();
        try {
            sent = fpv::replay(iq.size(), target, 65536,
                [&](size_t offset, size_t count, bool first) {
                    uhd::tx_metadata_t md;
                    md.start_of_burst = first;
                    md.has_time_spec = first;
                    md.time_spec = start;
                    const size_t accepted = tx->send(iq.data() + offset, count, md, 1.0);
                    accepted_samples += accepted;
                    events.poll(tx);
                    if (events.errors) throw std::runtime_error("fatal TX asynchronous event");
                    return accepted;
                }, [] { return interrupted != 0; });
            end_burst();
        } catch (...) {
            std::cerr << "FAILED frequency_hz=" << frequency << " accepted_samples=" << accepted_samples
                << " async_errors=" << events.errors << " underflows=" << events.underflows << " usb_send_frames=" << c.send_frames
                << " host_elapsed_s="
                << std::chrono::duration<double>(std::chrono::steady_clock::now() - host_start).count()
                << std::endl;
            try { end_burst(); } catch (...) {}
            throw;
        }
        // send() may return while samples are still queued. Wait for burst ACK
        // before retuning, or this frequency's tail would move to the next LO.
        const double remaining = std::max(0.0, (start - radio->get_time_now()).get_real_secs() + sent / c.rate);
        deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(remaining + 2));
        while (!events.ack && std::chrono::steady_clock::now() < deadline) events.poll(tx, .05);
        clean = clean && events.ack && !events.errors && sent == target;
        std::cout << "RESULT frequency_hz=" << frequency << " accepted_samples=" << sent
            << " async_errors=" << events.errors << " underflows=" << events.underflows
            << " continuous=" << (events.underflows == 0 && events.errors == 0 && sent == target && events.ack)
            << " burst_ack=" << events.ack
            << " interrupted=" << (interrupted != 0) << std::endl;
        if (!clean) break;
    }
    return clean && !interrupted;
}
} // namespace
int main(int argc, char** argv)
{
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") { usage(); return 0; }
        auto config = parse(argc, argv);
        auto iq = load(config);
        std::vector<Sample> alternate;
        if (config.live_control) {
            Config other = config;
            other.input = config.alternate_input; other.metadata = config.alternate_metadata;
            alternate = load(other);
            if ((config.standard != "pal" && config.standard != "ntsc") ||
                (other.standard != "pal" && other.standard != "ntsc") || config.standard == other.standard)
                throw std::runtime_error("live control requires one PAL and one NTSC waveform");
        }
        std::cout << std::setprecision(12) << "PLAN mode=" << (config.transmit ? "transmit" : "offline")
            << " samples=" << iq.size() << " rate_hz=" << config.rate
            << " loop_seconds=" << iq.size() / config.rate << " dwell_seconds=" << config.dwell
            << " level_control=" << (config.attenuation ? "attenuation" : "gain");
        if (config.attenuation) std::cout << " attenuation_db=" << *config.attenuation;
        else std::cout << " gain_db=" << config.gain;
        std::cout << " channel=" << config.channel << " underflow_policy=continue usb_send_frames=" << config.send_frames << " frequencies_hz=";
        for (auto f : config.frequencies) std::cout << f << ',';
        std::cout << std::endl;
        if (!config.transmit) return 0; // No UHD construction, enumeration or RF configuration.
        std::signal(SIGINT, stop_handler);
        std::signal(SIGTERM, stop_handler);
        return transmit(config, iq, alternate) ? 0 : 3;
    } catch (const std::exception& e) {
        std::cerr << "u220_fpv_tx: " << e.what() << '\n';
        return 1;
    }
}
