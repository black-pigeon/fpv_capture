#pragma once
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cmath>
#include <deque>
#include <mutex>
#include <optional>
#include <poll.h>
#include <set>
#include <sstream>
#include <string>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fpv {
struct Update {
    uint64_t id = 0;
    std::vector<double> frequencies;
    std::string standard;
    double attenuation = 0, dwell = 0;
};
inline Update parse_update(const std::string& text)
{
    boost::property_tree::ptree tree;
    std::istringstream input(text);
    boost::property_tree::read_json(input, tree);
    std::set<std::string> keys;
    for (const auto& entry : tree) {
        if (!keys.insert(entry.first).second) throw std::runtime_error("duplicate field");
    }
    if (keys != std::set<std::string>{"id", "frequencies_hz", "standard", "attenuation_db", "dwell_s"})
        throw std::runtime_error("expected id, frequencies_hz, standard, attenuation_db, dwell_s");
    Update result;
    const std::string id = tree.get<std::string>("id");
    if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("id must be a positive integer");
    result.id = tree.get<uint64_t>("id");
    result.standard = tree.get<std::string>("standard");
    result.attenuation = tree.get<double>("attenuation_db");
    result.dwell = tree.get<double>("dwell_s");
    const auto& frequencies = tree.get_child("frequencies_hz");
    if (!frequencies.data().empty()) throw std::runtime_error("frequencies_hz must be an array");
    for (const auto& item : frequencies) {
        const double f = item.second.get_value<double>();
        if (!item.first.empty() || !item.second.empty() || !std::isfinite(f) || f < 50e6 || f > 6e9)
            throw std::runtime_error("frequency must be 50 MHz..6 GHz");
        result.frequencies.push_back(f);
    }
    if (!result.id || result.frequencies.empty() || result.frequencies.size() > 128 ||
        (result.standard != "pal" && result.standard != "ntsc") ||
        !std::isfinite(result.attenuation) || result.attenuation < 0 || result.attenuation > 89.75 ||
        std::abs(result.attenuation * 4 - std::round(result.attenuation * 4)) > 1e-6 ||
        !std::isfinite(result.dwell) || result.dwell <= 0 || result.dwell > 3600)
        throw std::runtime_error("invalid standard, attenuation, dwell or frequency list");
    return result;
}
struct ControlMessage {
    std::optional<Update> request;
    std::string error;
    uint64_t id = 0;
};
class ControlInput {
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::deque<ControlMessage> messages;
    std::thread worker;
    void push(ControlMessage message) {
        while (!stopping) {
            {
                std::lock_guard<std::mutex> guard(mutex);
                if (messages.size() < 16) { messages.push_back(std::move(message)); return; }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    void run(int fd) {
        std::string line;
        bool oversized = false;
        uint64_t last_id = 0;
        while (!stopping) {
            pollfd descriptor{fd, POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, 100);
            if (ready < 0) { if (errno == EINTR) continue; break; }
            if (!ready) continue;
            char data[4096];
            const auto count = ::read(fd, data, sizeof(data));
            if (count <= 0) { if (count < 0 && errno == EINTR) continue; break; }
            for (ssize_t i = 0; i < count && !stopping; ++i) {
                if (data[i] != '\n') {
                    if (line.size() < 8192 && !oversized) line += data[i];
                    else oversized = true;
                    continue;
                }
                ControlMessage message;
                try {
                    if (oversized) throw std::runtime_error("control line exceeds 8192 bytes");
                    // Preserve the request ID for a rejection, when possible.
                    boost::property_tree::ptree envelope;
                    std::istringstream stream(line);
                    boost::property_tree::read_json(stream, envelope);
                    message.id = envelope.get<uint64_t>("id", 0);
                    auto request = parse_update(line);
                    if (request.id <= last_id) throw std::runtime_error("id must increase");
                    last_id = request.id;
                    message.request = std::move(request);
                } catch (const std::exception& e) { message.error = e.what(); }
                push(std::move(message));
                line.clear(); oversized = false;
            }
        }
        closed = true;
    }
public:
    std::atomic<bool> closed{false};
    explicit ControlInput(int fd) { worker = std::thread([this, fd] { run(fd); }); }
    ~ControlInput() { stopping = true; worker.join(); }
    ControlInput(const ControlInput&) = delete;
    ControlInput& operator=(const ControlInput&) = delete;
    std::optional<ControlMessage> take() {
        std::lock_guard<std::mutex> guard(mutex);
        if (messages.empty()) return std::nullopt;
        auto message = std::move(messages.front()); messages.pop_front(); return message;
    }
};
} // namespace fpv
