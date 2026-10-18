/* HoRNDIS.cpp
 * Implementation of IOKit-derived classes
 * HoRNDIS, a RNDIS driver for Mac OS X
 *
 *   Copyright (c) 2012 Joshua Wise.
 *   Copyright (c) 2018 Mikhail Iakhiaev
 *
 * IOKit examples from Apple's USBCDCEthernet.cpp; not much of that code remains.
 *
 * RNDIS logic is from linux/drivers/net/usb/rndis_host.c, which is:
 *
 *   Copyright (c) 2005 David Brownell.
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "HoRNDIS.h"

#include <mach/kmod.h>
#include <libkern/version.h>
#include <IOKit/IOKitKeys.h>

#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/network/IOGatedOutputQueue.h>
// May be useful for supporting suspend/resume:
// #include <IOKit/pwr_mgt/RootDomain.h>


#define V_PTR 0
#define V_PACKET 1
#define V_DEBUG 2
#define V_NOTE 3
#define V_ERROR 4

// The XCode "Debug" build is now more verbose:
#if DEBUG == 1
	#define DEBUGLEVEL V_DEBUG
#else
	#define DEBUGLEVEL V_NOTE
#endif
#define LOG(verbosity, s, ...) do { if (verbosity >= DEBUGLEVEL) IOLog("HoRNDIS: %s: " s "\n", __func__, ##__VA_ARGS__); } while(0)

#define super IOEthernetController

OSDefineMetaClassAndStructors(HoRNDIS, IOEthernetController);
OSDefineMetaClassAndStructors(HoRNDISInterface, IOEthernetInterface);

/* 
================================================================
DESCRIPTION OF DEVICE DRIVER MATCHING (+ Info.plist description)
================================================================
The HoRNDIS driver classes are only instantiated when the MacOS matches 
"IOKitPersonalities" dictionary entries to the existing devices. The matching 
can be done 2 based on two different provider classes:
 
 - IOUSBHostInterface - matches an interface under a USB device. Here, we match
   based on the interface class/subclass/protocol. In order for the matching to
   work, some other driver has to open the USB device and call 
   "setConfiguration" method with matchInterfaces=true.
   The interface matching works out-of-the-box for interfaces under USB
   Composite Devices (class/subclass/protocol are 0/0/0), since there is an OS 
   driver that opens such devices for matching.
 
 - IOUSBHostDevice - match is performed on the whole device. Here, we match 
   based on class/subclass/protocol. The start method needs to call 
   'setConfiguration' in order for any IOUSBHostInterface instances under the 
   device to become available. If it specifies 'matchInterfaces', matching is 
   then performed on newly-created IOUSBHostInterfaces.

OUR APPROACH:
We'll match based on either device or interface, and let the probe and start 
methods handle the difference. Not calling setConfiguration with matchInterfaces
for now: if it's not a USB composite device, consider that we own it.
 
Subsequent logic:
After MacOS finds a match based on Info.plist, it instantiates the driver class,
and calls the 'probe' method that looks at descriptors and decides if this is 
really the device we care about (e.g. fine-grained filtering), and sets the 
"probeXxx" variables. Then, 'start' method calls 'openUSBInterfaces' that 
blindly follows the "probeXxx" variables to get the needed IOUSBHostInterface 
values from the opened device.

==========================
||  DEVICE VARIATIONS
==========================
This section must document ALL different variations of the devices that we
may be dealing with, so we have the whole picture when updating the "probe"
code or "Info.plist". Notation:
 * Device: 224 / 0 / 0
   - bDeviceClass / bDeviceSubClass / bDeviceProtocol
 * Interface Associaton[2]: 224 / 1 / 3
   - [bInterfaceCount]:  bFunctionClass / bFunctionSubClass / bFunctionProtocol
 * Interface: 224 / 3 / 1
   -  bInterfaceClass / bInterfaceSubClass / bInterfaceProtocol

[*] "Stock" Android. I believe most Android phone tethering should behave
    this way
    * USBCompositeDevice: 0 / 0 / 0
      - InterfaceAssociation[2] 224 / 1 / 3
        - ControlInterface: 224 / 1 / 3
        - DataInterface:     10 / 0 / 0
    * Info.plist entry: RNDISControlStockAndroid(interface)

[*] Linux USB Gadget drivers. Location:
    <LINUX_KERNEL>/drivers/usb/gadget/function/f_rndis.c
    These show up in various embedded Linux boards, such as Beagle Board,
    Analog Devices PlutoSDR, etc.
    * USBCompositeDevice: 0 / 0 / 0
      - InterfaceAssociation[2]: Configurable (e.g. 2/6/0, 239/4/1).
        - ControlInterface:  2 / 2 / 255
        - DataInterface:    10 / 0 / 0
    * Info.plist entry: RNDISControlLinuxGadget(interface)

[*] Wireless Controller Device (class 224). Some Samsung phones (e.g. S7 Edge) 
    specify device class 224 for tethering, instead of just being a USB
    composite device. The rest is the same as in "stock" Android.
    Note, the other Samsung phones (e.g. S8) behave like other Android devices.
	* Device: 224 / 0 / 0
	  - (same as "Stock" Android)

[*] Composite Device, using 0xEF/4/1 for RNDIS control: Nokia 7 Plus (issue #88)
    Also may apply to Sony Xperia XZ.
    This matches "RNDIS over Ethernet" specification given here:
	http://www.usb.org/developers/defined_class/#BaseClassEFh
    * USBCompositeDevice: 0 / 0 / 0
	  - InterfaceAssociation[2]: 239 / 4 / 1
        - ControlInterface: 239 / 4 / 1
        - DataInterface:     10 / 0 / 0
	* Info.plist entry: RNDISControlMiscDeviceRoE(interface)
*/

// Detects the 224/1/3 - stock Android RNDIS control interface.
static inline bool isRNDISControlStockAndroid(const InterfaceDescriptor *idesc) {
	return idesc->bInterfaceClass == 224  // Wireless Controller
		&& idesc->bInterfaceSubClass == 1  // Radio Frequency
		&& idesc->bInterfaceProtocol == 3;  // RNDIS protocol
}

// Miscellaneous Device (0xEF), RNDIS over Ethernet: some phones, see above.
static inline bool isRNDISControlMiscDeviceRoE(const InterfaceDescriptor *idesc) {
	return idesc->bInterfaceClass == 239  // Miscellaneous Device
		&& idesc->bInterfaceSubClass == 4  // RNDIS?
		&& idesc->bInterfaceProtocol == 1;  // RNDIS over Ethernet
}

// Detects RNDIS control on BeagleBoard and possibly other embedded Linux devices.
static inline bool isRNDISControlLinuxGadget(const InterfaceDescriptor *idesc) {
	return idesc->bInterfaceClass == 2  // Communications / CDC Control
		&& idesc->bInterfaceSubClass == 2  // Abstract (modem)
		&& idesc->bInterfaceProtocol == 255;  // Vendor Specific (RNDIS).
}

// Any of the above RNDIS control interface.
static inline bool isRNDISControlInterface(const InterfaceDescriptor *idesc) {
	return isRNDISControlStockAndroid(idesc)
		|| isRNDISControlLinuxGadget(idesc)
		|| isRNDISControlMiscDeviceRoE(idesc);
}

// Detects the class 10 - CDC data interface.
static inline bool isCDCDataInterface(const InterfaceDescriptor *idesc) {
	// Check for CDC class. Sub-class and Protocol are undefined:
	return idesc->bInterfaceClass == 10;
}

#if NCM_SUPPORT
// Detects the 2/0x0D/0 - CDC NCM control interface.
static inline bool isNCMControlInterface(const InterfaceDescriptor *idesc) {
	return idesc->bInterfaceClass == NCM_IF_CLASS
		&& idesc->bInterfaceSubClass == NCM_IF_SUBCLASS
		&& idesc->bInterfaceProtocol == 0;
}

// Parse the CDC Union functional descriptor from a control interface's
// extra descriptor bytes; returns the subordinate (data) interface number,
// or -1 when absent/malformed.
static inline int getUnionDataIfNum(const InterfaceDescriptor *idesc) {
	const uint8_t *p = (const uint8_t *)idesc + 9;  // Past the fixed fields.
	int remaining = idesc->bLength - 9;
	while (remaining >= 3) {
		uint8_t len = p[0];
		if (len < 3 || len > remaining) {
			break;  // Malformed extra bytes.
		}
		if (p[1] == 0x24 /* CS_INTERFACE */ &&
			p[2] == USB_CDC_UNION_TYPE) {
			return (len >= 5) ? p[4] : -1;  // bSubordinateInterface0.
		}
		p += len;
		remaining -= len;
	}
	return -1;
}
#endif

bool HoRNDIS::init(OSDictionary *properties) {
	extern kmod_info_t kmod_info;  // Getting the version from generated file.
	LOG(V_NOTE, "HoRNDIS tethering driver for Mac OS X, %s", kmod_info.version);
	
	if (super::init(properties) == false) {
		LOG(V_ERROR, "initialize superclass failed");
		return false;
	}

	LOG(V_PTR, "PTR: I am: %p", this);
	
	fNetworkInterface = NULL;
	fpNetStats = NULL;

	fReadyToTransfer = false;
	fNetifEnabled = false;
	fEnableDisableInProgress = false;
	fDataDead = false;

	fProbeConfigVal = 0;
	fProbeCommIfNum = 0;
	fProbeDataIfNum = 0;
	fProtocol = kProtocolRNDIS;

	fCallbackCount = 0;

	fCommInterface = NULL;
	fDataInterface = NULL;
	
	fInPipe = NULL;
	fOutPipe = NULL;
	fInPipe2 = NULL;
	fOutPipe2 = NULL;
	fTxOutPipeToggle = false;
	fIntPipe = NULL;
	fIntBuf.mdp = NULL;
	fIntBuf.dmaBuf = NULL;
#if RX_ZEROCOPY
	fIntBuf.mbufMdp = NULL;
	fIntBuf.m = NULL;
#endif
	fIntReadActive = false;

	fCmdInProgress = false;
	fCmdWaitActive = false;
	fCmdWaitEvent = false;
	fCtrlTimer = NULL;
	fCtrlTimerPurpose = kCtrlTimerNone;
	fCtrlTimerArmed = false;
	fKeepaliveEnabled = false;

	fCurrentFilter = 0;
	fMulticastEnabled = false;
	fPromiscEnabled = false;
	fHasHardwareAddress = false;

	numFreeOutBufs = 0;
	for (int i = 0; i < N_OUT_BUFS; i++) {
		outbufs[i].mdp = NULL;
		outbufs[i].dmaBuf = NULL;
		outbufStack[i] = i;  // Value does not matter here.
	}
	for (int i = 0 ; i < N_IN_BUFS; i++) {
		inbufs[i].mdp = NULL;
		inbufs[i].dmaBuf = NULL;
#if RX_ZEROCOPY
		inbufs[i].mbufMdp = NULL;
		inbufs[i].m = NULL;
#endif
	}

	fTxFillBufIdx = -1;
	fTxFillOffset = 0;
	fTxFlushTimer = NULL;
	fTxTimerArmed = false;
#if TX_ZEROCOPY
	fTxHead = NULL;
	fTxBytes = 0;
#endif
#if NCM_SUPPORT
	fNcmRxMax = 0;
	fNcmTxMax = 0;
	fNcmNdpAlign = NCM_NDP_ALIGN_MIN;
	fNcmModulus = NCM_NDP_ALIGN_MIN;
	fNcmRemainder = 0;
	fNcmMaxDatagrams = 0;
	fNcmMaxNdpSize = 0;
	fNcmSeq = 0;
	fNcmMaxDatagram = NCM_MIN_DATAGRAM_SIZE;
	fTxNdpOffset = 0;
	fTxNdpCount = 0;
#endif
	fNeedReadRestart = false;

	rndisXid = 1;
	maxOutTransferSize = 0;
	
	return true;
}

void HoRNDIS::free() {
	// Here, we shall free everything allocated by the 'init'.

	LOG(V_NOTE, "driver instance terminated");  // For the default level
	super::free();
}

/*
==================================================
INTERFACE PROLIFERATION AND PROVIDER CLASS NAME
==================================================
PROBLEM:
 Every time you connect the same Android device (or somewhat more rarely),
 MacOS creates a new entry under "Network" configurations tab. These entries
 keep on coming on and on, polluting the configuration.
 
ROOT CAUSE:
 Android devices randomly-generate Ethernet MAC address for
 RNDIS interface, so the system may think there is a new device
 every time you connect an Android phone, and may create a new
 network interface every such time.
 Luckily, it does extra check when Network Provider is a USB device:
 in that case, it would match based on USB data, creating an entry like:
		<key>SCNetworkInterfaceInfo</key>
		<dict>
			<key>USB Product Name</key>
			<string>Pixel 2</string>
			<key>UserDefinedName</key>
			<string>Pixel 2</string>
			<key>idProduct</key>
			<integer>20195</integer>
			...
 In: /Library/Preferences/SystemConfiguration/NetworkInterfaces.plist
 When that works, interfaces do not proliferate (at least in most cases).

PROBLEM CAUSE:
 The MacOS network daemon (or whatever it is) looks at interface's 
 "IOProviderClass" to see if it's USB Device. Unfortunately, it may not pick
 up all the names, e.g. it may trigger off the old "IOUSBDevice", but not
 the new "IOUSBHostDevice". This problem is present in El Capitan for both
 device and the interface, and seems to be present in later systems for 
 IOUSBDevice.
 
FIX/HACK:
 The IOUSBHostDevice and IOUSBHostInterface providers actually specify
 the "IOClassNameOverride" that gives the old name. We just take this value
 and update our "IOProviderClass" to that.
*/

bool HoRNDIS::start(IOService *provider) {
	LOG(V_DEBUG, ">");

	// Per comment in "IONetworkController.h", 'super::start' should be the
	// first method called in the overridden implementation. It allocates the
	// network queue for the interface. The rest of the networking
	// initialization will be done by 'createNetworkInterface', once USB
	// USB is ready.
	if(!super::start(provider)) {
		return false;
	}

	{  // TX flush timer: fires partially-filled aggregated transfers.
		// Also used as the RX re-arm retry tick when an 'allocatePacket'
		// fails (see 'txFlushTimerFired').
		fTxFlushTimer = IOTimerEventSource::timerEventSource(
			this, &HoRNDIS::txFlushTimerFired);
		if (!fTxFlushTimer ||
			getWorkLoop()->addEventSource(fTxFlushTimer) != kIOReturnSuccess) {
			LOG(V_ERROR, "Cannot create the TX flush timer");
			goto bailout;
		}
		fTxFlushTimer->enable();
	}

	{  // Control-channel timer: keep-alive ticks and command-wait timeouts.
		fCtrlTimer = IOTimerEventSource::timerEventSource(
			this, &HoRNDIS::ctrlTimerFired);
		if (!fCtrlTimer ||
			getWorkLoop()->addEventSource(fCtrlTimer) != kIOReturnSuccess) {
			LOG(V_ERROR, "Cannot create the control timer");
			goto bailout;
		}
		fCtrlTimer->enable();
	}

	{  // Fixing the Provider class name.
		// See "INTERFACE PROLIFERATION AND PROVIDER CLASS NAME" description.
		OSObject *providerClass = provider->getProperty("IOClassNameOverride");
		if (providerClass) {
			setProperty(kIOProviderClassKey, providerClass);
		}
	}

	if (!openUSBInterfaces(provider)) {
		goto bailout;
	}

	if (fProtocol == kProtocolNCM) {
#if NCM_SUPPORT
		if (!ncmInit()) {
			goto bailout;
		}
#else
		LOG(V_ERROR, "Matched NCM but the driver was built without NCM support");
		goto bailout;
#endif
	} else if (!rndisInit()) {
		goto bailout;
	}

	// NOTE: The RNDIS spec mandates the usage of Keep Alive timer; however,
	// the Android does not seem to be missing its absense, so there is
	// probably no use in implementing it.

	LOG(V_DEBUG, "done with RNDIS initialization: can start network interface");

	// Let's create the medium tables here, to avoid doing extra
	// steps in 'enable'. Also, comments recommend creating medium tables
	// in the 'setup' stage.
	const IONetworkMedium *primaryMedium;
	if (!createMediumTables(&primaryMedium) ||
		!setCurrentMedium(primaryMedium)) {
		goto bailout;
	}
	
	// Looks like everything's good... publish the interface!
	if (!createNetworkInterface()) {
		goto bailout;
	}

	// This call is based on traces of Thunderbolt Ethernet driver.
	// That driver calls 'setLinkStatus(0x1)' before interface publish 
	// callback (which happens after 'start').
	setLinkStatus(kIONetworkLinkValid);
	
	LOG(V_DEBUG, "successful");
	return true;

bailout:
	stop(provider);
	return false;
}

