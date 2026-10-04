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

// HP Fn wireless (airplane) button, e.g. Elite x2 1012 G2 with an Intel AX210 on itlwm + IntelBluetoothFirmware.
//
// What the hardware does: the EC toggles the card's hardware RF-kill line by itself (Wi-Fi and Bluetooth off/on)
// and the firmware's GPE handler (_L62) sends HP WMI event 0x05 with no on/off state. The event arrives right at
// the key press, slightly before the radios switch. The Intel Bluetooth USB device (vendor 0x8087, class 0xE0)
// disappears ~0.1 s after an "off" press and re-enumerates ~0.3 s after an "on" press.
//
// Icon (instant): the event has no state, so we keep our own radio state and flip it on every press. It starts
// from "Bluetooth USB device present" when first needed, and is re-synced whenever that device appears or
// disappears, so a missed press cannot leave the icons reversed for long.
//
// Bluetooth recovery (waits for the hardware): after the device re-enumerates, macOS leaves Bluetooth powered
// off, and bluetoothd crashes ~10 s later if nothing turns it on (acidanthera/bugtracker#1821, BlueToolFixup
// known issue); before this, only `killall bluetoothd` or a reboot brought Bluetooth back. Simply turning the
// controller power on from user space is enough (tested on the x2, 2026-10-02), so no root helper is needed.
// We do that from an IOKit "device appeared" notification, retried while bluetoothd attaches, and only if
// Bluetooth was on before the "off" press.

private var hpBluetoothWasOn = true
private var hpRadiosOn: Bool?           // our view of the radio state; nil = not known yet
private var hpRestorePending = false    // an "on" press is waiting for the Bluetooth device to come back
private var hpNotifyPort: IONotificationPortRef?

private func intelBluetoothMatching() -> NSMutableDictionary? {
    guard let match = IOServiceMatching("IOUSBHostDevice") as NSMutableDictionary? else { return nil }
    // Arbitrary properties must go under IOPropertyMatch; top-level keys are ignored here (matched 0 devices)
    match[kIOPropertyMatchKey] = ["idVendor": 0x8087, "bDeviceClass": 0xE0]
    return match
}

private func intelBluetoothPresent() -> Bool {
    guard let match = intelBluetoothMatching() else { return false }
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

// Drains an IOKit notification iterator (required to re-arm it) and reports whether it held anything.
private func hpDrain(_ iterator: io_iterator_t) -> Bool {
    var any = false
    while let device = IOIteratorNextOptional(iterator) {
        IOObjectRelease(device)
        any = true
    }
    return any
}

private func hpBluetoothAppeared() {
    hpRadiosOn = true
    if #available(macOS 10.12, *) {
        os_log("HP wireless: Bluetooth device appeared", type: .info)
    }
    if hpRestorePending {
        hpRestorePending = false
        hpRestoreBluetooth(1)
    }
}

private func hpBluetoothDisappeared() {
    hpRadiosOn = false
    if #available(macOS 10.12, *) {
        os_log("HP wireless: Bluetooth device gone", type: .info)
    }
}

// Registers once for "Intel Bluetooth USB device appeared / gone" on the main run loop.
private func hpWatchBluetooth() {
    guard hpNotifyPort == nil, let port = IONotificationPortCreate(kIOMasterPortDefault) else { return }
    hpNotifyPort = port
    CFRunLoopAddSource(CFRunLoopGetMain(), IONotificationPortGetRunLoopSource(port).takeUnretainedValue(), .defaultMode)

    var added: io_iterator_t = 0
    if let match = intelBluetoothMatching(),
       IOServiceAddMatchingNotification(port, kIOFirstMatchNotification, match, { _, iterator in
           if hpDrain(iterator) { hpBluetoothAppeared() }
       }, nil, &added) == kIOReturnSuccess {
        _ = hpDrain(added)  // arm; the device present at start is not an "appeared" event
    }

    var removed: io_iterator_t = 0
    if let match = intelBluetoothMatching(),
       IOServiceAddMatchingNotification(port, kIOTerminatedNotification, match, { _, iterator in
           if hpDrain(iterator) { hpBluetoothDisappeared() }
       }, nil, &removed) == kIOReturnSuccess {
        _ = hpDrain(removed)
    }
}

func hpWirelessHelper(_ name: String, _ display: Bool, _ service: io_service_t) {
    hpWatchBluetooth()
    let wasOn = hpRadiosOn ?? intelBluetoothPresent()
    let nowOn = !wasOn
    hpRadiosOn = nowOn
    if #available(macOS 10.12, *) {
        os_log("HP wireless: button, radios %{public}@", type: .info, nowOn ? "on" : "off")
    }

    // Icon right away, from our own state
    if display { showOSDRes(name, nowOn ? "On" : "Off", nowOn ? .kAntenna : .kAirplaneMode) }

    if nowOn {
        if intelBluetoothPresent() {
            hpRestoreBluetooth(1)       // already back (or never left)
        } else {
            hpRestorePending = true     // hpBluetoothAppeared() finishes the job
            // Fallback in case the notification is missed
            DispatchQueue.main.asyncAfter(deadline: .now() + 5) {
                if hpRestorePending, intelBluetoothPresent() {
                    hpRestorePending = false
                    hpRestoreBluetooth(1)
                }
            }
        }
    } else {
        hpRestorePending = false
        // The "off" event arrives before the radios switch, so the device and its power state are still readable
        if intelBluetoothPresent(), IOBluetoothPreferencesAvailable() != 0 {
            hpBluetoothWasOn = IOBluetoothPreferenceGetControllerPowerState() != 0
        }
    }
}
