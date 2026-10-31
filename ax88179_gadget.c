/*
 * ax88179_gadget.c — minimal AX88179 USB gadget built on raw-gadget.
 *
 * STAGE 1 ONLY.
 *   - enumerate as 0B95:1790 (AX88179 Gigabit Ethernet)
 *   - log every USB control request the host sends
 *   - answer vendor requests with zeroes
 *
 * Nothing about the actual AX88179 register protocol is implemented yet.
 * The point of this stage is to find out whether the Switch's ethernet
 * driver binds to us at all, and to capture the exact sequence of
 * control requests it makes.
 *
 * build:  gcc -O2 -o ax88179_gadget ax88179_gadget.c
 * run:    ./ax88179_gadget [udc-name]        (default: c9040000.usb)
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <linux/if_tun.h>
#include <linux/usb/ch9.h>
#include <linux/usb/raw_gadget.h>

/*
 * Distros ship a linux-libc-dev older than the running kernel, so the
 * event types added after RESUME may be missing from the header even
 * though the kernel emits them.  The values are ABI and cannot change:
 * they are the enum in include/uapi/linux/usb/raw_gadget.h.
 */
#ifndef USB_RAW_EVENT_RESET
#define USB_RAW_EVENT_RESET	5
#endif
#ifndef USB_RAW_EVENT_DISCONNECT
#define USB_RAW_EVENT_DISCONNECT 6
#endif

#define UDC_DEFAULT "c9040000.usb"

/* ------------------------------------------------------------------ */
/* descriptors                                                         */
/* ------------------------------------------------------------------ */

struct __attribute__((packed)) ax_config {
	struct usb_config_descriptor    config;
	struct usb_interface_descriptor intf;
	struct usb_endpoint_descriptor  ep_bulk_in;
	struct usb_endpoint_descriptor  ep_bulk_out;
	struct usb_endpoint_descriptor  ep_int_in;
};

static struct usb_device_descriptor dev_desc;
static struct ax_config             cfg;

/*
 * Wire-format configuration descriptor.
 *
 * struct usb_endpoint_descriptor is 9 bytes in memory (it also carries
 * bRefresh and bSynchAddress, which only audio endpoints use), while a
 * USB endpoint descriptor on the wire is 7 bytes.  Handing the structs
 * to the host directly appends those two extra bytes to every endpoint,
 * so the host walks off the end of each descriptor, fails to parse the
 * configuration and re-enumerates forever without ever sending
 * SET_CONFIGURATION.  Build the byte stream explicitly instead.
 */
static uint8_t config_buf[64];
static int     config_len;

/* ------------------------------------------------------------------ */
/* register model                                                      */
/* ------------------------------------------------------------------ */

/*
 * The vendor requests name a register space in bRequest:
 *   0x01 AX_ACCESS_MAC     wValue = register, wIndex = size
 *   0x02 AX_ACCESS_PHY     wValue = phy id,   wIndex = phy register
 *   0x04 AX_ACCESS_EEPROM
 *   0x05 AX_ACCESS_EFUS
 *   0x06 AX_RELOAD_EEPROM_EFUSE
 */
#define AX_ACCESS_MAC     0x01
#define AX_ACCESS_PHY     0x02
#define AX_ACCESS_EEPROM  0x04
#define AX_ACCESS_EFUS    0x05

#define PHY_REGS 32

static uint8_t  mac_regs[256];
static uint16_t phy_regs[PHY_REGS];

