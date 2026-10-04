//
//  DYVPC.cpp
//  YogaSMC
//
//  Created by Zhen on 1/7/21.
//  Copyright © 2021 Zhen. All rights reserved.
//

#include "DYVPC.hpp"
#include "YogaWMI.hpp"
OSDefineMetaClassAndStructors(DYVPC, YogaVPC);

bool DYVPC::probeVPC(IOService *provider) {
    YWMI = new WMI(provider);
    if (!YWMI->initialize())
        return false;

    inputCap = YWMI->hasMethod(INPUT_WMI_EVENT, ACPI_WMI_EVENT);
    BIOSCap = YWMI->hasMethod(BIOS_QUERY_WMI_METHOD);

    if (!inputCap && !BIOSCap) {
        delete YWMI;
        return false;
    }

    vendorWMISupport = true;
    return true;
}

bool DYVPC::initEC() {
    UInt32 state, attempts = 0;

    // _REG will write Arg1 to ECRG to connect / disconnect the region
    if (ec->validateObject("ECRG") == kIOReturnSuccess) {
        do {
            if (ec->evaluateInteger("ECRG", &state) == kIOReturnSuccess && state != 0) {
                if (attempts != 0)
                    setProperty("EC Access Retries", attempts, 8);
                return true;
            }
            IOSleep(100);
        } while (++attempts < 5);
        AlwaysLog(updateFailure, "ECRG");
    }

    return true;
}

bool DYVPC::initVPC() {
    if (!initEC())
        return false;

    super::initVPC();

    OSObject * result;
    if (vpc->evaluateObject("RCDS", &result) == kIOReturnSuccess) {
        readCommandDateSize = OSDynamicCast(OSArray, result);
        readCommandDateSize->retain();
    }
    OSSafeReleaseNULL(result);
    if (vpc->evaluateObject("WCDS", &result) == kIOReturnSuccess) {
        writeCommandDateSize = OSDynamicCast(OSArray, result);
        writeCommandDateSize->retain();
    }
    OSSafeReleaseNULL(result);

    YWMI->start();

    if (inputCap)
        YWMI->enableEvent(INPUT_WMI_EVENT, true);

    resetHotkeyMode("start");

//    if (BIOSCap) {
//        UInt32 value;
//        if (WMIQuery(HPWMI_HARDWARE_QUERY, &value))
//            setProperty("HARDWARE", value, 32);
//        if (WMIQuery(HPWMI_WIRELESS_QUERY, &value))
//            setProperty("WIRELESS", value, 32);
//        if (WMIQuery(HPWMI_WIRELESS2_QUERY, &value))
//            setProperty("WIRELESS2", value, 32);
//        if (WMIQuery(HPWMI_FEATURE2_QUERY, &value))
//            setProperty("FEATURE2", value, 32);
//        else if (WMIQuery(HPWMI_FEATURE_QUERY, &value))
//            setProperty("FEATURE", value, 32);
//        if (WMIQuery(HPWMI_POSTCODEERROR_QUERY, &value))
//            setProperty("POSTCODE", value, 32);
//        if (WMIQuery(HPWMI_THERMAL_POLICY_QUERY, &value))
//            setProperty("THERMAL_POLICY", value, 32);
//    }
    return true;
}

bool DYVPC::exitVPC() {
    if (inputCap)
        YWMI->enableEvent(INPUT_WMI_EVENT, false);

    if (YWMI)
        delete YWMI;

    OSSafeReleaseNULL(readCommandDateSize);
    OSSafeReleaseNULL(writeCommandDateSize);
    return super::exitVPC();
}

void DYVPC::updateVPC(UInt32 event) {
    UInt32 id;
    UInt32 data;

    OSObject *result;
    if (!YWMI->getEventData(event, &result)) {
        AlwaysLog("message: Unknown ACPI notification 0x%04x", event);
        return;
    }

    OSData *buf = OSDynamicCast(OSData, result);
    if (!buf) {
        DebugLog("Unknown response type");
        result->release();
        return;
    }

    switch (buf->getLength()) {
        case 8:
            id = *(reinterpret_cast<UInt32 const*>(buf->getBytesNoCopy(0, 4)));
            data = *(reinterpret_cast<UInt32 const*>(buf->getBytesNoCopy(4, 4)));
            break;
            
        case 16:
            id = *(reinterpret_cast<UInt32 const*>(buf->getBytesNoCopy(0, 8)));
            data = *(reinterpret_cast<UInt32 const*>(buf->getBytesNoCopy(8, 8)));
            break;
            
        default:
            DebugLog("Unknown response length %d", buf->getLength());
            buf->release();
            return;
    }
    buf->release();

    if (id == HPWMI_BEZEL_BUTTON)
        DebugLog("Bezel id: 0x%x - 0x%x", id, data);
    else
        DebugLog("Unknown id: 0x%x - 0x%x", id, data);

    // Forward to YogaSMCNC (e.g. 0x05 = HPWMI_WIRELESS, the Fn wireless button)
    if (client)
        client->sendNotification(id, data);
}

