#pragma once
// The head unit certificate Android Auto needs (see third_party/aap_cert/README.md).
#include <QByteArray>
#include <QString>

/// A replacement headunit.crt / headunit.key in `dataDir` wins; otherwise the built-in pair.
bool aaLoadCertificate(QByteArray *cert, QByteArray *key, const QString &dataDir, QString *error);
