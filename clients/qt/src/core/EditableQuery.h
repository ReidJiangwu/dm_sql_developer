#pragma once

#include <QString>

struct EditableQueryTarget {
    bool forUpdate = false;
    bool structurallyEligible = false;
    QString schema;
    QString table;
    QString reason;
};

class EditableQuery final {
public:
    [[nodiscard]] static EditableQueryTarget analyze(
        const QString& sql, const QString& defaultSchema);
    [[nodiscard]] static QString withoutForUpdate(const QString& sql);
};
