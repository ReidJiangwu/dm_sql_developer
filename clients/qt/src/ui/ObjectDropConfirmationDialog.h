#pragma once
#include "../core/ConnectionProfile.h"
#include "../data/MetadataTypes.h"
#include <QDialog>

class ObjectDropConfirmationDialog final : public QDialog {
    Q_OBJECT
public:
    ObjectDropConfirmationDialog(const ConnectionProfile& profile,
        const QVector<ObjectKey>& keys, QWidget* parent = nullptr, bool stash = false, bool fromEditor = false);
};