static void init_regs(void)
{
	/* Locally administered unicast MAC at AX_NODE_ID (0x10); an
	 * all-zero address makes the driver discard the device.
	 * AX_MAC=aa:bb:cc:dd:ee:ff overrides it, which is needed when two
	 * of these gadgets share a network. */
	uint8_t mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
	const char *env = getenv("AX_MAC");
	unsigned m[6];
	int i;

	if (env && sscanf(env, "%x:%x:%x:%x:%x:%x",
			  &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6)
		for (i = 0; i < 6; i++)
			mac[i] = (uint8_t)m[i];

	memcpy(&mac_regs[0x10], mac, sizeof(mac));

	/* PHYSICAL_LINK_STATUS: report a high speed (USB 2.0) link so the
	 * driver picks a sane bulk-in buffer size. */
	mac_regs[0x02] = 0x02;

	phy_regs[0x00] = 0x1140;   /* BMCR: autoneg on */
	phy_regs[0x01] = 0x796d;   /* BMSR: autoneg done, link up */
	phy_regs[0x11] = 0xa400;   /* PHYSR: gigabit, full duplex, link */
}

static const char *str_manufacturer = "ASIX";
static const char *str_product      = "AX88179 USB 3.0 to Gigabit Ethernet Adapter";
static const char *str_serial       = "0000000000";

static void init_descriptors(void)
{
	memset(&dev_desc, 0, sizeof(dev_desc));
	dev_desc.bLength            = USB_DT_DEVICE_SIZE;
	dev_desc.bDescriptorType    = USB_DT_DEVICE;
	dev_desc.bcdUSB             = htole16(0x0200);
	/*
	 * A real AX88179 is vendor specific at the device level too:
	 * ff/ff/00.  Keep this in sync with the interface triple below.
	 */
	dev_desc.bDeviceClass       = 0xff;
	dev_desc.bDeviceSubClass    = 0xff;
	dev_desc.bDeviceProtocol    = 0x00;
	dev_desc.bMaxPacketSize0    = 64;
	dev_desc.idVendor           = htole16(0x0b95);
	dev_desc.idProduct          = htole16(0x1790);
	dev_desc.bcdDevice          = htole16(0x0200);
	dev_desc.iManufacturer      = 1;
	dev_desc.iProduct           = 2;
	dev_desc.iSerialNumber      = 3;
	dev_desc.bNumConfigurations = 1;

	memset(&cfg, 0, sizeof(cfg));

	cfg.config.bLength             = USB_DT_CONFIG_SIZE;
	cfg.config.bDescriptorType     = USB_DT_CONFIG;
	cfg.config.wTotalLength        = htole16(sizeof(cfg));
	cfg.config.bNumInterfaces      = 1;
	cfg.config.bConfigurationValue = 1;
	cfg.config.iConfiguration      = 0;
	cfg.config.bmAttributes        = 0xa0;        /* bus powered, wakeup */
	cfg.config.bMaxPower           = 23;          /* 46 mA, as on a real unit */

	cfg.intf.bLength            = USB_DT_INTERFACE_SIZE;
	cfg.intf.bDescriptorType    = USB_DT_INTERFACE;
	cfg.intf.bInterfaceNumber   = 0;
	cfg.intf.bAlternateSetting  = 0;
	cfg.intf.bNumEndpoints      = 3;
	/*
	 * 0xff/0xff/0x00 is what a real AX88179 reports, and it is load
	 * bearing: the driver registers
	 *     usb:v0B95p1790d*dc*dsc*dp*icFFiscFFip00in*
	 * so a different subclass makes usb_match_id() reject the
	 * interface and the driver never probes.
	 */
	cfg.intf.bInterfaceClass    = 0xff;           /* vendor specific */
	cfg.intf.bInterfaceSubClass = 0xff;
	cfg.intf.bInterfaceProtocol = 0x00;
	cfg.intf.iInterface         = 0;

	/*
	 * Endpoint layout copied from a real AX88179:
	 *   0x81  interrupt IN   16 bytes, interval 11
	 *   0x82  bulk      IN   512 bytes (1024 superspeed)
	 *   0x03  bulk      OUT  512 bytes (1024 superspeed)
	 * The real chip also has a second bulk OUT at 0x05, which the
	 * driver does not need.
	 */
	cfg.ep_int_in.bLength          = USB_DT_ENDPOINT_SIZE;
	cfg.ep_int_in.bDescriptorType  = USB_DT_ENDPOINT;
	cfg.ep_int_in.bEndpointAddress = 0x81;
	cfg.ep_int_in.bmAttributes     = USB_ENDPOINT_XFER_INT;
	cfg.ep_int_in.wMaxPacketSize   = htole16(16);
	cfg.ep_int_in.bInterval        = 11;

	cfg.ep_bulk_in.bLength          = USB_DT_ENDPOINT_SIZE;
	cfg.ep_bulk_in.bDescriptorType  = USB_DT_ENDPOINT;
	cfg.ep_bulk_in.bEndpointAddress = 0x82;
	cfg.ep_bulk_in.bmAttributes     = USB_ENDPOINT_XFER_BULK;
	cfg.ep_bulk_in.wMaxPacketSize   = htole16(512);
	cfg.ep_bulk_in.bInterval        = 0;

	cfg.ep_bulk_out.bLength          = USB_DT_ENDPOINT_SIZE;
	cfg.ep_bulk_out.bDescriptorType  = USB_DT_ENDPOINT;
	cfg.ep_bulk_out.bEndpointAddress = 0x03;
	cfg.ep_bulk_out.bmAttributes     = USB_ENDPOINT_XFER_BULK;
	cfg.ep_bulk_out.wMaxPacketSize   = htole16(512);
	cfg.ep_bulk_out.bInterval        = 0;

	{
		uint8_t *p = config_buf;

		memcpy(p, &cfg.config, USB_DT_CONFIG_SIZE);
		p += USB_DT_CONFIG_SIZE;
		memcpy(p, &cfg.intf, USB_DT_INTERFACE_SIZE);
		p += USB_DT_INTERFACE_SIZE;
		memcpy(p, &cfg.ep_int_in, USB_DT_ENDPOINT_SIZE);
		p += USB_DT_ENDPOINT_SIZE;
		memcpy(p, &cfg.ep_bulk_in, USB_DT_ENDPOINT_SIZE);
		p += USB_DT_ENDPOINT_SIZE;
		memcpy(p, &cfg.ep_bulk_out, USB_DT_ENDPOINT_SIZE);
		p += USB_DT_ENDPOINT_SIZE;

		config_len = (int)(p - config_buf);
		((struct usb_config_descriptor *)config_buf)->wTotalLength =
			htole16((uint16_t)config_len);
	}
}

static int build_string(int idx, uint8_t *buf)
{
	const char *s;
	int i, len;

	switch (idx) {
	case 0:                                  /* LANGID list */
		buf[0] = 4;
		buf[1] = USB_DT_STRING;
		buf[2] = 0x09;                   /* en-US */
		buf[3] = 0x04;
		return 4;
	case 1:
		s = str_manufacturer;
		break;
	case 2:
		s = str_product;
		break;
	case 3:
		s = str_serial;
		break;
	default:
		return -1;
	}

	len = (int)strlen(s);
	if (len > 126)
		len = 126;

	buf[0] = (uint8_t)(2 + 2 * len);
	buf[1] = USB_DT_STRING;
	for (i = 0; i < len; i++) {
		buf[2 + 2 * i] = (uint8_t)s[i];
		buf[3 + 2 * i] = 0;
	}
	return 2 + 2 * len;
}

/* ------------------------------------------------------------------ */
/* raw-gadget plumbing                                                 */
/* ------------------------------------------------------------------ */

struct raw_control_event {
	struct usb_raw_event inner;
	struct usb_ctrlrequest ctrl;
};

static int ep0_write(int fd, const void *data, unsigned len)
{
	struct usb_raw_ep_io *io = malloc(sizeof(*io) + len);
	int r;

	if (!io)
		return -1;
	io->ep     = 0;
	io->flags  = 0;
	io->length = len;
	if (len && data)
		memcpy(io->data, data, len);

	r = ioctl(fd, USB_RAW_IOCTL_EP0_WRITE, io);
	free(io);
	return r;
}

static int ep0_read(int fd, void *data, unsigned len)
{
	struct usb_raw_ep_io *io = malloc(sizeof(*io) + len);
	int r;

	if (!io)
		return -1;
	io->ep     = 0;
	io->flags  = 0;
	io->length = len;

	r = ioctl(fd, USB_RAW_IOCTL_EP0_READ, io);
	if (r >= 0 && data && len)
		memcpy(data, io->data, len);

	free(io);
	return r;
}

/*
 * Complete a control request that has no data stage (e.g.
 * SET_CONFIGURATION, SET_INTERFACE).
 *
 * raw_gadget.c's gadget_setup() marks every request without an IN data
 * stage as ep0_out_pending, and EP0_WRITE refuses requests whose
 * ep0_in_pending is clear (returns -EBUSY), which the host sees as a
 * stall.  The status stage therefore has to be issued through EP0_READ
 * with a zero length.
 */
static int ep0_ack(int fd)
{
	return ep0_read(fd, NULL, 0);
}

/*
 * Endpoints must be released when the host disconnects or resets the
 * device: a later SET_CONFIGURATION re-enables the same endpoints, and
 * EP_ENABLE on an endpoint that is still enabled fails with EBUSY, which
 * leaves the configuration half applied and the host driver never runs.
 */
static int ep_idx[3] = { -1, -1, -1 };

static void disable_eps(int fd)
{
	unsigned i;

	for (i = 0; i < 3; i++) {
		if (ep_idx[i] < 0)
			continue;
		ioctl(fd, USB_RAW_IOCTL_EP_DISABLE, ep_idx[i]);
		ep_idx[i] = -1;
	}
}

static void enable_eps(int fd)
{
	struct usb_endpoint_descriptor *eps[] = {
		&cfg.ep_int_in, &cfg.ep_bulk_in, &cfg.ep_bulk_out
	};
	unsigned i;

	for (i = 0; i < sizeof(eps) / sizeof(eps[0]); i++) {
		int r;

		if (ep_idx[i] >= 0)
			continue;
		r = ioctl(fd, USB_RAW_IOCTL_EP_ENABLE, eps[i]);
		if (r < 0)
			fprintf(stderr, "  ep 0x%02x enable failed: %s\n",
				eps[i]->bEndpointAddress, strerror(errno));
		else {
			ep_idx[i] = r;
			fprintf(stderr, "  ep 0x%02x enabled as %d\n",
				eps[i]->bEndpointAddress, r);
		}
	}

	if (ioctl(fd, USB_RAW_IOCTL_CONFIGURE, 0) < 0)
		fprintf(stderr, "  CONFIGURE failed: %s\n", strerror(errno));
}

/* ------------------------------------------------------------------ */
/* data path: bulk endpoints <-> TAP                                   */
/* ------------------------------------------------------------------ */

#define TAP_NAME  "ax88179"
#define TAP_IP    "10.10.10.1"
#define TAP_MASK  "255.255.255.0"
#define MAX_FRAME 2048

static int tap_fd = -1;
static int gfd = -1;
static pthread_t tx_thread, rx_thread, int_thread;
static volatile int data_running;

static int tap_open(const char *name)
{
	struct ifreq ifr;
	int fd = open("/dev/net/tun", O_RDWR);

	if (fd < 0) {
		perror("open /dev/net/tun");
		return -1;
	}
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
	strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
	if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
		perror("TUNSETIFF");
		close(fd);
		return -1;
	}
	return fd;
}

