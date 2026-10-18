/* HoRNDIS.h
 * Declaration of IOKit-derived classes
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

#include <machine/limits.h>			/* UINT_MAX */
#include <libkern/OSAtomic.h>
#include <libkern/OSByteOrder.h>
#include <libkern/OSTypes.h>

#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>

#include <IOKit/IOTimerEventSource.h>
#include <IOKit/assert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMultiMemoryDescriptor.h>
#include <IOKit/IOMessage.h>

#include <IOKit/usb/IOUSBHostFamily.h>
#include <IOKit/usb/IOUSBHostPipe.h>

extern "C"
{
	#include <sys/param.h>
	#include <sys/mbuf.h>
}

#define cpu_to_le16(x) OSSwapHostToLittleInt16(x)
#define le16_to_cpu(x) OSSwapLittleToHostInt16(x)
#define cpu_to_le32(x) OSSwapHostToLittleInt32(x)
#define le32_to_cpu(x) OSSwapLittleToHostInt32(x)
// Helps to avoid including private classes and methods into the symbol table.
#define NOEXPORT	__attribute__((visibility("hidden")))

// REFERENCES:
// [MS-RNDIS]: Remote Network Driver Interface Specification (RNDIS) Protocol
//   https://winprotocoldoc.blob.core.windows.net/productionwindowsarchives/WinArchive/[MS-RNDIS].pdf
// [MSDN-RNDISUSB]: Remote NDIS To USB Mapping
//   https://docs.microsoft.com/en-us/windows-hardware/drivers/network/remote-ndis-to-usb-mapping

#define TRANSMIT_QUEUE_SIZE     256
// Aggregated TX buffer size. RNDIS data messages may carry several PDUs per
// USB transfer ([MS-RNDIS]: "1-N packets"), and devices report a
// max_transfer_size (spec-recommended 16K) that is typically far above the
// legacy 4K used here before. One 16K transfer with ~10 full-size frames in
// it removes most of the per-transfer overhead on SuperSpeed links.
#define OUT_BUF_SIZE            16384

// TX_AGGREGATE: pack multiple RNDIS PDUs into one USB transfer.
// 1 = aggregate (timer-flushed), 0 = one transfer per packet (legacy
// behavior). Keep the escape hatch in case a device's RNDIS firmware
// mishandles multi-PDU data messages.
#define TX_AGGREGATE            1

// How long the aggregated TX buffer is allowed to sit partially filled
// before it is flushed anyway (microseconds). Lower = less added latency,
// higher = better aggregation for small-packet flows.
#define TX_FLUSH_US             1000

// RX_ZEROCOPY: received packets are handed to the network stack as mbufs
// that reference the USB receive buffer directly (mbuf_attachcluster over
// the IOBufferMemoryDescriptor), instead of copying every byte into a
// fresh mbuf. Buffer lifetime is managed by a per-buffer lease refcount
// freed by the mbuf cluster free callback. 0 = copy-based RX.
#define RX_ZEROCOPY             1

// TX_ZEROCOPY: the outbound transfer is built as an mbuf chain (RNDIS
// header mbuf + packet chains) and submitted through a scatter-gather
// IOMemoryDescriptor (per-mbuf withAddress + IOMultiMemoryDescriptor)
// instead of copying payload bytes into a wired buffer. 0 = copy path.
#define TX_ZEROCOPY             1

// NCM_SUPPORT: additionally implement the CDC Network Control Model
// (Ethernet over USB, NTB16) protocol alongside RNDIS. The protocol is
// selected at probe time: a 02/0D/00 control interface picks NCM, anything
// else matching the RNDIS personalities keeps RNDIS. Set to 0 to build a
// pure RNDIS driver.
#define NCM_SUPPORT             1

// RNDIS_DYNAMIC_FILTER: keep the device's packet filter minimal (unicast +
// broadcast), adding multicast-all / promiscuous only while the network
// stack actually requests them (setMulticastMode/setPromiscuousMode). This
// stops the host from receiving every multicast/broadcast frame on the
// tether segment. 0 = legacy always-open filter (RNDIS_DEFAULT_FILTER).
#define RNDIS_DYNAMIC_FILTER    1

