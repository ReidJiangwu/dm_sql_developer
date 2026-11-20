#pragma once
#include "PublicationTask.h"
#include <QSet>

namespace Publication {
struct FileJob {
    QVector<int> units; // zero-based positions in the immutable parent task
    QSet<QString> locks;
    bool exclusive = false;
    bool initialServerOutput = false;
};
struct BatchPlan {
    QVector<FileJob> jobs;
    bool requiresSameSession = false;
    int workers = 1;
};
BatchPlan planBatch(const Task& task);

// Deterministic, single-coordinator scheduling. A later conflicting job never
// overtakes an earlier pending job, even when an unrelated worker becomes free.
class Schedule final {
public:
    explicit Schedule(BatchPlan plan);
    int takeReady();
    void complete(int job);
    void stop() { stopped_ = true; }
    int active() const { return active_; }
    const BatchPlan& plan() const { return plan_; }
private:
    bool conflicts(int left, int right) const;
    BatchPlan plan_;
    QVector<int> states_; // 0 pending, 1 active, 2 completed
    int active_ = 0;
    bool stopped_ = false;
};
}
