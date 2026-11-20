#include "PublicationSchedule.h"
#include "CommandParallelPlan.h"
#include "SqlSafetyAnalyzer.h"
#include <algorithm>

namespace Publication {
BatchPlan planBatch(const Task& task) {
    BatchPlan plan;
    bool output = task.initialServerOutput;
    for (int index = 0; index < task.units.size(); ++index) {
        const auto& unit = task.units[index];
        if (plan.jobs.isEmpty() || index == 0
                || unit.source.file != task.units[index - 1].source.file
                || unit.source.includeChain != task.units[index - 1].source.includeChain) {
            FileJob job;
            job.initialServerOutput = output;
            plan.jobs.append(job);
        }
        auto& job = plan.jobs.last();
        job.units.append(index);
        job.locks.insert(QStringLiteral("file:") + unit.source.file.toCaseFolded());
        if (unit.type == CommandUnitType::SetServerOutput)
            output = unit.argument.compare(QStringLiteral("ON"), Qt::CaseInsensitive) == 0;
        if (unit.type != CommandUnitType::Sql) continue;
        plan.requiresSameSession |= CommandParallelPlan::requiresSameSession(unit.text);
        const auto object = CommandScript::compiledObject(unit.text, task.schema);
        if (object) {
            // Spec/body and repeated publications of the same object serialize.
            job.locks.insert(QStringLiteral("object:") + object->owner + '.' + object->name);
        } else if (!SqlSafetyAnalyzer::isReadOnly(unit.text)) {
            // Unknown write targets cannot safely acquire an object-specific lock.
            job.exclusive = true;
        }
    }
    plan.workers = plan.requiresSameSession ? 1
        : std::max(1, std::min({task.parallelism, maximumFileParallelism, static_cast<int>(plan.jobs.size())}));
    return plan;
}
Schedule::Schedule(BatchPlan plan) : plan_(std::move(plan)), states_(plan_.jobs.size(), 0) {}
bool Schedule::conflicts(int left, int right) const {
    const auto& a = plan_.jobs[left];
    const auto& b = plan_.jobs[right];
    if (a.exclusive || b.exclusive) return true;
    for (const auto& key : a.locks) if (b.locks.contains(key)) return true;
    return false;
}
int Schedule::takeReady() {
    if (stopped_ || active_ >= plan_.workers) return -1;
    for (int index = 0; index < states_.size(); ++index) {
        if (states_[index] != 0) continue;
        bool blocked = false;
        for (int other = 0; other < states_.size(); ++other) {
            if (other == index || states_[other] == 2) continue;
            if ((states_[other] == 1 || other < index) && conflicts(index, other)) {
                blocked = true;
                break;
            }
        }
        if (blocked) continue;
        states_[index] = 1;
        ++active_;
        return index;
    }
    return -1;
}
void Schedule::complete(int job) {
    if (job < 0 || job >= states_.size() || states_[job] != 1)
        throw std::logic_error("Invalid publication job completion");
    states_[job] = 2;
    --active_;
}
}