// RNDIS_KEEPALIVE: send REMOTE_NDIS_KEEPALIVE_MSG on the control channel
// every RNDIS_KEEPALIVE_US while the interface is up (per [MS-RNDIS] the
// host SHOULD do this). Fire-and-forget: the response is drained by the
// next command's response loop. 0 = disable.
#define RNDIS_KEEPALIVE         1
#define RNDIS_KEEPALIVE_US      5000000

// Control channel timing: 'rndisCommand' waits up to
// RNDIS_CMD_RETRIES x RNDIS_CMD_WAIT_US for the device to produce a
// response, releasing the command gate while waiting (the device's
// interrupt notification, when present, wakes us early).
#define RNDIS_CMD_RETRIES       10
#define RNDIS_CMD_WAIT_US       10000

// Per [MS-RNDIS], description of REMOTE_NDIS_INITIALIZE_MSG:
//    "MaxTransferSize (4 bytes): ... It SHOULD be set to 0x00004000"
// I.e. specs recommends we should be able to input 16K in a single transfer.
// Also, some Android versions (e.g. 8.1.0 on Pixel 2) seem to ignore
// "max_transfer_size" in "REMOTE_NDIS_INITIALIZE_MSG" and use packets up to
// 16K regardless.
#define IN_BUF_SIZE             16384

#define N_OUT_BUFS              4
// The N_IN_BUFS value should either be 1 or 2: the number of concurrent
// in-flight RX transfers. With RX_ZEROCOPY the receive buffer is handed to
// the network stack as an mbuf, so more in-flight reads = deeper pipeline.
// Historically 1 performed better on USB2 (half-duplex bus contention); on
// SuperSpeed full-duplex links 2 in-flight reads generally win. If your
// device still negotiates USB2, try 1.
#define N_IN_BUFS               2

// Maximum payload size in a standard (non-jumbo) Ethernet frame.
#define ETHERNET_MTU            1500

/***** RNDIS definitions -- from linux/include/linux/usb/rndis_host.h ****/

// Per [MSDN-RNDISUSB], "Control Channel Characteristics", it's the minumim
// buffer size the host should support (and it's way bigger than we need).
#define RNDIS_CMD_BUF_SZ		0x400

struct rndis_msg_hdr {
	uint32_t msg_type;
	uint32_t msg_len;
	uint32_t request_id;
	uint32_t status;
} __attribute__((packed));

struct rndis_data_hdr {
	uint32_t msg_type;
	uint32_t msg_len;
	uint32_t data_offset;
	uint32_t data_len;
	
	uint32_t oob_data_offset;
	uint32_t oob_data_len;
	uint32_t num_oob;
	uint32_t packet_data_offset;
	
	uint32_t packet_data_len;
	uint32_t vc_handle;
	uint32_t reserved;
} __attribute__((packed));

struct rndis_query {
	uint32_t msg_type;
	uint32_t msg_len;
	uint32_t request_id;
	uint32_t oid;
	uint32_t len;
	uint32_t offset;
	uint32_t handle;
} __attribute__((packed));

struct rndis_query_c {
	uint32_t msg_type;
	uint32_t msg_len;
	uint32_t request_id;
	uint32_t status;
	uint32_t len;
	uint32_t offset;
} __attribute__((packed));

struct rndis_init {
	uint32_t msg_type;
	uint32_t msg_len;
	uint32_t request_id;
	uint32_t major_version;
	uint32_t minor_version;
	uint32_t max_transfer_size;
} __attribute__((packed));

struct rndis_init_c {
	uint32_t msg_type;
	uint32_t msg_len;
	uint32_t request_id;
	uint32_t status;
	uint32_t major_version;
	uint32_t minor_version;
	uint32_t device_flags;
	uint32_t medium;
	uint32_t max_packets_per_transfer;
	uint32_t max_transfer_size;
	uint32_t packet_alignment;
	uint32_t af_list_offset;
	uint32_t af_list_size;
} __attribute__((packed));