bool HoRNDIS::willTerminate(IOService *provider, IOOptionBits options) {
	LOG(V_DEBUG, ">");
	// The 'willTerminate' is called when USB device disappears - the user
	// either disconnected the USB, or switched-off tethering. It's likely
	// that the pending read has already invoked a callback with unreachable
	// device or aborted status, and already terminated. If not, closing of
	// the USB Data interface would force it to abort.
	//
	// Note, per comments in 'IOUSBHostInterface.h' (for some later version
	// of MacOS SDK), this is the recommended place to close USB interfaces.
	//
	// This happens before ::stop, but after some of the read jobs fail
	// with kIOReturnNotResponding (and some of the writers might fail,
	// too).  ::disable happens sometime after we get done here, too --
	// potentially invoked by super::willTerminate.
	
	disableNetworkQueue();
	closeUSBInterfaces();

	return super::willTerminate(provider, options);
}

void HoRNDIS::stop(IOService *provider) {
	LOG(V_DEBUG, ">");

	if (fTxFlushTimer) {
		fTxFlushTimer->cancelTimeout();
		fTxFlushTimer->disable();
		getWorkLoop()->removeEventSource(fTxFlushTimer);
		fTxFlushTimer->release();
		fTxFlushTimer = NULL;
	}
	fTxTimerArmed = false;

	if (fCtrlTimer) {
		fCtrlTimer->cancelTimeout();
		fCtrlTimer->disable();
		getWorkLoop()->removeEventSource(fCtrlTimer);
		fCtrlTimer->release();
		fCtrlTimer = NULL;
	}
	fCtrlTimerPurpose = kCtrlTimerNone;
	fCtrlTimerArmed = false;
	
	OSSafeReleaseNULL(fNetworkInterface);
	
	closeUSBInterfaces();  // Just in case - supposed to be closed by now.

	super::stop(provider);
}


// Convenience function: to retain and assign in one step:
template <class T> static inline T *retainT(T *ptr) {
	ptr->retain();
	return ptr;
}

bool HoRNDIS::openUSBInterfaces(IOService *provider) {
	if (fProbeConfigVal == 0) {
		// Must have been set by 'probe' before 'start' function call:
		LOG(V_ERROR, "'fProbeConfigVal' has not been set, bailing out");
		return false;
	}

	IOUSBHostDevice *device = OSDynamicCast(IOUSBHostDevice, provider);
	if (device) {
		// Set the device configuration, so we can start looking at the interfaces:
		if (device->setConfiguration(fProbeConfigVal, false) != kIOReturnSuccess) {
			LOG(V_ERROR, "Cannot set the USB Device configuration");
			return false;
		}
	} else {
		IOUSBHostInterface *iface = OSDynamicCast(IOUSBHostInterface, provider);
		if (iface == NULL) {
			LOG(V_ERROR, "start: BUG unexpected provider class");
			return false;
		}
		device = iface->getDevice();
		// Make sure it's the one we care about:
		bool match = iface->getConfigurationDescriptor()->bConfigurationValue == fProbeConfigVal
			&& iface->getInterfaceDescriptor()->bInterfaceNumber == fProbeCommIfNum;
		if (!match) {
			LOG(V_ERROR, "BUG! Did we see a different provider in probe?");
			return false;
		}
	}

	{  // Now, find the interfaces:
		OSIterator *iterator = device->getChildIterator(gIOServicePlane);
		OSObject *obj = NULL;
		while(iterator != NULL && (obj = iterator->getNextObject()) != NULL) {
			IOUSBHostInterface *iface = OSDynamicCast(IOUSBHostInterface, obj);
			if (iface == NULL) {
				continue;
			}
			if (iface->getConfigurationDescriptor()->bConfigurationValue !=
					fProbeConfigVal) {
				continue;
			}
			const InterfaceDescriptor *desc = iface->getInterfaceDescriptor();
			uint8_t ifaceNum = desc->bInterfaceNumber;
			if (!fCommInterface && ifaceNum == fProbeCommIfNum) {
				LOG(V_DEBUG, "Found control interface: %d/%d/%d, opening",
					desc->bInterfaceClass, desc->bInterfaceSubClass,
					desc->bInterfaceProtocol);
				if (!iface->open(this)) {
					LOG(V_ERROR, "Could not open RNDIS control interface");
					return false;
				}
				// Note, we retain AFTER opening the interface, because once
				// 'fCommInterface' is set, the 'closeUSBInterfaces' would
				// always try to close it before releasing:
				fCommInterface = retainT(iface);
			} else if (ifaceNum == fProbeDataIfNum) {
				LOG(V_DEBUG, "Found data interface: %d/%d/%d, opening",
					desc->bInterfaceClass, desc->bInterfaceSubClass,
					desc->bInterfaceProtocol);
				if (!iface->open(this)) {
					LOG(V_ERROR, "Could not open RNDIS data interface");
					return false;
				}
				// open before retain, see above:
				fDataInterface = retainT(iface);
				break;  // We should be done by now.
			}
		}
		OSSafeReleaseNULL(iterator);
	}

	// WARNING, it is a WRONG idea to attach 'fDataInterface' as a second
	// provider, because both providers would be calling 'willTerminate', and
	// 'stop' methods, resulting in chaos.

	if (!fCommInterface || !fDataInterface) {
		LOG(V_ERROR, "could not find the required interfaces, despite seeing "
			"their descriptors during 'probe' method call");
		return false;
	}
	
	{  // Get the pipes for the data interface. NCM data interfaces may
		// expose two bulk pairs ("two endpoints"); we collect up to two
		// per direction. RNDIS devices have a single pair.
		const EndpointDescriptor *candidate = NULL;
		const InterfaceDescriptor *intDesc = fDataInterface->getInterfaceDescriptor();
		const ConfigurationDescriptor *confDesc = fDataInterface->getConfigurationDescriptor();
		if (intDesc->bNumEndpoints != 2 && intDesc->bNumEndpoints != 4) {
			LOG(V_ERROR, "Expected 2 or 4 endpoints for the Data Interface, got: %d",
				intDesc->bNumEndpoints);
			return false;
		}
		while((candidate = StandardUSB::getNextEndpointDescriptor(
					confDesc, intDesc, candidate)) != NULL) {
			// Only bulk endpoints belong on the data pipes:
			if ((candidate->bmAttributes & 0x03) != 0x02) {
				continue;
			}
			const bool isEPIn =
				(candidate->bEndpointAddress & kEndpointDescriptorDirection) != 0;
			IOUSBHostPipe *&pipe1 = isEPIn ? fInPipe : fOutPipe;
			IOUSBHostPipe *&pipe2 = isEPIn ? fInPipe2 : fOutPipe2;
			if (pipe1 == NULL) {
				// Note, 'copyPipe' already performs 'retain'.
				pipe1 = fDataInterface->copyPipe(candidate->bEndpointAddress);
			} else if (pipe2 == NULL) {
				pipe2 = fDataInterface->copyPipe(candidate->bEndpointAddress);
			}
		}
		if (fInPipe == NULL || fOutPipe == NULL) {
			LOG(V_ERROR, "Could not init IN/OUT pipes in the Data Interface");
			return false;
		}
		if (fInPipe2 || fOutPipe2) {
			LOG(V_NOTE, "Data interface has two bulk pairs; using both");
		}
	}

	{  // Find the interrupt IN pipe of the control interface. It carries
		// response-available and media connect/disconnect notifications
		// (per [MSDN-RNDISUSB], "Control Channel Characteristics"). It is
		// optional: some devices omit it, and the command loop then falls
		// back to bounded polling.
		const InterfaceDescriptor *commDesc =
			fCommInterface->getInterfaceDescriptor();
		const ConfigurationDescriptor *confDesc =
			fCommInterface->getConfigurationDescriptor();
		const EndpointDescriptor *cand = NULL;
		while ((cand = StandardUSB::getNextEndpointDescriptor(
					confDesc, commDesc, cand)) != NULL) {
			const bool isEPIn =
				(cand->bEndpointAddress & kEndpointDescriptorDirection) != 0;
			// Transfer-type bits 0..1 of bmAttributes: 3 == interrupt.
			if (isEPIn && (cand->bmAttributes & 0x03) == 0x03) {
				fIntPipe = fCommInterface->copyPipe(cand->bEndpointAddress);
				break;
			}
		}
		LOG(V_DEBUG, "Control interrupt pipe: %p", fIntPipe);
	}
	
	return true;
}

void HoRNDIS::closeUSBInterfaces() {
	fReadyToTransfer = false;  // Interfaces are about to be closed.
	// Close the interfaces - this would abort the transfers (if present):
	if (fDataInterface) {
		fDataInterface->close(this);
	}
	if (fCommInterface) {
		fCommInterface->close(this);
	}

	OSSafeReleaseNULL(fInPipe);
	OSSafeReleaseNULL(fOutPipe);
	OSSafeReleaseNULL(fInPipe2);
	OSSafeReleaseNULL(fOutPipe2);
	OSSafeReleaseNULL(fIntPipe);
	OSSafeReleaseNULL(fDataInterface);
	OSSafeReleaseNULL(fCommInterface);  // First one to open, last one to die.
}

IOService *HoRNDIS::probe(IOService *provider, SInt32 *score) {
	LOG(V_DEBUG, "came in with a score of %d", *score);
	{  // Check if this is a device-based matching:
		IOUSBHostDevice *device = OSDynamicCast(IOUSBHostDevice, provider);
		if (device) {
			return probeDevice(device, score);
		}
	}
	
	IOUSBHostInterface *controlIf = OSDynamicCast(IOUSBHostInterface, provider);
 	if (controlIf == NULL) {
		LOG(V_ERROR, "unexpected provider class (wrong Info.plist)");
		return NULL;
	}

	const InterfaceDescriptor *desc = controlIf->getInterfaceDescriptor();
	LOG(V_DEBUG, "Interface-based matching, probing for device '%s', "
		"interface %d/%d/%d", controlIf->getDevice()->getName(),
		desc->bInterfaceClass, desc->bInterfaceSubClass,
		desc->bInterfaceProtocol);

	bool ncm = false;
#if NCM_SUPPORT
	ncm = isNCMControlInterface(desc);
#endif
	if (!ncm && !isRNDISControlInterface(desc)) {
		LOG(V_ERROR, "not an RNDIS/NCM control interface (wrong Info.plist)");
		return NULL;
	}

	const ConfigurationDescriptor *configDesc =
		controlIf->getConfigurationDescriptor();

	// The data interface number: explicit from the Union descriptor for
	// NCM, implicitly control+1 for RNDIS:
	uint8_t dataIfNum;
#if NCM_SUPPORT
	if (ncm) {
		int dnum = getUnionDataIfNum(desc);
		if (dnum < 0) {
			LOG(V_DEBUG, "NCM control interface without a Union descriptor");
			return NULL;
		}
		dataIfNum = (uint8_t)dnum;
	} else
#endif
	{
		dataIfNum = desc->bInterfaceNumber + 1;
	}

	const InterfaceDescriptor *dataDesc =
		StandardUSB::getNextInterfaceDescriptor(configDesc, desc);
	bool match = isCDCDataInterface(dataDesc) &&
		(dataDesc->bInterfaceNumber == dataIfNum);
	if (!match) {
		LOG(V_DEBUG, "Could not find the CDC data interface (number %d)",
			dataIfNum);
		return NULL;
	}
	fProbeConfigVal = configDesc->bConfigurationValue;
	fProbeCommIfNum = desc->bInterfaceNumber;
	fProbeDataIfNum = dataIfNum;
#if NCM_SUPPORT
	fProtocol = ncm ? kProtocolNCM : kProtocolRNDIS;
#endif
	LOG(V_NOTE, "Matched a %s control interface", ncm ? "NCM" : "RNDIS");
	*score += 100000;
	return this;
}

IOService *HoRNDIS::probeDevice(IOUSBHostDevice *device, SInt32 *score) {
	const DeviceDescriptor *desc = device->getDeviceDescriptor();
	LOG(V_DEBUG, "Device-based matching, probing: '%s', %d/%d/%d",
		device->getName(), desc->bDeviceClass, desc->bDeviceSubClass,
		desc->bDeviceProtocol);
	// Look through all configurations and find the one we want:
	for (int i = 0; i < desc->bNumConfigurations; i++) {
		const ConfigurationDescriptor *configDesc =
			device->getConfigurationDescriptor(i);
		if (configDesc == NULL) {
			LOG(V_ERROR, "Cannot get device's configuration descriptor");
			return NULL;
		}
		int controlIfNum = -1;
		int dataIfNum = -1;
		int proto = -1;
		bool foundData = false;
		const InterfaceDescriptor *intDesc = NULL;
		while((intDesc = StandardUSB::getNextInterfaceDescriptor(configDesc, intDesc)) != NULL) {
			// If this is a device-level match, check for the control
			// interface (either protocol; the data number comes from the
			// Union descriptor for NCM, control+1 for RNDIS):
#if NCM_SUPPORT
			if (isNCMControlInterface(intDesc)) {
				int dnum = getUnionDataIfNum(intDesc);
				if (dnum >= 0) {
					controlIfNum = intDesc->bInterfaceNumber;
					dataIfNum = dnum;
					proto = kProtocolNCM;
					continue;
				}
			}
#endif
			if (isRNDISControlInterface(intDesc)) {  // Just check them all.
				controlIfNum = intDesc->bInterfaceNumber;
				dataIfNum = controlIfNum + 1;
				proto = kProtocolRNDIS;
				continue;
			}
		
			// We check for the data interface AND make sure it is the one
			// paired with the control interface found above:
			if (controlIfNum >= 0 && isCDCDataInterface(intDesc) &&
				intDesc->bInterfaceNumber == dataIfNum) {
				foundData = true;
				break;
			}
		}
		if (foundData) {
			// We've found it! Save the information and return:
			fProbeConfigVal = configDesc->bConfigurationValue;
			fProbeCommIfNum = (uint8_t)controlIfNum;
			fProbeDataIfNum = (uint8_t)dataIfNum;
#if NCM_SUPPORT
			fProtocol = (proto == kProtocolNCM) ? kProtocolNCM : kProtocolRNDIS;
#endif
			*score += 10000;
			return this;
		}
	}

	// Did not find any interfaces we can use:
	LOG(V_DEBUG, "The device '%s' does not contain the required interfaces: "
			"it is not for us", device->getName());
	return NULL;
}

/***** Ethernet interface bits *****/

/* We need our own createInterface (overriding the one in IOEthernetController) 
 * because we need our own subclass of IOEthernetInterface.  Why's that, you say?
 * Well, we need that because that's the only way to set a different default MTU,
 * because it seems like MacOS code just assumes that any EthernetController
 * driver must be able to handle al leaset 1500-byte Ethernet payload.
 * Sigh...
 * The MTU-limiting code may never come into play though, because the devices
 * I've seen have "max_transfer_size" large enough to accomodate a max-length 
 * Ethernet frames. */

