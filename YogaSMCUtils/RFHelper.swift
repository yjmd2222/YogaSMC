//
//  RFHelper.swift
//  YogaSMCNC
//
//  Created by Zhen on 10/16/20.
//  Copyright © 2020 Zhen. All rights reserved.
//

import Foundation
import IOBluetooth
import IOKit
import os.log
import CoreWLAN

func bluetoothHelper(_ name: String, _ display: Bool) {
    guard IOBluetoothPreferencesAvailable() != 0 else {
        showOSDRes("Bluetooth", "Unavailable", .kBluetooth)
        if #available(macOS 10.12, *) {
            os_log("Bluetooth unavailable!", type: .error)
        }
        return
    }
    let status = (IOBluetoothPreferenceGetControllerPowerState() == 0)
    IOBluetoothPreferenceSetControllerPowerState(status ? 1 : 0)
    if display {
        showOSDRes(name.isEmpty ? "Bluetooth" : name, status ? "On" : "Off", .kBluetooth)
    }
}

func bluetoothDiscoverableHelper(_ name: String, _ display: Bool) {
    guard IOBluetoothPreferencesAvailable() != 0 else {
        showOSDRes("Bluetooth", "Unavailable", .kBluetooth)
        if #available(macOS 10.12, *) {
            os_log("Bluetooth unavailable!", type: .error)
        }
        return
    }
    let status = (IOBluetoothPreferenceGetDiscoverableState() == 0)
    IOBluetoothPreferenceSetDiscoverableState(status ? 1 : 0)
    if display {
        showOSDRes(name.isEmpty ? "BT Discoverable" : name, status ? "On" : "Off", .kBluetooth)
    }
}

func wirelessHelper(_ name: String, _ display: Bool) {
    guard let iface = CWWiFiClient.shared().interface() else {
        showOSDRes("Wireless", "Unavailable", .kWifi)
        if #available(macOS 10.12, *) {
            os_log("Wireless unavailable!", type: .error)
        }
        return
    }
    let status = !iface.powerOn()
    do {
        try iface.setPower(status)
        if display {
            showOSDRes(name, status ? "On" : "Off", status ? .kWifi : .kWifiOff)
        }
    } catch {
        showOSDRes("Wireless", "Toggle failed", .kWifi)
        if #available(macOS 10.12, *) {
            os_log("Wireless toggle failed!", type: .error)
        }
    }
}

func airplaneModeHelper(_ name: String, _ display: Bool) {
    guard IOBluetoothPreferencesAvailable() != 0 else {
        showOSDRes("Bluetooth", "Unavailable", .kBluetooth)
        if #available(macOS 10.12, *) {
            os_log("Bluetooth unavailable!", type: .error)
        }
        return
    }
    guard let iface = CWWiFiClient.shared().interface() else {
        showOSDRes("Wireless", "Unavailable", .kWifi)
        if #available(macOS 10.12, *) {
            os_log("Wireless unavailable!", type: .error)
        }
        return
    }
    let status = (IOBluetoothPreferenceGetDiscoverableState() == 0 && !iface.powerOn())
    do {
        try iface.setPower(status)
        IOBluetoothPreferenceSetControllerPowerState(status ? 1 : 0)
        if display {
            showOSDRes(name, status ? "Off" : "On", status ? .kAntenna : .kAirplaneMode)
        }
    } catch {
        showOSDRes("Wireless", "Toggle failed", .kWifi)
        if #available(macOS 10.12, *) {
            os_log("Wireless toggle failed!", type: .error)
        }
    }
}

// MARK: - HP wireless button

// On HP machines (e.g. Elite x2 1012 G2) the EC toggles the card's hardware RF-kill line when the
// Fn wireless button is pressed, and sends WMI event 0x05 with no state. The Intel Bluetooth USB device
// disappears on kill and re-enumerates on release, but macOS leaves Bluetooth powered off afterwards.
// So: look a few seconds later whether the Bluetooth USB device is back, then fix up Bluetooth power,
// and show the matching OSD.

private var hpBluetoothWasOn = true

private func intelBluetoothPresent() -> Bool {
    guard let match = IOServiceMatching("IOUSBHostDevice") as NSMutableDictionary? else { return false }
    // Arbitrary properties must go under IOPropertyMatch; top-level keys are ignored here (matched 0 devices)
    match[kIOPropertyMatchKey] = ["idVendor": 0x8087, "bDeviceClass": 0xE0]
    var iter: io_iterator_t = 0
    guard IOServiceGetMatchingServices(kIOMasterPortDefault, match, &iter) == kIOReturnSuccess else { return false }
    defer { IOObjectRelease(iter) }
    guard let device = IOIteratorNextOptional(iter) else { return false }
    IOObjectRelease(device)
    return true
}

private func hpRestoreBluetooth(_ attempt: Int) {
    guard hpBluetoothWasOn, IOBluetoothPreferencesAvailable() != 0 else { return }
    if IOBluetoothPreferenceGetControllerPowerState() == 0 {
        IOBluetoothPreferenceSetControllerPowerState(1)
        if #available(macOS 10.12, *) {
            os_log("HP wireless: Bluetooth power on (attempt %d)", type: .info, attempt)
        }
    }
    // bluetoothd may still be attaching to the re-enumerated controller; check again
    if attempt < 5 {
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { hpRestoreBluetooth(attempt + 1) }
    }
}

func hpWirelessHelper(_ name: String, _ display: Bool, _ service: io_service_t) {
    // The Bluetooth device is still present right after a "kill" press: remember whether it was on
    if intelBluetoothPresent(), IOBluetoothPreferencesAvailable() != 0 {
        hpBluetoothWasOn = IOBluetoothPreferenceGetControllerPowerState() != 0
    }

    DispatchQueue.main.asyncAfter(deadline: .now() + 3) {
        if intelBluetoothPresent() {
            hpRestoreBluetooth(1)
            if display { showOSDRes(name, "On", .kAntenna) }
        } else {
            if display { showOSDRes(name, "Off", .kAirplaneMode) }
        }
    }
}