struct rndis_set {
	uint32_t msg_type;
	uint32_t msg_len;
	uint32_t request_id;
	uint32_t oid;
	uint32_t len;
	uint32_t offset;
	uint32_t handle;
} __attribute__((packed));

struct rndis_set_c {
	uint32_t msg_type;
	uint32_t msg_len;
	uint32_t request_id;
	uint32_t status;
} __attribute__((packed));

#define RNDIS_MSG_COMPLETION                    cpu_to_le32(0x80000000)
#define RNDIS_MSG_PACKET                        cpu_to_le32(0x00000001) /* 1-N packets */
#define RNDIS_MSG_INIT                          cpu_to_le32(0x00000002)
#define RNDIS_MSG_INIT_C                        (RNDIS_MSG_INIT|RNDIS_MSG_COMPLETION)
#define RNDIS_MSG_HALT                          cpu_to_le32(0x00000003)
#define RNDIS_MSG_QUERY                         cpu_to_le32(0x00000004)
#define RNDIS_MSG_QUERY_C                       (RNDIS_MSG_QUERY|RNDIS_MSG_COMPLETION)
#define RNDIS_MSG_SET                           cpu_to_le32(0x00000005)
#define RNDIS_MSG_SET_C                         (RNDIS_MSG_SET|RNDIS_MSG_COMPLETION)
#define RNDIS_MSG_RESET                         cpu_to_le32(0x00000006)
#define RNDIS_MSG_RESET_C                       (RNDIS_MSG_RESET|RNDIS_MSG_COMPLETION)
#define RNDIS_MSG_INDICATE                      cpu_to_le32(0x00000007)
#define RNDIS_MSG_KEEPALIVE                     cpu_to_le32(0x00000008)
#define RNDIS_MSG_KEEPALIVE_C                   (RNDIS_MSG_KEEPALIVE|RNDIS_MSG_COMPLETION)

#define RNDIS_STATUS_SUCCESS                    cpu_to_le32(0x00000000)
#define RNDIS_STATUS_FAILURE                    cpu_to_le32(0xc0000001)
#define RNDIS_STATUS_INVALID_DATA               cpu_to_le32(0xc0010015)
#define RNDIS_STATUS_NOT_SUPPORTED              cpu_to_le32(0xc00000bb)
#define RNDIS_STATUS_MEDIA_CONNECT              cpu_to_le32(0x4001000b)
#define RNDIS_STATUS_MEDIA_DISCONNECT           cpu_to_le32(0x4001000c)
#define RNDIS_STATUS_MEDIA_SPECIFIC_INDICATION  cpu_to_le32(0x40010012)

#define RNDIS_PHYSICAL_MEDIUM_UNSPECIFIED       cpu_to_le32(0x00000000)
#define RNDIS_PHYSICAL_MEDIUM_WIRELESS_LAN      cpu_to_le32(0x00000001)
#define RNDIS_PHYSICAL_MEDIUM_CABLE_MODEM       cpu_to_le32(0x00000002)
#define RNDIS_PHYSICAL_MEDIUM_PHONE_LINE        cpu_to_le32(0x00000003)
#define RNDIS_PHYSICAL_MEDIUM_POWER_LINE        cpu_to_le32(0x00000004)
#define RNDIS_PHYSICAL_MEDIUM_DSL               cpu_to_le32(0x00000005)
#define RNDIS_PHYSICAL_MEDIUM_FIBRE_CHANNEL     cpu_to_le32(0x00000006)
#define RNDIS_PHYSICAL_MEDIUM_1394              cpu_to_le32(0x00000007)
#define RNDIS_PHYSICAL_MEDIUM_WIRELESS_WAN      cpu_to_le32(0x00000008)
#define RNDIS_PHYSICAL_MEDIUM_MAX               cpu_to_le32(0x00000009)