bool HoRNDISInterface::init(IONetworkController *controller, int mtu) {
	maxmtu = mtu;
	if (IOEthernetInterface::init(controller) == false) {
		return false;
	}
	LOG(V_NOTE, "(network interface) starting up with MTU %d", mtu);
	setMaxTransferUnit(mtu);
	return true;
}

bool HoRNDISInterface::setMaxTransferUnit(UInt32 mtu) {
	if (mtu > maxmtu) {
		LOG(V_NOTE, "Excuse me, but I said you could have an MTU of %u, and you just tried to set an MTU of %d.  Good try, buddy.", maxmtu, mtu);
		return false;
	}
	IOEthernetInterface::setMaxTransferUnit(mtu);
	return true;
}

/* Overrides IOEthernetController::createInterface */
IONetworkInterface *HoRNDIS::createInterface() {
	LOG(V_DEBUG, ">");
	HoRNDISInterface *netif = new HoRNDISInterface;
	
	if (!netif) {
		return NULL;
	}

	int mtuLimit;
#if NCM_SUPPORT
	if (fProtocol == kProtocolNCM) {
		// NCM: bounded by the negotiated max datagram size:
		mtuLimit = (int)fNcmMaxDatagram - 14;
	} else
#endif
	{
		mtuLimit = maxOutTransferSize
			- (int)sizeof(rndis_data_hdr)
			- 14;  // Size of ethernet header (no QLANs). Checksum is not included.
	}

	if (!netif->init(this, min(ETHERNET_MTU, mtuLimit))) {
		netif->release();
		return NULL;
	}
	
	return netif;
}

bool HoRNDIS::createNetworkInterface() {
	LOG(V_DEBUG, "attaching and registering interface");
	
	// MTU is initialized before we get here, so this is a safe time to do this.
	if (!attachInterface((IONetworkInterface **)&fNetworkInterface, true)) {
		LOG(V_ERROR, "attachInterface failed?");	  
		return false;
	}
	LOG(V_PTR, "fNetworkInterface: %p", fNetworkInterface);

	// The 'registerService' should be called by 'attachInterface' (with second
	// parameter set to true). No need to do it here.
	
	return true;	
}

/***** Interface enable and disable logic *****/
HoRNDIS::ReentryLocker::ReentryLocker(IOCommandGate *inGate, bool &inGuard)
		: gate(inGate), entryGuard(inGuard), result(kIOReturnSuccess) {
	// Note, please see header comment for the motivation behind
	// 'ReentryLocker' and its high-level functionality.
	// Wait until we exit the previously-entered enable or disable method:
	while (entryGuard) {
		LOG(V_DEBUG, "Delaying the re-entered call");
		result = gate->commandSleep(&entryGuard);
		// If "commandSleep" has failed, stop immediately, and don't
		// touch the 'entryGuard':
		if (isInterrupted()) {
			return;
		}
	}
	entryGuard = true;  // Mark the entry into one of the protected methods.
}

HoRNDIS::ReentryLocker::~ReentryLocker() {
	if (!isInterrupted()) {
		entryGuard = false;
		gate->commandWakeup(&entryGuard);
	}
}

static IOReturn loopClearPipeStall(IOUSBHostPipe *pipe) {
	IOReturn rc = kUSBHostReturnPipeStalled;
	int count = 0;
	// For some reason, 'clearStall' may keep on returning
	// kUSBHostReturnPipeStalled many times, before finally returning success
	// (Android keeps on sending packtes, each generating a stall?).
	const int NUM_RETRIES = 1000;
	for (; count < NUM_RETRIES && rc == kUSBHostReturnPipeStalled; count++) {
		rc = pipe->clearStall(true);
	}
	LOG(V_DEBUG, "Called 'clearStall' %d times", count);
	return rc;
}

/*!
 * Calls 'pipe->io', and if there is a stall, tries to clear that 
 * stall and calls it again.
 */
static inline IOReturn robustIO(IOUSBHostPipe *pipe, pipebuf_t *buf,
	uint32_t len) {
	IOReturn rc = pipe->io(buf->dmaBuf, len, &buf->comp);
	if (rc == kUSBHostReturnPipeStalled) {
		LOG(V_DEBUG, "USB Pipe is stalled. Trying to clear ...");
		rc = loopClearPipeStall(pipe);
		// If clearing the stall succeeded, try the IO operation again:
		if (rc == kIOReturnSuccess) {
			LOG(V_DEBUG, "Cleared USB Stall, Retrying the operation");
			rc = pipe->io(buf->dmaBuf, len, &buf->comp);
		}
	}
	return rc;
}

/* Contains buffer alloc and dealloc, notably.  Why do that here?  
   Not just because that's what Apple did. We don't want to consume these 
   resources when the interface is sitting disabled and unused. */
IOReturn HoRNDIS::enable(IONetworkInterface *netif) {
	IOReturn rtn = kIOReturnSuccess;

	LOG(V_DEBUG, "begin for thread_id=%lld", thread_tid(current_thread()));
	ReentryLocker locker(this, fEnableDisableInProgress);
	if (locker.isInterrupted()) {
		LOG(V_ERROR, "Waiting interrupted");
		return locker.getResult();
	}

	if (fNetifEnabled) {
		LOG(V_DEBUG, "Repeated call (thread_id=%lld), returning success",
			thread_tid(current_thread()));
		return kIOReturnSuccess;
	}

	if (fCallbackCount != 0) {
		LOG(V_ERROR, "Invalid state: fCallbackCount(=%d) != 0", fCallbackCount);
		return kIOReturnError;
	}

	if (!allocateResources()) {
		return kIOReturnNoMemory;
	}

	// Tell the other end to start transmitting. The packet filter starts
	// minimal (unicast + broadcast) and grows as the network stack asks
	// for multicast/promiscuous reception.
	fCurrentFilter = 0;
	fMulticastEnabled = false;
	fPromiscEnabled = false;
	if (!updatePacketFilter()) {
		goto bailout;
	}

	// The pipe stall clearning is not needed for the first "enable" call after
	// pugging in the device, but it becomes necessary when "disable" is called
	// after that, followed by another "enable". This happens when user runs
	// "sudo ifconfig <netif> down", followed by "sudo ifconfig <netif> up"
	LOG(V_DEBUG, "Clearing potential Pipe stalls on Input and Output pipes");
	loopClearPipeStall(fInPipe);
	loopClearPipeStall(fOutPipe);

	// We can now perform reads and writes between Network stack and USB device:
	fReadyToTransfer = true;

	// Assign each RX slot its pipe (NCM with two bulk pairs uses both):
	for (int i = 0; i < N_IN_BUFS; i++) {
		inbufs[i].pipe = fInPipe;
	}
#if NCM_SUPPORT
	if (fProtocol == kProtocolNCM && fInPipe2 && N_IN_BUFS >= 2) {
		inbufs[1].pipe = fInPipe2;
	}
#endif
	
	// Kick off the read requests:
	for (int i = 0; i < N_IN_BUFS; i++) {
		if (!startRead(&inbufs[i])) {
			LOG(V_ERROR, "Failed to start the first read (slot %d)", i);
			goto bailout;
		}
	}

	// Tell the world that the link is up...
	if (!setLinkStatus(kIONetworkLinkActive | kIONetworkLinkValid,
			getCurrentMedium())) {
		LOG(V_ERROR, "Cannot set link status");
		rtn = kIOReturnError;
		goto bailout;
	}

	// ... and then listen for packets!
	getOutputQueue()->setCapacity(TRANSMIT_QUEUE_SIZE);
	getOutputQueue()->start();
	LOG(V_DEBUG, "txqueue started");

	// Start the control-channel interrupt reader. Best-effort: without an
	// interrupt pipe the command loop falls back to bounded polling, and
	// media indications are simply not delivered.
	fIntReadActive = true;
	if (!startIntRead()) {
		LOG(V_DEBUG, "No control interrupt reader "
			"(device has no interrupt pipe?)");
		fIntReadActive = false;
	}

#if RNDIS_KEEPALIVE
	// Start the keep-alive cadence (RNDIS-only; NCM has no keep-alive):
	if (fProtocol == kProtocolRNDIS) {
		fKeepaliveEnabled = true;
		armCtrlTimer(kCtrlTimerKeepalive, RNDIS_KEEPALIVE_US);
	}
#endif

	// Now we can say we're alive.
	fNetifEnabled = true;
	LOG(V_NOTE, "completed (thread_id=%lld): RNDIS network interface '%s' "
		"should be live now", thread_tid(current_thread()), netif->getName());
	
	return kIOReturnSuccess;
	
bailout:
	disableImpl();
	return rtn;
}

void HoRNDIS::disableNetworkQueue() {
	// Disable the queue (no more outputPacket),
	// and then flush everything in the queue.
	getOutputQueue()->stop();
	getOutputQueue()->flush();
	getOutputQueue()->setCapacity(0);
}

IOReturn HoRNDIS::disable(IONetworkInterface *netif) {
	LOG(V_DEBUG, "begin for thread_id=%lld", thread_tid(current_thread()));
	// This function can be called as a consequence of:
	//  1. USB Disconnect
	//  2. Some action, while the device is up and running
	//     (e.g. "ifconfig en6 down").
	// In the second case, we'll need to do more cleanup:
	// ask the RNDIS device to stop transmitting, and abort the callbacks.
	//

	ReentryLocker locker(this, fEnableDisableInProgress);
	if (locker.isInterrupted()) {
		LOG(V_ERROR, "Waiting interrupted");
		return locker.getResult();
	}

	if (!fNetifEnabled) {
		LOG(V_DEBUG, "Repeated call (thread_id=%lld)", thread_tid(current_thread()));
		return kIOReturnSuccess;
	}

	disableImpl();

	LOG(V_DEBUG, "completed (thread_id=%lld)", thread_tid(current_thread()));
	return kIOReturnSuccess;
}

void HoRNDIS::disableImpl() {
	disableNetworkQueue();

	// Stop the the new transfers. The code below would cancel the pending ones:
	fReadyToTransfer = false;

	// Drop any partially-filled TX aggregate (never submitted) and disarm
	// the flush timer: the timers/state are reset here, so no extra work is
	// needed in 'releaseResources' - it just frees the buffers.
	if (fTxFlushTimer) {
		fTxFlushTimer->cancelTimeout();
	}
	fTxTimerArmed = false;
	fNeedReadRestart = false;
	if (fTxFillBufIdx >= 0) {
		// Note: 'numFreeOutBufs' must be in sync: the fill buffer was taken
		// off the free stack, so returning it pushes it back on.
		outbufStack[numFreeOutBufs] = (uint16_t)fTxFillBufIdx;
		numFreeOutBufs++;
		fTxFillBufIdx = -1;
	}
	fTxFillOffset = 0;
#if TX_ZEROCOPY
	if (fTxHead) {
		freePacket(fTxHead);
		fTxHead = NULL;
		fTxBytes = 0;
	}
#endif

	// If the device has not been disconnected, ask it to stop xmitting:
	if (fCommInterface) {
#if NCM_SUPPORT
		if (fProtocol == kProtocolNCM) {
			ncmSetPacketFilter(0);
		} else
#endif
		{
			rndisSetPacketFilter(0);
		}
	}
	fCurrentFilter = 0;

	// Stop the control-channel housekeeping: keep-alive cadence,
	// command-wait timeouts and the interrupt reader. Note the interrupt
	// reader is stopped only AFTER the packet-filter command above, so its
	// notifications can still wake that command's wait.
#if RNDIS_KEEPALIVE
	fKeepaliveEnabled = false;
#endif
	if (fCtrlTimer) {
		fCtrlTimer->cancelTimeout();
	}
	fCtrlTimerPurpose = kCtrlTimerNone;
	fCtrlTimerArmed = false;
	fIntReadActive = false;

	// Again, based on Thunderbolt Ethernet controller traces.
	// It sets the link status to 0x1 in the disable call:
	setLinkStatus(kIONetworkLinkValid, 0);

	// If USB interfaces are still up, abort the reader and writer:
	if (fInPipe) {
		fInPipe->abort(IOUSBHostIOSource::kAbortSynchronous,
			kIOReturnAborted, NULL);
	}
	if (fOutPipe) {
		fOutPipe->abort(IOUSBHostIOSource::kAbortSynchronous,
			kIOReturnAborted, NULL);
	}
	if (fInPipe2) {
		fInPipe2->abort(IOUSBHostIOSource::kAbortSynchronous,
			kIOReturnAborted, NULL);
	}
	if (fOutPipe2) {
		fOutPipe2->abort(IOUSBHostIOSource::kAbortSynchronous,
			kIOReturnAborted, NULL);
	}
	if (fIntPipe) {
		fIntPipe->abort(IOUSBHostIOSource::kAbortSynchronous,
			kIOReturnAborted, NULL);
	}
	// Make sure all the callbacks have exited:
	LOG(V_DEBUG, "Callback count: %d. If not zero, delaying ...",
		fCallbackCount);
	while (fCallbackCount > 0) {
		// No timeout: in our callbacks we trust!
		getCommandGate()->commandSleep(&fCallbackCount);
	}
	LOG(V_DEBUG, "All callbacks exited");

	// Release all resources
	releaseResources();

	fNetifEnabled = false;
}

bool HoRNDIS::createMediumTables(const IONetworkMedium **primary) {
	IONetworkMedium	*medium;
	
	OSDictionary *mediumDict = OSDictionary::withCapacity(1);
	if (mediumDict == NULL) {
		LOG(V_ERROR, "Cannot allocate OSDictionary");
		return false;
	}
	
	// Advertise a link speed matching the negotiated USB speed, so System
	// Profiler reports something sensible for SuperSpeed devices (5 Gb/s)
	// instead of always claiming USB2 High-Speed (480 Mb/s).
	uint64_t speed = 480 * 1000000ULL;
	if (fDataInterface && fDataInterface->getDevice() &&
		fDataInterface->getDevice()->getSpeed() == kUSBDeviceSpeedSuper) {
		speed = 5000 * 1000000ULL;
	}

	medium = IONetworkMedium::medium(kIOMediumEthernetAuto, speed);
	IONetworkMedium::addMedium(mediumDict, medium);
	medium->release();  // 'mediumDict' holds a ref now.
	if (primary) {
		*primary = medium;
	}
	
	bool result = publishMediumDictionary(mediumDict);
	if (!result) {
		LOG(V_ERROR, "Cannot publish medium dictionary!");
	}

	// Per comment for 'publishMediumDictionary' in NetworkController.h, the
	// medium dictionary is copied and may be safely relseased after the call.
	mediumDict->release();
	
	return result;
}

