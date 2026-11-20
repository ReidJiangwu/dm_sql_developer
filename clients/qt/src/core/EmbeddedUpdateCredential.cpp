#include "EmbeddedUpdateCredential.h"

#include "EmbeddedDownloadKeyData.h"

namespace reid::update {

QByteArray embeddedDownloadKey()
{
    QByteArray key(static_cast<qsizetype>(detail::kEncodedDownloadKey.size()),
                   Qt::Uninitialized);
    const volatile unsigned char *encoded = detail::kEncodedDownloadKey.data();
    for (qsizetype index = 0; index < key.size(); ++index) {
        const unsigned char mask = static_cast<unsigned char>(
            (0xA7u + static_cast<unsigned int>(index) * 73u) & 0xFFu);
        key[index] = static_cast<char>(encoded[index] ^ mask);
    }
    return key;
}

} // namespace reid::update