#define OID_802_3_PERMANENT_ADDRESS             cpu_to_le32(0x01010101)
#define OID_GEN_MAXIMUM_FRAME_SIZE              cpu_to_le32(0x00010106)
#define OID_GEN_CURRENT_PACKET_FILTER           cpu_to_le32(0x0001010e)
#define OID_GEN_PHYSICAL_MEDIUM                 cpu_to_le32(0x00010202)

/* packet filter bits used by OID_GEN_CURRENT_PACKET_FILTER */
#define RNDIS_PACKET_TYPE_DIRECTED              cpu_to_le32(0x00000001)
#define RNDIS_PACKET_TYPE_MULTICAST             cpu_to_le32(0x00000002)
#define RNDIS_PACKET_TYPE_ALL_MULTICAST         cpu_to_le32(0x00000004)
#define RNDIS_PACKET_TYPE_BROADCAST             cpu_to_le32(0x00000008)
#define RNDIS_PACKET_TYPE_SOURCE_ROUTING        cpu_to_le32(0x00000010)
#define RNDIS_PACKET_TYPE_PROMISCUOUS           cpu_to_le32(0x00000020)
#define RNDIS_PACKET_TYPE_SMT                   cpu_to_le32(0x00000040)
#define RNDIS_PACKET_TYPE_ALL_LOCAL             cpu_to_le32(0x00000080)
#define RNDIS_PACKET_TYPE_GROUP                 cpu_to_le32(0x00001000)
#define RNDIS_PACKET_TYPE_ALL_FUNCTIONAL        cpu_to_le32(0x00002000)
#define RNDIS_PACKET_TYPE_FUNCTIONAL            cpu_to_le32(0x00004000)
#define RNDIS_PACKET_TYPE_MAC_FRAME             cpu_to_le32(0x00008000)

/* default filter used with RNDIS devices */
#define RNDIS_DEFAULT_FILTER ( \
        RNDIS_PACKET_TYPE_DIRECTED | \
        RNDIS_PACKET_TYPE_BROADCAST | \
        RNDIS_PACKET_TYPE_ALL_MULTICAST | \
        RNDIS_PACKET_TYPE_PROMISCUOUS)

#define USB_CDC_SEND_ENCAPSULATED_COMMAND       0x00
#define USB_CDC_GET_ENCAPSULATED_RESPONSE       0x01

/***** CDC NCM definitions -- from linux/include/linux/usb/cdc_ncm.h,
 ***** include/uapi/linux/usb/cdc.h and drivers/net/usb/cdc_ncm.c *****/

#if NCM_SUPPORT

// Control interface matching: Communications class / Network Control Model:
#define NCM_IF_CLASS            2
#define NCM_IF_SUBCLASS         0x0D

// Functional descriptor subtypes (bDescriptorType == 0x24, CS_INTERFACE):
#define USB_CDC_HEADER_TYPE     0x00
#define USB_CDC_UNION_TYPE      0x06
#define USB_CDC_ETHERNET_TYPE   0x0F
#define USB_CDC_NCM_TYPE        0x1A

// Control requests (class, interface recipient):
#define USB_CDC_SET_ETHERNET_MULTICAST_FILTERS  0x40
#define USB_CDC_SET_ETHERNET_PACKET_FILTER      0x43
#define USB_CDC_GET_NTB_PARAMETERS              0x80
#define USB_CDC_GET_NET_ADDRESS                 0x81
#define USB_CDC_SET_NET_ADDRESS                 0x82
#define USB_CDC_GET_NTB_FORMAT                  0x83
#define USB_CDC_SET_NTB_FORMAT                  0x84
#define USB_CDC_GET_NTB_INPUT_SIZE              0x85
#define USB_CDC_SET_NTB_INPUT_SIZE              0x86
#define USB_CDC_GET_MAX_DATAGRAM_SIZE           0x87
#define USB_CDC_SET_MAX_DATAGRAM_SIZE           0x88

