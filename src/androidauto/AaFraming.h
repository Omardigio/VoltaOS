#pragma once
// Android Auto framing: how messages travel inside the byte stream (USB cable or TCP).
//
//   frame = channel (1) | flags (1) | payload size (2, big endian) | [total size (4)] | payload
//   flags: bit0 FIRST fragment, bit1 LAST fragment (both = a whole message), bit2 CONTROL
//          (control-type message), bit3 ENCRYPTED (payload is TLS data)
//   the 4-byte total size is only present on a FIRST frame that is not also LAST.
//   Messages longer than 16 KiB are split in several frames.
//   The payload of a message starts with the message id (2 bytes, big endian).
//
// Format reference: the open source aasdk library (GPLv3).
#include <QByteArray>

namespace aa {

constexpr quint8 kFirst = 0x01, kLast = 0x02, kBulk = 0x03, kControl = 0x04, kEncrypted = 0x08;
constexpr int kMaxFramePayload = 0x4000;

struct Frame {
    quint8 channel = 0;
    quint8 flags = 0;
    QByteArray payload;       // still encrypted if kEncrypted is set
};

/// Cuts the incoming byte stream into frames.
class Deframer {
public:
    void feed(const QByteArray &bytes) { m_buf += bytes; }
    /// Next complete frame, false if more bytes are needed.
    bool next(Frame *out);
    void clear() { m_buf.clear(); }
    int pending() const { return m_buf.size(); }
private:
    QByteArray m_buf;
};

/// One frame ready to send.
QByteArray buildFrame(quint8 channel, quint8 flags, const QByteArray &payload, quint32 totalSize);

}   // namespace aa
