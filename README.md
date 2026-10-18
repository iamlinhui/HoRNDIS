# HoRNDIS(the USB tethering driver for Mac OS X)

**HoRNDIS** (pronounce: *"horrendous"*) is a driver for Mac OS X that allows you to use your Android phone's native [USB tethering](http://en.wikipedia.org/wiki/Tethering) mode to get Internet access.

For more information, [visit the home page for HoRNDIS on my site](http://www.joshuawise.com/horndis).

## Installation

### From Source/Binary

* Get the installation package ([Download](http://www.joshuawise.com/horndis) or [Build](#building-the-source) the installation package from source yourself)
* Run the installation package

### From Homebrew

```sh
brew cask install horndis
sudo kextload /Library/Extensions/HoRNDIS.kext
```

## Configuration

* Assuming that the installation proceeds without errors, after it completes, connect your phone to your Mac by USB.
* Enter the settings menu on your phone.
* In the connections section, below Wi-Fi and Bluetooth:
  * Select "More..."
  * Select "Tethering & portable hotspot"
* Check the "USB tethering" box. It should flash once, and then become solidly checked.

## Uninstallation

* Delete the `HoRNDIS.kext` under `/System/Library/Extensions` and `/Library/Extensions` folder
* Restart your computer

## Building the source

* `git clone` the repository
* Simply running xcodebuild in the checkout directory should be sufficient to build the kext.
* If you wish to package it up, you can run `make` to assemble the package in the build/ directory

## Performance Tuning

The driver has several compile-time knobs in `HoRNDIS.h` that control the
performance-critical data paths. They default to the "SuperSpeed" (USB 3.x)
configuration:

| Knob | Default | Meaning |
| --- | --- | --- |
| `OUT_BUF_SIZE` | `16384` | TX buffer size. Multiple RNDIS PDUs are packed into one USB transfer (RNDIS data messages carry "1-N packets" per the spec). |
| `TX_AGGREGATE` | `1` | `1` = aggregate several packets per transfer (timer-flushed); `0` = one transfer per packet (legacy behavior). Set to `0` if a device's RNDIS firmware mishandles multi-PDU messages. |
| `TX_FLUSH_US` | `1000` | How long a partially-filled TX aggregate waits before being flushed (µs). Lower = less added latency; higher = better small-packet aggregation. |
| `RX_ZEROCOPY` | `1` | Received packets are handed to the stack as mbuf clusters referencing the USB receive buffer (`mbuf_attachcluster` + a per-buffer lease refcount) instead of being copied. `0` = legacy copy path. |
| `N_IN_BUFS` | `2` | Concurrent in-flight RX transfers. `2` wins on USB3 full-duplex links; on a device that still negotiates USB2, `1` may perform better (half-duplex bus contention). |
| `RNDIS_DYNAMIC_FILTER` | `1` | Keep the device's packet filter minimal (unicast+broadcast), adding multicast-all / promiscuous only while the network stack asks for them. Stops the host from receiving every frame on the tether segment. `0` = legacy always-open filter. |
| `RNDIS_KEEPALIVE` | `1` | Send `REMOTE_NDIS_KEEPALIVE_MSG` on the control channel every 5 s while the interface is up, per the RNDIS spec (fire-and-forget; the response is drained by the next command). |
| `RNDIS_CMD_RETRIES` / `RNDIS_CMD_WAIT_US` | `10` / `10000` | Control command patience: up to 10 waits of 10 ms for the device to produce a response. |
| `TX_ZEROCOPY` | `1` | The outbound RNDIS transfer is an mbuf chain submitted through a scatter-gather descriptor (per-mbuf `withAddress` + `IOMultiMemoryDescriptor`) instead of copying payload bytes. `0` = copy-based aggregation. |
| `NCM_SUPPORT` | `1` | Additionally implement CDC NCM (Ethernet over USB, NTB16). The protocol is chosen at probe time: a `02/0D/00` control interface picks NCM, the RNDIS personalities keep RNDIS. NCM requires `RX_ZEROCOPY=1`. Set to `0` for a pure RNDIS build. |

### RNDIS protocol layer

The control channel was reworked to follow the RNDIS-over-USB mapping:

- The control interface's **interrupt endpoint is now opened**: response-available
  notifications wake pending commands, and `MEDIA_CONNECT` / `MEDIA_DISCONNECT`
  indications drive the reported link state (e.g. the stack fails over when the
  module drops the connection). Devices without an interrupt pipe fall back to
  bounded polling.
- `rndisCommand` no longer busy-waits with `IOSleep` while holding the command
  gate: it sleeps with the **gate released** and serializes concurrent callers.
- The **packet filter** now follows the stack's multicast/promiscuous requests
  instead of being permanently wide open.
- The **MAC address is queried once** and cached for the lifetime of the driver
  instance.
- If the receive path dies while the device is still attached, the driver now
  reports **link down** instead of pretending everything is fine.

Expected effects on a SuperSpeed (USB 3.x) device: RX is zero-copy, and TX
transfers carry ~10 full-size frames each, removing most per-transfer
overhead. The advertised medium speed now follows the negotiated USB speed
(5 Gb/s for SuperSpeed instead of always 480 Mb/s).

### CDC NCM support

With `NCM_SUPPORT=1` (default) the driver speaks **both** protocols and picks
one at probe time:

- A control interface with class/subclass/protocol `02/0D/00` (CDC Network
  Control Model) selects **NCM**; the data interface is located via the CDC
  Union functional descriptor.
- Any interface matching the RNDIS personalities (`224/1/3`, `239/4/1`,
  `2/2/255`) keeps the **RNDIS** path.

The NCM layer follows Linux `cdc_ncm.c`: NTB16 format, `GET_NTB_PARAMETERS`
negotiation (sizes clamped to 2K-64K), `SET_NTB_INPUT_SIZE` before RX, NDP
table parsing with zero-copy `mbuf_split` extraction, TX NTB aggregation
with the device's datagram alignment rules, and CDC `NETWORK_CONNECTION`
notifications driving the link state. If the data interface exposes **two
bulk endpoint pairs**, both are used (concurrent RX reads, alternating TX
submits). NCM requires `RX_ZEROCOPY=1`.

For a module like the TD Tech MT5700M-CN:

```sh
# NCM (recommended by the module vendor):
AT^SETMODE=4          # Linux NCM Normal (the factory default)
AT^SETAUTODIAL=1,1    # auto-dial over USB 3.0
# RNDIS (fallback):
AT^SETMODE=6          # Windows RNDIS
```

Verify in the kernel log: `Matched a NCM control interface`, the
`NCM: rxMax=... txMax=...` parameter dump, and `submitted pkt sz` lines.

### Verifying a build

```sh
# From the checkout directory (Xcode with the macOS SDK):
xcodebuild -project HoRNDIS.xcodeproj
# or, to also assemble the installer package (set CODESIGN_INST=none if you
# have no installer certificate):
make CODESIGN_INST=none
```

Then load the freshly built kext and watch the log:

```sh
sudo kextutil -v build/Release/HoRNDIS.kext
sudo kextload /Library/Extensions/HoRNDIS.kext
log stream --predicate 'process == "kernel"' | grep HoRNDIS
```

### What to check on the target device

1. **USB speed negotiated**: *System Report → Hardware → USB* — the device
   should show `Up to 5 Gb/s`. If it says `Up to 480 Mb/s`, the enclosure,
   adapter or cable is USB2 and no driver change will lift that cap.
2. **Throughput**: `iperf3` between the Mac and a host behind the tether;
   compare with `TX_AGGREGATE=0` / `RX_ZEROCOPY=0` builds to isolate which
   change helps.
3. **Latency**: `ping` a nearby host — the `TX_FLUSH_US` knob adds at most
   one flush interval of latency to idle TX flows.
4. **Stability**: watch the kernel log for `mbuf_split failed`,
   `non-PACKET over data channel` or `write failed` messages. Multi-PDU TX
   aggregation relies on the device implementing the RNDIS data message
   format correctly; `TX_AGGREGATE=0` is the fallback if it does not.

## Debugging and Development Notes

This sections contains tips and tricks for developing and debugging the driver.

### USB Device Information

*Mac OS System Menu* -> *About This Mac* -> *System Report* --> *Hardware*/*USB* <br>
Lists all USB devices that OS recognizes. Unfortunately, it does not give USB descriptors.

`lsusb -v`<br>
It prints USB configuration, such as interface and endpoint descriptors. You can print it for all devices or limit the output to specific ones. In order to run this command, you need to install *usbutils*.
* Homebrew users: `brew install mikhailai/misc/usbutils`<br>
  Please *do not* install *lsusb* package from Homebrew Core, it's a different utility with the same name.
* Macports users: `sudo port install usbutils`

### IO Registry

`ioreg -l -r -c IOUSBHostDevice`<br>
This command lists all the Mac OS IO Registry information under all USB devices. Unlike *lsusb*, ioreg tells how Mac OS recognized USB devices and interfaces, and how it matched drivers to these interfaces. The `-r -c IOUSBHostDevice` limits the output to USB devices; to get complete OS registry, please run `ioreg -l`.

### OS Logging

The `LOG(....)` statements, sprinkled throughout the HoRNDIS code, call the `IOLog` functions. On Mac OS *El Capitan* (10.11) and earlier, the log messages go into `/var/log/system.log`. Starting from *Sierra* (10.12), these messages are no longer written to `system.log` and instead can be viewed via:
* *GUI*, using *Console* application, located in *Utilities* folder. You need to enter `process:kernel` in the search box in order to filter the relevant messages.
* *Command Line*, using the `log` command. For example:<br>
  `log show --predicate process==\"kernel\" --start "2018-12-11 22:54:00"`<br>
  The start value needs to be several minutes in the past, so it would not flood the console. Here is a convenient command that prints the messages from the past 3 mintes:<br>
  `log show --predicate process==\"kernel\" --start "$(date -v-3M +'%F %T')"`

I've observed that Mac OS logging is unreliable (especially in *Sierra*). In some cases, the messages may come out garbled (looking like bad multi-threaded code). In other cases, either GUI or Command Line may be missing messages that were emitted. Sometimes, reloading the driver may fix the problem.
