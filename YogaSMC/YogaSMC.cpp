//
//  YogaSMC.cpp
//  YogaSMC
//
//  Created by Zhen on 7/29/20.
//  Copyright © 2020 Zhen. All rights reserved.
//

#include "YogaSMC.hpp"
#include "HPFanControl.hpp"

OSDefineMetaClassAndStructors(YogaSMC, YogaBaseService);

bool ADDPR(debugEnabled) = false;
uint32_t ADDPR(debugPrintDelay) = 0;

static const struct sensorPair presetTemperatureDeciKelvin[] = {
    {KeyTB0T(0), "Battery"},
    {KeyTB0T(1), "Battery Sensor 1"},
    {KeyTB0T(2), "Battery Sensor 2"}
};

static const struct sensorPair presetTemperature[] = {
    {KeyTCSA,    "CPU System Agent Core"},
    {KeyTCXC,    "CPU Core PECI"},
    // Laptops only have 1 key for both channel
    {KeyTM0P,    "Memory Proximity"},
    // Desktops
    {KeyTM0p(0), "SO-DIMM 1 Proximity"},
    {KeyTM0p(1), "SO-DIMM 2 Proximity"},
    {KeyTM0p(2), "SO-DIMM 3 Proximity"},
    {KeyTM0p(3), "SO-DIMM 4 Proximity"},
    {KeyTPCD,    "Platform Controller Hub Die"},
    {KeyTW0P,    "Airport Proximity"},
    {KeyTaLC,    "Airflow Left"},
    {KeyTaRC,    "Airflow Right"},
    {KeyTh0H(1), "Fin Stack Proximity Right"},
    {KeyTh0H(2), "Fin Stack Proximity Left"},
    {KeyTs0P(0), "Palm Rest"},
    {KeyTs0P(1), "Trackpad Actuator"}
};

void YogaSMC::addVSMCKey() {
    addHPThermalKeys();
    // ACPI-based
    if (!conf || !ec)
        return;

    ECSensorBase = sensorCount;

    OSDictionary *status = OSDictionary::withCapacity(1);
    OSString *method;

    for (auto &pair : presetTemperatureDeciKelvin)
        addECKeySp(pair.key, pair.name, atomicSpDeciKelvinKey);

    for (auto &pair : presetTemperature) {
        if (hpThermalEnabled && pair.key == KeyTPCD) continue;
        addECKeySp(pair.key, pair.name, atomicSpKey);
    }

    setProperty("DirectECKey", status);
    status->release();
}

bool YogaSMC::start(IOService *provider) {
    isPMsupported = true;

    if (!super::start(provider))
        return false;

    atomic_init(&hpFanTarget, 0); atomic_init(&hpPCHTemperature, 0); atomic_init(&hpSSDTemperature, 0); atomic_init(&hpChargerTemperature, 0);
    atomic_init(&hpFanRequest, 7447); atomic_init(&hpFanMode, 2);
    atomic_init(&hpFanAcceptWrites, false);
    hpThermalEnabled = getProperty("HPX2G2ThermalSMC") == kOSBooleanTrue;
    DebugLog("Starting");

    validateEC();

    poller = IOTimerEventSource::timerEventSource(this, [](OSObject *object, IOTimerEventSource *sender) {
        auto smc = OSDynamicCast(YogaSMC, object);
        if (smc) smc->updateEC();
    });

    if (!poller || (workLoop->addEventSource(poller) != kIOReturnSuccess)) {
        AlwaysLog("Failed to add poller");
        return false;
    }

    hpFanMethods = hpThermalEnabled && ec && ec->validateObject("HFST") == kIOReturnSuccess
        && ec->validateObject("HFAR") == kIOReturnSuccess;
    setProperty("FanControlMethodsAvailable", hpFanMethods);
    if (hpFanMethods) restoreHPFan("startup");
    // WARNING: watch out, key addition is sorted here!
    addVSMCKey();
    qsort(const_cast<VirtualSMCKeyValue *>(vsmcPlugin.data.data()), vsmcPlugin.data.size(), sizeof(VirtualSMCKeyValue), VirtualSMCKeyValue::compare);
    setProperty("Key Submitted", vsmcPlugin.data.size(), 32);
    vsmcNotifier = VirtualSMCAPI::registerHandler(vsmcNotificationHandler, this);

    awake = true;
    ready = true;
    if (hpFanMethods && !hpFanRestorePending) enableHPFanWrites();
    poller->setTimeoutMS(POLLING_INTERVAL);
    poller->enable();
    registerService();
    return true;
}