bool HoRNDIS::allocateResources() {
	LOG(V_DEBUG, "Allocating %d input buffers (size=%d) and %d output "
		"buffers (size=%d)", N_IN_BUFS, IN_BUF_SIZE, N_OUT_BUFS, OUT_BUF_SIZE);
	
	// Grab a memory descriptor pointer for data-in.
	uint32_t inSize = IN_BUF_SIZE;
#if NCM_SUPPORT
	if (fProtocol == kProtocolNCM && fNcmRxMax > 0) {
		inSize = fNcmRxMax;
	}
#endif
	for (int i = 0; i < N_IN_BUFS; i++) {
#if RX_ZEROCOPY
		// Zero-copy RX buffers are mbuf packets, allocated per-transfer in
		// 'startRead': nothing to preallocate here.
#else
		inbufs[i].mdp = IOBufferMemoryDescriptor::withCapacity(inSize, kIODirectionIn);
		if (!inbufs[i].mdp) {
			return false;
		}
		inbufs[i].mdp->setLength(inSize);
		inbufs[i].dmaBuf = inbufs[i].mdp;
		LOG(V_PTR, "PTR: inbuf[%d].mdp: %p", i, inbufs[i].mdp);
#endif
	}

	// Interrupt reader buffer for the control interface (optional: when the
	// device has no interrupt pipe, this just sits unused):
	fIntBuf.mdp = IOBufferMemoryDescriptor::withCapacity(
		RNDIS_CMD_BUF_SZ, kIODirectionIn);
	if (fIntBuf.mdp) {
		fIntBuf.mdp->setLength(RNDIS_CMD_BUF_SZ);
		fIntBuf.dmaBuf = fIntBuf.mdp;
	}

	// And a handful for data-out...
	for (int i = 0; i < N_OUT_BUFS; i++) {
		outbufs[i].mdp = IOBufferMemoryDescriptor::withCapacity(
			OUT_BUF_SIZE, kIODirectionOut);
		if (!outbufs[i].mdp) {
			LOG(V_ERROR, "allocate output descriptor failed");
			return false;
		}
		LOG(V_PTR, "PTR: outbufs[%d].mdp: %p", i, outbufs[i].mdp);
		outbufs[i].mdp->setLength(OUT_BUF_SIZE);
		outbufs[i].dmaBuf = outbufs[i].mdp;
		outbufStack[i] = i;
	}
	numFreeOutBufs = N_OUT_BUFS;

	fTxFillBufIdx = -1;
	fTxFillOffset = 0;
	fTxTimerArmed = false;
#if TX_ZEROCOPY
	fTxHead = NULL;
	fTxBytes = 0;
#endif
#if NCM_SUPPORT
	fTxNdpCount = 0;
#endif
	fNeedReadRestart = false;
	
	return true;
}

void HoRNDIS::releaseResources() {
	LOG(V_DEBUG, "releaseResources");

	fReadyToTransfer = false;  // No transfers without buffers.

	// Drop any partially-filled TX aggregate (never submitted):
	if (fTxFillBufIdx >= 0) {
		outbufStack[numFreeOutBufs] = (uint16_t)fTxFillBufIdx;
		numFreeOutBufs++;
		fTxFillBufIdx = -1;
	}
	fTxFillOffset = 0;
	fTxTimerArmed = false;
	fNeedReadRestart = false;
	if (fTxFlushTimer) {
		fTxFlushTimer->cancelTimeout();
	}
#if TX_ZEROCOPY
	// Drop the partially-built zero-copy aggregate chain (never submitted):
	if (fTxHead) {
		freePacket(fTxHead);
		fTxHead = NULL;
		fTxBytes = 0;
	}
#endif

	for (int i = 0; i < N_OUT_BUFS; i++) {
		OSSafeReleaseNULL(outbufs[i].mdp);
#if TX_ZEROCOPY
		// Leftover in-flight chains (aborted writes are not freed by their
		// completion, see 'dataWriteComplete'):
		OSSafeReleaseNULL(outbufs[i].mbufMdp);
		if (outbufs[i].m) {
			freePacket(outbufs[i].m);
			outbufs[i].m = NULL;
		}
#endif
		outbufs[i].dmaBuf = NULL;
		outbufStack[i] = i;
	}
	numFreeOutBufs = 0;

	for (int i = 0; i < N_IN_BUFS; i++) {
#if RX_ZEROCOPY
		if (inbufs[i].m) {
			freePacket(inbufs[i].m);
			inbufs[i].m = NULL;
		}
		OSSafeReleaseNULL(inbufs[i].mbufMdp);
		inbufs[i].dmaBuf = NULL;
#else
		OSSafeReleaseNULL(inbufs[i].mdp);
		inbufs[i].dmaBuf = NULL;
#endif
	}

	OSSafeReleaseNULL(fIntBuf.mdp);
	fIntBuf.dmaBuf = NULL;
}

IOOutputQueue *HoRNDIS::createOutputQueue() {
	LOG(V_DEBUG, ">");
	// The gated Output Queue keeps things simple: everything is
	// serialized, no need to worry about locks or concurrency.
	// The device is not very fast, so the serial execution should be more
	// than capable of keeping up.
	// Note, if we ever switch to non-gated queue, we shall update the
	// 'outputPacket' to access the shared state using locks + update all the
	// other users of that state + may want to use locks for USB calls as well.
	return IOGatedOutputQueue::withTarget(this,
		getWorkLoop(), TRANSMIT_QUEUE_SIZE);
}

bool HoRNDIS::configureInterface(IONetworkInterface *netif) {
	LOG(V_DEBUG, ">");
	IONetworkData *nd;
	
	if (super::configureInterface(netif) == false) {
		LOG(V_ERROR, "super failed");
		return false;
	}
	
	nd = netif->getNetworkData(kIONetworkStatsKey);
	if (!nd || !(fpNetStats = (IONetworkStats *)nd->getBuffer())) {
		LOG(V_ERROR, "network statistics buffer unavailable?");
		return false;
	}
	
	LOG(V_PTR, "fpNetStats: %p", fpNetStats);
	
	return true;
}


/***** All-purpose IOKit network routines *****/

IOReturn HoRNDIS::getPacketFilters(const OSSymbol *group, UInt32 *filters) const {
	IOReturn	rtn = kIOReturnSuccess;
	
	if (group == gIOEthernetWakeOnLANFilterGroup) {
		*filters = 0;
	} else if (group == gIONetworkFilterGroup) {
		*filters = kIOPacketFilterUnicast | kIOPacketFilterBroadcast
			| kIOPacketFilterPromiscuous | kIOPacketFilterMulticast
			| kIOPacketFilterMulticastAll;
	} else {
		rtn = super::getPacketFilters(group, filters);
	}

	return rtn;
}

IOReturn HoRNDIS::getMaxPacketSize(UInt32 *maxSize) const {
	IOReturn rc = super::getMaxPacketSize(maxSize);
	if (rc != kIOReturnSuccess) {
		return rc;
	}
	// The max packet size is limited by the protocol's max frame size:
#if NCM_SUPPORT
	if (fProtocol == kProtocolNCM) {
		*maxSize = min(*maxSize, fNcmMaxDatagram);
	} else
#endif
	{
		*maxSize = min(*maxSize, maxOutTransferSize - sizeof(rndis_data_hdr));
	}
	LOG(V_DEBUG, "returning %d", *maxSize);
	return kIOReturnSuccess;
}

IOReturn HoRNDIS::selectMedium(const IONetworkMedium *medium) {
	LOG(V_DEBUG, ">");
	setSelectedMedium(medium);
	
	return kIOReturnSuccess;
}

IOReturn HoRNDIS::getHardwareAddress(IOEthernetAddress *ea) {
	LOG(V_DEBUG, ">");
	UInt32	  i;
	void *buf;
	unsigned char *bp;
	int rlen = -1;
	int rv;

	// The MAC is queried once and cached: it does not change while the
	// driver instance is alive (Android randomizes it per USB connect, but
	// a new connect creates a new driver instance).
	if (fHasHardwareAddress) {
		for (i = 0; i < 6; i++) {
			ea->bytes[i] = fHardwareAddress.bytes[i];
		}
		return kIOReturnSuccess;
	}

	buf = IOMallocAligned(RNDIS_CMD_BUF_SZ, sizeof(void *));
	if (!buf) {
		return kIOReturnNoMemory;
	}

	// WARNING: Android devices may randomly-generate RNDIS MAC address.
	// The function may return different results for the same device.

	rv = rndisQuery(buf, OID_802_3_PERMANENT_ADDRESS, 48, (void **) &bp, &rlen);
	if (rv < 0) {
		LOG(V_ERROR, "getHardwareAddress OID failed?");
		IOFreeAligned(buf, RNDIS_CMD_BUF_SZ);
		return kIOReturnIOError;
	}
	LOG(V_DEBUG, "MAC Address %02x:%02x:%02x:%02x:%02x:%02x -- rlen %d",
	      bp[0], bp[1], bp[2], bp[3], bp[4], bp[5],
	      rlen);
	
	for (i=0; i<6; i++) {
		ea->bytes[i] = bp[i];
		fHardwareAddress.bytes[i] = bp[i];
	}
	fHasHardwareAddress = true;
	
	IOFreeAligned(buf, RNDIS_CMD_BUF_SZ);
	return kIOReturnSuccess;
}

IOReturn HoRNDIS::setMulticastMode(bool active) {
	// Toggle RNDIS_PACKET_TYPE_ALL_MULTICAST in the device's packet
	// filter. 'setMulticastList' is not implemented (we pass all multicast
	// through when multicast is on), which matches the old behavior while
	// still keeping the filter minimal when multicast is NOT needed.
	LOG(V_DEBUG, "%s", active ? "on" : "off");
	fMulticastEnabled = active;
#if RNDIS_DYNAMIC_FILTER
	if (!updatePacketFilter()) {
		LOG(V_ERROR, "Cannot update packet filter for multicast");
	}
#endif
	return kIOReturnSuccess;
}

IOReturn HoRNDIS::setMulticastList(IOEthernetAddress *addrs,
	                            UInt32             count) {
	// We use the MULTICAST_ALL flag (see 'setMulticastMode'): the explicit
	// list is not programmed into the device.
	return kIOReturnSuccess;
}

IOReturn HoRNDIS::setPromiscuousMode(bool active) {
	// Toggle RNDIS_PACKET_TYPE_PROMISCUOUS in the device's packet filter.
	LOG(V_DEBUG, "%s", active ? "on" : "off");
	fPromiscEnabled = active;
#if RNDIS_DYNAMIC_FILTER
	if (!updatePacketFilter()) {
		LOG(V_ERROR, "Cannot update packet filter for promiscuous mode");
	}
#endif
	return kIOReturnSuccess;
}

/***** Packet transmit logic *****/

static inline bool isTransferStopStatus(IOReturn rc) {
	// IOReturn indicating that we need to stop transfers:
	return rc == kIOReturnAborted || rc == kIOReturnNotResponding;
}

// Split 'm' at 'offset' bytes: 'm' keeps the first half, '*tail' receives
// the second half (modern mbuf_split signature: errno_t + output param).
// Returns true on success; on failure the original chain is preserved and
// '*tail' is NULL.
static inline bool splitMbuf(mbuf_t m, size_t offset, mbuf_t *tail) {
	*tail = NULL;
	return mbuf_split(m, offset, MBUF_WAITOK, tail) == 0;
}

// TX aggregation helpers. Everything below runs on the workloop; the command
// gate serializes outputPacket, the flush timer and the pipe completions.

// Take the next free buffer as the one being filled.
bool HoRNDIS::grabFillBuffer() {
	if (numFreeOutBufs <= 0) {
		return false;
	}
	fTxFillBufIdx = outbufStack[numFreeOutBufs - 1];
	if (fTxFillBufIdx < 0 || fTxFillBufIdx >= N_OUT_BUFS) {
		LOG(V_ERROR, "BUG: poolIndex out-of-bounds");
		return false;
	}
	numFreeOutBufs--;
	fTxFillOffset = 0;
#if NCM_SUPPORT
	if (fProtocol == kProtocolNCM) {
		// Reserve the NTB headers: NTH + alignment pad + NDP:
		uint32_t pad = ((12 + fNcmNdpAlign - 1) / fNcmNdpAlign)
			* fNcmNdpAlign - 12;
		fTxNdpOffset = 12 + pad;
		fTxFillOffset = fTxNdpOffset + fNcmMaxNdpSize;
		fTxNdpCount = 0;
		uint8_t *buf = (uint8_t *)outbufs[fTxFillBufIdx].mdp->getBytesNoCopy();
		memset(buf, 0, fTxFillOffset);
		struct ncm_nth16 *nth = (struct ncm_nth16 *)buf;
		nth->dwSignature = cpu_to_le32(USB_CDC_NCM_NTH16_SIGN);
		nth->wHeaderLength = cpu_to_le16(sizeof(struct ncm_nth16));
		nth->wSequence = cpu_to_le16(fNcmSeq++);
		nth->wNdpIndex = cpu_to_le16((uint16_t)fTxNdpOffset);
	}
#endif
	return true;
}

// Submit the aggregated fill buffer (if it holds any data), then grab a
// fresh fill buffer. Returns false when no fresh buffer could be grabbed;
// the caller then has to stall the queue.
bool HoRNDIS::submitFillBuffer() {
	if (fTxFillBufIdx < 0) {
		return grabFillBuffer();
	}

	// OUT pipe selection: NCM with a second bulk pair alternates pipes:
	IOUSBHostPipe *outPipe = fOutPipe;
#if NCM_SUPPORT
	if (fProtocol == kProtocolNCM && fOutPipe2) {
		fTxOutPipeToggle = !fTxOutPipeToggle;
		outPipe = fTxOutPipeToggle ? fOutPipe2 : fOutPipe;
	}
#endif

#if TX_ZEROCOPY
	if (fProtocol == kProtocolRNDIS && fTxHead != NULL && fTxBytes > 0) {
		// Zero-copy TX: wrap the mbuf chain in a memory descriptor and
		// send it as a single transfer - no payload copy.
		const uint32_t xmitLen = fTxBytes;
		pipebuf_t &outbuf = outbufs[fTxFillBufIdx];
		// 'mbuf_concatenate' does not maintain the packet-header length:
		// fix it up so the descriptor reflects the real chain length.
		mbuf_pkthdr_setlen(fTxHead, fTxBytes);
		IOMbufMemoryDescriptor *mdp =
			IOMbufMemoryDescriptor::withBuffer(fTxHead, kIODirectionOut);
		if (!mdp) {
			LOG(V_ERROR, "IOMbufMemoryDescriptor::withBuffer failed");
			freePacket(fTxHead);
			fTxHead = NULL;
			fTxBytes = 0;
			return true;  // Keep the (now empty) fill slot.
		}
		mdp->setLength(xmitLen);
		outbuf.m = fTxHead;
		outbuf.mbufMdp = mdp;
		outbuf.dmaBuf = mdp;
		outbuf.pipe = outPipe;
		fTxHead = NULL;
		fTxBytes = 0;

		IOUSBHostCompletion *const comp = &outbuf.comp;
		comp->owner     = this;
		comp->parameter = (void *)(uintptr_t)fTxFillBufIdx;
		comp->action    = dataWriteComplete;

		IOReturn ior = robustIO(outPipe, &outbuf, xmitLen);
		if (ior != kIOReturnSuccess) {
			if (isTransferStopStatus(ior)) {
				LOG(V_DEBUG, "WRITER: The device was possibly disconnected: ignoring the error");
			} else {
				LOG(V_ERROR, "write failed: %08x", ior);
				fpNetStats->outputErrors++;
			}
			outbuf.mbufMdp->release();
			outbuf.mbufMdp = NULL;
			freePacket(outbuf.m);
			outbuf.m = NULL;
			outbuf.dmaBuf = NULL;
			outbufStack[numFreeOutBufs] = (uint16_t)fTxFillBufIdx;
			numFreeOutBufs++;
			fTxFillBufIdx = -1;
			fTxTimerArmed = false;
			return grabFillBuffer();
		}

		fCallbackCount++;
		fpNetStats->outputPackets++;
		fTxFillBufIdx = -1;
		fTxTimerArmed = false;
		return grabFillBuffer();
	}
#endif

	if (fTxFillOffset > 0) {
		pipebuf_t &outbuf = outbufs[fTxFillBufIdx];

#if NCM_SUPPORT
		if (fProtocol == kProtocolNCM) {
			ncmFlushNtb();
		}
#endif
		outbuf.dmaBuf->setLength(fTxFillOffset);

		IOUSBHostCompletion *const comp = &outbuf.comp;
		comp->owner     = this;
		comp->parameter = (void *)(uintptr_t)fTxFillBufIdx;
		comp->action    = dataWriteComplete;

		IOReturn ior = robustIO(outPipe, &outbuf, fTxFillOffset);
		if (ior != kIOReturnSuccess) {
			// The buffer contents are lost: put the buffer back and report.
			if (isTransferStopStatus(ior)) {
				LOG(V_DEBUG, "WRITER: The device was possibly disconnected: ignoring the error");
			} else {
				LOG(V_ERROR, "write failed: %08x", ior);
				fpNetStats->outputErrors++;
			}
			outbufStack[numFreeOutBufs] = (uint16_t)fTxFillBufIdx;
			numFreeOutBufs++;
			fTxFillBufIdx = -1;
			fTxFillOffset = 0;
			fTxTimerArmed = false;
			return grabFillBuffer();
		}

		fCallbackCount++;
		fpNetStats->outputPackets++;
		fTxFillBufIdx = -1;
		fTxFillOffset = 0;
		fTxTimerArmed = false;
		return grabFillBuffer();
	}

	// Nothing to submit: keep the (empty) fill buffer in place.
	return true;
}

