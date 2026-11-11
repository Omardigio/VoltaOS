# Android Auto head unit certificate

`headunit.crt` / `headunit.key` are the head-unit certificate and key that Android Auto requires for the
TLS handshake with the phone. They are the same files shipped, in the open, by the
[aasdk](https://github.com/opencardev/aasdk) library (GPLv3) and used by OpenAuto and other open
source head units. The certificate was issued by Google to a third-party vendor and has been public for
years; it is **not a secret** and **not Volta OS's own credential**.

Google may revoke it at any time, in which case native Android Auto would stop connecting until a
replacement certificate is available. The app looks for `headunit.crt` / `headunit.key` in
`~/.local/share/volta_os/` first, so a replacement can be dropped in without rebuilding.
