#pragma once

#include <QByteArray>
#include <QString>

enum class CredentialStatus {
    Success,
    NotFound,
    Unavailable,
    Error,
};

struct CredentialResult {
    CredentialStatus status = CredentialStatus::Error;
    QByteArray password;
    QString message;

    [[nodiscard]] bool succeeded() const {
        return status == CredentialStatus::Success;
    }
};

class CredentialStore {
public:
    virtual ~CredentialStore() = default;

    [[nodiscard]] virtual bool isAvailable(QString* reason = nullptr) const = 0;
    [[nodiscard]] virtual CredentialResult read(const QString& profileId) = 0;
    [[nodiscard]] virtual CredentialResult write(
        const QString& profileId, QByteArray password) = 0;
    [[nodiscard]] virtual CredentialResult remove(const QString& profileId) = 0;

    [[nodiscard]] static CredentialStore& systemStore();
    [[nodiscard]] static bool purgeApplicationCredentials(QString* error = nullptr);
    [[nodiscard]] static bool ownsCredentialTarget(const QString& target);
};
