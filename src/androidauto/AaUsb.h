#pragma once
// Wired Android Auto: the phone is put in "Android Open Accessory" mode over USB, after which it
// shows up as a device (18d1:2d00 / 2d01) with two bulk endpoints that carry the Android Auto
// byte stream. Runs in its own thread; the callbacks are called FROM that thread.
//
// Access to the phone needs a udev rule (packaging/51-volta-android-auto.rules), otherwise the
// state says "permission".
#include <QByteArray>
#include <QString>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

struct libusb_context;
struct libusb_device_handle;

class AaUsb {
public:
    AaUsb();
    ~AaUsb();
    /// Starts looking for a phone.
    void start();
    void stop();
    /// Sends bytes to the phone (any thread). false if there is no connection.
    bool write(const QByteArray &data);

    // callbacks, all invoked from the USB thread
    std::function<void()> onConnected;
    std::function<void(const QByteArray &)> onData;
    std::function<void(const QString &reason)> onDisconnected;
    /// searching | permission | probing — what the search is doing (for the screen)
    std::function<void(const QString &what, const QString &detail)> onStatus;

private:
    void loop();
    bool probePhone(libusb_device_handle *h);          // asks the phone to switch to accessory mode
    void runConnection(libusb_device_handle *h, unsigned char epIn, unsigned char epOut);

    libusb_context *m_ctx = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::mutex m_txLock;
    libusb_device_handle *m_dev = nullptr;
    unsigned char m_epOut = 0;
    std::atomic<bool> m_up{false};
};