void DYVPC::resetHotkeyMode(const char *reason) {
    if (!ec || ec->validateObject("SSHK") != kIOReturnSuccess)
        return;

    UInt32 shk = 0;
    if (readECName("SHK_", &shk) != kIOReturnSuccess || shk == 0) {
        DebugLog("%s: SHK 0x%02x, nothing to do", reason, shk);
        return;
    }

    OSObject *params[1] = { OSNumber::withNumber(0ULL, 8) };
    IOReturn ret = ec->evaluateObject("SSHK", nullptr, params, 1);
    params[0]->release();

    UInt32 after = 0xFFFF;
    readECName("SHK_", &after);
    AlwaysLog("%s: hotkey mode SHK 0x%02x -> 0x%02x (SSHK ret 0x%x)", reason, shk, after, ret);
}

IOReturn DYVPC::setPowerState(unsigned long powerStateOrdinal, IOService * whatDevice) {
    IOReturn ret = super::setPowerState(powerStateOrdinal, whatDevice);
    if (ret == kIOPMAckImplied && powerStateOrdinal != 0)
        resetHotkeyMode("wake");
    return ret;
}

IOReturn DYVPC::message(UInt32 type, IOService *provider, void *argument) {
    if (type != kIOACPIMessageDeviceNotification || !argument)
        return super::message(type, provider, argument);

    updateVPC(*(reinterpret_cast<UInt32 *>(argument)));

    return kIOReturnSuccess;
}

bool DYVPC::WMIQuery(UInt32 query, void *buffer, enum hp_wmi_command command, UInt32 insize, UInt32 outsize, UInt32 midOverride) {
    struct bios_args args = {
        .signature      = 0x55434553,
        .command        = command,
        .commandtype    = query,
        .datasize       = insize,
        .data           = { 0 },
    };

    if (insize > sizeof(args.data))
        return false;

    UInt32 mid;
    if (outsize > 4096)
        return false;
    else if (outsize > 1024)
        mid = 5;
    else if (outsize > 128)
        mid = 4;
    else if (outsize > 4)
        mid = 3;
    else if (outsize > 0)
        mid = 2;
    else
        mid = 1;
    if (midOverride)
        mid = midOverride;

    OSNumber *vsize = nullptr;
    if (command == HPWMI_READ && readCommandDateSize)
        vsize = OSDynamicCast(OSNumber, readCommandDateSize->getObject(query-1));
    if (command == HPWMI_WRITE && writeCommandDateSize)
        vsize = OSDynamicCast(OSNumber, writeCommandDateSize->getObject(query-1));
    if (vsize != nullptr && vsize->unsigned32BitValue() != insize)
        AlwaysLog("Input size mismatch: expected 0x%x, actual 0x%x", vsize->unsigned32BitValue(), insize);

    memcpy(&args.data[0], buffer, insize);
    OSData *in = OSData::withBytesNoCopy(&args, sizeof(struct bios_args));

    OSObject *result;

    DebugLog("BIOS query 0x%x: cmd %d mid %d insize %d in.data[0..3] %02x %02x %02x %02x",
             query, command, mid, insize, args.data[0], args.data[1], args.data[2], args.data[3]);
    IOReturn ret = YWMI->evaluateMethod(BIOS_QUERY_WMI_METHOD, 0, mid, &result, in);
    OSSafeReleaseNULL(in);

    if (ret != kIOReturnSuccess) {
        AlwaysLog("BIOS query 0x%x: evaluation failed", query);
        OSSafeReleaseNULL(result);
        return false;
    }
#ifdef DEBUG
    setProperty("WMIQuery", result);
#endif
    OSData *output = OSDynamicCast(OSData, result);
    if (output == nullptr) {
        AlwaysLog("BIOS query 0x%x: unexpected output type", query);
        OSSafeReleaseNULL(result);
        return false;
    }

    {
        // x2g2: dump the raw reply (sigpass, return_code, first data bytes)
        const UInt8 *b = reinterpret_cast<const UInt8 *>(output->getBytesNoCopy());
        UInt32 len = output->getLength();
        char hex[3 * 24 + 1] = {0};
        for (UInt32 k = 0; k < len && k < 24; k++)
            snprintf(hex + 3 * k, 4, "%02x ", b[k]);
        AlwaysLog("BIOS query 0x%x: reply len %d: %s", query, len, hex);
    }

    const struct bios_return *biosRet = reinterpret_cast<const struct bios_return*>(output->getBytesNoCopy());
    switch (biosRet->return_code) {
        case 0:
            ret = true;
            break;
            
        case HPWMI_RET_UNKNOWN_COMMAND:
            DebugLog("BIOS query 0x%x: unknown COMMAND", query);
            ret = false;
            break;
            
        case HPWMI_RET_UNKNOWN_CMDTYPE:
            DebugLog("BIOS query 0x%x: unknown CMDTYPE", query);
            ret = false;
            break;
            
        default:
            AlwaysLog("BIOS query 0x%x: return code error - %d", query, biosRet->return_code);
            ret = false;
            break;
    }

    if (ret && outsize != 0) {
        memset(buffer, 0, outsize);
        outsize = min(outsize, output->getLength() - sizeof(*biosRet));
        memcpy(buffer, output->getBytesNoCopy(sizeof(*biosRet), outsize), outsize);
    }

    output->release();
    return ret;
}

