#pragma once

#include "MetadataTypes.h"
#include <QHash>
#include <QSet>
#include <algorithm>

namespace PowerCompilePlan {
inline QString identity(const ObjectKey& key) {
    return key.connectionId + QChar(0) + key.schema + QChar(0)
        + QString::number(static_cast<int>(key.type)) + QChar(0) + key.name;
}
inline QString bodyIdentity(const ObjectKey& key) {
    return key.schema + QChar(0) + key.name;
}
struct Edge { ObjectKey dependent; ObjectKey prerequisite; };
using Group = QVector<ObjectKey>; // A dependency cycle is executed serially.
using Wave = QVector<Group>;     // Independent groups may run concurrently.
using Waves = QVector<Wave>;

// Iterative Kosaraju avoids overflowing the Windows stack on long dependency chains.
inline Waves waves(const QVector<ObjectKey>& targets, const QVector<Edge>& edges, bool known) {
    if (targets.isEmpty()) return {};
    if (!known) return {Wave{targets}};
    QHash<QString, int> indices;
    for (int index = 0; index < targets.size(); ++index) indices.insert(identity(targets.at(index)), index);
    QVector<QVector<int>> forward(targets.size()), reverse(targets.size());
    QSet<QString> seen;
    for (const auto& edge : edges) {
        const int from = indices.value(identity(edge.prerequisite), -1);
        const int to = indices.value(identity(edge.dependent), -1);
        if (from < 0 || to < 0 || from == to) continue;
        const QString pair = QString::number(from) + QLatin1Char(':') + QString::number(to);
        if (seen.contains(pair)) continue;
        seen.insert(pair);
        forward[from].append(to);
        reverse[to].append(from);
    }
    QVector<bool> visited(targets.size(), false);
    QVector<int> order;
    for (int start = 0; start < targets.size(); ++start) {
        if (visited.at(start)) continue;
        QVector<QPair<int, int>> stack{{start, 0}};
        visited[start] = true;
        while (!stack.isEmpty()) {
            const int node = stack.last().first;
            const int next = stack.last().second++;
            if (next < forward.at(node).size()) {
                const int child = forward.at(node).at(next);
                if (!visited.at(child)) { visited[child] = true; stack.append({child, 0}); }
            } else { order.append(node); stack.removeLast(); }
        }
    }
    QVector<int> component(targets.size(), -1);
    QVector<QVector<int>> members;
    for (auto it = order.crbegin(); it != order.crend(); ++it) {
        if (component.at(*it) >= 0) continue;
        const int id = members.size();
        QVector<int> group, stack{*it};
        component[*it] = id;
        while (!stack.isEmpty()) {
            const int node = stack.takeLast();
            group.append(node);
            for (int child : reverse.at(node)) if (component.at(child) < 0) {
                component[child] = id;
                stack.append(child);
            }
        }
        std::sort(group.begin(), group.end());
        members.append(std::move(group));
    }
    QVector<QSet<int>> successors(members.size());
    QVector<int> incoming(members.size(), 0);
    for (int from = 0; from < forward.size(); ++from) for (int to : forward.at(from)) {
        const int a = component.at(from), b = component.at(to);
        if (a != b && !successors[a].contains(b)) { successors[a].insert(b); ++incoming[b]; }
    }
    QVector<int> ready;
    for (int id = 0; id < members.size(); ++id) if (incoming.at(id) == 0) ready.append(id);
    Waves result;
    while (!ready.isEmpty()) {
        std::sort(ready.begin(), ready.end(), [&](int a, int b) {
            return members.at(a).first() < members.at(b).first();
        });
        Wave wave;
        QVector<int> next;
        for (int id : ready) {
            Group group;
            for (int index : members.at(id)) group.append(targets.at(index));
            wave.append(std::move(group));
            for (int successor : successors.at(id)) if (--incoming[successor] == 0) next.append(successor);
        }
        result.append(std::move(wave));
        ready = std::move(next);
    }
    return result;
}

inline bool dependencyFailure(const ProgramCompileResult& result) {
    if (result.succeeded) return false;
    if (result.code == -3325) return true;
    return std::any_of(result.diagnostics.cbegin(), result.diagnostics.cend(),
        [](const ProgramDiagnostic& diagnostic) { return diagnostic.code == -3325; });
}
}
