//
//  KeyImplementations.cpp
//  YogaSMC
//
//  Created by Zhen on 7/28/20.
//  Copyright © 2020 Zhen. All rights reserved.
//

#include "KeyImplementations.hpp"
#include "YogaSMC.hpp"

SMC_RESULT BDVT::writeAccess() {
    if (!drv)
        return SmcNotWritable;
    return SmcSuccess;
}

SMC_RESULT BDVT::update(const SMC_DATA *src) {
    bool newValue = *(reinterpret_cast<const bool *>(src));
    bool *oldValue = reinterpret_cast<bool *>(data);
    DBGLOG("vpckey", "BDVT update %d -> %d", *oldValue, newValue);
    drv->dispatchMessage(kSMC_setConservation, &newValue);
    *oldValue = newValue;
    return SmcSuccess;
}

SMC_RESULT CH0B::writeAccess() {
    // TODO: determine whether charger control is supported
    return SmcSuccess;
}

SMC_RESULT CH0B::update(const SMC_DATA *src) {
    DBGLOG("vpckey", "CH0B update 0x%x -> 0x%x", data[0], src[0]);
    lilu_os_memcpy(data, src, 1);
    return SmcSuccess;
}

SMC_RESULT atomicSpKey::readAccess() {
    uint32_t value = atomic_load_explicit(currentSensor, memory_order_acquire);
    *reinterpret_cast<uint16_t *>(data) = VirtualSMCAPI::encodeSp(type, value);
    return SmcSuccess;
}

SMC_RESULT atomicSpDeciKelvinKey::readAccess() {
    uint32_t value = atomic_load_explicit(currentSensor, memory_order_acquire);
    *reinterpret_cast<uint16_t *>(data) = VirtualSMCAPI::encodeSp(type, ((double)value - 2731)/10);
    return SmcSuccess;
}

SMC_RESULT atomicFltKey::readAccess() {
    uint32_t value = atomic_load_explicit(currentSensor, memory_order_acquire);
    *reinterpret_cast<uint32_t *>(data) = VirtualSMCAPI::encodeFlt(value);
    return SmcSuccess;
}

SMC_RESULT atomicFpKey::readAccess() {
    uint16_t value = atomic_load_explicit(currentSensor, memory_order_acquire);
    *reinterpret_cast<uint16_t *>(data) = VirtualSMCAPI::encodeIntFp(type, value);
    return SmcSuccess;
}

SMC_RESULT hpCachedKey::readAccess() {
    auto snapshot = atomic_load_explicit(cache, memory_order_acquire);
    auto at = static_cast<uint32_t>(snapshot >> 32);
    if (!snapshot || hpSensorSeconds() - at > maxAge) return SmcNotReadable;
    auto value = static_cast<uint32_t>(snapshot);
    *reinterpret_cast<uint16_t *>(data) = rpm ? VirtualSMCAPI::encodeIntFp(type, value)
        : VirtualSMCAPI::encodeSp(type, static_cast<double>(value) / 10.0);
    return SmcSuccess;
}

SMC_RESULT hpFanTargetKey::writeAccess() { return drv->acceptsHPFanWrites() ? SmcSuccess : SmcNotWritable; }
SMC_RESULT hpFanTargetKey::update(const SMC_DATA *src) {
    uint16_t encoded; lilu_os_memcpy(&encoded, src, sizeof(encoded));
    return drv->requestHPFan(true, VirtualSMCAPI::decodeIntFp(SmcKeyTypeFpe2, encoded));
}
SMC_RESULT hpFanModeKey::readAccess() {
    auto mode = drv->currentHPFanMode();
    if (mode > 1) return SmcNotReadable;
    if (mask) { data[0] = 0; data[1] = mode; } else data[0] = mode;
    return SmcSuccess;
}
SMC_RESULT hpFanModeKey::writeAccess() { return drv->acceptsHPFanWrites() ? SmcSuccess : SmcNotWritable; }
SMC_RESULT hpFanModeKey::update(const SMC_DATA *src) {
    return drv->requestHPFan(false, mask ? (static_cast<uint16_t>(src[0]) << 8) | src[1] : src[0]);
}