// CDC packet-filter bits (different encoding than RNDIS!):
#define USB_CDC_PACKET_TYPE_PROMISCUOUS         0x01
#define USB_CDC_PACKET_TYPE_ALL_MULTICAST       0x02
#define USB_CDC_PACKET_TYPE_DIRECTED            0x04
#define USB_CDC_PACKET_TYPE_BROADCAST           0x08
#define USB_CDC_PACKET_TYPE_MULTICAST           0x10

// CDC notification on the interrupt pipe:
//   byte 0: bmRequestType == 0xA1
//   byte 1: bNotificationType == 0x00 (NETWORK_CONNECTION)
//   bytes 2-3: wValue (1 = connected, 0 = disconnected)
#define USB_CDC_NOTIFY_NETWORK_CONNECTION       0x00

// NTB16 signatures (on the wire, little-endian):
#define USB_CDC_NCM_NTH16_SIGN          0x484D434E /* "NCMH" */
#define USB_CDC_NCM_NDP16_NOCRC_SIGN    0x304D434E /* "NCM0" */
#define USB_CDC_NCM_NDP16_CRC_SIGN      0x314D434E /* "NCM1" */
#define USB_CDC_NCM_NTB16_FORMAT        0x00

// Sane bounds (from Linux cdc_ncm.h / cdc_ncm.c):
#define NCM_NTB_MIN_SIZE                2048
#define NCM_NTB_MAX_SIZE                65536
#define NCM_NTB_DEF_SIZE                16384
#define NCM_DPT_DATAGRAMS_MAX           40
#define NCM_NDP_ALIGN_MIN               4
#define NCM_MIN_DATAGRAM_SIZE           1514
#define NCM_MAX_DATAGRAM_SIZE           8192

struct ncm_nth16 {
	uint32_t dwSignature;
	uint16_t wHeaderLength;
	uint16_t wSequence;
	uint16_t wBlockLength;
	uint16_t wNdpIndex;
} __attribute__((packed));

struct ncm_ndp16 {
	uint32_t dwSignature;
	uint16_t wLength;
	uint16_t wNextNdpIndex;
	// Followed by (wDatagramIndex, wDatagramLength) uint16 pairs and a
	// (0, 0) terminator pair.
} __attribute__((packed));

struct ncm_ntb_parameters {
	uint16_t wLength;
	uint16_t bmNtbFormatsSupported;
	uint32_t dwNtbInMaxSize;
	uint16_t wNdpInDivisor;
	uint16_t wNdpInPayloadRemainder;
	uint16_t wNdpInAlignment;
	uint16_t wPadding1;
	uint32_t dwNtbOutMaxSize;
	uint16_t wNdpOutDivisor;
	uint16_t wNdpOutPayloadRemainder;
	uint16_t wNdpOutAlignment;
	uint16_t wNtbOutMaxDatagrams;
} __attribute__((packed));

#endif /* NCM_SUPPORT */

/***** Actual class definitions *****/

// Zero-copy RX buffer lease: heap-allocated, referenced by every
// attachclustered mbuf handed to the network stack. When the last cluster
// is freed, the free callback releases the buffer descriptor and the lease
// itself, so an outstanding lease outlives even the driver instance.
struct RxBufLease {
	IOBufferMemoryDescriptor *mdp;
	SInt32 count;        // Outstanding clusters (atomic).
	bool freeWhenDone;   // Set at teardown: free on the last release.
};

typedef struct {
	// Descriptor handed to 'pipe->io': either kind below.
	IOMemoryDescriptor *dmaBuf;
	// TX (and legacy copy-based RX): contiguous wired buffer.
	IOBufferMemoryDescriptor *mdp;
	// Zero-copy RX: the lease owning 'mdp' (NULL in copy mode).
	RxBufLease *lease;
	// True while a transfer is outstanding on this slot.
	bool inFlight;
	// The pipe this buffer is submitted on (RX slots; TX slots always use
	// the first OUT pipe for RNDIS, alternating pipes for NCM).
	IOUSBHostPipe *pipe;
#if TX_ZEROCOPY
	// Zero-copy TX: the in-flight aggregate chain (freed on completion).
	mbuf_t m;
#endif
	IOUSBHostCompletion comp;
} pipebuf_t;

