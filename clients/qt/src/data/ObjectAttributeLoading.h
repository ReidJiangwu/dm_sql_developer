#pragma once

#include "MetadataTypes.h"
#include <QHash>

// Optional enrichment never determines whether the base object list is usable.
// Show confirmed matches as each delta arrives. A complete batch range still
// requires a successful complete scan. With an attribute filter, unread/failed
// records never match.
class ObjectAttributeLoading {
public:
    explicit ObjectAttributeLoading(bool caseInsensitive = false) : folded_(caseInsensitive) {}
    void clear() { values_.clear(); errors_.clear(); loading = false; completed = total = 0; issue_.clear(); }
    void begin(int count) { clear(); total = count; loading = count > 0; }
    void progress(const QVector<InvalidObjectModule>& delta, int done, int count, const QString& error = {}) {
        if (done == 0 && delta.isEmpty()) { values_.clear(); errors_.clear(); }
        for (const auto& value : delta) {
            const auto identity = key(value.schema, value.name);
            if (value.error.isEmpty()) errors_.remove(identity);
            else errors_.insert(identity, value.error);
            values_.insert(identity, value);
        }
        completed = done; total = count; loading = done < count && error.isEmpty(); issue_ = error;
    }
    void finish(const QVector<InvalidObjectModule>& snapshot) {
        values_.clear();
        errors_.clear();
        progress(snapshot, total, total);
        loading = false;
    }
    InvalidObjectModule value(const QString& schema, const QString& name) const { return values_.value(key(schema, name)); }
    bool matches(const ObjectKey& object, const QString& moduleTerm, const QString& luNameTerm = {}) const {
        if (moduleTerm.isEmpty() && luNameTerm.isEmpty()) return true;
        const auto found = values_.constFind(key(object.schema, object.name));
        if (found == values_.cend() || !found->error.isEmpty()) return false;
        if (!found->module.contains(moduleTerm, Qt::CaseInsensitive)) return false;
        if (found->luName.contains(luNameTerm, Qt::CaseInsensitive)) return true;
        const QString compactTerm = compactLuName(luNameTerm);
        return !compactTerm.isEmpty()
            && compactLuName(found->luName).contains(compactTerm, Qt::CaseInsensitive);
    }
    bool ready() const { return !loading && issue_.isEmpty() && failures() == 0; }
    int failures() const { return errors_.size(); }
    QString error() const {
        if (!issue_.isEmpty()) return issue_;
        return errors_.isEmpty() ? QString() : errors_.cbegin().value();
    }
    QString cell(const ObjectKey& object, bool luName) const {
        const auto found = values_.constFind(key(object.schema, object.name));
        if (found == values_.cend()) {
            if (object.type != DatabaseObjectType::Package && object.type != DatabaseObjectType::PackageBody
                    && object.type != DatabaseObjectType::View) return QStringLiteral("-");
            return loading ? QStringLiteral("待补充") : ready() ? QStringLiteral("-") : QStringLiteral("未获取");
        }
        if (!found->error.isEmpty()) return QStringLiteral("读取失败");
        const QString text = luName ? found->luName : found->module;
        return text.isEmpty() ? QStringLiteral("-") : text;
    }
    QString summary(bool filterIncomplete) const {
        QString text;
        if (loading) text = QStringLiteral("模块与 LuName 后台补充 %1/%2（基础列表可用）").arg(completed).arg(total);
        else if (!ready()) text = failures() > 0
            ? QStringLiteral("%1 个对象的属性读取失败（基础列表可用）").arg(failures())
            : QStringLiteral("属性补充未完成（基础列表可用）");
        if (filterIncomplete) text += loading
            ? QStringLiteral("；按已读取属性筛选，结果待补齐")
            : QStringLiteral("；按成功读取的属性筛选，结果尚未完整");
        return text;
    }
    bool loading = false;
    int completed = 0, total = 0;
private:
    static QString compactLuName(const QString& text) {
        QString compact;
        compact.reserve(text.size());
        for (const QChar character : text)
            if (character != QLatin1Char('_') && !character.isSpace()) compact.append(character);
        return compact;
    }
    QString key(const QString& schema, const QString& name) const {
        const QString identity = schema + QLatin1Char('\n') + name;
        return folded_ ? identity.toUpper() : identity;
    }
    bool folded_ = false;
    QHash<QString, QString> errors_;
    QString issue_;
    QHash<QString, InvalidObjectModule> values_;
};
