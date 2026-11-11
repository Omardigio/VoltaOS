#include "AaFraming.h"
#include <QtEndian>

namespace aa {

bool Deframer::next(Frame *out)
{
    if (m_buf.size() < 4) return false;
    const quint8 flags = quint8(m_buf[1]);
    const bool extended = (flags & kBulk) == kFirst;           // FIRST but not LAST
    const int header = extended ? 8 : 4;
    if (m_buf.size() < header) return false;
    const int size = qFromBigEndian<quint16>(m_buf.constData() + 2);
    if (m_buf.size() < header + size) return false;
    out->channel = quint8(m_buf[0]);
    out->flags = flags;
    out->payload = m_buf.mid(header, size);
    m_buf.remove(0, header + size);
    return true;
}

QByteArray buildFrame(quint8 channel, quint8 flags, const QByteArray &payload, quint32 totalSize)
{
    QByteArray f;
    f.reserve(payload.size() + 8);
    f.append(char(channel));
    f.append(char(flags));
    char sz[2];
    qToBigEndian<quint16>(quint16(payload.size()), sz);
    f.append(sz, 2);
    if ((flags & kBulk) == kFirst) {
        char tot[4];
        qToBigEndian<quint32>(totalSize, tot);
        f.append(tot, 4);
    }
    f.append(payload);
    return f;
}

}   // namespace aa
