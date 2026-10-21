// usbnet.cpp — optional USB network (CDC-NCM) to a computer, preferred over WiFi.
//
// The firmware is built for the chip's USB-Serial-JTAG (console, web flasher,
// panic output); that stays the default ("Serial"). Network mode switches the
// USB PHY to the USB-OTG controller at run time (USB.begin(), TinyUSB): the
// computer sees a new composite device, CDC (console) + NCM (Ethernet). Back
// to Serial = reboot (TinyUSB cannot be stopped; usbnet_early() hands the PHY
// back to the Serial-JTAG, also after a crash).
//
// The computer shares its connectivity (NetworkManager "shared": DHCP + NAT),
// Paddle2IP is a DHCP client. While the USB interface has an address WiFi is
// off: lwIP sends to the WiFi subnet over WiFi whatever the route priority.
// No STATUS for FALLBACK_MS over USB -> the USB interface is taken down and
// WiFi comes back, until the cable (or the host's NCM interface) re-attaches.
//
// Mode choice: g_set.usbNet. When a computer appears on the cable (SOF frames
// on the Serial-JTAG; a charger sends none) the display offers Serial /
// Network and takes the saved choice after 10 s (ui.cpp). A crash within
// STABLE_MS after starting network mode keeps the next boot in Serial.
//
// NCM class driver: ncm_device.c (TinyUSB 0.15.0), registered as an app
// driver. Its state is unlocked, so every entry (USB task, TCP/IP task) goes
// through ncmMtx.
#include "shared.h"
#include <WiFi.h>
#include "USB.h"
#include "USBCDC.h"
#include "esp32-hal-tinyusb.h"
#include "device/usbd_pvt.h"
#include "class/net/net_device.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_attr.h"
#include "driver/periph_ctrl.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/usb_serial_jtag_reg.h"

static const uint32_t FALLBACK_MS = 10000;      // no STATUS over USB this long -> WiFi
static const uint32_t STABLE_MS   = 60000;      // network mode survived: crash guard off
static const uint32_t RTC_MAGIC   = 0x55534E31; // "USN1"

static RTC_NOINIT_ATTR uint32_t rtcMagic;
static RTC_NOINIT_ATTR uint32_t rtcStarting;    // network mode started < STABLE_MS before the reset

enum UState : uint8_t { U_SERIAL, U_CHOOSE, U_NET };
static UState   ust = U_SERIAL;
static bool     failed = false;                 // network mode crashed: this boot stays Serial
static bool     asked = false;                  // menu shown for this plug-in of the cable
static uint16_t sofLast = 0;
static uint32_t sofMs = 0;
static bool     sofSeen = false;
static uint32_t netStartMs = 0;

static USBCDC*            cdc = nullptr;
static esp_netif_t*       netif = nullptr;
static SemaphoreHandle_t  ncmMtx = nullptr;
static usbd_class_driver_t ncmDrv;
static volatile bool      ncmAlt = false;       // host enabled the data interface (alt 1)
static bool               linkPrev = false, linkUp = false, fallback = false;
static volatile bool      ipUp = false;
static volatile uint32_t  ipAddr = 0;
static uint32_t           freshMs = 0;
static uint8_t            devMac[6], hostMac[6];
static char               macDesc[13], macText[18];

bool        usbnet_choosing() { return ust == U_CHOOSE; }
bool        usbnet_active()   { return ust == U_NET; }
bool        usbnet_up()       { return ipUp; }
bool        usbnet_failed()   { return failed; }
IPAddress   usbnet_ip()       { return IPAddress(ipAddr); }
const char* usbnet_host_mac() { return macText; }
Stream&     con()             { return cdc ? (Stream&)*cdc : (Stream&)Serial; }

static uint16_t sof_frame() { return REG_GET_FIELD(USB_SERIAL_JTAG_FRAM_NUM_REG, USB_SERIAL_JTAG_SOF_FRAME_INDEX); }