class HoRNDIS : public IOEthernetController {
	OSDeclareDefaultStructors(HoRNDIS);	// Constructor & Destructor stuff

private:
	/*!
	 * This class protects method calls against re-entry, when the IOCommand
	 * gate is being released due to synchronous IO, or some other reason.
	 * Use case:
	 * Unlike start/stop calls, that are triggered by a single IO provider,
	 * the enable/disable calls can be triggered by multiple interface clients,
	 * as well as user's actions, e.g. "ifconfig en6 up" - potentially multiple
	 * "ifconfig" processes running in parallel.
	 * Even though the calls to this class are protected by the IOCommandGate,
	 * synchronous USB transfers release the gate (by using 
	 * IOCommandGate::commandSleep), allowing another enable/disable call
	 * to "sneak in". We use the "ReentryLocker" to delay additional
	 * enable/disable calls until the first one completes.
	 */
	class NOEXPORT ReentryLocker {
	public:
		// 'inGuard' is instance-level variable that would be set by
		// "ReentryLocker" whenever someone is executing the protected section.
		ReentryLocker(IOCommandGate *inGate, bool &inGuard);
		ReentryLocker(IONetworkController *controller, bool &inGuard):
			ReentryLocker(controller->getCommandGate(), inGuard) {}
		~ReentryLocker();
		IOReturn getResult() const { return result; }
		bool isInterrupted() const { return result != kIOReturnSuccess; }
	private:
		IOCommandGate *const gate;
		bool &entryGuard;
		IOReturn result;
	};
		
	IOEthernetInterface *fNetworkInterface;
	IONetworkStats *fpNetStats;

	bool fReadyToTransfer;  // Ready to transmit: Android <-> MAC.
	// Set to true when 'enable' succeeds, and
	// set to false when 'disable' succeeds:
	bool fNetifEnabled;
	bool fEnableDisableInProgress;  // Guards against re-entry
	bool fDataDead;

	// These pass information from 'probe' to 'openUSBInterfaces':
	uint8_t fProbeConfigVal;
	uint8_t fProbeCommIfNum;
	uint8_t fProbeDataIfNum;  // NCM: from the Union descriptor; RNDIS: comm+1.

	// Which protocol this instance runs (chosen in 'probe'):
	enum HoRNDISProtocol {
		kProtocolRNDIS = 0,
		kProtocolNCM
	};
	HoRNDISProtocol fProtocol;

	// fCallbackCount is the number of callbacks concurrently running
	// (possibly offset by a certain value).
	//  - Every successful async API call shall "fCallbackCount++".
	//  - Every time we exit the completion without making another call:
	//    'callbackExit()'.
	int fCallbackCount;
	
	// USB Communication:
	IOUSBHostInterface *fCommInterface;
	IOUSBHostInterface *fDataInterface;
	
	IOUSBHostPipe *fInPipe;
	IOUSBHostPipe *fOutPipe;
	// Second bulk pair: NCM data interfaces may expose two IN and two OUT
	// bulk endpoints ("two endpoints" per the module documentation). RNDIS
	// instances leave these NULL.
	IOUSBHostPipe *fInPipe2;
	IOUSBHostPipe *fOutPipe2;
	bool fTxOutPipeToggle;  // Alternates OUT pipes for NCM transfers.
	// Interrupt pipe of the control interface (optional): carries
	// response-available (RNDIS) and media connect/disconnect (CDC)
	// notifications.
	IOUSBHostPipe *fIntPipe;
	pipebuf_t fIntBuf;    // Interrupt reader buffer.
	bool fIntReadActive;  // Interrupt reader loop is running.