/*
 * Give the tap an address and bring it up, so that restarting this
 * program does not require re-running "ip addr add" by hand.
 */
static void tap_configure(const char *name, const char *ip, const char *mask)
{
	struct ifreq ifr;
	struct sockaddr_in *sin;
	int s = socket(AF_INET, SOCK_DGRAM, 0);

	if (s < 0)
		return;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
	sin = (struct sockaddr_in *)&ifr.ifr_addr;
	sin->sin_family = AF_INET;
	if (inet_pton(AF_INET, ip, &sin->sin_addr) == 1)
		ioctl(s, SIOCSIFADDR, &ifr);

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
	sin = (struct sockaddr_in *)&ifr.ifr_netmask;
	sin->sin_family = AF_INET;
	if (inet_pton(AF_INET, mask, &sin->sin_addr) == 1)
		ioctl(s, SIOCSIFNETMASK, &ifr);

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
	if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
		ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
		ioctl(s, SIOCSIFFLAGS, &ifr);
	}

	close(s);
}

static int ep_write(int fd, int ep, const void *data, unsigned len)
{
	struct usb_raw_ep_io *io = malloc(sizeof(*io) + len);
	int r;

	if (!io)
		return -1;
	io->ep = (uint16_t)ep;
	io->flags = 0;
	io->length = len;
	memcpy(io->data, data, len);
	r = ioctl(fd, USB_RAW_IOCTL_EP_WRITE, io);
	free(io);
	return r;
}