void HoRNDIS::armTxFlushTimer() {
	if (fTxTimerArmed || !fTxFlushTimer) {
		return;
	}
	fTxTimerArmed = true;
	fTxFlushTimer->setTimeoutUS(TX_FLUSH_US);
}

void HoRNDIS::txFlushTimerFired(OSObject *owner, IOTimerEventSource *timer) {
	HoRNDIS *me = (HoRNDIS *)owner;

	// One-shot timer: the flag is cleared no matter what we do below.
	me->fTxTimerArmed = false;

	if (!me->fReadyToTransfer) {
		return;
	}

#if TX_AGGREGATE
	// TX: flush whatever is pending in the fill buffer. Note: we do NOT
	// call 'service()' on the output queue here - flushing submits a
	// transfer, it does not free a buffer, so it cannot unblock a stalled
	// queue. 'dataWriteComplete' takes care of that when a buffer frees up.
	if (me->fTxFillBufIdx >= 0 &&
#if TX_ZEROCOPY
		((me->fTxHead != NULL && me->fTxBytes > 0) ||
		 me->fTxFillOffset > 0)
#else
		me->fTxFillOffset > 0
#endif
		) {
		me->submitFillBuffer();
	}
#endif

#if RX_ZEROCOPY
	// RX: retry reads that failed to re-arm due to allocation failures.
	if (me->fNeedReadRestart) {
		me->fNeedReadRestart = false;
		for (int i = 0; i < N_IN_BUFS; i++) {
			if (me->inbufs[i].m == NULL) {
				if (!me->startRead(&me->inbufs[i])) {
					me->fNeedReadRestart = true;
				}
			}
		}
		if (me->fNeedReadRestart) {
			me->armTxFlushTimer();
		}
	}
#endif
}

// Allocate an RX buffer and submit a read on it. On success the callback
// count is bumped (one more in-flight transfer). The slot's 'pipe' must
// have been assigned by the caller ('enable').
bool HoRNDIS::startRead(pipebuf_t *inbuf) {
	inbuf->comp.owner = this;
	inbuf->comp.action = dataReadComplete;
	inbuf->comp.parameter = inbuf;

	// RX transfer size: NCM uses the negotiated NTB input size:
	uint32_t rxLen = IN_BUF_SIZE;
#if NCM_SUPPORT
	if (fProtocol == kProtocolNCM && fNcmRxMax > 0) {
		rxLen = fNcmRxMax;
	}
#endif

#if RX_ZEROCOPY
	mbuf_t m = allocatePacket(rxLen);
	if (!m) {
		LOG(V_ERROR, "allocatePacket(%d) failed", rxLen);
		return false;
	}
	IOMbufMemoryDescriptor *mdp = IOMbufMemoryDescriptor::withBuffer(m, kIODirectionIn);
	if (!mdp) {
		LOG(V_ERROR, "IOMbufMemoryDescriptor::withBuffer failed");
		freePacket(m);
		return false;
	}
	inbuf->m = m;
	inbuf->mbufMdp = mdp;
	inbuf->dmaBuf = mdp;
	IOReturn rtn = robustIO(inbuf->pipe, inbuf, rxLen);
#else
	inbuf->dmaBuf = inbuf->mdp;
	IOReturn rtn = robustIO(inbuf->pipe, inbuf, (uint32_t)inbuf->mdp->getLength());
#endif
	if (rtn != kIOReturnSuccess) {
		LOG(V_ERROR, "Failed to start a read: %08x", rtn);
		releaseReadBuffer(inbuf);
		return false;
	}
	fCallbackCount++;
	return true;
}

// Release the RX resources held by a slot (used on read teardown).
void HoRNDIS::releaseReadBuffer(pipebuf_t *inbuf) {
#if RX_ZEROCOPY
	if (inbuf->m) {
		freePacket(inbuf->m);
		inbuf->m = NULL;
	}
	OSSafeReleaseNULL(inbuf->mbufMdp);
	inbuf->dmaBuf = NULL;
#else
	// The buffer descriptor itself is owned by allocate/releaseResources.
	inbuf->dmaBuf = NULL;
#endif
}

UInt32 HoRNDIS::outputPacket(mbuf_t packet, void *param) {
	// Note, this function MAY or MAY NOT be protected by the IOCommandGate,
	// depending on the kind of OutputQueue used.
	// Here, we assume that IOCommandGate is used: no need to lock.

	if (!fReadyToTransfer) {
		// Technically, we must never be here, because we always disable the
		// queue before clearing 'fReadyToTransfer', but double-checking here
		// just in-case: better safe than sorry.
		LOG(V_DEBUG, "fReadyToTransfer=false: dropping packet "
			"(we shouldn't even be here)");
		freePacket(packet);
		return kIOReturnOutputDropped;
	}
	
	// Count the total size of this packet
	size_t pktlen = 0;
	for (mbuf_t m = packet; m; m = mbuf_next(m)) {
		pktlen += mbuf_len(m);
	}
	
	LOG(V_PACKET, "%ld bytes", pktlen);

#if NCM_SUPPORT
	if (fProtocol == kProtocolNCM) {
		// NCM: raw Ethernet frames packed into NTBs (no per-packet header).
		if (pktlen > fNcmMaxDatagram || pktlen > 0xFFFF) {
			LOG(V_ERROR, "packet too large (%ld bytes, max %u)",
				pktlen, fNcmMaxDatagram);
			fpNetStats->outputErrors++;
			freePacket(packet);
			return kIOReturnOutputDropped;
		}
		if (fTxFillBufIdx < 0) {
			if (!grabFillBuffer()) {
				return kIOOutputStatusRetry | kIOOutputCommandStall;
			}
		}
		if (!ncmAppendPacket(packet, pktlen)) {
			// The frame does not fit: flush the NTB and try again.
			if (!submitFillBuffer()) {
				// Packet not consumed: stall, keep it for later.
				return kIOOutputStatusRetry | kIOOutputCommandStall;
			}
			if (!ncmAppendPacket(packet, pktlen)) {
				// Cannot happen (the fresh NTB is empty):
				LOG(V_ERROR, "BUG: frame does not fit an empty NTB");
				fpNetStats->outputErrors++;
				freePacket(packet);
				return kIOReturnOutputDropped;
			}
		}
		freePacket(packet);

		// Flush when full, otherwise let the timer flush the tail:
		if (fTxFillOffset >= fNcmTxMax ||
			fTxNdpCount >= fNcmMaxDatagrams) {
			submitFillBuffer();
		} else {
			armTxFlushTimer();
		}

		const bool stallQueue = (numFreeOutBufs == 0);
		if (stallQueue) {
			LOG(V_PACKET, "Issuing stall command to the output queue");
		}
		return kIOOutputStatusAccepted |
			(stallQueue ? kIOOutputCommandStall : kIOOutputCommandNone);
	}
#endif

	const uint32_t msgLen = (uint32_t)(pktlen + sizeof(struct rndis_data_hdr));
	
	if (msgLen > maxOutTransferSize || msgLen > OUT_BUF_SIZE) {
		LOG(V_ERROR, "packet too large (%ld bytes, maximum can transmit %ld)",
			pktlen, maxOutTransferSize - sizeof(rndis_data_hdr));
		fpNetStats->outputErrors++;
		freePacket(packet);
		return kIOReturnOutputDropped;
	}

	// Make sure there is a fill buffer to append to:
	if (fTxFillBufIdx < 0) {
		if (!grabFillBuffer()) {
			// No free buffer: stall and re-try the same packet later.
			return kIOOutputStatusRetry | kIOOutputCommandStall;
		}
	}

#if TX_AGGREGATE
	// If the current packet does not fit, flush the aggregate first:
	{
#if TX_ZEROCOPY
		const bool fits = !fTxHead ||
			(fTxBytes + msgLen <= (uint32_t)OUT_BUF_SIZE);
#else
		const bool fits = (fTxFillOffset + msgLen <= OUT_BUF_SIZE);
#endif
		if (!fits) {
			if (!submitFillBuffer()) {
				// Packet not consumed: stall, keep the packet for later.
				return kIOOutputStatusRetry | kIOOutputCommandStall;
			}
		}
	}
#endif

#if TX_ZEROCOPY
	// Zero-copy append: [RNDIS header mbuf + packet chain], concatenated
	// onto the aggregate chain - the payload is never copied.
	mbuf_t hdrM = allocatePacket(sizeof(struct rndis_data_hdr));
	if (!hdrM) {
		LOG(V_ERROR, "allocatePacket for the RNDIS header failed");
		fpNetStats->outputErrors++;
		freePacket(packet);
		return kIOReturnOutputDropped;
	}
	struct rndis_data_hdr *hdr = (struct rndis_data_hdr *)mbuf_data(hdrM);
	memset(hdr, 0, sizeof *hdr);
	hdr->msg_type = RNDIS_MSG_PACKET;
	hdr->msg_len = cpu_to_le32(pktlen + sizeof *hdr);
	hdr->data_offset = cpu_to_le32(sizeof(*hdr) - 8);
	hdr->data_len = cpu_to_le32(pktlen);
	mbuf_setlen(hdrM, sizeof *hdr);
	mbuf_pkthdr_setlen(hdrM, sizeof *hdr);
	// 'mbuf_concatenate' returns the merged chain (NULL only if dst was
	// NULL, which cannot happen here) and does NOT maintain the packet
	// header length - we track the aggregate length ourselves:
	mbuf_t merged = mbuf_concatenate(hdrM, packet);
	if (!merged) {
		LOG(V_ERROR, "mbuf_concatenate failed");
		fpNetStats->outputErrors++;
		freePacket(hdrM);
		freePacket(packet);
		return kIOReturnOutputDropped;
	}
	hdrM = merged;
	if (fTxHead) {
		merged = mbuf_concatenate(fTxHead, hdrM);
		if (!merged) {
			LOG(V_ERROR, "mbuf_concatenate (aggregate) failed");
			fpNetStats->outputErrors++;
			freePacket(fTxHead);
			fTxHead = NULL;
			fTxBytes = 0;
			freePacket(hdrM);
			return kIOReturnOutputDropped;
		}
		fTxHead = merged;
	} else {
		fTxHead = hdrM;
	}
	fTxBytes += msgLen;

	// Flush immediately when full, otherwise let the timer flush the tail:
	if (fTxBytes >= (uint32_t)OUT_BUF_SIZE) {
		submitFillBuffer();
	} else {
		armTxFlushTimer();
	}
#else
	// Copy-based append (legacy path):
	struct rndis_data_hdr *hdr = (struct rndis_data_hdr *)
		((char *)outbufs[fTxFillBufIdx].mdp->getBytesNoCopy() + fTxFillOffset);
	memset(hdr, 0, sizeof *hdr);
	hdr->msg_type = RNDIS_MSG_PACKET;
	hdr->msg_len = cpu_to_le32(pktlen + sizeof *hdr);
	hdr->data_offset = cpu_to_le32(sizeof(*hdr) - 8);
	hdr->data_len = cpu_to_le32(pktlen);
	mbuf_copydata(packet, 0, pktlen, hdr + 1);
	fTxFillOffset += msgLen;
	freePacket(packet);

#if TX_AGGREGATE
	// Flush immediately when full, otherwise let the timer flush the tail:
	if (fTxFillOffset >= OUT_BUF_SIZE) {
		submitFillBuffer();
	} else {
		armTxFlushTimer();
	}
#else
	// Legacy behavior: one RNDIS data message per USB transfer.
	submitFillBuffer();
#endif
#endif

	// If no free buffer remains, stall the queue until a write completes:
	const bool stallQueue = (numFreeOutBufs == 0);
	if (stallQueue) {
		LOG(V_PACKET, "Issuing stall command to the output queue");
	}
	return kIOOutputStatusAccepted |
		(stallQueue ? kIOOutputCommandStall : kIOOutputCommandNone);
}

void HoRNDIS::callbackExit() {
	fCallbackCount--;
	// Notify the 'disable' that may be waiting for callback count to reach 0:
	if (fCallbackCount <= 0) {
		LOG(V_DEBUG, "Notifying last callback exited");
		getCommandGate()->commandWakeup(&fCallbackCount);
	}
}

void HoRNDIS::dataWriteComplete(void *obj, void *param, IOReturn rc, UInt32 transferred) {
	HoRNDIS	*me = (HoRNDIS *)obj;
	unsigned long poolIndx = (unsigned long)param;

	LOG(V_PACKET, "(rc %08x, poolIndx %ld)", rc, poolIndx);
	// Callback completed. We don't know when/if we launch another one:
	me->callbackExit();

	// Note, if 'fReadyToTransfer' is false, we shall not go further:
	// it's a good idea NOT to touch the 'outbufs'.
	if (isTransferStopStatus(rc) || !me->fReadyToTransfer) {
		LOG(V_DEBUG, "Data Write Aborted, or ready-to-transfer is cleared.");
		return;
	}

	if (rc != kIOReturnSuccess) {
		// Write error. In case of pipe stall,
		// we shall clear it on the next transmit.
		LOG(V_ERROR, "I/O error: %08x", rc);
		me->fpNetStats->outputErrors++;
	}

	// Free the buffer: put the index back onto the stack:
	if (me->numFreeOutBufs >= N_OUT_BUFS) {
		LOG(V_ERROR, "BUG: more free buffers than was allocated");
		return;
	}

#if TX_ZEROCOPY
	// Zero-copy TX: release the mbuf chain + descriptor held by the slot:
	OSSafeReleaseNULL(me->outbufs[poolIndx].mbufMdp);
	if (me->outbufs[poolIndx].m) {
		freePacket(me->outbufs[poolIndx].m);
		me->outbufs[poolIndx].m = NULL;
	}
	me->outbufs[poolIndx].dmaBuf = NULL;
#endif

	me->outbufStack[me->numFreeOutBufs] = (uint16_t)poolIndx;
	me->numFreeOutBufs++;
	// Unstall the queue whenever the number of free buffers goes 0->1.
	// I.e. we unstall it the moment we're able to write something into it:
	if (me->numFreeOutBufs == 1) {
		me->getOutputQueue()->service();
	}
}