void DYVPC::setPropertiesGated(OSObject *props) {
    OSDictionary *dict = OSDynamicCast(OSDictionary, props);
    if (!dict)
        return;

//    AlwaysLog("%d objects in properties", dict->getCount());
    OSCollectionIterator* i = OSCollectionIterator::withCollection(dict);

    if (i) {
        while (OSString* key = OSDynamicCast(OSString, i->getNextObject())) {
            if (key->isEqualTo("BIOSQuery")) {
                if (!BIOSCap) {
                    AlwaysLog(notSupported, "BIOSQuery");
                    continue;
                }

                OSNumber *value;
                getPropertyNumber("BIOSQuery");

                UInt32 result;

                if (WMIQuery(value->unsigned32BitValue(), &result))
                    AlwaysLog("%s 0x%x result: 0x%x", "BIOSQuery", value->unsigned32BitValue(), result);
                else
                    AlwaysLog("%s failed 0x%x", "BIOSQuery", value->unsigned32BitValue());
            } else if (key->isEqualTo("SetSHK")) {
                // x2g2 debug: write the EC hotkey-mode byte (SHK, EC 0xE6) through the BIOS's own EC0.SSHK.
                // 0x00 = normal keys (power-on default), 0x6e = WMI hotkey mode (what Windows/Linux hp-wmi set).
                OSNumber *value;
                getPropertyNumber("SetSHK");

                UInt32 v = value->unsigned32BitValue();
                if (v > 0xFF) {
                    AlwaysLog("SetSHK: 0x%x out of range", v);
                    continue;
                }
                UInt32 before = 0xFFFF, after = 0xFFFF;
                readECName("SHK_", &before);
                OSObject *params[1] = { OSNumber::withNumber(v, 8) };
                IOReturn r = ec->evaluateObject("SSHK", nullptr, params, 1);
                params[0]->release();
                readECName("SHK_", &after);
                AlwaysLog("SetSHK 0x%02x: SSHK ret 0x%x, SHK 0x%02x -> 0x%02x", v, r, before, after);
            } else if (key->isEqualTo("BIOSQueryRaw")) {
                // x2g2 debug: value = 0xTTMM (commandtype, method id 1-5); READ only, data 0
                if (!BIOSCap) {
                    AlwaysLog(notSupported, "BIOSQueryRaw");
                    continue;
                }

                OSNumber *value;
                getPropertyNumber("BIOSQueryRaw");

                UInt32 raw = value->unsigned32BitValue();
                UInt32 type = (raw >> 8) & 0xFF, mid = raw & 0xFF;
                if (mid < 1 || mid > 5) {
                    AlwaysLog("BIOSQueryRaw: mid %d out of range 1-5", mid);
                    continue;
                }
                UInt32 result = 0;
                bool ok = WMIQuery(type, &result, HPWMI_READ, sizeof(UInt32), sizeof(UInt32), mid);
                AlwaysLog("BIOSQueryRaw type 0x%x mid %d: %s result 0x%x", type, mid, ok ? "ok" : "failed", result);
            } else {
                OSDictionary *entry = OSDictionary::withCapacity(1);
                entry->setObject(key, dict->getObject(key));
                super::setPropertiesGated(entry);
                entry->release();
            }
        }
        i->release();
    }

    return;
}

bool DYVPC::examineWMI(IOService *provider) {
    OSString *feature;
    if ((feature = OSDynamicCast(OSString, provider->getProperty("Feature"))) &&
        feature->isEqualTo("Sensor")) {
        setProperty("DYSensor", provider);
    }
    return true;
}

IOService* DYVPC::initWMI(WMI *instance) {
    return YogaWMI::withDYWMI(instance);
}
