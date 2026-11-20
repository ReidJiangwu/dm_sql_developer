#pragma once

#include "CredentialStore.h"

#include <QEventLoop>
#include <QFutureWatcher>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>

#include <memory>
#include <utility>

namespace reid::security {

inline QThreadPool& keychainTaskPool() {
    static const auto pool = [] {
        auto value = std::make_unique<QThreadPool>();
        // QtKeychain's process-wide JobExecutor must retain one thread affinity,
        // including across idle periods and calls from different caller threads.
        value->setMaxThreadCount(1);
        value->setExpiryTimeout(-1);
        return value;
    }();
    return *pool;
}

template<typename Task>
CredentialResult runKeychainTask(Task task) {
    QEventLoop loop;
    QFutureWatcher<CredentialResult> watcher;
    QObject::connect(&watcher, &QFutureWatcher<CredentialResult>::finished,
        &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run(&keychainTaskPool(), std::move(task)));
    // The synchronous CredentialStore interface still processes caller events;
    // native credential APIs and QtKeychain jobs never execute on the GUI thread.
    if (!watcher.isFinished()) loop.exec();
    return watcher.result();
}

}  // namespace reid::security