void YogaSMC::stop(IOService *provider)
{
    DebugLog("Stopping");

    if (commandGate) commandGate->runAction(hpFanRestoreGated, const_cast<char *>("stop"));
    poller->disable();
    workLoop->removeEventSource(poller);
    OSSafeReleaseNULL(poller);

    OSSafeReleaseNULL(hpCPUZone);
    OSSafeReleaseNULL(hpPCHZone);
    OSSafeReleaseNULL(hpChargerZone);
    terminate();

    super::stop(provider);
}

bool YogaSMC::vsmcNotificationHandler(void *sensors, void *refCon, IOService *vsmc, IONotifier *notifier) {
    auto self = OSDynamicCast(YogaSMC, reinterpret_cast<OSMetaClassBase*>(sensors));
    if (sensors && vsmc) {
        DBGLOG("yogasmc", "got vsmc notification");
        auto &plugin = self->vsmcPlugin;
        auto ret = vsmc->callPlatformFunction(VirtualSMCAPI::SubmitPlugin, true, sensors, &plugin, nullptr, nullptr);
        if (ret == kIOReturnSuccess) {
            DBGLOG("yogasmc", "submitted plugin");
            return true;
        } else if (ret != kIOReturnUnsupported) {
            SYSLOG("yogasmc", "plugin submission failure %X", ret);
        } else {
            DBGLOG("yogasmc", "plugin submission to non vsmc");
        }
    } else {
        SYSLOG("yogasmc", "got null vsmc notification");
    }
    return false;
}

IOService* YogaSMC::probe(IOService *provider, SInt32 *score) {
    ec = OSDynamicCast(IOACPIPlatformDevice, provider->getProperty("ECDevice"));
    if (!ec) {
        DebugLog("EC not found");
        return nullptr;
    }
    iname = ec->getName();

    conf = OSDynamicCast(OSDictionary, getProperty("Sensors"));
    if (!conf) {
        DebugLog("conf not found");
        return nullptr;
    }
    getWMISensor(provider);
    return this;
}

void YogaSMC::updateEC() {
    if (!awake)
        return;

    updateHPFanControl();
    updateHPThermal();
    updateECVendor();

    UInt32 result = 0;
    for (UInt8 i = ECSensorBase; i < sensorCount; i++)
        if (ec->evaluateInteger(sensorMethods[i], &result) == kIOReturnSuccess && result != 0)
            atomic_store_explicit(&currentSensor[i], result, memory_order_release);
    poller->setTimeoutMS(POLLING_INTERVAL);
}

IOReturn YogaSMC::setPowerState(unsigned long powerStateOrdinal, IOService * whatDevice) {
    if (super::setPowerState(powerStateOrdinal, whatDevice) != kIOPMAckImplied)
        return kIOReturnInvalid;

    if (powerStateOrdinal == 0) {
        if (awake) {
            commandGate->runAction(hpFanRestoreGated, const_cast<char *>("sleep"));
            poller->disable();
            workLoop->removeEventSource(poller);
            awake = false;
            atomic_store_explicit(&hpFanTarget, 0, memory_order_release);
            atomic_store_explicit(&hpPCHTemperature, 0, memory_order_release);
            atomic_store_explicit(&hpSSDTemperature, 0, memory_order_release);
            atomic_store_explicit(&hpChargerTemperature, 0, memory_order_release);
            DebugLog("Going to sleep");
        }
    } else {
        if (!awake && ready) {
            awake = true;
            commandGate->runAction(hpFanRestoreGated, const_cast<char *>("wake"));
            if (hpFanMethods && !hpFanRestorePending) enableHPFanWrites();
            workLoop->addEventSource(poller);
            poller->setTimeoutMS(POLLING_INTERVAL);
            poller->enable();
            DebugLog("Woke up");
        }
    }
    return kIOPMAckImplied;
}