void usbnet_early() {
  // The PHY selection lives in the RTC domain and survives a reboot or a
  // crash in network mode: give the USB back to the Serial-JTAG and drop the
  // pad for a moment, so the computer enumerates the console again.
  if (REG_GET_BIT(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL)) {
    periph_module_reset(PERIPH_USB_MODULE);
    periph_module_disable(PERIPH_USB_MODULE);
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE);
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PHY_SEL | USB_SERIAL_JTAG_USB_PAD_ENABLE);
    delay(20);
    SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
  }
  esp_reset_reason_t r = esp_reset_reason();
  bool crash = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
               r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
  failed = rtcMagic == RTC_MAGIC && rtcStarting == 1 && crash;
  rtcMagic = RTC_MAGIC;
  rtcStarting = 0;

  // ESP side = the Ethernet MAC; the computer's interface (enx...) gets the
  // same one as a locally administered address
  esp_read_mac(devMac, ESP_MAC_ETH);
  memcpy(hostMac, devMac, 6);
  hostMac[0] |= 0x02;
  snprintf(macDesc, sizeof(macDesc), "%02X%02X%02X%02X%02X%02X", hostMac[0], hostMac[1], hostMac[2], hostMac[3], hostMac[4], hostMac[5]);
  snprintf(macText, sizeof(macText), "%02x:%02x:%02x:%02x:%02x:%02x", hostMac[0], hostMac[1], hostMac[2], hostMac[3], hostMac[4], hostMac[5]);
  sofLast = sof_frame();
}

// ---------------- NCM <-> TinyUSB ----------------
static void     d_reset(uint8_t p) { xSemaphoreTake(ncmMtx, portMAX_DELAY); netd_reset(p); ncmAlt = false; xSemaphoreGive(ncmMtx); }
static uint16_t d_open(uint8_t p, tusb_desc_interface_t const* d, uint16_t n) {
  xSemaphoreTake(ncmMtx, portMAX_DELAY); uint16_t r = netd_open(p, d, n); xSemaphoreGive(ncmMtx); return r;
}
static bool d_ctrl(uint8_t p, uint8_t st, tusb_control_request_t const* rq) {
  xSemaphoreTake(ncmMtx, portMAX_DELAY); bool r = netd_control_xfer_cb(p, st, rq); xSemaphoreGive(ncmMtx); return r;
}
static bool d_xfer(uint8_t p, uint8_t ep, xfer_result_t res, uint32_t n) {
  xSemaphoreTake(ncmMtx, portMAX_DELAY); bool r = netd_xfer_cb(p, ep, res, n); xSemaphoreGive(ncmMtx); return r;
}

extern "C" usbd_class_driver_t const* usbd_app_driver_get_cb(uint8_t* count) {
  *count = 1;
  return &ncmDrv;
}

extern "C" void tud_network_link_state_cb(bool up) { ncmAlt = up; }

static void ncm_free_rx(void*, void* buf) { free(buf); }

// USB task, inside ncmMtx: hand a copy to lwIP (it frees it via ncm_free_rx)
extern "C" bool tud_network_recv_cb(const uint8_t* src, uint16_t size) {
  if (netif && size) {
    void* b = malloc(size);
    if (b) { memcpy(b, src, size); esp_netif_receive(netif, b, size, nullptr); }
  }
  tud_network_recv_renew();                      // next datagram of the NTB, or the next NTB
  return true;
}

extern "C" uint16_t tud_network_xmit_cb(uint8_t* dst, void* ref, uint16_t arg) {
  memcpy(dst, ref, arg);
  return arg;
}

// TCP/IP task: both NTBs busy -> wait up to ~5 ms, then drop
static esp_err_t ncm_transmit(void*, void* buf, size_t len) {
  if (!ncmAlt || len > 1514) return ESP_FAIL;
  for (int i = 0; i < 5; i++) {
    xSemaphoreTake(ncmMtx, portMAX_DELAY);
    bool ok = tud_network_can_xmit((uint16_t)len);
    if (ok) tud_network_xmit(buf, (uint16_t)len);
    xSemaphoreGive(ncmMtx);
    if (ok) return ESP_OK;
    vTaskDelay(1);
  }
  return ESP_FAIL;
}

static esp_err_t ncm_post_attach(esp_netif_t* n, esp_netif_iodriver_handle h) {
  ((esp_netif_driver_base_t*)h)->netif = n;
  esp_netif_driver_ifconfig_t c = {};
  c.handle = h;
  c.transmit = ncm_transmit;
  c.driver_free_rx_buffer = ncm_free_rx;
  return esp_netif_set_driver_config(n, &c);
}
static esp_netif_driver_base_t ncmBase = { ncm_post_attach, nullptr };

