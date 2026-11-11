#include "AaUsb.h"

#include <libusb-1.0/libusb.h>

#include <QDebug>
#include <QSet>
#include <chrono>
#include <cstring>

static constexpr uint16_t kGoogleVid = 0x18d1;
static constexpr uint16_t kCarlinkVid = 0x1314;                       // the dongle: not a phone
static constexpr uint8_t kReqGetProtocol = 51, kReqSendString = 52, kReqStart = 53;

static bool isAccessory(uint16_t vid, uint16_t pid)
{
    return vid == kGoogleVid && (pid == 0x2d00 || pid == 0x2d01 || pid == 0x2d04 || pid == 0x2d05);
}

AaUsb::AaUsb() = default;

AaUsb::~AaUsb() { stop(); }

void AaUsb::start()
{
    if (m_thread.joinable()) return;
    m_stop = false;
    m_thread = std::thread([this] { loop(); });
}

void AaUsb::stop()
{
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
}

bool AaUsb::write(const QByteArray &data)
{
    std::lock_guard<std::mutex> lock(m_txLock);
    if (!m_up || !m_dev) return false;
    int off = 0;
    while (off < data.size()) {
        const int n = std::min(16384, int(data.size()) - off);
        int sent = 0;
        const int r = libusb_bulk_transfer(m_dev, m_epOut,
                          reinterpret_cast<unsigned char *>(const_cast<char *>(data.constData())) + off, n, &sent, 5000);
        if (r != 0) { qWarning() << "[AA/USB] write error" << libusb_error_name(r); return false; }
        off += sent;
    }
    return true;
}

// "Android Open Accessory" protocol: ask the phone to restart as an accessory device.
bool AaUsb::probePhone(libusb_device_handle *h)
{
    unsigned char ver[2] = {0, 0};
    if (libusb_control_transfer(h, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
                                kReqGetProtocol, 0, 0, ver, 2, 1000) != 2)
        return false;
    const int protocol = ver[0] | (ver[1] << 8);
    if (protocol < 1) return false;
    qInfo() << "[AA/USB] the device speaks the accessory protocol, version" << protocol;
    const char *strings[] = {"Android", "Android Auto", "Android Auto", "2.0.1",
                             "https://developer.android.com/auto/", "HU-AAAAAA"};
    for (uint16_t i = 0; i < 6; ++i) {
        auto *s = reinterpret_cast<unsigned char *>(const_cast<char *>(strings[i]));
        if (libusb_control_transfer(h, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
                                    kReqSendString, 0, i, s, uint16_t(strlen(strings[i]) + 1), 1000) < 0)
            return false;
    }
    return libusb_control_transfer(h, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
                                   kReqStart, 0, 0, nullptr, 0, 1000) >= 0;
}

// Devices that cannot be a phone (hubs, keyboards, disks...) are not even opened.
static bool looksLikePhone(libusb_device *dev, const libusb_device_descriptor &d)
{
    if (d.idVendor == kCarlinkVid || d.bDeviceClass == LIBUSB_CLASS_HUB) return false;
    libusb_config_descriptor *cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0 || !cfg) return true;   // cannot tell: try
    bool plausible = false;
    for (int i = 0; i < cfg->bNumInterfaces; ++i)
        for (int a = 0; a < cfg->interface[i].num_altsetting; ++a) {
            const int c = cfg->interface[i].altsetting[a].bInterfaceClass;
            if (c == 0xFF || c == LIBUSB_CLASS_IMAGE || c == 0 || c == LIBUSB_CLASS_PER_INTERFACE) plausible = true;
        }
    libusb_free_config_descriptor(cfg);
    return plausible;
}