	// Control channel state. 'fCmdInProgress' serializes 'rndisCommand':
	// the command wait below releases the command gate, so without it
	// another client (enable/disable/keepalive) could issue a command
	// concurrently and corrupt the single response buffer.
	// 'rndisCommand' MUST NOT be called from a workloop event-source action
	// (it may sleep releasing the gate, which would block the workloop).
	bool fCmdInProgress;
	bool fCmdWaitActive;  // A command is sleeping on 'fCmdWaitEvent'.
	bool fCmdWaitEvent;   // commandSleep/commandWakeup event token.
	IOTimerEventSource *fCtrlTimer;
	enum CtrlTimerPurpose {
		kCtrlTimerNone = 0,
		kCtrlTimerKeepalive,
		kCtrlTimerCmdWait
	};
	CtrlTimerPurpose fCtrlTimerPurpose;
	bool fCtrlTimerArmed;
	bool fKeepaliveEnabled;

	// Packet filter state (see RNDIS_DYNAMIC_FILTER):
	uint32_t fCurrentFilter;
	bool fMulticastEnabled;
	bool fPromiscEnabled;

	// Cached hardware address (queried from the device once):
	IOEthernetAddress fHardwareAddress;
	bool fHasHardwareAddress;
	
	uint32_t rndisXid;  // RNDIS request_id count.
	int32_t maxOutTransferSize;  // Set by 'rdisInit' from device reply.

	pipebuf_t outbufs[N_OUT_BUFS];
	// Allow double-buffering to enable the best hardware utilization:
	pipebuf_t inbufs[N_IN_BUFS];
	uint16_t outbufStack[N_OUT_BUFS];
	int numFreeOutBufs;

	// TX aggregation state. 'fTxFillBufIdx' is the buffer we are appending
	// packets to, -1 when none. Only touched on the workloop (the command
	// gate serializes outputPacket, the flush timer and the completions).
	int fTxFillBufIdx;
	uint32_t fTxFillOffset;
	IOTimerEventSource *fTxFlushTimer;
	bool fTxTimerArmed;

#if TX_ZEROCOPY
	// Zero-copy TX: the aggregate under construction is an mbuf chain
	// ([RNDIS hdr + packet] concatenated), 'fTxBytes' is its length.
	mbuf_t fTxHead;
	uint32_t fTxBytes;
#endif

#if NCM_SUPPORT
	// NCM state, negotiated by 'ncmInit':
	uint32_t fNcmRxMax;       // RX transfer (NTB input) size.
	uint32_t fNcmTxMax;       // TX NTB size (<= OUT_BUF_SIZE).
	uint16_t fNcmNdpAlign;    // wNdpOutAlignment (min 4).
	uint16_t fNcmModulus;     // wNdpOutDivisor (min 4).
	uint16_t fNcmRemainder;   // wNdpOutPayloadRemainder.
	uint16_t fNcmMaxDatagrams;// wNtbOutMaxDatagrams (<= 40).
	uint16_t fNcmMaxNdpSize;  // 16 + (maxDatagrams+1)*4.
	uint16_t fNcmSeq;         // NTB sequence number.
	uint32_t fNcmMaxDatagram; // Max frame size (MTU + 14).
	// TX NTB under construction (copy-based fill, protocol-independent of
	// TX_ZEROCOPY): the NDP sits in the fill buffer after the NTH; its
	// entries are recorded here and materialized by 'ncmFlushNtb'.
	uint32_t fTxNdpOffset;    // NDP offset within the fill buffer.
	struct {
		uint16_t index;       // Datagram offset within the NTB.
		uint16_t len;         // Datagram length.
	} fTxNdpEntries[NCM_DPT_DATAGRAMS_MAX];
	int fTxNdpCount;
#endif

	// RX zero-copy: a read could not be re-armed because 'allocatePacket'
	// failed; the flush timer retries until a buffer becomes available.
	bool fNeedReadRestart;

	void callbackExit();
	static void dataWriteComplete(void *obj, void *param, IOReturn ior, UInt32 transferred);
	static void dataReadComplete(void *obj, void *param, IOReturn ior, UInt32 transferred);
	static void dataIntReadComplete(void *obj, void *param, IOReturn ior, UInt32 transferred);
	static void txFlushTimerFired(OSObject *owner, IOTimerEventSource *timer);
	static void ctrlTimerFired(OSObject *owner, IOTimerEventSource *timer);

