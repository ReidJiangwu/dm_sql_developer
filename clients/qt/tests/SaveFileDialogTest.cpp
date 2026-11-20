#include "../src/ui/SaveFileDialog.h"

#include <QApplication>
#include <iostream>
#include <stdexcept>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        using namespace SaveFileDialog;
        require(extensions(QStringLiteral("SQL (*.sql *.pls *.pck);;All (*)"))
                == QStringList{QStringLiteral("sql"), QStringLiteral("pls"), QStringLiteral("pck")},
            "SQL script extensions were not preserved.");
        require(extensions(QStringLiteral("All (*.*)")).isEmpty()
                && extensions(QStringLiteral("All (*)")).isEmpty()
                && extensions(QString()).isEmpty(), "An unrestricted save became extension-restricted.");
        require(extensions(QStringLiteral("Archive (*.tar.gz *.zip *.zip)"))
                == QStringList{QStringLiteral("tar.gz"), QStringLiteral("zip")},
            "Compound or duplicate extensions were parsed incorrectly.");
        const QString path = QStringLiteral("/Users/fixture/中文目录/报价 'one' \"two\" $().sql");
        const auto selected = parseReply(QJsonDocument(QJsonObject{
            {QStringLiteral("status"), QStringLiteral("selected")}, {QStringLiteral("path"), path}
        }).toJson());
        require(selected.path == path && selected.error.isEmpty(),
            "A selected Unicode path did not survive the JSON protocol.");
        const auto cancelled = parseReply("{\"status\":\"cancelled\"}");
        require(cancelled.path.isEmpty() && cancelled.error.isEmpty(), "Cancel was treated as an error or save.");
        for (const auto& bytes : {QByteArray(), QByteArray("[]"), QByteArray("invalid"),
                QByteArray("{\"status\":\"error\"}"), QByteArray("{\"status\":\"selected\",\"path\":\"relative.sql\"}"),
                QByteArray("{\"status\":\"selected\",\"path\":\"/tmp/bad\\u0000.sql\"}")}) {
            const auto invalid = parseReply(bytes);
            require(invalid.path.isEmpty() && !invalid.error.isEmpty(), "An invalid native reply was accepted.");
        }
        std::cout << "SAVE_FILE_DIALOG_PROTOCOL=PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
