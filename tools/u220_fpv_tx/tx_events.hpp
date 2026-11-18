#pragma once
#include <uhd/types/metadata.hpp>
#include <cstdint>

namespace fpv {
struct TxEventState {
    uint64_t underflows = 0, errors = 0;
    bool ack = false;
    static bool is_underflow(uhd::async_metadata_t::event_code_t code)
    {
        return code == uhd::async_metadata_t::EVENT_CODE_UNDERFLOW ||
               code == uhd::async_metadata_t::EVENT_CODE_UNDERFLOW_IN_PACKET;
    }
    // Return whether to log this event. Repeated underflows must not turn
    // console output into another bottleneck in the sample supply loop.
    bool record(uhd::async_metadata_t::event_code_t code)
    {
        if (is_underflow(code)) {
            ++underflows;
            return (underflows & (underflows - 1)) == 0;
        }
        if (code == uhd::async_metadata_t::EVENT_CODE_BURST_ACK) {
            ack = true;
            return false;
        }
        ++errors;
        return true;
    }
};
} // namespace fpv