	bool grabFillBuffer(void);
	bool submitFillBuffer(void);
	void armTxFlushTimer(void);
	bool startRead(pipebuf_t *inbuf);
	void releaseReadBuffer(pipebuf_t *inbuf);
#if RX_ZEROCOPY || NCM_SUPPORT
	// (NCM requires RX_ZEROCOPY, so these are compiled together.)
	static void rxClusterFreed(caddr_t extbuf, u_int extsize, caddr_t extarg);
	void receivePacketZeroCopy(pipebuf_t *slot, UInt32 size);
	bool submitClusterPacket(uint8_t *data, UInt32 len, RxBufLease *lease);
#endif

	// RNDIS protocol layer:
	bool startIntRead(void);
	void armCtrlTimer(CtrlTimerPurpose purpose, uint32_t us);
	void sendKeepalive(void);
	void handleIndication(uint32_t status);
	void handleInterruptData(const void *data, UInt32 len);
	bool updatePacketFilter(void);

#if NCM_SUPPORT
	// NCM protocol layer:
	IOReturn ncmControlRequest(uint8_t bRequest, uint16_t wValue,
		void *data, uint16_t wLength, bool isIn);
	bool ncmInit(void);
	bool ncmSetPacketFilter(uint32_t filter);
	bool ncmAppendPacket(mbuf_t packet, size_t pktlen);
	void ncmFlushNtb(void);
	void ncmReceivePacket(pipebuf_t *slot, UInt32 size);
	void ncmHandleNotification(const void *data, UInt32 len);
	void ncmFallbackMAC(void);
#endif

	bool rndisInit();
	IOReturn rndisCommand(struct rndis_msg_hdr *buf, int buflen);
	int rndisQuery(void *buf, uint32_t oid, uint32_t in_len, void **reply, int *reply_len);
	bool rndisSetPacketFilter(uint32_t filter);

	IOService *probeDevice(IOUSBHostDevice *device, SInt32 *score);

	bool openUSBInterfaces(IOService *provider);
	void closeUSBInterfaces();
	void disableNetworkQueue();
	void disableImpl();

	bool createMediumTables(const IONetworkMedium **primary);
	bool allocateResources(void);
	void releaseResources(void);
	bool createNetworkInterface(void);

	void receivePacket(void *packet, UInt32 size);

public:
	// IOKit overrides
	virtual bool init(OSDictionary *properties = 0) override;
	virtual void free() override;
	virtual IOService *probe(IOService *provider, SInt32 *score) override;
	virtual bool start(IOService *provider) override;
	virtual bool willTerminate(IOService *provider, IOOptionBits options) override;
	virtual void stop(IOService *provider) override;

	// IOEthernetController overrides
	virtual IOOutputQueue *createOutputQueue(void) override;
	virtual IOReturn getHardwareAddress(IOEthernetAddress *addr) override;
	virtual IOReturn getMaxPacketSize(UInt32 *maxSize) const override;
	virtual IOReturn getPacketFilters(const OSSymbol *group,
									  UInt32 *filters ) const  override;
	virtual IONetworkInterface *createInterface() override;
	virtual bool configureInterface(IONetworkInterface *netif) override;
	
	virtual IOReturn enable(IONetworkInterface *netif) override;
	virtual IOReturn disable(IONetworkInterface *netif) override;
	virtual IOReturn selectMedium(const IONetworkMedium *medium) override;
	virtual IOReturn setMulticastMode(bool active) override;
	virtual IOReturn setMulticastList(IOEthernetAddress *addrs,
	                                  UInt32             count) override;
	virtual IOReturn setPromiscuousMode(bool active) override;
	virtual UInt32 outputPacket(mbuf_t pkt, void *param) override;
};

class HoRNDISInterface : public IOEthernetInterface {
	OSDeclareDefaultStructors(HoRNDISInterface);
	int maxmtu;
public:
	virtual bool init(IONetworkController *controller, int mtu);
	virtual bool setMaxTransferUnit(UInt32 mtu) override;
};