static int ep_read(int fd, int ep, void *data, unsigned len)
{
	struct usb_raw_ep_io *io = malloc(sizeof(*io) + len);
	int r;

	if (!io)
		return -1;
	io->ep = (uint16_t)ep;
	io->flags = 0;
	io->length = len;
	r = ioctl(fd, USB_RAW_IOCTL_EP_READ, io);
	if (r > 0 && data)
		memcpy(data, io->data, (size_t)r);
	free(io);
	return r;
}

/*
 * TX checksum offload.
 *
 * The driver enables AX_TXCOE_CTL, so the kernel hands us frames whose
 * IPv4 header checksum (and L4 checksum) are left for the "hardware" to
 * fill in.  We are the hardware, so compute them here; without this the
 * host stack drops every TCP/UDP frame as IpInHdrErrors.
 */
static uint32_t csum_add(uint32_t sum, const uint8_t *p, int n)
{
	int i;

	for (i = 0; i + 1 < n; i += 2)
		sum += (uint32_t)((p[i] << 8) | p[i + 1]);
	if (n & 1)
		sum += (uint32_t)p[n - 1] << 8;
	return sum;
}

static uint16_t csum_fold(uint32_t sum)
{
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

static void fix_tx_checksums(uint8_t *f, int len)
{
	uint8_t *ip, *l4;
	int ihl, tot, proto, l4len;
	uint32_t sum;

	if (len < 34)
		return;
	if (((f[12] << 8) | f[13]) != 0x0800)
		return;

	ip = f + 14;
	if ((ip[0] >> 4) != 4)
		return;
	ihl = (ip[0] & 0x0f) * 4;
	if (ihl < 20 || 14 + ihl > len)
		return;

	ip[10] = 0;
	ip[11] = 0;
	{
		uint16_t c = csum_fold(csum_add(0, ip, ihl));

		ip[10] = (uint8_t)(c >> 8);
		ip[11] = (uint8_t)(c & 0xff);
	}

	tot = (ip[2] << 8) | ip[3];
	if (tot < ihl || 14 + tot > len)
		return;

	proto = ip[9];
	l4 = ip + ihl;
	l4len = tot - ihl;

	if (proto == 17) {              /* UDP */
		if (l4len < 8)
			return;
		l4[6] = 0;
		l4[7] = 0;
		sum = csum_add(0, ip + 12, 8);           /* src + dst */
		sum += 0x0011;                            /* proto, zero pad */
		sum += (uint32_t)l4len;
		sum = csum_add(sum, l4, l4len);
		{
			uint16_t c = csum_fold(sum);

			if (c == 0)
				c = 0xffff;               /* 0 means "none" */
			l4[6] = (uint8_t)(c >> 8);
			l4[7] = (uint8_t)(c & 0xff);
		}
	} else if (proto == 6) {        /* TCP */
		if (l4len < 20)
			return;
		l4[16] = 0;
		l4[17] = 0;
		sum = csum_add(0, ip + 12, 8);
		sum += 0x0006;
		sum += (uint32_t)l4len;
		sum = csum_add(sum, l4, l4len);
		{
			uint16_t c = csum_fold(sum);

			l4[16] = (uint8_t)(c >> 8);
			l4[17] = (uint8_t)(c & 0xff);
		}
	}
}

/*
 * Host -> device.  ax88179_tx_fixup() prepends 8 bytes: a little endian
 * frame length followed by TSO/padding flags.
 */
static void *tx_fn(void *arg)
{
	uint8_t buf[MAX_FRAME + 64];

	(void)arg;
	while (data_running) {
		int r = ep_read(gfd, ep_idx[2], buf, sizeof(buf));
		uint32_t len;

		if (r <= 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r <= 8)
			continue;
		memcpy(&len, buf, 4);
		len = le32toh(len);
		if (len > (uint32_t)(r - 8))
			len = (uint32_t)(r - 8);
		if (len && tap_fd >= 0) {
			static int dumped;
			int i;

			if (!dumped) {
				dumped = 1;
				fprintf(stderr, "tx frame r=%d len=%u:", r, len);
				for (i = 0; i < 20 && i < (int)len; i++)
					fprintf(stderr, " %02x", buf[8 + i]);
				fprintf(stderr, "\n");
			}
			fix_tx_checksums(buf + 8, (int)len);
			if (write(tap_fd, buf + 8, len) < 0)
				;
		}
	}
	fprintf(stderr, "tx thread exit\n");
	return NULL;
}

/*
 * Device -> host.  ax88179_rx_fixup() expects, for a single frame:
 *   [2 byte alignment header][frame][pad to 8]
 *   [4 byte metadata: pkt_len << 16][4 byte rx_hdr: (hdr_off << 16) | 1]
 */
static void *rx_fn(void *arg)
{
	uint8_t frame[MAX_FRAME];
	uint8_t out[MAX_FRAME + 64];
	struct pollfd pfd;

	(void)arg;
	pfd.fd = tap_fd;
	pfd.events = POLLIN;

	while (data_running) {
		int n, pkt_len, padd;
		uint32_t meta, hdr;

		if (poll(&pfd, 1, 200) <= 0)
			continue;

		n = (int)read(tap_fd, frame, sizeof(frame));
		if (n <= 0)
			continue;

		/*
		 * A real MAC pads every frame to the Ethernet minimum (60
		 * bytes without FCS).  The tap hands us the frame exactly as
		 * the kernel built it, so short frames (ARP in particular)
		 * arrive unpadded; hosts drop those as runts.
		 */
		if (n < 60) {
			memset(frame + n, 0, (size_t)(60 - n));
			n = 60;
		}

		/*
		 * AX_RX_CTL (0x0b) bit 0x0200 is IPE: only when the driver
		 * sets it does the controller prepend the 2 byte IP-alignment
		 * padding.  FreeBSD's axge driver deliberately leaves it
		 * clear, in which case the frame starts at offset 0.  Adding
		 * the padding anyway shifts the Ethernet header by two bytes
		 * and every frame gets dropped by the host.
		 */
		{
			unsigned rcr = mac_regs[0x0b] | (mac_regs[0x0c] << 8);
			const char *force = getenv("AX_PAD");
			int pad;

			/*
			 * IPE decides it in theory, but some drivers (the old
			 * asix ones) pull the 2 byte pseudo header
			 * unconditionally.  AX_PAD=2 / AX_PAD=0 overrides.
			 */
			if (force)
				pad = atoi(force) ? 2 : 0;
			else
				pad = (rcr & 0x0200) ? 2 : 0;

			pkt_len = n + pad;
			padd = (pkt_len + 7) & ~7;

			memset(out, 0, (size_t)padd + 8);
			memcpy(out + pad, frame, (size_t)n);

			meta = ((uint32_t)pkt_len) << 16;
			memcpy(out + padd, &meta, 4);
			hdr = ((uint32_t)padd << 16) | 1u;
			memcpy(out + padd + 4, &hdr, 4);

			fprintf(stderr, "rx frame len=%d pad=%d -> ep_write...\n", n, pad);
			{
				static int dumped, dumped_small;
				int w;

				if (!dumped && n > 300) {
					int i;

					dumped = 1;
					fprintf(stderr, "rx dump frame[0..23]:");
					for (i = 0; i < 24 && i < n; i++)
						fprintf(stderr, " %02x", frame[i]);
					fprintf(stderr, "\nrx dump header tail:");
					for (i = padd; i < padd + 8; i++)
						fprintf(stderr, " %02x", out[i]);
					fprintf(stderr, "\n");
				}

				/* First small (likely ARP) frame: print the
				 * Ethernet header so the destination MAC can be
				 * checked. */
				if (!dumped_small && n <= 64) {
					int i;

					dumped_small = 1;
					fprintf(stderr, "rx small frame len=%d:", n);
					for (i = 0; i < 20; i++)
						fprintf(stderr, " %02x", frame[i]);
					fprintf(stderr, "\n");
				}

				w = ep_write(gfd, ep_idx[1], out, (unsigned)(padd + 8));
				fprintf(stderr, "rx ep_write = %d\n", w);
				if (w < 0)
					break;
			}
		}
	}
	fprintf(stderr, "rx thread exit\n");
	return NULL;
}

/*
 * Link status is delivered on the interrupt endpoint, not by polling a
 * register.  ax88179_status() looks at bit 16 of intdata1
 * (AX_INT_PPLS_LINK); without it the driver keeps the carrier down and
 * never starts the bulk traffic.
 */
static void *int_fn(void *arg)
{
	uint8_t pkt[8] = { 0 };

	(void)arg;
	pkt[2] = 0x01;               /* intdata1 = 0x00010000, little endian */

	while (data_running) {
		int w;

		sleep(1);
		w = ep_write(gfd, ep_idx[0], pkt, sizeof(pkt));
		fprintf(stderr, "int ep_write = %d\n", w);
		if (w < 0)
			break;
	}
	fprintf(stderr, "int thread exit\n");
	return NULL;
}

static void data_path_start(int fd)
{
	if (data_running)
		return;
	if (tap_fd < 0) {
		tap_fd = tap_open(TAP_NAME);
		if (tap_fd < 0)
			return;
		tap_configure(TAP_NAME, TAP_IP, TAP_MASK);
		fprintf(stderr, "tap %s ready (%s/%s)\n", TAP_NAME, TAP_IP, TAP_MASK);
	}
	gfd = fd;
	data_running = 1;
	if (pthread_create(&tx_thread, NULL, tx_fn, NULL) != 0)
		data_running = 0;
	if (pthread_create(&rx_thread, NULL, rx_fn, NULL) != 0)
		data_running = 0;
	if (pthread_create(&int_thread, NULL, int_fn, NULL) != 0)
		data_running = 0;
}

static void data_path_stop(void)
{
	data_running = 0;
}

static void dump_setup(const struct usb_ctrlrequest *c)
{
	fprintf(stderr, "setup %s type=%02x req=%02x val=%04x idx=%04x len=%u\n",
		(c->bRequestType & USB_DIR_IN) ? "IN " : "OUT",
		c->bRequestType, c->bRequest,
		le16toh(c->wValue), le16toh(c->wIndex), le16toh(c->wLength));
}

static int handle_control(int fd, struct usb_ctrlrequest *c)
{
	uint8_t  buf[4096];
	unsigned wlen  = le16toh(c->wLength);
	uint8_t  type  = c->bRequestType & USB_TYPE_MASK;
	uint8_t  dir_in = c->bRequestType & USB_DIR_IN;

	if (type == USB_TYPE_STANDARD) {
		switch (c->bRequest) {

		case USB_REQ_GET_DESCRIPTOR: {
			uint8_t dt  = (uint8_t)(le16toh(c->wValue) >> 8);
			uint8_t idx = (uint8_t)(le16toh(c->wValue) & 0xff);
			int len = -1;

			switch (dt) {
			case USB_DT_DEVICE:
				len = (int)sizeof(dev_desc);
				memcpy(buf, &dev_desc, len);
				break;
			case USB_DT_CONFIG:
				len = config_len;
				memcpy(buf, config_buf, (size_t)len);
				break;
			case USB_DT_STRING:
				len = build_string(idx, buf);
				break;
			default:
				return -1;      /* qualifier / BOS: not supported */
			}
			if (len < 0)
				return -1;
			if ((unsigned)len > wlen)
				len = (int)wlen;
			return ep0_write(fd, buf, (unsigned)len);
		}

		case USB_REQ_SET_CONFIGURATION:
			if (le16toh(c->wValue) == 1) {
				enable_eps(fd);
				data_path_start(fd);
			}
			return ep0_ack(fd);

		case USB_REQ_GET_CONFIGURATION:
			buf[0] = 1;
			return ep0_write(fd, buf, 1);

		case USB_REQ_GET_STATUS:
			buf[0] = 0;
			buf[1] = 0;
			return ep0_write(fd, buf, 2);

		case USB_REQ_SET_INTERFACE:
			return ep0_ack(fd);

		case USB_REQ_GET_INTERFACE:
			buf[0] = 0;
			return ep0_write(fd, buf, 1);

		default:
			return -1;
		}
	}

	if (type == USB_TYPE_VENDOR) {
		unsigned v = le16toh(c->wValue);
		unsigned i = le16toh(c->wIndex);
		unsigned n = wlen > sizeof(buf) ? sizeof(buf) : wlen;
		unsigned k;

		if (dir_in) {
			memset(buf, 0, n);
			switch (c->bRequest) {
			case AX_ACCESS_MAC:
				for (k = 0; k < n; k++)
					buf[k] = mac_regs[(v + k) & 0xff];
				break;
			case AX_ACCESS_PHY:
				if (i < PHY_REGS && n >= 2) {
					buf[0] = (uint8_t)(phy_regs[i] & 0xff);
					buf[1] = (uint8_t)(phy_regs[i] >> 8);
				}
				break;
			case AX_ACCESS_EEPROM:
			case AX_ACCESS_EFUS:
				/* 0xff makes the driver fall back to its defaults. */
				memset(buf, 0xff, n);
				break;
			default:
				break;
			}
			return ep0_write(fd, buf, n);
		}

		if (n) {
			int r = ep0_read(fd, buf, n);

			if (r > 0 && c->bRequest == AX_ACCESS_MAC) {
				for (k = 0; k < (unsigned)r; k++)
					mac_regs[(v + k) & 0xff] = buf[k];

				/* AX_RX_CTL: log the raw value so the IPE
				 * assumption can be checked. */
				if (v == 0x0b && r >= 2)
					fprintf(stderr,
						"  AX_RX_CTL <- %02x %02x (IPE=%d)\n",
						buf[0], buf[1],
						(buf[1] & 0x02) ? 1 : 0);
			}
			return r;
		}
		return ep0_ack(fd);
	}

	return -1;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [udc-name]\n"
		"\n"
		"  udc-name   USB device controller to bind to, as listed in\n"
		"             /sys/class/udc.  Detected automatically when exactly\n"
		"             one controller is present.\n"
		"\n"
		"environment:\n"
		"  AX_PAD=0|2 force the RX alignment padding instead of following\n"
		"             the host's AX_RX_CTL.IPE setting\n"
		"  AX_MAC=..  MAC address reported to the host\n"
		"             (default 02:00:00:00:00:01)\n",
		prog);
}

