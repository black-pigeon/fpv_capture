#include "tx_events.hpp"
#include "replay.hpp"
#include <iostream>
#include <stdexcept>

void require(bool ok) { if (!ok) throw std::runtime_error("TX event test failed"); }
int main()
{
    using Event = uhd::async_metadata_t;
    fpv::TxEventState state;
    require(state.record(Event::EVENT_CODE_UNDERFLOW));
    require(state.record(Event::EVENT_CODE_UNDERFLOW_IN_PACKET));
    require(!state.record(Event::EVENT_CODE_UNDERFLOW)); // log only powers of two
    require(state.underflows == 3 && state.errors == 0 && !state.ack);
    require(!state.record(Event::EVENT_CODE_BURST_ACK));
    require(state.ack && state.errors == 0);
    state.record(Event::EVENT_CODE_SEQ_ERROR);
    state.record(Event::EVENT_CODE_TIME_ERROR);
    require(state.errors == 2 && state.underflows == 3);

    fpv::TxEventState recovered;
    size_t calls = 0;
    const auto sent = fpv::replay(100, 1000, 25,
        [&](size_t, size_t count, bool) {
            if (++calls == 3) recovered.record(Event::EVENT_CODE_UNDERFLOW);
            if (recovered.errors) throw std::runtime_error("fatal event");
            return count;
        }, [] { return false; });
    require(sent == 1000 && calls == 40 && recovered.underflows == 1 && !recovered.errors);
    std::cout << "TX event tests passed: recoverable underflows, fatal events, throttled logs, replay continues\n";
}
