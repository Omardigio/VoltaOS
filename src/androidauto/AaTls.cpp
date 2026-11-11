#include "AaTls.h"
#include <openssl/err.h>
#include <openssl/pem.h>

AaTls::AaTls(bool serverRole) : m_server(serverRole) {}

AaTls::~AaTls()
{
    if (m_ssl) SSL_free(m_ssl);       // also frees the two BIOs
    if (m_ctx) SSL_CTX_free(m_ctx);
}

static QString sslError()
{
    char buf[256];
    const unsigned long e = ERR_get_error();
    if (!e) return QStringLiteral("unknown OpenSSL error");
    ERR_error_string_n(e, buf, sizeof(buf));
    return QString::fromLatin1(buf);
}

bool AaTls::init(const QByteArray &certPem, const QByteArray &keyPem, QString *error)
{
    const auto fail = [&](const QString &why) { if (error) *error = why; return false; };
    m_ctx = SSL_CTX_new(m_server ? TLS_server_method() : TLS_client_method());
    if (!m_ctx) return fail(sslError());
    SSL_CTX_set_min_proto_version(m_ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(m_ctx, TLS1_2_VERSION);
    SSL_CTX_set_security_level(m_ctx, 0);                 // the phone's side is old-style RSA
    SSL_CTX_set_verify(m_ctx, SSL_VERIFY_NONE, nullptr);

    BIO *cb = BIO_new_mem_buf(certPem.constData(), certPem.size());
    X509 *cert = cb ? PEM_read_bio_X509(cb, nullptr, nullptr, nullptr) : nullptr;
    BIO_free(cb);
    if (!cert) return fail(QStringLiteral("certificate: ") + sslError());
    const int c = SSL_CTX_use_certificate(m_ctx, cert);
    X509_free(cert);
    if (c != 1) return fail(QStringLiteral("certificate: ") + sslError());

    BIO *kb = BIO_new_mem_buf(keyPem.constData(), keyPem.size());
    EVP_PKEY *key = kb ? PEM_read_bio_PrivateKey(kb, nullptr, nullptr, nullptr) : nullptr;
    BIO_free(kb);
    if (!key) return fail(QStringLiteral("private key: ") + sslError());
    const int k = SSL_CTX_use_PrivateKey(m_ctx, key);
    EVP_PKEY_free(key);
    if (k != 1) return fail(QStringLiteral("private key: ") + sslError());

    m_ssl = SSL_new(m_ctx);
    m_in = BIO_new(BIO_s_mem());
    m_out = BIO_new(BIO_s_mem());
    if (!m_ssl || !m_in || !m_out) return fail(sslError());
    SSL_set_bio(m_ssl, m_in, m_out);                      // the SSL object owns the BIOs now
    if (m_server) SSL_set_accept_state(m_ssl); else SSL_set_connect_state(m_ssl);
    return true;
}

QByteArray AaTls::drainOut()
{
    QByteArray out;
    const int n = int(BIO_ctrl_pending(m_out));
    if (n > 0) {
        out.resize(n);
        const int r = BIO_read(m_out, out.data(), n);
        out.resize(r > 0 ? r : 0);
    }
    return out;
}

QByteArray AaTls::handshake(const QByteArray &in, bool *done, bool *failed)
{
    *done = false; *failed = false;
    if (!m_ssl) { *failed = true; return {}; }
    if (!in.isEmpty()) BIO_write(m_in, in.constData(), in.size());
    const int r = SSL_do_handshake(m_ssl);
    if (r == 1) { m_active = true; *done = true; }
    else {
        const int e = SSL_get_error(m_ssl, r);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) { *failed = true; }
    }
    return drainOut();
}

QByteArray AaTls::encrypt(const QByteArray &plain)
{
    if (!m_active) return {};
    if (SSL_write(m_ssl, plain.constData(), plain.size()) != plain.size()) return {};
    return drainOut();
}

QByteArray AaTls::decrypt(const QByteArray &cipher)
{
    if (!m_active) return {};
    BIO_write(m_in, cipher.constData(), cipher.size());
    QByteArray out;
    char buf[0x4000];
    for (;;) {
        const int r = SSL_read(m_ssl, buf, sizeof(buf));
        if (r <= 0) break;
        out.append(buf, r);
    }
    return out;
}