/***** Packet receive logic *****/
void HoRNDIS::dataReadComplete(void *obj, void *param, IOReturn rc, UInt32 transferred) {
	HoRNDIS	*me = (HoRNDIS *)obj;
	pipebuf_t *inbuf = (pipebuf_t *)param;

	// Stop conditions. Not separating them out, since reacting to individual
	// ones would be very timing-sansitive.
	if (isTransferStopStatus(rc) || !me->fReadyToTransfer) {
		LOG(V_DEBUG, "READER STOPPED: USB device aborted or not responding, "
			"or 'fReadyToTransfer' flag is cleared.");
		me->releaseReadBuffer(inbuf);
		me->callbackExit();
		return;
	}

#if RX_ZEROCOPY
	// Detach the completed packet from the slot BEFORE re-arming, so that a
	// failed re-arm leaves the slot in a clean, retryable state:
	mbuf_t data = inbuf->m;
	IOMbufMemoryDescriptor *mdp = inbuf->mbufMdp;
	inbuf->m = NULL;
	inbuf->mbufMdp = NULL;
	inbuf->dmaBuf = NULL;
	OSSafeReleaseNULL(mdp);  // Descriptor's job is done; the mbuf holds the data.

	// Re-arm the read first: keeps the RX pipeline full.
	bool rearmed = me->startRead(inbuf);
	if (!rearmed) {
		me->fNeedReadRestart = true;
		me->armTxFlushTimer();  // Retry tick (see 'txFlushTimerFired').
	}

	if (rc == kIOReturnSuccess) {
		// Hand the transfer to the parser; it takes ownership of 'data'.
		LOG(V_PACKET, "Reader(%ld), tid=%lld: %d bytes", inbuf - me->inbufs,
			thread_tid(current_thread()), transferred);
#if NCM_SUPPORT
		if (me->fProtocol == kProtocolNCM) {
			me->ncmReceivePacket(data, transferred);
		} else
#endif
		{
			me->receivePacketZeroCopy(data, transferred);
		}
	} else {
		LOG(V_ERROR, "dataReadComplete: I/O error: %08x", rc);
		freePacket(data);
	}

	if (rearmed) {
		return;  // Callback continues via the new read.
	}
	me->callbackExit();
#else
	if (rc == kIOReturnSuccess) {
		// Got one?  Hand it to the back end.
		LOG(V_PACKET, "Reader(%ld), tid=%lld: %d bytes", inbuf - me->inbufs,
			thread_tid(current_thread()), transferred);
		me->receivePacket(inbuf->mdp->getBytesNoCopy(), transferred);
	} else {
		LOG(V_ERROR, "dataReadComplete: I/O error: %08x", rc);
	}
	
	// Queue the next one up.
	IOReturn ior = robustIO(inbuf->pipe, inbuf, (uint32_t)inbuf->mdp->getLength());
	if (ior == kIOReturnSuccess) {
		return;  // Callback is in-progress.
	}

	LOG(V_ERROR, "READER STOPPED: USB failure trying to read: %08x", ior);
	me->callbackExit();
	me->fDataDead = true;
	// The data channel is dead while the device may still be attached:
	// report link down so the network stack can fail over.
	me->setLinkStatus(kIONetworkLinkValid, 0);
#endif
}

/*!
 * Transfer the packet we've received to the MAC OS Network stack.
 */
void HoRNDIS::receivePacket(void *packet, UInt32 size) {
	mbuf_t m;
	UInt32 submit;
	IOReturn rv;
	
	LOG(V_PACKET, "packet sz %d", (int)size);
	
	while (size) {
		struct rndis_data_hdr *hdr = (struct rndis_data_hdr *)packet;
		uint32_t msg_len, data_ofs, data_len;
		
		if (size <= sizeof(struct rndis_data_hdr)) {
			LOG(V_ERROR, "receivePacket() on too small packet? (size %d)", size);
			return;
		}
		
		msg_len = le32_to_cpu(hdr->msg_len);
		data_ofs = le32_to_cpu(hdr->data_offset);
		data_len = le32_to_cpu(hdr->data_len);
		
		if (hdr->msg_type != RNDIS_MSG_PACKET) { // both are LE, so that's okay
			LOG(V_ERROR, "non-PACKET over data channel? (msg_type %08x)", hdr->msg_type);
			return;
		}
		
		if (msg_len > size) {
			LOG(V_ERROR, "msg_len too big?");
			return;
		}
		
		if ((data_ofs + data_len + 8) > msg_len) {
			LOG(V_ERROR, "data bigger than msg?");
			return;
		}
	
		m = allocatePacket(data_len);
		if (!m) {
			LOG(V_ERROR, "allocatePacket for data_len %d failed", data_len);
			fpNetStats->inputErrors++;
			return;
		}
		LOG(V_PTR, "PTR: mbuf: %p", m);
		
		rv = mbuf_copyback(m, 0, data_len, (char *)packet + data_ofs + 8, MBUF_WAITOK);
		if (rv) {
			LOG(V_ERROR, "mbuf_copyback failed, rv %08x", rv);
			fpNetStats->inputErrors++;
			freePacket(m);
			return;
		}

		submit = fNetworkInterface->inputPacket(m, data_len);
		LOG(V_PACKET, "submitted pkt sz %d", data_len);
		fpNetStats->inputPackets++;
		
		size -= msg_len;
		packet = (char *)packet + msg_len;
	}
}

#if RX_ZEROCOPY
/*!
 * Zero-copy variant of 'receivePacket': the transfer already landed in an
 * mbuf packet (see 'startRead'). Each RNDIS PDU is carved out of the chain
 * with 'mbuf_split', the RNDIS header is stripped with 'mbuf_adj', and the
 * resulting mbuf is handed straight to the network stack - no data copy.
 * Takes ownership of 'm' in all cases.
 */
void HoRNDIS::receivePacketZeroCopy(mbuf_t m, UInt32 size) {
	if (size == 0) {
		freePacket(m);
		return;
	}

	while (size) {
		struct rndis_data_hdr hdr;
		uint32_t msg_len, data_ofs, data_len;

		if (size <= sizeof(hdr)) {
			LOG(V_ERROR, "receivePacket() on too small packet? (size %d)", size);
			break;
		}

		mbuf_copydata(m, 0, sizeof(hdr), &hdr);

		msg_len = le32_to_cpu(hdr.msg_len);
		data_ofs = le32_to_cpu(hdr.data_offset);
		data_len = le32_to_cpu(hdr.data_len);

		if (hdr.msg_type != RNDIS_MSG_PACKET) { // both are LE, so that's okay
			LOG(V_ERROR, "non-PACKET over data channel? (msg_type %08x)",
				le32_to_cpu(hdr.msg_type));
			break;
		}

		if (msg_len > size || msg_len < sizeof(hdr)) {
			LOG(V_ERROR, "msg_len too big (or too small)?");
			break;
		}

		if (data_len == 0 || (data_ofs + data_len + 8) > msg_len) {
			LOG(V_ERROR, "data bigger than msg?");
			break;
		}

		// Carve this PDU out of the chain. When it is the last one and fills
		// the transfer exactly, there is nothing to split off, so we just
		// consume the whole chain in place:
		mbuf_t tail = NULL;
		if (msg_len < size) {
			if (!splitMbuf(m, msg_len, &tail)) {
				LOG(V_ERROR, "mbuf_split failed");
				break;
			}
		}
		// 'm' is now exactly this PDU's message. Strip the RNDIS header
		// from the front, and any trailing padding from the end (negative
		// mbuf_adj trims the chain tail and fixes up the packet length):
		mbuf_adj(m, data_ofs + 8);
		size_t leftover = mbuf_pkthdr_len(m) - data_len;
		if (leftover > 0) {
			mbuf_adj(m, -(int)leftover);
		}

		LOG(V_PACKET, "submitted pkt sz %d", data_len);
		fNetworkInterface->inputPacket(m, data_len);
		fpNetStats->inputPackets++;

		m = tail;
		size -= msg_len;
	}

	// Drop whatever could not be consumed (malformed remainder or empty tail):
	if (m) {
		freePacket(m);
	}
}
#endif


/***** RNDIS command logic *****/

IOReturn HoRNDIS::rndisCommand(struct rndis_msg_hdr *buf, int buflen) {
	IOReturn rc = kIOReturnSuccess;
	if (!fCommInterface) {  // Safety: make sure 'fCommInterface' is valid.
		LOG(V_ERROR, "fCommInterface is NULL, bailing out");
		return kIOReturnError;
	}

	// Serialize control commands. The response wait below releases the
	// command gate, so another client (enable/disable/keep-alive) could
	// otherwise issue a command concurrently and stomp on the response
	// buffer. NOTE: this function MUST NOT be called from a workloop
	// event-source action - it may sleep with the gate released, which
	// would block the workloop thread itself.
	while (fCmdInProgress) {
		rc = getCommandGate()->commandSleep(&fCmdInProgress);
		if (rc != kIOReturnSuccess) {  // Interrupted.
			LOG(V_ERROR, "rndisCommand: interrupted while waiting "
				"for the control channel");
			return rc;
		}
	}
	fCmdInProgress = true;

	const uint8_t ifNum = fCommInterface->getInterfaceDescriptor()->bInterfaceNumber;

	if (buf->msg_type != RNDIS_MSG_HALT && buf->msg_type != RNDIS_MSG_RESET) {
		buf->request_id = cpu_to_le32(rndisXid++);
		if (!buf->request_id) {
			buf->request_id = cpu_to_le32(rndisXid++);
		}
		
		LOG(V_DEBUG, "Generated xid: %d", le32_to_cpu(buf->request_id));
	}
	const uint32_t old_msg_type = buf->msg_type;
	const uint32_t old_request_id = buf->request_id;
	
	{
		DeviceRequest rq;
		rq.bmRequestType = kDeviceRequestDirectionOut |
			kDeviceRequestTypeClass | kDeviceRequestRecipientInterface;
		rq.bRequest = USB_CDC_SEND_ENCAPSULATED_COMMAND;
		rq.wValue = 0;
		rq.wIndex = ifNum;
		rq.wLength = le32_to_cpu(buf->msg_len);
	
		uint32_t bytes_transferred;
		if ((rc = fCommInterface->deviceRequest(rq, buf, bytes_transferred)) != kIOReturnSuccess) {
			LOG(V_DEBUG, "Device request send error");
			goto out;
		}
		if (bytes_transferred != rq.wLength) {
			LOG(V_DEBUG, "Incomplete device transfer");
			rc = kIOReturnError;
			goto out;
		}
	}

	// The RNDIS control messages are done via 'deviceRequest' - issue control
	// transfers on the device's default endpoint. Per [MSDN-RNDISUSB], once
	// the driver sends an OUT device transfer, it should wait for a
	// notification on the interrupt endpoint and only then retrieve the
	// result. The wait below does exactly that - with the command gate
	// RELEASED while sleeping - and falls back to a bounded tick-poll for
	// devices that do not send notifications (or have no interrupt pipe).
	//
	// Reference:
	// https://docs.microsoft.com/en-us/windows-hardware/drivers/network/control-channel-characteristics

	// Now we wait around a while for the device to get back to us.
	int count;
	for (count = 0; count < RNDIS_CMD_RETRIES; count++) {
		// Sleep with the gate released; the interrupt notification (or the
		// control-timer tick as a fallback) wakes us:
		fCmdWaitActive = true;
		armCtrlTimer(kCtrlTimerCmdWait, RNDIS_CMD_WAIT_US);
		rc = getCommandGate()->commandSleep(&fCmdWaitEvent);
		fCmdWaitActive = false;
		if (rc != kIOReturnSuccess) {  // Interrupted.
			LOG(V_ERROR, "rndisCommand: wait interrupted");
			goto out;
		}

		// Make sure 'fCommInterface' was not taken away from us while
		// we were doing synchronous IO:
		if (!fCommInterface) {
			LOG(V_ERROR, "fCommInterface was closed, bailing out");
			rc = kIOReturnError;
			goto out;
		}

		DeviceRequest rq;
		rq.bmRequestType = kDeviceRequestDirectionIn |
			kDeviceRequestTypeClass | kDeviceRequestRecipientInterface;
		rq.bRequest = USB_CDC_GET_ENCAPSULATED_RESPONSE;
		rq.wValue = 0;
		rq.wIndex = ifNum;
		rq.wLength = RNDIS_CMD_BUF_SZ;

		uint32_t bytes_transferred;
		if ((rc = fCommInterface->deviceRequest(rq, buf, bytes_transferred)) != kIOReturnSuccess) {
			goto out;
		}

		if (bytes_transferred < 12) {
			LOG(V_ERROR, "short read on control request?");
			continue;
		}
		
		if (buf->msg_type == (old_msg_type | RNDIS_MSG_COMPLETION)) {
			if (buf->request_id == old_request_id) {
				if (buf->msg_type == RNDIS_MSG_RESET_C) {
					// This is probably incorrect: the RESET_C does not have
					// 'request_id', but we don't issue resets => don't care.
					break;
				}
				if (buf->status != RNDIS_STATUS_SUCCESS) {
					LOG(V_ERROR, "RNDIS command returned status %08x",
						le32_to_cpu(buf->status));
					rc = kIOReturnError;
					break;
				}
				if (le32_to_cpu(buf->msg_len) != bytes_transferred) {
					LOG(V_ERROR, "Message Length mismatch: expected: %d, actual: %d",
						le32_to_cpu(buf->msg_len), bytes_transferred);
					rc = kIOReturnError;
					break;
				}
				LOG(V_DEBUG, "RNDIS command completed");
				break;
			} else {
				// A stale response (e.g. the unsolicited KEEPALIVE_C left
				// behind by the keep-alive sender): keep waiting.
				LOG(V_DEBUG, "RNDIS return had incorrect xid?");
			}
		} else if (buf->msg_type == RNDIS_MSG_INDICATE) {
			// Unsolicited status message: act on media state, then keep
			// waiting for our own completion:
			handleIndication(le32_to_cpu(buf->status));
			continue;
		} else if (buf->msg_type == RNDIS_MSG_KEEPALIVE) {
			// Unsolicited keep-alive (some devices poke the host first):
			LOG(V_DEBUG, "unsolicited RNDIS_MSG_KEEPALIVE");
			continue;
		} else {
			LOG(V_ERROR, "unexpected msg type %08x, msg_len %08x",
				le32_to_cpu(buf->msg_type), le32_to_cpu(buf->msg_len));
			continue;
		}
	}
	if (count == RNDIS_CMD_RETRIES) {
		LOG(V_ERROR, "command timed out?");
		rc = kIOReturnTimeout;
	}

out:
	fCmdInProgress = false;
	getCommandGate()->commandWakeup(&fCmdInProgress);
	return rc;
}

/***** RNDIS control-channel helpers *****/

// Start (or restart) the control-interrupt read. Returns false when there
// is no interrupt pipe or the transfer could not be submitted.
bool HoRNDIS::startIntRead() {
	if (!fIntPipe || !fIntBuf.mdp) {
		return false;
	}
	fIntBuf.comp.owner = this;
	fIntBuf.comp.action = dataIntReadComplete;
	fIntBuf.comp.parameter = &fIntBuf;
	fIntBuf.dmaBuf = fIntBuf.mdp;
	IOReturn rtn = robustIO(fIntPipe, &fIntBuf, RNDIS_CMD_BUF_SZ);
	if (rtn != kIOReturnSuccess) {
		LOG(V_DEBUG, "Failed to start the control interrupt read: %08x", rtn);
		return false;
	}
	fCallbackCount++;
	return true;
}

void HoRNDIS::dataIntReadComplete(void *obj, void *param, IOReturn rc, UInt32 transferred) {
	HoRNDIS *me = (HoRNDIS *)obj;
	pipebuf_t *inbuf = (pipebuf_t *)param;

	if (isTransferStopStatus(rc) || !me->fIntReadActive) {
		LOG(V_DEBUG, "INT READER STOPPED");
		me->callbackExit();
		return;
	}

	if (rc == kIOReturnSuccess && transferred >= sizeof(struct rndis_msg_hdr)) {
		me->handleInterruptData(inbuf->mdp->getBytesNoCopy(), transferred);
	}

	// Re-arm:
	IOReturn ior = robustIO(me->fIntPipe, inbuf, RNDIS_CMD_BUF_SZ);
	if (ior == kIOReturnSuccess) {
		return;  // Still in flight.
	}
	LOG(V_ERROR, "INT READER STOPPED: USB failure: %08x", ior);
	me->callbackExit();
}

