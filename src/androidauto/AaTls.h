#pragma once
// TLS for Android Auto, over memory buffers: the bytes of the handshake travel inside Android Auto
// messages (not on a socket), so OpenSSL reads and writes through two BIOs.
// The head unit is the TLS *client* and presents the head unit certificate to the phone.
#include <QByteArray>
#include <QString>
#include <openssl/ssl.h>

class AaTls {
public:
    explicit AaTls(bool serverRole = false);   // serverRole: only for the tests (fake phone)
    ~AaTls();
    AaTls(const AaTls &) = delete;
    AaTls &operator=(const AaTls &) = delete;

    bool init(const QByteArray &certPem, const QByteArray &keyPem, QString *error = nullptr);
    /// Feeds handshake bytes from the other side (empty to start); returns the bytes to send back.
    QByteArray handshake(const QByteArray &in, bool *done, bool *failed);
    /// Encrypts a block of up to 16 KiB; empty on error.
    QByteArray encrypt(const QByteArray &plain);
    /// Decrypts a received block; empty on error or if more data is needed.
    QByteArray decrypt(const QByteArray &cipher);
    bool active() const { return m_active; }

private:
    QByteArray drainOut();
    bool m_server;
    bool m_active = false;
    SSL_CTX *m_ctx = nullptr;
    SSL *m_ssl = nullptr;
    BIO *m_in = nullptr;      // data coming from the phone
    BIO *m_out = nullptr;     // data to send to the phone
};