void YogaSMC::setPropertiesGated(OSObject* props) {
    OSDictionary* dict = OSDynamicCast(OSDictionary, props);
    if (!dict)
        return;

    OSCollectionIterator* i = OSCollectionIterator::withCollection(dict);

    if (i) {
        while (OSString* key = OSDynamicCast(OSString, i->getNextObject())) {
            if (hpThermalEnabled && key->isEqualTo("SSDTemperatureDeciC")) {
                auto value = OSDynamicCast(OSNumber, dict->getObject(key));
                if (value && value->unsigned64BitValue() <= 1250) {
                    auto deciC = value->unsigned32BitValue();
                    hpCacheStore(&hpSSDTemperature, deciC);
                    setProperty("SSDTemperatureDeciC", deciC, 32);
                    setProperty("SSDTemperatureUpdated", hpSensorSeconds(), 32);
                }
            } else if (key->isEqualTo(readECPrompt)) {
                if (!ec)
                    continue;
                OSNumber *value;
                getPropertyNumber(readECPrompt);
                if (value->unsigned8BitValue() >= sensorCount) {
                    AlwaysLog(valueInvalid, readECPrompt);
                    continue;
                }
                uint32_t raw = atomic_load_explicit(&currentSensor[value->unsigned8BitValue()], memory_order_acquire);
                AlwaysLog(updateSuccess, readECPrompt, raw);
            } else {
                AlwaysLog("Unknown property %s", key->getCStringNoCopy());
            }
        }
        i->release();
    }

    return;
}

IOReturn YogaSMC::setProperties(OSObject *props) {
    commandGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &YogaSMC::setPropertiesGated), props);
    return kIOReturnSuccess;
}

EXPORT extern "C" kern_return_t ADDPR(kern_start)(kmod_info_t *, void *) {
    // Report success but actually do not start and let I/O Kit unload us.
    // This works better and increases boot speed in some cases.
    PE_parse_boot_argn("liludelay", &ADDPR(debugPrintDelay), sizeof(ADDPR(debugPrintDelay)));
    ADDPR(debugEnabled) = checkKernelArgument("-vsmcdbg");
    return KERN_SUCCESS;
}

EXPORT extern "C" kern_return_t ADDPR(kern_stop)(kmod_info_t *, void *) {
    // It is not safe to unload VirtualSMC plugins!
    return KERN_FAILURE;
}