// configuration descriptor part, loaded by USB.begin() after the CDC
static uint16_t ncm_descriptor(uint8_t* dst, uint8_t* itf) {
  uint8_t sName = tinyusb_add_string_descriptor("Paddle2IP Network");
  uint8_t sMac  = tinyusb_add_string_descriptor(macDesc);
  uint8_t epNotif = tinyusb_get_free_in_endpoint();
  uint8_t epData  = tinyusb_get_free_duplex_endpoint();
  if (!epNotif || !epData) return 0;
  uint8_t d[TUD_CDC_NCM_DESC_LEN] = {
    TUD_CDC_NCM_DESCRIPTOR(*itf, sName, sMac, (uint8_t)(0x80 | epNotif), 64, epData, (uint8_t)(0x80 | epData), 64, 1514)
  };
  *itf += 2;
  memcpy(dst, d, sizeof(d));
  return sizeof(d);
}

static void net_start() {
  ust = U_NET;
  netStartMs = millis();
  rtcStarting = 1;
  ncmMtx = xSemaphoreCreateMutex();

  esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
  base.if_key = "ETH_DEF";                       // mDNS (IDF 4.4) serves only its predefined keys
  base.if_desc = "usb";
  base.route_prio = 200;                         // above the WiFi STA (100)
  memcpy(base.mac, devMac, 6);
  esp_netif_config_t cfg = {};
  cfg.base = &base;
  cfg.stack = ESP_NETIF_NETSTACK_DEFAULT_ETH;
  netif = esp_netif_new(&cfg);
  esp_netif_attach(netif, &ncmBase);
  esp_netif_action_start(netif, nullptr, 0, nullptr);
  esp_netif_set_hostname(netif, HOSTNAME);

  ncmDrv.init = netd_init;
  ncmDrv.reset = d_reset;
  ncmDrv.open = d_open;
  ncmDrv.control_xfer_cb = d_ctrl;
  ncmDrv.xfer_cb = d_xfer;
  ncmDrv.sof = nullptr;

  Serial.end();                                  // the Serial-JTAG leaves with the PHY
  cdc = new USBCDC(0);                           // registers the CDC interface (first)
  tinyusb_enable_interface(USB_INTERFACE_CUSTOM, TUD_CDC_NCM_DESC_LEN, ncm_descriptor);
  cdc->begin(115200);
  USB.productName("Paddle2IP");
  USB.begin();                                   // PHY -> USB-OTG, the computer re-enumerates
  // arduino creates the USB task unpinned at configMAX_PRIORITIES - 1, above the keyer
  TaskHandle_t t = xTaskGetHandle("usbd");
  if (t) vTaskPrioritySet(t, configMAX_PRIORITIES - 3);
}

void usbnet_choose(bool net) {
  if (ust == U_NET) return;
  if (net) net_start(); else ust = U_SERIAL;
}

static bool any_wifi() {
  for (int i = 0; i < WIFI_SLOTS; i++) if (g_set.ssid[i][0]) return true;
  return false;
}

void usbnet_service() {
  uint32_t ms = millis();
  if (ust != U_NET) {
    // a computer on the cable sends a SOF frame every 1 ms (11-bit frame number)
    uint16_t f = sof_frame();
    if (f != sofLast) { sofLast = f; sofMs = ms; sofSeen = true; }
    bool host = sofSeen && ms - sofMs < 500;
    if (!host) {
      if (ust == U_CHOOSE) ust = U_SERIAL;
      if (ms - sofMs > 2000) asked = false;      // unplugged: ask again next time
    } else if (!asked) {
      asked = true;
      if (!failed && !wlan_setup_mode()) ust = U_CHOOSE;
    }
    return;
  }

  if (rtcStarting && ms - netStartMs > STABLE_MS) rtcStarting = 0;
  bool link = tud_mounted() && !tud_suspended() && ncmAlt;
  if (link && !linkPrev) fallback = false;       // (re)attached: USB first again
  linkPrev = link;
  bool want = link && !fallback;
  if (want != linkUp) {
    linkUp = want;
    if (want) esp_netif_action_connected(netif, nullptr, 0, nullptr);   // DHCP client
    else      esp_netif_action_disconnected(netif, nullptr, 0, nullptr);
  }
  esp_netif_ip_info_t ip;
  bool has = linkUp && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
  ipAddr = has ? ip.ip.addr : 0;
  ipUp = has;

  LinkState L;
  net_get(&L);
  if (!has || L.fresh) freshMs = ms;
  if (has && g_set.keySet && g_set.host[0] && any_wifi() && ms - freshMs > FALLBACK_MS) fallback = true;

  // the session id needs the radio on once (true RNG), see net.cpp
  wlan_suspend(has && net_session() != 0 && !wlan_setup_mode());
}
