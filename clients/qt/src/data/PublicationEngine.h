#pragma once
#include "DpiSession.h"
#include "../core/PublicationTask.h"
#include "../core/ContextBootstrapTask.h"
#include <functional>
#include <thread>

namespace Publication {
// No widgets, profile mutation, automatic retries or destructive recovery.
class Engine final {
public:
    explicit Engine(std::function<bool()> cancelled = {});
    ~Engine();
    QJsonObject connect(const QString& id, const QString& schema, const QString& expected = {});
    QJsonObject inspect(const CommandCompiledObject& object);
    QJsonObject compile(const CommandCompiledObject& object);
    int bootstrapContexts(const ContextBootstrapTask& task, QJsonObject& result,
                          const std::function<void(QJsonObject)>& event, bool apply = true);
    int moduleObjects(const QString& module, const QString& type, bool compileInvalid,
                      QJsonObject& result, const std::function<void(QJsonObject)>& event);
    int publish(const Task& task, QJsonObject& result,
                const std::function<void(QJsonObject)>& event);
    DpiSession& session() { return session_; }
    QJsonObject stateFlags() const;
private:
    int publishSerial(const Task& task, QJsonObject& result,
                      const std::function<void(QJsonObject)>& event, bool reuseConnection = false);
    int publishBatch(const Task& task, QJsonObject& result,
                     const std::function<void(QJsonObject)>& event);
    struct Reply {
        bool success = false;
        bool finished = false;
        int code = 0;
        QString message;
        QString category;
        QString output;
        QJsonArray diagnostics;
        bool diagnosticsComplete = true;
        QVector<QStringList> rows;
    };
    Reply execute(const QString& sql);
    QJsonObject verify(const QString& expected);
    void checkCancelled();
    QJsonObject initializeFndSession();
    QJsonArray snapshots(const QVector<CommandCompiledObject>& objects);
    QJsonObject objectModule(const CommandCompiledObject& object);
    int finishObjects(const Task& task, QJsonObject& result,
                      const std::function<void(QJsonObject)>& event, int exit, bool compileAllowed);
    DpiSession session_;
    Reply reply_;
    bool more_ = false;
    bool connected_ = false;
    bool broken_ = false;
    bool uncertainCompile_ = false;
    bool timedOut_ = false;
    ConnectionEndpoint endpoint_;
    ConnectionProfile profile_;
    QString schema_;
    QString identity_;
    std::function<bool()> cancelled_;
    std::atomic_bool monitorDone_{false};
    std::thread monitor_;
};
}
