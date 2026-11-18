#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace fpv {
// A short UHD send advances only by the accepted samples. File wrap must not
// insert an EOB or restart timed metadata: every dwell is one continuous burst.
template<class Send, class Stop>
uint64_t replay(size_t samples, uint64_t target, size_t block, Send send, Stop stop)
{
    if (!samples || !block) throw std::invalid_argument("empty IQ or zero block size");
    size_t offset = 0;
    uint64_t total = 0;
    while (total < target && !stop()) {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(
            std::min(samples - offset, block), target - total));
        const size_t accepted = send(offset, count, total == 0);
        if (!accepted || accepted > count) throw std::runtime_error("TX send made invalid progress");
        total += accepted;
        offset = (offset + accepted) % samples;
    }
    return total;
}
} // namespace fpv
