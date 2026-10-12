// HP fan request encoding shared with offline tests. No ACPI on SMC callbacks.
#ifndef HPFanControl_hpp
#define HPFanControl_hpp
#include <stdint.h>
#include <stdatomic.h>
inline uint32_t hpFanCount(uint32_t rpm) {
    uint32_t count = (245760 + rpm/2) / rpm;
    return count < 33 ? 33 : count > 55 ? 55 : count;
}
// 0 queued, 1 frozen, 2 invalid. Bit 17 freezes requests during automatic restore.
inline unsigned hpQueueFan(_Atomic(uint64_t) *state, bool target, uint16_t value) {
    if ((target && (value < 4468 || value > 7447)) || (!target && value > 1)) return 2;
    uint64_t old = atomic_load_explicit(state, memory_order_acquire), next;
    do {
        if (old & 0x20000) return 1;
        next = (((old >> 32) + 1) << 32) | (target ? (old & 0x10000) | value
            : (old & 0xffff) | (static_cast<uint64_t>(value) << 16));
    } while (!atomic_compare_exchange_weak_explicit(state, &old, next, memory_order_release, memory_order_relaxed));
    return 0;
}
#endif