void HoRNDIS::handleInterruptData(const void *data, UInt32 len) {
#if NCM_SUPPORT
	if (fProtocol == kProtocolNCM) {
		// CDC notifications (NETWORK_CONNECTION etc.); NCM has no
		// encapsulated-command responses to wait for.
		ncmHandleNotification(data, len);
		return;
	}
#endif

	const struct rndis_msg_hdr *hdr = (const struct rndis_msg_hdr *)data;

	// Any notification implies a control-channel response *may* be ready:
	// wake a sleeping 'rndisCommand' (it re-checks the actual response).
	if (fCmdWaitActive) {
		fCmdWaitActive = false;
		getCommandGate()->commandWakeup(&fCmdWaitEvent);
	}

	if (hdr->msg_type == RNDIS_MSG_INDICATE && len >= 16) {
		handleIndication(le32_to_cpu(hdr->status));
	}
}

void HoRNDIS::handleIndication(uint32_t status) {
	if (status == le32_to_cpu(RNDIS_STATUS_MEDIA_CONNECT)) {
		LOG(V_NOTE, "Media connect indication");
		if (fNetifEnabled) {
			setLinkStatus(kIONetworkLinkActive | kIONetworkLinkValid,
				getCurrentMedium());
		}
	} else if (status == le32_to_cpu(RNDIS_STATUS_MEDIA_DISCONNECT)) {
		LOG(V_NOTE, "Media disconnect indication");
		if (fNetifEnabled) {
			setLinkStatus(kIONetworkLinkValid, 0);
		}
	} else {
		LOG(V_DEBUG, "RNDIS indication, status %08x", status);
	}
}

void HoRNDIS::armCtrlTimer(CtrlTimerPurpose purpose, uint32_t us) {
	if (!fCtrlTimer) {
		return;
	}
	// A command wait always wins over a pending keep-alive tick (it is the
	// shorter deadline); the keep-alive cadence re-arms from the tick
	// handler afterwards.
	if (fCtrlTimerArmed && purpose == kCtrlTimerKeepalive &&
		fCtrlTimerPurpose == kCtrlTimerCmdWait) {
		return;
	}
	fCtrlTimerPurpose = purpose;
	fCtrlTimerArmed = true;
	fCtrlTimer->setTimeoutUS(us);
}

void HoRNDIS::ctrlTimerFired(OSObject *owner, IOTimerEventSource *timer) {
	HoRNDIS *me = (HoRNDIS *)owner;
	CtrlTimerPurpose purpose = me->fCtrlTimerPurpose;
	me->fCtrlTimerPurpose = kCtrlTimerNone;
	me->fCtrlTimerArmed = false;

	switch (purpose) {
	case kCtrlTimerCmdWait:
		if (me->fCmdWaitActive) {
			me->fCmdWaitActive = false;
			me->getCommandGate()->commandWakeup(&me->fCmdWaitEvent);
		}
		break;
	case kCtrlTimerKeepalive:
		me->sendKeepalive();
		break;
	case kCtrlTimerNone:
		break;
	}

	// A keep-alive tick displaced by a command wait must not kill the
	// keep-alive cadence: re-arm it unless a command wait is currently
	// armed (armCtrlTimer refuses to displace those).
#if RNDIS_KEEPALIVE
	if (purpose != kCtrlTimerKeepalive && me->fKeepaliveEnabled &&
		me->fNetifEnabled) {
		me->armCtrlTimer(kCtrlTimerKeepalive, RNDIS_KEEPALIVE_US);
	}
#endif
}

#if RNDIS_KEEPALIVE
void HoRNDIS::sendKeepalive() {
	if (!fKeepaliveEnabled || !fNetifEnabled || !fCommInterface) {
		return;
	}
	if (fCmdInProgress) {
		// A command is running (this tick displaced its wait): try again
		// on the next cadence.
		armCtrlTimer(kCtrlTimerKeepalive, RNDIS_KEEPALIVE_US);
		return;
	}

	struct {
		struct rndis_msg_hdr hdr;
	} __attribute__((packed)) ka;
	memset(&ka, 0, sizeof ka);
	ka.hdr.msg_type = RNDIS_MSG_KEEPALIVE;
	ka.hdr.msg_len = cpu_to_le32(sizeof ka);

	// Fire-and-forget OUT transfer: this function runs as a workloop timer
	// action and must not block. The KEEPALIVE_C response is drained by
	// the next command's response loop (xid mismatch is handled there).
	DeviceRequest rq;
	rq.bmRequestType = kDeviceRequestDirectionOut |
		kDeviceRequestTypeClass | kDeviceRequestRecipientInterface;
	rq.bRequest = USB_CDC_SEND_ENCAPSULATED_COMMAND;
	rq.wValue = 0;
	rq.wIndex = fCommInterface->getInterfaceDescriptor()->bInterfaceNumber;
	rq.wLength = sizeof ka;
	uint32_t transferred = 0;
	IOReturn rc = fCommInterface->deviceRequest(rq, &ka, transferred);
	LOG(V_DEBUG, "RNDIS keep-alive sent: %08x", rc);

	if (fNetifEnabled && fCommInterface) {
		armCtrlTimer(kCtrlTimerKeepalive, RNDIS_KEEPALIVE_US);
	}
}
#else
void HoRNDIS::sendKeepalive() {
	// RNDIS_KEEPALIVE is disabled: nothing to do.
}
#endif

// Recompute the device's packet filter from the current multicast /
// promiscuous requests and apply it if changed. Returns false when the
// filter could not be applied (a fresh filter counts as a change).
bool HoRNDIS::updatePacketFilter() {
	if (!fCommInterface) {
		return false;
	}

#if NCM_SUPPORT
	if (fProtocol == kProtocolNCM) {
		// CDC filter bits (different encoding than RNDIS):
		uint32_t filter = USB_CDC_PACKET_TYPE_DIRECTED |
			USB_CDC_PACKET_TYPE_BROADCAST;
#if RNDIS_DYNAMIC_FILTER
		if (fMulticastEnabled) {
			filter |= USB_CDC_PACKET_TYPE_ALL_MULTICAST;
		}
		if (fPromiscEnabled) {
			filter |= USB_CDC_PACKET_TYPE_PROMISCUOUS;
		}
#else
		filter |= USB_CDC_PACKET_TYPE_ALL_MULTICAST |
			USB_CDC_PACKET_TYPE_PROMISCUOUS;
#endif
		if (filter == fCurrentFilter) {
			return true;  // Nothing to do.
		}
		if (!ncmSetPacketFilter(filter)) {
			LOG(V_ERROR, "Cannot set the NCM packet filter to %08x", filter);
			return false;
		}
		fCurrentFilter = filter;
		LOG(V_DEBUG, "NCM packet filter is now %08x", filter);
		return true;
	}
#endif

#if RNDIS_DYNAMIC_FILTER
	uint32_t filter = RNDIS_PACKET_TYPE_DIRECTED | RNDIS_PACKET_TYPE_BROADCAST;
	if (fMulticastEnabled) {
		filter |= RNDIS_PACKET_TYPE_ALL_MULTICAST;
	}
	if (fPromiscEnabled) {
		filter |= RNDIS_PACKET_TYPE_PROMISCUOUS;
	}
#else
	uint32_t filter = RNDIS_DEFAULT_FILTER;
#endif
	if (filter == fCurrentFilter) {
		return true;  // Nothing to do.
	}
	if (!rndisSetPacketFilter(filter)) {
		LOG(V_ERROR, "Cannot set the packet filter to %08x", filter);
		return false;
	}
	fCurrentFilter = filter;
	LOG(V_DEBUG, "Packet filter is now %08x", filter);
	return true;
}

int HoRNDIS::rndisQuery(void *buf, uint32_t oid, uint32_t in_len, void **reply, int *reply_len) {
	int rc;
	
	union {
		void *buf;
		struct rndis_msg_hdr *hdr;
		struct rndis_query *get;
		struct rndis_query_c *get_c;
	} u;
	uint32_t off, len;
	
	u.buf = buf;
	
	memset(u.get, 0, sizeof(*u.get) + in_len);
	u.get->msg_type = RNDIS_MSG_QUERY;
	u.get->msg_len = cpu_to_le32(sizeof(*u.get) + in_len);
	u.get->oid = oid;
	u.get->len = cpu_to_le32(in_len);
	u.get->offset = cpu_to_le32(20);
	
	rc = rndisCommand(u.hdr, RNDIS_CMD_BUF_SZ);
	if (rc != kIOReturnSuccess) {
		LOG(V_ERROR, "RNDIS_MSG_QUERY failure? %08x", rc);
		return rc;
	}
	
	off = le32_to_cpu(u.get_c->offset);
	len = le32_to_cpu(u.get_c->len);
	LOG(V_DEBUG, "RNDIS query completed");
	
	if ((8 + off + len) > RNDIS_CMD_BUF_SZ) {
		goto fmterr;
	}
	if (*reply_len != -1 && len != *reply_len) {
		goto fmterr;
	}
	
	*reply = ((unsigned char *) &u.get_c->request_id) + off;
	*reply_len = len;
	
	return 0;

fmterr:
	LOG(V_ERROR, "protocol error?");
	return -1;
}

bool HoRNDIS::rndisInit() {
	int rc;
	union {
		struct rndis_msg_hdr *hdr;
		struct rndis_init *init;
		struct rndis_init_c *init_c;
	} u;
	
	u.hdr = (rndis_msg_hdr *)IOMallocAligned(RNDIS_CMD_BUF_SZ, sizeof(void *));
	if (!u.hdr) {
		LOG(V_ERROR, "out of memory?");
		return false;
	}
	
	u.init->msg_type = RNDIS_MSG_INIT;
	u.init->msg_len = cpu_to_le32(sizeof *u.init);
	u.init->major_version = cpu_to_le32(1);
	u.init->minor_version = cpu_to_le32(0);
	// This is the maximum USB transfer the device is allowed to make to host:
	u.init->max_transfer_size = IN_BUF_SIZE;
	rc = rndisCommand(u.hdr, RNDIS_CMD_BUF_SZ);
	if (rc != kIOReturnSuccess) {
		LOG(V_ERROR, "INIT not successful?");
		IOFreeAligned(u.hdr, RNDIS_CMD_BUF_SZ);
		return false;
	}

	if (fCommInterface) {  // Safety: don't accesss 'fCommInterface if NULL.
		LOG(V_NOTE, "'%s': ver=%d.%d, max_packets_per_transfer=%d, "
			"max_transfer_size=%d, packet_alignment=2^%d",
			fCommInterface->getDevice()->getName(),
			le32_to_cpu(u.init_c->major_version),
			le32_to_cpu(u.init_c->minor_version),
			le32_to_cpu(u.init_c->max_packets_per_transfer),
			le32_to_cpu(u.init_c->max_transfer_size),
			le32_to_cpu(u.init_c->packet_alignment));
	}

	maxOutTransferSize = le32_to_cpu(u.init_c->max_transfer_size);
	// For now, let's limit the maxOutTransferSize by the Output Buffer size.
	// If we implement transmitting multiple PDUs in a single USB transfer,
	// we may want to size the output buffers based on
	// "u.init_c->max_transfer_size".
	maxOutTransferSize = min(maxOutTransferSize, OUT_BUF_SIZE);
	
	IOFreeAligned(u.hdr, RNDIS_CMD_BUF_SZ);
	
	return true;
}

bool HoRNDIS::rndisSetPacketFilter(uint32_t filter) {
	union {
		struct rndis_msg_hdr *hdr;
		struct rndis_set *set;
		struct rndis_set_c *set_c;
	} u;
	int rc;
	
	u.hdr = (rndis_msg_hdr *)IOMallocAligned(RNDIS_CMD_BUF_SZ, sizeof(void *));
	if (!u.hdr) {
		LOG(V_ERROR, "out of memory?");
		return false;;
	}
	
	memset(u.set, 0, sizeof *u.set);
	u.set->msg_type = RNDIS_MSG_SET;
	u.set->msg_len = cpu_to_le32(4 + sizeof *u.set);
	u.set->oid = OID_GEN_CURRENT_PACKET_FILTER;
	u.set->len = cpu_to_le32(4);
	u.set->offset = cpu_to_le32((sizeof *u.set) - 8);
	*(uint32_t *)(u.set + 1) = filter;
	
	rc = rndisCommand(u.hdr, RNDIS_CMD_BUF_SZ);
	if (rc != kIOReturnSuccess) {
		LOG(V_ERROR, "SET not successful?");
		IOFreeAligned(u.hdr, RNDIS_CMD_BUF_SZ);
		return false;
	}
	
	IOFreeAligned(u.hdr, RNDIS_CMD_BUF_SZ);
	
	return true;
}

#if NCM_SUPPORT
/***** CDC NCM protocol layer *****/

// Direct CDC class control request on the control interface. Unlike RNDIS
// there is no encapsulated-command indirection: the data comes back in the
// same control transfer, so this is a single synchronous request.
IOReturn HoRNDIS::ncmControlRequest(uint8_t bRequest, uint16_t wValue,
	void *data, uint16_t wLength, bool isIn) {
	if (!fCommInterface) {
		return kIOReturnError;
	}
	DeviceRequest rq;
	rq.bmRequestType =
		(isIn ? kDeviceRequestDirectionIn : kDeviceRequestDirectionOut) |
		kDeviceRequestTypeClass | kDeviceRequestRecipientInterface;
	rq.bRequest = bRequest;
	rq.wValue = wValue;
	rq.wIndex = fCommInterface->getInterfaceDescriptor()->bInterfaceNumber;
	rq.wLength = wLength;
	uint32_t transferred = 0;
	IOReturn rc = fCommInterface->deviceRequest(rq, data, transferred);
	if (rc != kIOReturnSuccess) {
		LOG(V_DEBUG, "NCM control request %02x failed: %08x", bRequest, rc);
		return rc;
	}
	if (transferred != wLength) {
		LOG(V_ERROR, "NCM control request %02x: short transfer (%d of %d)",
			bRequest, transferred, wLength);
		return kIOReturnError;
	}
	return kIOReturnSuccess;
}

