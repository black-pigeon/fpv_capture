#include "replay.hpp"
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

void require(bool ok) { if (!ok) throw std::runtime_error("replay test failed"); }
int main()
{
    using Call = std::pair<size_t, size_t>;
    std::vector<Call> calls;
    bool stopped = false;
    auto partial = [&](size_t offset, size_t count, bool first) {
        require(first == calls.empty());
        calls.emplace_back(offset, count);
        return std::min<size_t>(2, count);
    };
    require(fpv::replay(5, 13, 4, partial, [&] { return stopped; }) == 13);
    require((calls == std::vector<Call>{{0,4},{2,3},{4,1},{0,4},{2,3},{4,1},{0,3},{2,1}}));
    require(fpv::replay(5, 10, 4, [&](size_t, size_t count, bool) {
        stopped = true; return count;
    }, [&] { return stopped; }) == 4);
    for (auto bad : {size_t(0), size_t(5)}) {
        bool threw = false;
        try {
            fpv::replay(5, 10, 4, [=](size_t, size_t, bool) { return bad; }, [] { return false; });
        } catch (const std::runtime_error&) { threw = true; }
        require(threw);
    }
    for (auto size : {size_t(0), size_t(5)}) {
        bool threw = false;
        try { fpv::replay(size, 10, 0, partial, [] { return false; }); }
        catch (const std::invalid_argument&) { threw = true; }
        require(threw);
    }
    std::cout << "Replay tests passed: partial sends, wrap, stop, zero/oversized send, invalid sizes\n";
}
