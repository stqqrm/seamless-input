#pragma once
#include <cstdint>

// What one UDP datagram carries between two PCs while one controls the other (see inputudp.hpp).
//
// Pointer motion and wheel travel are sent as RUNNING TOTALS since the start of the epoch, not as steps: the
// receiver applies the difference to the totals it saw last. A lost or late datagram therefore costs nothing,
// the next one makes up for it, and the cursor can never drift. The values wrap around (32-bit arithmetic).
//
// Keys and buttons stay on TCP. To keep them in order with the motion, every datagram says how many of those
// TCP events must be applied before it ('tcpCount'), and every key/button line says which datagram it must come
// after (see Format() and RxPumpLocked() in input.cpp).
struct MotionPacket {
    std::uint16_t epoch = 0;        // restarts the totals (a new control session, or a switch between UDP and TCP)
    std::uint32_t tcpCount = 0;     // key/button events of this epoch that came before this datagram
    std::int32_t mx = 0, my = 0;    // pointer motion, running total
    std::int32_t wx = 0, wy = 0;    // wheel (in 1/120 notches like the TCP W line), running total
};