/*
 * Pick the UDC.  Returns the number of controllers found; the first one
 * is copied into out when there is exactly one.
 */
static int find_udc(char *out, size_t len)
{
	DIR *d = opendir("/sys/class/udc");
	struct dirent *de;
	int count = 0;

	if (!d) {
		perror("opendir /sys/class/udc");
		return -1;
	}
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		count++;
		if (count == 1)
			snprintf(out, len, "%s", de->d_name);
	}
	closedir(d);
	return count;
}

static uint8_t udc_max_speed(const char *udc)
{
	char path[256];
	char buf[32];
	FILE *f;

	snprintf(path, sizeof(path), "/sys/class/udc/%s/maximum_speed", udc);
	f = fopen(path, "r");
	if (!f)
		return USB_SPEED_HIGH;
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return USB_SPEED_HIGH;
	}
	fclose(f);

	if (strstr(buf, "super"))
		return USB_SPEED_SUPER;
	if (strstr(buf, "high"))
		return USB_SPEED_HIGH;
	if (strstr(buf, "full"))
		return USB_SPEED_FULL;
	if (strstr(buf, "low"))
		return USB_SPEED_LOW;
	return USB_SPEED_HIGH;
}

int main(int argc, char **argv)
{
	char udc_buf[128];
	const char *udc;
	struct usb_raw_init arg;
	struct raw_control_event ev;
	int fd, r, found;

	if (argc > 1 && (strcmp(argv[1], "-h") == 0 ||
			 strcmp(argv[1], "--help") == 0)) {
		usage(argv[0]);
		return 0;
	}

	if (argc > 1) {
		udc = argv[1];
	} else {
		found = find_udc(udc_buf, sizeof(udc_buf));
		if (found <= 0) {
			fprintf(stderr, "no USB device controller found in /sys/class/udc\n");
			fprintf(stderr, "this kernel/hardware cannot do USB gadget mode\n");
			return 1;
		}
		if (found > 1) {
			fprintf(stderr,
				"several UDC controllers found; pass one explicitly:\n");
			fprintf(stderr, "  %s <udc-name>\n", argv[0]);
			fprintf(stderr, "  see /sys/class/udc\n");
			return 1;
		}
		udc = udc_buf;
	}

	fprintf(stderr, "udc: %s\n", udc);

	if (geteuid() != 0) {
		fprintf(stderr,
			"this program must run as root: it opens /dev/raw-gadget\n"
			"and /dev/net/tun and configures the TAP interface\n");
		return 1;
	}

	init_descriptors();
	init_regs();

	fd = open("/dev/raw-gadget", O_RDWR);
	if (fd < 0) {
		if (errno == ENOENT) {
			fprintf(stderr,
				"/dev/raw-gadget does not exist.\n"
				"the raw_gadget driver is not loaded; try:\n"
				"    modprobe raw_gadget\n"
				"if that fails, the kernel was built without\n"
				"CONFIG_USB_RAW_GADGET and cannot run this program\n");
		} else {
			perror("open /dev/raw-gadget");
		}
		return 1;
	}

	memset(&arg, 0, sizeof(arg));
	/*
	 * Both fields must hold the UDC name.
	 *
	 * raw_gadget.c's gadget_bind() rejects the bind with -ENODEV unless
	 *     strcmp(gadget->name, dev->udc_name) == 0
	 * and dev->udc_name comes from arg.driver_name.  arg.device_name
	 * becomes driver.udc_name, which the UDC core uses to select the
	 * controller.  Passing the controller *driver* name (e.g. "dwc2")
	 * here makes RUN fail with EBUSY after the core already found the
	 * UDC, which looks exactly like "the UDC is busy".
	 */
	strncpy((char *)arg.driver_name, udc, sizeof(arg.driver_name) - 1);
	strncpy((char *)arg.device_name, udc, sizeof(arg.device_name) - 1);
	arg.speed = udc_max_speed(udc);

	r = ioctl(fd, USB_RAW_IOCTL_INIT, &arg);
	if (r < 0) {
		perror("USB_RAW_IOCTL_INIT");
		fprintf(stderr, "hint: is another gadget bound to %s?\n", udc);
		fprintf(stderr, "      echo \"\" > "
			"/sys/kernel/config/usb_gadget/g1/UDC\n");
		return 1;
	}

	r = ioctl(fd, USB_RAW_IOCTL_RUN, 0);
	if (r < 0) {
		perror("USB_RAW_IOCTL_RUN");
		return 1;
	}

	fprintf(stderr, "listening on %s as 0b95:1790, plug the cable\n", udc);

	for (;;) {
		memset(&ev, 0, sizeof(ev));
		ev.inner.type   = 0;
		ev.inner.length = sizeof(ev.ctrl);

		r = ioctl(fd, USB_RAW_IOCTL_EVENT_FETCH, &ev);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			perror("EVENT_FETCH");
			break;
		}

		switch (ev.inner.type) {
		case USB_RAW_EVENT_CONNECT:
			fprintf(stderr, "== connected ==\n");
			continue;
		case USB_RAW_EVENT_RESET:
			fprintf(stderr, "== reset ==\n");
			data_path_stop();
			disable_eps(fd);
			continue;
		case USB_RAW_EVENT_DISCONNECT:
			fprintf(stderr, "== disconnected ==\n");
			data_path_stop();
			disable_eps(fd);
			continue;
		case USB_RAW_EVENT_CONTROL:
			break;
		default:
			fprintf(stderr, "== event %u ==\n", ev.inner.type);
			continue;
		}

		dump_setup(&ev.ctrl);

		r = handle_control(fd, &ev.ctrl);
		if (r < 0) {
			fprintf(stderr, "  -> stall (r=%d)\n", r);
			ioctl(fd, USB_RAW_IOCTL_EP0_STALL, 0);
		} else {
			fprintf(stderr, "  -> ok (r=%d)\n", r);
		}
	}

	close(fd);
	return 0;
}
