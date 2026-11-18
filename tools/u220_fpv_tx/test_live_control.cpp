#include "live_control.hpp"
#include <iostream>
#include <stdexcept>
#include <unistd.h>

void require(bool ok) { if (!ok) throw std::runtime_error("live control test failed"); }
const std::string valid = R"({"id":1,"frequencies_hz":[5800000000],"standard":"ntsc","attenuation_db":89.75,"dwell_s":1000})";
int main()
{
    auto request = fpv::parse_update(valid);
    require(request.id == 1 && request.standard == "ntsc" && request.frequencies[0] == 5800000000);
    for (const auto& text : {std::string("{}"), std::string(R"({"id":1,"standard":"secam"})"),
                            std::string(R"({"id":1,"frequencies_hz":[7000000000],"standard":"pal","attenuation_db":1,"dwell_s":1})")}) {
        bool failed = false;
        try { fpv::parse_update(text); } catch (...) { failed = true; }
        require(failed);
    }
    int descriptors[2]; require(pipe(descriptors) == 0);
    {
        fpv::ControlInput input(descriptors[0]);
        const std::string lines = "invalid json\n" + valid + "\n";
        require(write(descriptors[1], lines.data(), lines.size()) == static_cast<ssize_t>(lines.size()));
        unsigned received = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (received != 2 && std::chrono::steady_clock::now() < deadline) {
            if (auto message = input.take()) {
                if (!received) require(!message->error.empty());
                else require(message->request && message->request->standard == "ntsc");
                ++received;
            } else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(received == 2);
        close(descriptors[1]);
        while (!input.closed && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(input.closed);
    }
    close(descriptors[0]);
    // An open idle pipe must not prevent cancellation/destruction.
    require(pipe(descriptors) == 0);
    const auto before = std::chrono::steady_clock::now();
    { fpv::ControlInput input(descriptors[0]); }
    require(std::chrono::steady_clock::now() - before < std::chrono::seconds(1));
    close(descriptors[0]); close(descriptors[1]);
    std::cout << "Live control tests passed: validation, line framing, invalid requests, EOF and shutdown\n";
}