void YogaSMC::addHPThermalKeys() {
    if (!hpThermalEnabled) return;
    const SMC_KEY keys[] = {KeyF0Tg(0), KeyTPCD, SMC_MAKE_IDENTIFIER('T','H','0','A'), SMC_MAKE_IDENTIFIER('T','C','H','P')};
    _Atomic(uint64_t) *caches[] = {&hpFanTarget, &hpPCHTemperature, &hpSSDTemperature, &hpChargerTemperature};
    for (unsigned i=0; i<4; ++i) {
        bool duplicate=false;
        for (size_t n=0; n<vsmcPlugin.data.size(); ++n) if (vsmcPlugin.data[n].key==keys[i]) duplicate=true;
        if (duplicate) { AlwaysLog("HP thermal key already supplied, slot %u", i); continue; }
        hpCachedKey *key = i==0 && hpFanMethods ? new hpFanTargetKey(caches[i], this)
            : new hpCachedKey(caches[i], i==2 ? 30 : 10, i==0);
        VirtualSMCAPI::addKey(keys[i], vsmcPlugin.data, i==0
            ? VirtualSMCAPI::valueWithFp(0, SmcKeyTypeFpe2, key, SMC_KEY_ATTRIBUTE_READ | (hpFanMethods ? SMC_KEY_ATTRIBUTE_WRITE : 0))
            : VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp78, key, SMC_KEY_ATTRIBUTE_READ));
    }
    // Endpoints of HP P87 01.51's normal BIOS profile (0/1), not motor limits.
    // Static policy metadata only: these keys never write a fan target or mode.
    const SMC_KEY limitKeys[] = {KeyF0Mn(0), KeyF0Mx(0)};
    const uint16_t limitRPM[] = {4468, 7447};
    for (unsigned i=0; i<2; ++i) {
        bool duplicate=false;
        for (size_t n=0; n<vsmcPlugin.data.size(); ++n)
            if (vsmcPlugin.data[n].key==limitKeys[i]) duplicate=true;
        if (duplicate) { AlwaysLog("HP fan policy limit key already supplied, slot %u", i); continue; }
        VirtualSMCAPI::addKey(limitKeys[i], vsmcPlugin.data,
            VirtualSMCAPI::valueWithFp(limitRPM[i], SmcKeyTypeFpe2, nullptr,
                SMC_KEY_ATTRIBUTE_CONST | SMC_KEY_ATTRIBUTE_READ));
    }
    if (hpFanMethods) {
        VirtualSMCAPI::addKey(KeyF0Md(0), vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(0,
            new hpFanModeKey(this, false), SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE));
        VirtualSMCAPI::addKey(KeyFS__, vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(0,
            new hpFanModeKey(this, true), SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE));
    }
    setProperty("FanControlVersion", 1, 32);
    setProperty("FanPolicyMinimumRPM", 4468, 32);
    setProperty("FanPolicyMaximumRPM", 7447, 32);
    setProperty("FanPolicyLimitsSource", "HP P87 01.51 normal BIOS profile 0/1");
    setProperty("HPThermalSMCVersion", 1, 32);
    AlwaysLog("HP thermal caches and fan policy limits enabled; fan control requires HFST/HFAR");
}
void YogaSMC::updateHPThermal() {
    if (!hpThermalEnabled) return;
    UInt32 raw=0;
    auto ret=ec ? ec->evaluateInteger("FTGC", &raw) : kIOReturnNotReady;
    setProperty("FanTargetReadError", static_cast<uint32_t>(ret), 32);
    if (ret==kIOReturnSuccess && raw<0xFF) {
        // Match HP GTRM rounding/zero; reject undefined 0xFF and unencodable RPM.
        auto rpm=raw==0 ? 0 : (245760+(raw>>1))/raw;
        if (rpm>16383) atomic_store_explicit(&hpFanTarget, 0, memory_order_release);
        else hpCacheStore(&hpFanTarget, rpm); setProperty("FanTargetRaw", raw, 32);
        setProperty("FanTargetRPM", rpm, 32);
    } else atomic_store_explicit(&hpFanTarget, 0, memory_order_release);
    if (!hpPCHZone) {
        auto iterator=IORegistryIterator::iterateOver(gIOACPIPlane,kIORegistryIterateRecursively);
        if (iterator) {
            while (auto entry=iterator->getNextObject()) {
                if (!strcmp(entry->getName(gIOACPIPlane),"PHWZ")) {
                    hpPCHZone=OSDynamicCast(IOACPIPlatformDevice,entry);
                    if (hpPCHZone) {hpPCHZone->retain();break;}
                }
            }
            iterator->release();
        }
    }
    UInt32 sta=0; raw=0;
    ret=hpPCHZone ? hpPCHZone->evaluateInteger("_STA",&sta) : kIOReturnNotFound;
    if (ret==kIOReturnSuccess && (sta&1)) ret=hpPCHZone->evaluateInteger("_TMP",&raw);
    else if (ret==kIOReturnSuccess) ret=kIOReturnNotReady;
    setProperty("PCHTemperatureReadError",static_cast<uint32_t>(ret),32);
    if (ret==kIOReturnSuccess && raw>=2732 && raw<=3982) {
        hpCacheStore(&hpPCHTemperature,raw-2732);setProperty("PCHTemperatureDeciC",raw-2732,32);
    } else atomic_store_explicit(&hpPCHTemperature,0,memory_order_release);
    // Stock CHGZ has no _STA; existence is sufficient for this allowlisted zone.
    if (!hpChargerZone) {
        auto iterator=IORegistryIterator::iterateOver(gIOACPIPlane,kIORegistryIterateRecursively);
        if (iterator) {
            while (auto entry=iterator->getNextObject()) {
                if (!strcmp(entry->getName(gIOACPIPlane),"CHGZ")) {
                    hpChargerZone=OSDynamicCast(IOACPIPlatformDevice,entry);
                    if (hpChargerZone) {hpChargerZone->retain();break;}
                }
            }
            iterator->release();
        }
    }
    raw=0;
    ret=hpChargerZone ? hpChargerZone->evaluateInteger("_TMP",&raw) : kIOReturnNotFound;
    setProperty("ChargerTemperatureReadError",static_cast<uint32_t>(ret),32);
    setProperty("ChargerTemperatureRaw",raw,32);
    bool valid=ret==kIOReturnSuccess && raw>2732 && raw<=3982;
    setProperty("ChargerTemperatureValid",valid);
    if (valid) {
        hpCacheStore(&hpChargerTemperature,raw-2732);
        setProperty("ChargerTemperatureDeciC",raw-2732,32);
        setProperty("ChargerTemperatureUpdated",hpSensorSeconds(),32);
    } else atomic_store_explicit(&hpChargerTemperature,0,memory_order_release);
}

