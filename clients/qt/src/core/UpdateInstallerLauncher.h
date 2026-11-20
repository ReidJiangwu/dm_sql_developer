#pragma once

#include <QByteArray>
#include <QString>

namespace reid::update {

struct VerifiedInstallerCandidate {
    QString path;
    quint64 byteSize = 0;
    QByteArray sha256;
};

class UpdateInstallerLauncher final
{
public:
    [[nodiscard]] static bool validate(
        const VerifiedInstallerCandidate &candidate, QString *error = nullptr);
    [[nodiscard]] static bool launch(
        const VerifiedInstallerCandidate &candidate, QString *error = nullptr);
};

} // namespace reid::update