void AaUsb::loop()
{
    if (libusb_init(&m_ctx) != 0) {
        if (onStatus) onStatus(QStringLiteral("error"), QStringLiteral("USB not available"));
        return;
    }
    QSet<quint32> tried;               // bus<<8|address of the devices we already asked to switch
    qint64 lastClear = 0;
    bool reportedPermission = false;
    while (!m_stop) {
        const qint64 now = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - lastClear > 15) { tried.clear(); lastClear = now; }      // let it retry from time to time

        libusb_device **list = nullptr;
        const ssize_t n = libusb_get_device_list(m_ctx, &list);
        bool connected = false, permissionDenied = false, probing = false;
        for (ssize_t i = 0; i < n && !connected && !m_stop; ++i) {
            libusb_device *dev = list[i];
            libusb_device_descriptor d;
            if (libusb_get_device_descriptor(dev, &d) != 0) continue;
            const quint32 key = (quint32(libusb_get_bus_number(dev)) << 8) | libusb_get_device_address(dev);

            if (isAccessory(d.idVendor, d.idProduct)) {
                libusb_device_handle *h = nullptr;
                const int r = libusb_open(dev, &h);
                if (r != 0) { if (r == LIBUSB_ERROR_ACCESS) permissionDenied = true; continue; }
                // find the interface with the two bulk endpoints (skip the ADB one of 2d01)
                libusb_config_descriptor *cfg = nullptr;
                int ifc = -1; unsigned char epIn = 0, epOut = 0;
                if (libusb_get_active_config_descriptor(dev, &cfg) == 0) {
                    for (int k = 0; k < cfg->bNumInterfaces && ifc < 0; ++k) {
                        const libusb_interface_descriptor &alt = cfg->interface[k].altsetting[0];
                        if (alt.bInterfaceClass == 0xFF && alt.bInterfaceSubClass == 0x42 && alt.bInterfaceProtocol == 1)
                            continue;                                          // ADB
                        unsigned char in = 0, out = 0;
                        for (int e = 0; e < alt.bNumEndpoints; ++e) {
                            const auto &ep = alt.endpoint[e];
                            if ((ep.bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK) continue;
                            if (ep.bEndpointAddress & LIBUSB_ENDPOINT_IN) in = ep.bEndpointAddress; else out = ep.bEndpointAddress;
                        }
                        if (in && out) { ifc = alt.bInterfaceNumber; epIn = in; epOut = out; }
                    }
                    libusb_free_config_descriptor(cfg);
                }
                if (ifc >= 0) {
                    libusb_set_auto_detach_kernel_driver(h, 1);
                    if (libusb_claim_interface(h, ifc) == 0) {
                        connected = true;
                        runConnection(h, epIn, epOut);
                        libusb_release_interface(h, ifc);
                    } else {
                        qWarning() << "[AA/USB] cannot claim the accessory interface";
                    }
                }
                libusb_close(h);
                continue;
            }

            if (tried.contains(key) || !looksLikePhone(dev, d)) continue;
            libusb_device_handle *h = nullptr;
            const int r = libusb_open(dev, &h);
            if (r != 0) {
                if (r == LIBUSB_ERROR_ACCESS) { permissionDenied = true; qInfo().nospace() << "[AA/USB] no permission for " << Qt::hex << d.idVendor << ":" << d.idProduct; }
                continue;
            }
            tried.insert(key);
            qInfo().nospace() << "[AA/USB] asking " << Qt::hex << d.idVendor << ":" << d.idProduct << " to switch to accessory mode";
            if (probePhone(h)) probing = true;
            libusb_close(h);
        }
        libusb_free_device_list(list, 1);

        if (!connected) {
            if (permissionDenied && !reportedPermission) {
                reportedPermission = true;
                if (onStatus) onStatus(QStringLiteral("permission"), QString());
            } else if (probing) {
                if (onStatus) onStatus(QStringLiteral("probing"), QString());
            } else if (!permissionDenied) {
                reportedPermission = false;
                if (onStatus) onStatus(QStringLiteral("searching"), QString());
            }
            for (int i = 0; i < 10 && !m_stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    libusb_exit(m_ctx);
    m_ctx = nullptr;
}

void AaUsb::runConnection(libusb_device_handle *h, unsigned char epIn, unsigned char epOut)
{
    qInfo() << "[AA/USB] phone in accessory mode, connected";
    {
        std::lock_guard<std::mutex> lock(m_txLock);
        m_dev = h; m_epOut = epOut; m_up = true;
    }
    if (onConnected) onConnected();
    QString reason = QStringLiteral("stopped");
    unsigned char buf[16384];
    while (!m_stop) {
        int got = 0;
        const int r = libusb_bulk_transfer(h, epIn, buf, sizeof(buf), &got, 1000);
        if (r == LIBUSB_ERROR_TIMEOUT) { if (got > 0 && onData) onData(QByteArray(reinterpret_cast<char *>(buf), got)); continue; }
        if (r != 0) { reason = QStringLiteral("USB: ") + QString::fromLatin1(libusb_error_name(r)); break; }
        if (got > 0 && onData) onData(QByteArray(reinterpret_cast<char *>(buf), got));
    }
    {
        std::lock_guard<std::mutex> lock(m_txLock);
        m_up = false; m_dev = nullptr;
    }
    qInfo() << "[AA/USB] disconnected:" << reason;
    if (onDisconnected) onDisconnected(reason);
}