// Initial one-time device setup; mirrors Linux cdc_ncm.c 'cdc_ncm_init'.
bool HoRNDIS::ncmInit() {
#if !RX_ZEROCOPY
	// The NCM RX path is zero-copy only (mbuf_split-based parsing); build
	// with RX_ZEROCOPY=1 or disable NCM_SUPPORT.
	LOG(V_ERROR, "NCM support requires RX_ZEROCOPY=1");
	return false;
#endif
	// 1. NTB format: we speak NTB16 (format 0, the device default). If the
	//    device reports another format, request 16-bit explicitly.
	uint16_t format = 0xFFFF;
	if (ncmControlRequest(USB_CDC_GET_NTB_FORMAT, 0, &format,
			sizeof(format), true) == kIOReturnSuccess) {
		if (le16_to_cpu(format) != USB_CDC_NCM_NTB16_FORMAT) {
			uint16_t newFmt = USB_CDC_NCM_NTB16_FORMAT;
			if (ncmControlRequest(USB_CDC_SET_NTB_FORMAT, 0, &newFmt,
					sizeof(newFmt), false) != kIOReturnSuccess) {
				LOG(V_ERROR, "Cannot select the NTB16 format "
					"(device reported %04x)", le16_to_cpu(format));
				return false;
			}
		}
	} else {
		LOG(V_DEBUG, "GET_NTB_FORMAT unsupported; assuming NTB16");
	}

	// 2. NTB parameters (required):
	struct ncm_ntb_parameters parm;
	if (ncmControlRequest(USB_CDC_GET_NTB_PARAMETERS, 0, &parm,
			sizeof(parm), true) != kIOReturnSuccess) {
		LOG(V_ERROR, "GET_NTB_PARAMETERS failed");
		return false;
	}

	uint32_t rxMax = le32_to_cpu(parm.dwNtbInMaxSize);
	uint32_t txMax = le32_to_cpu(parm.dwNtbOutMaxSize);
	// Clamp to sane bounds (mirrors Linux cdc_ncm.c):
	if (rxMax < NCM_NTB_MIN_SIZE) {
		LOG(V_NOTE, "dwNtbInMaxSize=%u is too small; using %d",
			rxMax, NCM_NTB_DEF_SIZE);
		rxMax = NCM_NTB_DEF_SIZE;
	}
	rxMax = min(rxMax, (uint32_t)NCM_NTB_MAX_SIZE);
	if (txMax < NCM_NTB_MIN_SIZE) {
		LOG(V_NOTE, "dwNtbOutMaxSize=%u is too small; using %d",
			txMax, NCM_NTB_DEF_SIZE);
		txMax = NCM_NTB_DEF_SIZE;
	}
	txMax = min(txMax, (uint32_t)NCM_NTB_MAX_SIZE);
	fNcmRxMax = rxMax;
	// The TX NTB has to fit into the existing OUT buffers:
	fNcmTxMax = min(txMax, (uint32_t)OUT_BUF_SIZE);

	fNcmNdpAlign = le16_to_cpu(parm.wNdpOutAlignment);
	if (fNcmNdpAlign < NCM_NDP_ALIGN_MIN) {
		fNcmNdpAlign = NCM_NDP_ALIGN_MIN;
	}
	fNcmModulus = le16_to_cpu(parm.wNdpOutDivisor);
	if (fNcmModulus < NCM_NDP_ALIGN_MIN) {
		fNcmModulus = NCM_NDP_ALIGN_MIN;
	}
	fNcmRemainder = le16_to_cpu(parm.wNdpOutPayloadRemainder);
	if (fNcmRemainder >= fNcmModulus) {
		// Bogus remainder would break the alignment math:
		fNcmRemainder = 0;
	}
	fNcmMaxDatagrams = le16_to_cpu(parm.wNtbOutMaxDatagrams);
	if (fNcmMaxDatagrams == 0 || fNcmMaxDatagrams > NCM_DPT_DATAGRAMS_MAX) {
		fNcmMaxDatagrams = NCM_DPT_DATAGRAMS_MAX;
	}
	fNcmMaxNdpSize = (uint16_t)(sizeof(struct ncm_ndp16) +
		(fNcmMaxDatagrams + 1) * 2 * sizeof(uint16_t));
	fNcmSeq = 0;

	LOG(V_NOTE, "NCM: rxMax=%u txMax=%u ndpAlign=%u modulus=%u "
		"remainder=%u maxDatagrams=%u maxNdpSize=%u",
		fNcmRxMax, fNcmTxMax, fNcmNdpAlign, fNcmModulus,
		fNcmRemainder, fNcmMaxDatagrams, fNcmMaxNdpSize);

	// 3. Tell the device our RX NTB size (required before RX works):
	uint32_t rxSize = fNcmRxMax;
	if (ncmControlRequest(USB_CDC_SET_NTB_INPUT_SIZE, 0, &rxSize,
			sizeof(rxSize), false) != kIOReturnSuccess) {
		LOG(V_ERROR, "SET_NTB_INPUT_SIZE failed");
		return false;
	}

	// 4. MAC address (cache it; the network stack reads it via
	//    'getHardwareAddress'):
	unsigned char mac[6];
	if (ncmControlRequest(USB_CDC_GET_NET_ADDRESS, 0, mac, sizeof(mac),
			true) == kIOReturnSuccess) {
		bool allZero = true;
		for (int i = 0; i < 6; i++) {
			if (mac[i] != 0) {
				allZero = false;
				break;
			}
		}
		if (!allZero) {
			memcpy(fHardwareAddress.bytes, mac, 6);
			fHasHardwareAddress = true;
			LOG(V_NOTE, "NCM MAC %02x:%02x:%02x:%02x:%02x:%02x",
				mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		}
	}
	if (!fHasHardwareAddress) {
		ncmFallbackMAC();
	}

	// 5. Max datagram (frame) size, for the MTU:
	uint16_t maxDgram = 0;
	if (ncmControlRequest(USB_CDC_GET_MAX_DATAGRAM_SIZE, 0, &maxDgram,
			sizeof(maxDgram), true) == kIOReturnSuccess) {
		maxDgram = le16_to_cpu(maxDgram);
	}
	if (maxDgram < NCM_MIN_DATAGRAM_SIZE || maxDgram > NCM_MAX_DATAGRAM_SIZE) {
		maxDgram = NCM_MIN_DATAGRAM_SIZE;
	}
	fNcmMaxDatagram = maxDgram;
	LOG(V_NOTE, "NCM max datagram size: %u", fNcmMaxDatagram);

	return true;
}

// Fabricate a locally-administered, per-device deterministic MAC so the
// interface identity is stable when the device reports no address.
void HoRNDIS::ncmFallbackMAC() {
	uint16_t vid = 0, pid = 0;
	if (fCommInterface && fCommInterface->getDevice() &&
		fCommInterface->getDevice()->getDeviceDescriptor()) {
		const DeviceDescriptor *dd =
			fCommInterface->getDevice()->getDeviceDescriptor();
		vid = le16_to_cpu(dd->idVendor);
		pid = le16_to_cpu(dd->idProduct);
	}
	fHardwareAddress.bytes[0] = 0x02;
	fHardwareAddress.bytes[1] = (unsigned char)(vid >> 8);
	fHardwareAddress.bytes[2] = (unsigned char)(vid & 0xFF);
	fHardwareAddress.bytes[3] = (unsigned char)(pid >> 8);
	fHardwareAddress.bytes[4] = (unsigned char)(pid & 0xFF);
	fHardwareAddress.bytes[5] = 0x01;
	fHasHardwareAddress = true;
	LOG(V_NOTE, "Fabricated NCM MAC %02x:%02x:%02x:%02x:%02x:%02x",
		fHardwareAddress.bytes[0], fHardwareAddress.bytes[1],
		fHardwareAddress.bytes[2], fHardwareAddress.bytes[3],
		fHardwareAddress.bytes[4], fHardwareAddress.bytes[5]);
}

bool HoRNDIS::ncmSetPacketFilter(uint32_t filter) {
	uint16_t f = (uint16_t)filter;
	return ncmControlRequest(USB_CDC_SET_ETHERNET_PACKET_FILTER, 0, &f,
		sizeof(f), false) == kIOReturnSuccess;
}

// Align the fill offset per the device's datagram alignment rules and
// append the frame into the TX NTB (copy-based; Ethernet frames are short
// enough that this copy is not the bottleneck). Returns false when the
// frame does not fit; 'packet' is consumed only on success.
bool HoRNDIS::ncmAppendPacket(mbuf_t packet, size_t pktlen) {
	uint32_t align = (fTxFillOffset - fNcmRemainder) % fNcmModulus;
	uint32_t pad = align ? (fNcmModulus - align) : 0;
	if (fTxFillOffset + pad + pktlen > fNcmTxMax) {
		return false;
	}
	uint8_t *buf = (uint8_t *)outbufs[fTxFillBufIdx].mdp->getBytesNoCopy();
	if (pad) {
		memset(buf + fTxFillOffset, 0, pad);
		fTxFillOffset += pad;
	}
	mbuf_copydata(packet, 0, pktlen, buf + fTxFillOffset);
	fTxNdpEntries[fTxNdpCount].index = (uint16_t)fTxFillOffset;
	fTxNdpEntries[fTxNdpCount].len = (uint16_t)pktlen;
	fTxNdpCount++;
	fTxFillOffset += (uint32_t)pktlen;
	return true;
}

// Materialize the NTH/NDP headers of the TX NTB sitting in the fill buffer
// (called by 'submitFillBuffer' right before the transfer goes out).
void HoRNDIS::ncmFlushNtb() {
	if (fTxFillBufIdx < 0 || fTxFillOffset == 0) {
		return;
	}
	uint8_t *buf = (uint8_t *)outbufs[fTxFillBufIdx].mdp->getBytesNoCopy();
	struct ncm_nth16 *nth = (struct ncm_nth16 *)buf;
	nth->wBlockLength = cpu_to_le16((uint16_t)fTxFillOffset);

	struct ncm_ndp16 *ndp = (struct ncm_ndp16 *)(buf + fTxNdpOffset);
	ndp->dwSignature = cpu_to_le32(USB_CDC_NCM_NDP16_NOCRC_SIGN);
	ndp->wLength = cpu_to_le16((uint16_t)(sizeof(struct ncm_ndp16) +
		(fTxNdpCount + 1) * 2 * sizeof(uint16_t)));
	ndp->wNextNdpIndex = 0;
	uint16_t *e = (uint16_t *)(ndp + 1);
	for (int i = 0; i < fTxNdpCount; i++) {
		e[2 * i] = cpu_to_le16(fTxNdpEntries[i].index);
		e[2 * i + 1] = cpu_to_le16(fTxNdpEntries[i].len);
	}
	e[2 * fTxNdpCount] = 0;      // Terminator pair (index, len) == (0, 0).
	e[2 * fTxNdpCount + 1] = 0;
}

// Parse a received NTB and hand its datagrams to the network stack.
// Zero-copy: datagrams are carved out with 'mbuf_split' and no data is
// copied. Takes ownership of 'm' in all cases.
void HoRNDIS::ncmReceivePacket(mbuf_t m, UInt32 size) {
	if (size == 0) {
		freePacket(m);
		return;
	}

	struct ncm_nth16 nth;
	if (size < sizeof(nth)) {
		LOG(V_ERROR, "NCM RX: transfer too small (%d)", size);
		freePacket(m);
		return;
	}
	mbuf_copydata(m, 0, sizeof(nth), &nth);
	if (nth.dwSignature != cpu_to_le32(USB_CDC_NCM_NTH16_SIGN)) {
		LOG(V_ERROR, "NCM RX: bad NTH signature %08x",
			le32_to_cpu(nth.dwSignature));
		freePacket(m);
		return;
	}
	uint16_t hdrLen = le16_to_cpu(nth.wHeaderLength);
	uint16_t blockLen = le16_to_cpu(nth.wBlockLength);
	if (hdrLen != sizeof(nth) || blockLen < hdrLen || blockLen > size) {
		LOG(V_ERROR, "NCM RX: bad NTH lengths (hdr %d, block %d, size %d)",
			hdrLen, blockLen, size);
		freePacket(m);
		return;
	}
	uint16_t ndpIndex = le16_to_cpu(nth.wNdpIndex);
	if (ndpIndex < hdrLen || ndpIndex + 8 > blockLen) {
		LOG(V_ERROR, "NCM RX: bad NDP index %d", ndpIndex);
		freePacket(m);
		return;
	}

	// Walk the (possibly chained) NDPs and extract the datagrams:
	uint32_t curOff = 0;  // Absolute offset of the head of 'm'.
	bool ok = true;
	while (ndpIndex != 0 && ok) {
		struct ncm_ndp16 ndp;
		if (ndpIndex + 8 > blockLen || ndpIndex < curOff) {
			LOG(V_ERROR, "NCM RX: NDP out of bounds");
			ok = false;
			break;
		}
		mbuf_copydata(m, ndpIndex - curOff, 8, &ndp);
		if (ndp.dwSignature != cpu_to_le32(USB_CDC_NCM_NDP16_NOCRC_SIGN) &&
			ndp.dwSignature != cpu_to_le32(USB_CDC_NCM_NDP16_CRC_SIGN)) {
			LOG(V_ERROR, "NCM RX: bad NDP signature %08x",
				le32_to_cpu(ndp.dwSignature));
			ok = false;
			break;
		}
		uint16_t ndpLen = le16_to_cpu(ndp.wLength);
		uint16_t nextNdp = le16_to_cpu(ndp.wNextNdpIndex);
		if (ndpLen < sizeof(ndp) + 4 || ndpIndex + ndpLen > blockLen) {
			LOG(V_ERROR, "NCM RX: bad NDP length %d", ndpLen);
			ok = false;
			break;
		}

		// Snapshot the entry table BEFORE consuming the chain (the NDP
		// region itself is discarded with the first split):
		struct {
			uint16_t index;
			uint16_t len;
		} dgrams[NCM_DPT_DATAGRAMS_MAX];
		uint16_t nDgrams = 0;
		uint16_t nEntries = (ndpLen - sizeof(ndp)) / 4;  // Includes terminator.
		for (uint16_t i = 0;
			i < nEntries && nDgrams < NCM_DPT_DATAGRAMS_MAX; i++) {
			uint16_t vals[2];
			mbuf_copydata(m, ndpIndex + sizeof(ndp) + 4 * i - curOff,
				sizeof(vals), vals);
			uint16_t dgIndex = le16_to_cpu(vals[0]);
			uint16_t dgLen = le16_to_cpu(vals[1]);
			if (dgIndex == 0 && dgLen == 0) {
				break;  // End of this NDP.
			}
			dgrams[nDgrams].index = dgIndex;
			dgrams[nDgrams].len = dgLen;
			nDgrams++;
		}

		for (uint16_t i = 0; i < nDgrams; i++) {
			uint16_t dgIndex = dgrams[i].index;
			uint16_t dgLen = dgrams[i].len;
			if (dgIndex < curOff || dgLen < 14 ||
				dgIndex + dgLen > blockLen) {
				LOG(V_ERROR, "NCM RX: datagram out of bounds "
					"(index %d, len %d, cur %d)", dgIndex, dgLen, curOff);
				ok = false;
				break;
			}

			// Discard the region before the datagram (NTH/NDP/padding):
			if (dgIndex > curOff) {
				mbuf_t rest = NULL;
				if (!splitMbuf(m, dgIndex - curOff, &rest)) {
					LOG(V_ERROR, "NCM RX: mbuf_split (pre) failed");
					ok = false;
					break;
				}
				freePacket(m);  // NTH/NDP/padding head.
				m = rest;
				curOff = dgIndex;
			}

			// Carve out exactly the datagram:
			mbuf_t dgram;
			if ((uint32_t)mbuf_pkthdr_len(m) > (uint32_t)dgLen) {
				mbuf_t tail = NULL;
				if (!splitMbuf(m, dgLen, &tail)) {
					LOG(V_ERROR, "NCM RX: mbuf_split (dgram) failed");
					ok = false;
					break;
				}
				dgram = m;
				m = tail;
			} else {
				// Datagram fills the rest of the chain:
				dgram = m;
				m = NULL;
			}
			LOG(V_PACKET, "submitted pkt sz %d", dgLen);
			fNetworkInterface->inputPacket(dgram, dgLen);
			fpNetStats->inputPackets++;
		}
		ndpIndex = nextNdp;
	}

	// Drop whatever could not be consumed:
	if (m) {
		freePacket(m);
	}
}

// CDC notification on the interrupt pipe:
//   bmRequestType == 0xA1, bNotificationType == 0 (NETWORK_CONNECTION),
//   wValue == 1/0 -> link up/down.
void HoRNDIS::ncmHandleNotification(const void *data, UInt32 len) {
	const uint8_t *p = (const uint8_t *)data;
	if (len < 8 || p[0] != 0xA1 ||
		p[1] != USB_CDC_NOTIFY_NETWORK_CONNECTION) {
		LOG(V_DEBUG, "NCM: unhandled notification");
		return;
	}
	uint16_t connected = p[2] | (p[3] << 8);
	if (connected) {
		LOG(V_NOTE, "NCM: network connected notification");
		if (fNetifEnabled) {
			setLinkStatus(kIONetworkLinkActive | kIONetworkLinkValid,
				getCurrentMedium());
		}
	} else {
		LOG(V_NOTE, "NCM: network disconnected notification");
		if (fNetifEnabled) {
			setLinkStatus(kIONetworkLinkValid, 0);
		}
	}
}
#endif /* NCM_SUPPORT */