// SMC callbacks only queue one atomic snapshot. ACPI runs on the workloop gate.
SMC_RESULT YogaSMC::requestHPFan(bool target, uint16_t value) {
    if (!acceptsHPFanWrites()) return SmcNotWritable;
    auto result = hpQueueFan(&hpFanRequest, target, value);
    return result == 0 ? SmcSuccess : result == 1 ? SmcNotWritable : SmcBadArgumentError;
}
IOReturn YogaSMC::hpFanACPI(bool manual, uint32_t count) {
    if (!hpFanMethods) return kIOReturnUnsupported;
    UInt32 status = 0xffffffff;
    OSObject *args[] = {manual ? OSNumber::withNumber(count, 32) : nullptr};
    if (manual && !args[0]) return kIOReturnNoMemory;
    auto ret = ec->evaluateInteger(manual ? "HFST" : "HFAR", &status, args, manual ? 1 : 0);
    OSSafeReleaseNULL(args[0]);
    setProperty("FanControlACPIStatus", status, 32);
    setProperty("FanControlACPIError", static_cast<uint32_t>(ret), 32);
    return ret == kIOReturnSuccess && status != 0 ? kIOReturnError : ret;
}
void YogaSMC::restoreHPFan(const char *reason) {
    atomic_store_explicit(&hpFanAcceptWrites, false, memory_order_release);
    if (!hpFanMethods) return;
    // Invalidate pending manual requests; never carry manual mode across sleep/reboot.
    auto request = atomic_fetch_or_explicit(&hpFanRequest, static_cast<uint64_t>(0x20000), memory_order_acq_rel);
    request = (((request >> 32) + 1) << 32) | 0x20000 | 7447;
    atomic_store_explicit(&hpFanRequest, request, memory_order_release);
    auto ret = hpFanACPI(false, 0);
    hpFanRestorePending = ret != kIOReturnSuccess;
    atomic_store_explicit(&hpFanMode, hpFanRestorePending ? 2 : 0, memory_order_release);
    hpFanAppliedRequest = request & ~static_cast<uint64_t>(0x20000);
    setProperty("FanControlRestorePending", hpFanRestorePending);
    setProperty("FanControlReason", reason);
    setProperty("FanControlMode", hpFanRestorePending ? "unknown" : "automatic");
    AlwaysLog("HP fan restore %s: 0x%x", reason, ret);
}
IOReturn YogaSMC::hpFanRestoreGated(OSObject *owner, void *reason, void *, void *, void *) {
    auto self = OSDynamicCast(YogaSMC, owner);
    if (!self) return kIOReturnBadArgument;
    const auto why = static_cast<const char *>(reason);
    if (!strcmp(why, "shutdown") || !strcmp(why, "stop") || !strcmp(why, "sleep")) {
        self->awake = false;
        if (self->poller) self->poller->disable();
    }
    self->restoreHPFan(why);
    return kIOReturnSuccess;
}
void YogaSMC::systemWillShutdown(IOOptionBits specifier) {
    if (commandGate) commandGate->runAction(hpFanRestoreGated, const_cast<char *>("shutdown"));
    super::systemWillShutdown(specifier);
}
void YogaSMC::updateHPFanControl() {
    if (!hpFanMethods) return;
    if (hpFanRestorePending) {
        restoreHPFan("restore retry");
        if (!hpFanRestorePending) enableHPFanWrites();
        return;
    }
    auto request = atomic_load_explicit(&hpFanRequest, memory_order_acquire);
    bool manual = request & 0x10000;
    if (manual) {
        if (!hpCPUZone) {
            auto it = IORegistryIterator::iterateOver(gIOACPIPlane, kIORegistryIterateRecursively);
            if (it) {
                while (auto entry = it->getNextObject()) {
                    if (!strcmp(entry->getName(gIOACPIPlane), "CPUZ")) {
                        hpCPUZone = OSDynamicCast(IOACPIPlatformDevice, entry);
                        if (hpCPUZone) { hpCPUZone->retain(); break; }
                    }
                }
                it->release();
            }
        }
        UInt32 raw = 0;
        auto ret = hpCPUZone ? hpCPUZone->evaluateInteger("_TMP", &raw) : kIOReturnNotFound;
        setProperty("FanControlCPURaw", raw, 32);
        setProperty("FanControlCPUError", static_cast<uint32_t>(ret), 32);
        if (ret != kIOReturnSuccess || raw <= 2732 || raw >= 3582) {
            restoreHPFan("CPU temperature missing or >=85C");
            if (!hpFanRestorePending) enableHPFanWrites();
            return;
        }
    }
    if (request == hpFanAppliedRequest) return;
    uint32_t rpm = request & 0xffff;
    uint32_t count = hpFanCount(rpm);
    setProperty("FanControlRequestedRPM", rpm, 32);
    setProperty("FanControlRequestedCount", count, 32);
    setProperty("FanControlRequestSequence", request >> 32, 32);
    // Setting a desired target while already automatic only stages the next manual request.
    if (!manual && currentHPFanMode() == 0) { hpFanAppliedRequest = request; return; }
    if (hpFanACPI(manual, count) != kIOReturnSuccess) {
        restoreHPFan("fan write failed");
        if (!hpFanRestorePending) enableHPFanWrites();
        return;
    }
    atomic_store_explicit(&hpFanMode, manual ? 1 : 0, memory_order_release);
    hpFanAppliedRequest = request;
    setProperty("FanControlMode", manual ? "manual" : "automatic");
    setProperty("FanControlReason", "SMC request applied");
    AlwaysLog("HP fan request %u: %s %u RPM/count %u", static_cast<uint32_t>(request >> 32), manual ? "manual" : "automatic", rpm, count);
}
