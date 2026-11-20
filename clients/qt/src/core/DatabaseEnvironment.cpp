#include "DatabaseEnvironment.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <utility>

namespace {
QString cell(const DbaManagementResult& result, const QStringList& row, const QString& name) {
    const int index = result.columns.indexOf(name);
    return index < 0 ? QString() : row.value(index).trimmed();
}

EnvironmentSeverity severityFromKey(const QString& key) {
    if (key == QStringLiteral("critical")) return EnvironmentSeverity::Critical;
    if (key == QStringLiteral("warning")) return EnvironmentSeverity::Warning;
    return EnvironmentSeverity::Info;
}
}

bool EnvironmentParameter::isDefault() const {
    return !defaultValue.isEmpty() && currentValue == defaultValue;
}

bool EnvironmentParameter::pendingRestart() const {
    return !fileValue.isEmpty() && !currentValue.isEmpty() && fileValue != currentValue;
}

QString EnvironmentParameter::displayValue() const {
    return DatabaseEnvironment::formatValue(metadata.format, currentValue);
}

QString EnvironmentParameter::statusLabel() const {
    if (currentValue.isEmpty()) return QStringLiteral("未知");
    if (pendingRestart()) return QStringLiteral("待生效");
    if (isDefault()) return QStringLiteral("默认值");
    return QStringLiteral("已生效");
}

QVector<ParameterMetadata> DatabaseEnvironment::loadMetadata(QString* error) {
    QFile file(QStringLiteral(":/database-environment/parameters.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = QStringLiteral("无法读取参数元数据资源");
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        if (error != nullptr) *error = QStringLiteral("参数元数据格式错误：%1").arg(parseError.errorString());
        return {};
    }
    QVector<ParameterMetadata> result;
    for (const QJsonValue& value : document.array()) {
        const QJsonObject object = value.toObject();
        result.append({object.value(QStringLiteral("name")).toString(),
            object.value(QStringLiteral("title")).toString(),
            object.value(QStringLiteral("category")).toString(),
            object.value(QStringLiteral("format")).toString(),
            object.value(QStringLiteral("changePolicy")).toString(),
            severityFromKey(object.value(QStringLiteral("severity")).toString()),
            object.value(QStringLiteral("description")).toString(),
            object.value(QStringLiteral("details")).toString(),
            object.value(QStringLiteral("recommendation")).toString(),
            object.value(QStringLiteral("source")).toString(),
            object.value(QStringLiteral("modification")).toString(),
            object.value(QStringLiteral("modificationTemplate")).toString()});
        auto& item = result.last();
        item.valueKind = object.value(QStringLiteral("valueKind")).toString();
        item.defaultWatched = object.value(QStringLiteral("defaultWatched")).toBool();
        for (const auto& option : object.value(QStringLiteral("choices")).toArray()) {
            item.choices.append(option.toObject().value(QStringLiteral("value")).toString());
            item.choiceLabels.append(option.toObject().value(QStringLiteral("label")).toString());
        }
    }
    return result;
}

QString DatabaseEnvironment::canonicalParameterName(const QString& name) {
    const QString key = name.trimmed().toUpper();
    return key == QStringLiteral("CHARSET") ? QStringLiteral("UNICODE_FLAG") : key;
}

QStringList DatabaseEnvironment::watchedParameters(const QStringList& saved,
        const QVector<ParameterMetadata>& metadata, bool addDefaults) {
    QStringList names;
    const auto append = [&](const QString& name) {
        const auto key = canonicalParameterName(name);
        if (!key.isEmpty() && !names.contains(key)) names.append(key);
    };
    for (const auto& name : saved) append(name);
    if (addDefaults) for (const auto& item : metadata) if (item.defaultWatched) append(item.name);
    return names;
}

QVector<EnvironmentParameter> DatabaseEnvironment::snapshot(
        const DbaManagementResult& result, const QVector<ParameterMetadata>& metadata) {
    QMap<QString, ParameterMetadata> byName;
    for (const ParameterMetadata& item : metadata) byName.insert(item.name, item);
    QVector<EnvironmentParameter> parameters;
    QSet<QString> seen;
    parameters.reserve(result.rows.size());
    for (const QStringList& row : result.rows) {
        const QString name = canonicalParameterName(cell(result, row, QStringLiteral("PARA_NAME")));
        if (seen.contains(name)) continue;
        seen.insert(name);
        ParameterMetadata item = byName.value(name);
        if (item.name.isEmpty()) {
            item.name = name;
            item.title = name;
            item.category = moduleLabel(cell(result, row, QStringLiteral("PARA_MODULE")));
            item.format = QStringLiteral("text");
            item.changePolicy = QStringLiteral("unknown");
            item.description = cell(result, row, QStringLiteral("DESCRIPTION"));
        }
        parameters.append({item, name,
            cell(result, row, QStringLiteral("PARA_VALUE")),
            cell(result, row, QStringLiteral("SESS_VALUE")),
            cell(result, row, QStringLiteral("FILE_VALUE")),
            cell(result, row, QStringLiteral("DEFAULT_VALUE")),
            cell(result, row, QStringLiteral("MIN_VALUE")),
            cell(result, row, QStringLiteral("MAX_VALUE")),
            cell(result, row, QStringLiteral("PARA_TYPE")),
            cell(result, row, QStringLiteral("PARA_MODULE")),
            cell(result, row, QStringLiteral("DESCRIPTION")), byName.contains(name),
            cell(result, row, QStringLiteral("PRIMARY_SYNC")).toUpper()});
    }
    const DatabaseBuildInfo info = buildInfo(result);
    const auto mergeBuildAttribute = [&parameters, &byName](
            const QString& name, const QString& value) {
        if (value.isEmpty() || !byName.contains(name)) return;
        for (EnvironmentParameter& parameter : parameters) {
            if (parameter.name != name) continue;
            parameter.metadata = byName.value(name);
            parameter.currentValue = value;
            parameter.common = true;
            return;
        }
        const ParameterMetadata item = byName.value(name);
        parameters.append({item, name, value, {}, {}, {}, {}, {},
            QStringLiteral("READ ONLY"), QStringLiteral("DATABASE"),
            item.description, true});
    };
    mergeBuildAttribute(QStringLiteral("UNICODE_FLAG"), info.unicodeFlag);
    mergeBuildAttribute(QStringLiteral("CASE_SENSITIVE"), info.caseSensitive);
    mergeBuildAttribute(QStringLiteral("PAGE_SIZE"), info.pageSize);
    mergeBuildAttribute(QStringLiteral("EXTENT_SIZE"), info.extentSize);
    return parameters;
}

DatabaseBuildInfo DatabaseEnvironment::buildInfo(const DbaManagementResult& result) {
    if (result.rows.isEmpty()) return {};
    const QStringList& row = result.rows.first();
    return {cell(result, row, QStringLiteral("DB_UNICODE_FLAG")),
        cell(result, row, QStringLiteral("DB_CASE_SENSITIVE")),
        cell(result, row, QStringLiteral("DB_PAGE_SIZE")),
        cell(result, row, QStringLiteral("DB_EXTENT_SIZE")),
        cell(result, row, QStringLiteral("DB_ARCH_MODE"))};
}

QString DatabaseEnvironment::moduleLabel(const QString& value) {
    const QString module = value.trimmed();
    const QString key = module.toLower();
    static const QMap<QString, QString> labels = {
        {QStringLiteral("files location"), QStringLiteral("文件位置")},
        {QStringLiteral("instance name"), QStringLiteral("实例名称")},
        {QStringLiteral("memory pool and buffer"), QStringLiteral("内存池与缓冲区")},
        {QStringLiteral("thread"), QStringLiteral("线程")},
        {QStringLiteral("query"), QStringLiteral("查询")},
        {QStringLiteral("select rate"), QStringLiteral("查询限流")},
        {QStringLiteral("cpu"), QStringLiteral("CPU")},
        {QStringLiteral("checkpoint"), QStringLiteral("检查点")},
        {QStringLiteral("io"), QStringLiteral("I/O")},
        {QStringLiteral("database"), QStringLiteral("数据库")},
        {QStringLiteral("pre-load table"), QStringLiteral("预加载表")},
        {QStringLiteral("redo log"), QStringLiteral("重做日志")},
        {QStringLiteral("redo"), QStringLiteral("重做")},
        {QStringLiteral("transaction"), QStringLiteral("事务")},
        {QStringLiteral("security"), QStringLiteral("安全")},
        {QStringLiteral("security2"), QStringLiteral("安全（二）")},
        {QStringLiteral("compatibility"), QStringLiteral("兼容性")},
        {QStringLiteral("drop table"), QStringLiteral("表删除")},
        {QStringLiteral("fast insert"), QStringLiteral("快速插入")},
        {QStringLiteral("request trace"), QStringLiteral("请求跟踪")},
        {QStringLiteral("system trace"), QStringLiteral("系统跟踪")},
        {QStringLiteral("ecs"), QStringLiteral("ECS")},
        {QStringLiteral("monitor"), QStringLiteral("监控")},
        {QStringLiteral("data watch"), QStringLiteral("数据守护")},
        {QStringLiteral("for test only"), QStringLiteral("仅测试")},
        {QStringLiteral("for context index"), QStringLiteral("全文索引")},
        {QStringLiteral("rdma"), QStringLiteral("RDMA")},
        {QStringLiteral("configuration file"), QStringLiteral("配置文件")},
        {QStringLiteral("dsc"), QStringLiteral("DSC")},
        {QStringLiteral("mem only"), QStringLiteral("仅内存")},
        {QStringLiteral("backup and restore"), QStringLiteral("备份与还原")}
    };
    if (module.isEmpty()) return QStringLiteral("未分类");
    return labels.value(key, module);
}

QVector<EnvironmentDifference> DatabaseEnvironment::compare(
        const QVector<EnvironmentParameter>& left,
        const QVector<EnvironmentParameter>& right, bool primaryStandby) {
    QMap<QString, EnvironmentParameter> rightByName;
    for (const EnvironmentParameter& item : right) rightByName.insert(item.name, item);
    QVector<EnvironmentDifference> differences;
    const auto append = [&](const EnvironmentParameter& item, const EnvironmentParameter* other, bool leftPresent) {
        EnvironmentDifference difference;
        difference.name = item.name; difference.title = item.metadata.title;
        difference.leftValue = leftPresent ? item.displayValue() : QStringLiteral("未返回");
        difference.rightValue = other ? other->displayValue() : QStringLiteral("未返回");
        if (!leftPresent) difference.rightValue = item.displayValue();
        difference.policy = effectiveChangePolicyLabel(item);
        difference.severity = item.metadata.severity;
        difference.description = parameterSummary(item);
        difference.details = parameterDetails(item);
        difference.details += QStringLiteral("\n\n当前侧系统值：%1；目标侧系统值：%2。")
            .arg(difference.leftValue, difference.rightValue);
        if (!leftPresent) difference.details += QStringLiteral("\n当前侧未返回此项；本行说明来自目标侧，不能直接用于修改当前实例。");
        difference.synchronization = synchronizationDescription(item);
        difference.parameter = item;
        difference.judgement = !leftPresent || !other ? QStringLiteral("一侧未返回，不等于参数不受支持")
            : item.currentValue == other->currentValue ? QStringLiteral("当前值一致") : QStringLiteral("当前值不同");
        if (primaryStandby && other) {
            if (item.fileValue != other->fileValue) difference.judgement += QStringLiteral("；文件值不同");
            if (replaySynchronized(item.name) && item.currentValue != other->currentValue)
                difference.judgement += QStringLiteral("；核对日志重演与同步状态");
            difference.details += QStringLiteral("\n\n当前侧原始系统值：%1；文件值：%2\n目标侧原始系统值：%3；文件值：%4\n"
                "主备规则：%5\n静态参数可能只同步文件、等待重启；会话值不参与一致性判定。跨版本同步受限制，差异不代表自动故障结论。")
                .arg(item.currentValue, item.fileValue, other->currentValue, other->fileValue, difference.synchronization);
        }
        differences.append(std::move(difference));
    };
    for (const EnvironmentParameter& item : left) {
        if (!rightByName.contains(item.name)) {
            append(item, nullptr, true);
            continue;
        }
        const EnvironmentParameter other = rightByName.value(item.name);
        if (item.currentValue != other.currentValue || (primaryStandby
                && (item.fileValue != other.fileValue || replaySynchronized(item.name)))) {
            append(item, &other, true);
        }
        rightByName.remove(item.name);
    }
    for (const EnvironmentParameter& item : rightByName) {
        append(item, nullptr, false);
    }
    return differences;
}

QString DatabaseEnvironment::parameterSummary(const EnvironmentParameter& parameter) {
    const QString title = parameter.metadata.title == parameter.name ? QString() : parameter.metadata.title.trimmed();
    const QString summary = !parameter.metadata.description.trimmed().isEmpty()
        ? parameter.metadata.description.trimmed() : parameter.description.trimmed();
    if (!summary.isEmpty() && summary != parameter.name)
        return title.isEmpty() || summary.startsWith(title) ? summary : title + QStringLiteral("：") + summary;
    return title.isEmpty() ? QStringLiteral("服务端未提供详细说明，请查看本版本参数文档") : title;
}

QString DatabaseEnvironment::parameterDetails(const EnvironmentParameter& parameter) {
    QString text = parameterSummary(parameter);
    for (const QString& detail : {parameter.metadata.details, parameter.description, parameter.metadata.recommendation})
        if (!detail.trimmed().isEmpty() && !text.contains(detail.trimmed())) text += QStringLiteral("\n\n") + detail.trimmed();
    text += QStringLiteral("\n\n变更方式：%1\n主备同步：%2\n取值范围：%3 ～ %4\n来源：%5")
        .arg(effectiveChangePolicyLabel(parameter), synchronizationDescription(parameter),
            parameter.minimumValue.isEmpty() ? QStringLiteral("未返回") : parameter.minimumValue,
            parameter.maximumValue.isEmpty() ? QStringLiteral("未返回") : parameter.maximumValue,
            parameterSourceDescription(parameter));
    return text;
}

bool DatabaseEnvironment::replaySynchronized(const QString& name) {
    return QStringList{QStringLiteral("ENABLE_AUDIT"), QStringLiteral("PWD_POLICY"), QStringLiteral("PWD_MIN_LEN"),
        QStringLiteral("MAC_LABEL_OPTION"), QStringLiteral("ENABLE_DDL_ANY_PRIV")}.contains(name.toUpper());
}

QString DatabaseEnvironment::synchronizationDescription(const EnvironmentParameter& parameter) {
    if (replaySynchronized(parameter.name))
        return QStringLiteral("特殊日志重演同步：主库修改后备库随重演调整，不受 INI_SYNC 开关控制");
    if (parameter.primarySync.compare(QStringLiteral("Y"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("普通主备同步：由 PRIMARY_SYNC 标识，受 INI_SYNC、参数类型和版本限制");
    if (parameter.primarySync.compare(QStringLiteral("N"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("当前版本 PRIMARY_SYNC=N，不参与普通主备 INI 同步");
    return QStringLiteral("当前版本未返回 PRIMARY_SYNC，普通同步属性未知；不能按参数名推断");
}

QString DatabaseEnvironment::editBlockReason(const EnvironmentParameter& parameter) {
    const QString type = parameter.parameterType.trimmed().toUpper();
    if (parameter.metadata.changePolicy == QStringLiteral("recreate") || parameter.metadata.changePolicy == QStringLiteral("external")
            || type.contains(QStringLiteral("READ ONLY"))) return QStringLiteral("该参数需手工、建库或专用流程修改，不能通过通用参数修改入口执行。");
    if (type != QStringLiteral("SYS") && type != QStringLiteral("SESSION") && type != QStringLiteral("IN FILE"))
        return QStringLiteral("服务端未返回受支持的 PARA_TYPE，请刷新并核对当前版本。");
    if (!QRegularExpression(QStringLiteral("^[A-Z][A-Z0-9_]{0,127}$")).match(parameter.name).hasMatch())
        return QStringLiteral("参数名无效。");
    return {};
}

QString DatabaseEnvironment::valueKind(const EnvironmentParameter& parameter) {
    if (!parameter.metadata.valueKind.isEmpty()) return parameter.metadata.valueKind;
    if (!parameter.metadata.choices.isEmpty() || parameter.metadata.format == QStringLiteral("boolean")) return QStringLiteral("enum");
    if (parameter.metadata.format == QStringLiteral("path")) return QStringLiteral("text");
    if (parameter.minimumValue == QStringLiteral("0") && parameter.maximumValue == QStringLiteral("1")) return QStringLiteral("enum");
    if (QRegularExpression(QStringLiteral("^[+-]?[0-9]+$")).match(parameter.currentValue).hasMatch()
            && !parameter.minimumValue.isEmpty() && !parameter.maximumValue.isEmpty()) return QStringLiteral("integer");
    if (QRegularExpression(QStringLiteral("^[+-]?[0-9]+\\.[0-9]+$")).match(parameter.currentValue).hasMatch()
            && !parameter.minimumValue.isEmpty() && !parameter.maximumValue.isEmpty()) return QStringLiteral("decimal");
    return QStringLiteral("text");
}

QString DatabaseEnvironment::validateParameterValue(const EnvironmentParameter& parameter, const QString& value) {
    if (value.contains(QChar(0)) || value.contains(u'\n') || value.contains(u'\r') || value.size() > 8187)
        return QStringLiteral("参数值不能含换行、空字符或超过 8187 个字符。");
    const auto kind = valueKind(parameter);
    if (kind == QStringLiteral("enum")) {
        const QStringList choices = parameter.metadata.choices.isEmpty() ? QStringList{QStringLiteral("0"), QStringLiteral("1")} : parameter.metadata.choices;
        if (!choices.contains(value)) return QStringLiteral("请从该参数支持的固定选项中选择。");
    }
    const bool numericEnum = kind == QStringLiteral("enum")
        && QRegularExpression(QStringLiteral("^[+-]?[0-9]+$")).match(value).hasMatch();
    if (kind == QStringLiteral("integer") || numericEnum) {
        bool valid = false; const qlonglong number = value.toLongLong(&valid);
        if (!valid || !QRegularExpression(QStringLiteral("^[+-]?[0-9]+$")).match(value).hasMatch()) return QStringLiteral("请输入有效的 64 位整数。");
        bool minOk = false, maxOk = false;
        const auto minimum = parameter.minimumValue.toLongLong(&minOk), maximum = parameter.maximumValue.toLongLong(&maxOk);
        if ((minOk && number < minimum) || (maxOk && number > maximum)) return QStringLiteral("数值超出服务端返回的范围。");
    } else if (kind == QStringLiteral("decimal")) {
        bool valid = false; const double number = value.toDouble(&valid);
        if (!valid || !std::isfinite(number) || !QRegularExpression(QStringLiteral("^[+-]?[0-9]+(?:\\.[0-9]+)?$")).match(value).hasMatch()) return QStringLiteral("请输入有限十进制数值，使用小数点，不使用千分位。");
        bool minOk = false, maxOk = false;
        const auto minimum = parameter.minimumValue.toDouble(&minOk), maximum = parameter.maximumValue.toDouble(&maxOk);
        if ((minOk && number < minimum) || (maxOk && number > maximum)) return QStringLiteral("数值超出服务端返回的范围。");
    }
    return {};
}

QString DatabaseEnvironment::parameterChangeSql(const EnvironmentParameter& parameter, const QString& value,
        int scope, const QString& instanceName, const QString& instanceMode, const QString& oguid, QString* error) {
    QString failure = editBlockReason(parameter);
    if (failure.isEmpty()) failure = validateParameterValue(parameter, value);
    const QString type = parameter.parameterType.trimmed().toUpper();
    if (failure.isEmpty() && (scope < 0 || scope > 2 || (type == QStringLiteral("IN FILE") && scope != 2))) failure = QStringLiteral("所选作用范围不适用于该参数。");
    if (failure.isEmpty() && (instanceName.isEmpty() || instanceMode.isEmpty() || oguid.isEmpty())) failure = QStringLiteral("实例身份不完整，不能修改参数。");
    if (failure.isEmpty() && (replaySynchronized(parameter.name) || parameter.primarySync.toUpper() == QStringLiteral("Y"))
            && instanceMode.toUpper() == QStringLiteral("STANDBY")) failure = QStringLiteral("该参数参与主备同步，请连接主库修改，不能直接覆盖备库。");
    if (failure.isEmpty() && replaySynchronized(parameter.name) && instanceMode.toUpper() != QStringLiteral("PRIMARY")
            && instanceMode.toUpper() != QStringLiteral("NORMAL")) failure = QStringLiteral("必须先确认当前实例为主库或普通实例。");
    if (error) *error = failure;
    if (!failure.isEmpty()) return {};
    const auto quote = [](QString text) -> QString { return u'\'' + text.replace(u'\'', QStringLiteral("''")) + u'\''; };
    return QStringLiteral("DECLARE N INTEGER; BEGIN\n"
        " SELECT COUNT(*) INTO N FROM V$INSTANCE WHERE INSTANCE_NAME=%1 AND MODE$=%2 AND TO_CHAR(OGUID)=%3;\n"
        " IF N<>1 THEN RAISE_APPLICATION_ERROR(-20001,'Instance changed; refresh before modifying parameters'); END IF;\n"
        " SELECT COUNT(*) INTO N FROM V$DM_INI WHERE PARA_NAME=%4 AND PARA_TYPE=%5 AND COALESCE(NULLIF(PARA_VALUE,''),CHR(1))=COALESCE(NULLIF(%6,''),CHR(1)) AND COALESCE(NULLIF(FILE_VALUE,''),CHR(1))=COALESCE(NULLIF(%7,''),CHR(1));\n"
        " IF N<>1 THEN RAISE_APPLICATION_ERROR(-20002,'Parameter changed; refresh before modifying'); END IF;\n"
        " SP_SET_INI_PARA_VALUE(%8,%4,%9);\nEND;")
        .arg(quote(instanceName), quote(instanceMode), quote(oguid), quote(parameter.name), quote(parameter.parameterType),
            quote(parameter.currentValue), quote(parameter.fileValue), QString::number(scope), quote(value));
}

QString DatabaseEnvironment::severityLabel(EnvironmentSeverity severity) {
    switch (severity) {
    case EnvironmentSeverity::Critical: return QStringLiteral("严重");
    case EnvironmentSeverity::Warning: return QStringLiteral("警告");
    case EnvironmentSeverity::Info: return QStringLiteral("提示");
    }
    return QStringLiteral("提示");
}

QString DatabaseEnvironment::changePolicyLabel(const QString& policy) {
    if (policy == QStringLiteral("dynamic")) return QStringLiteral("动态生效");
    if (policy == QStringLiteral("session")) return QStringLiteral("会话级");
    if (policy == QStringLiteral("restart")) return QStringLiteral("重启生效");
    if (policy == QStringLiteral("recreate")) return QStringLiteral("重建数据库");
    if (policy == QStringLiteral("external")) return QStringLiteral("源库参数");
    return QStringLiteral("以数据库说明为准");
}

QString DatabaseEnvironment::effectiveChangePolicyLabel(
        const EnvironmentParameter& parameter) {
    if (parameter.metadata.changePolicy == QStringLiteral("external")) {
        return changePolicyLabel(parameter.metadata.changePolicy);
    }
    if (parameter.metadata.changePolicy == QStringLiteral("recreate")) {
        return changePolicyLabel(parameter.metadata.changePolicy);
    }
    const QString type = parameter.parameterType.trimmed().toUpper();
    if (type.contains(QStringLiteral("SESSION"))) return QStringLiteral("会话级");
    if (type.contains(QStringLiteral("SYS"))) return QStringLiteral("动态系统级");
    if (type.contains(QStringLiteral("IN FILE"))) return QStringLiteral("文件值／重启生效");
    if (type.contains(QStringLiteral("READ ONLY"))) return QStringLiteral("只读／专用方式");
    return changePolicyLabel(parameter.metadata.changePolicy);
}

QString DatabaseEnvironment::parameterSourceDescription(
        const EnvironmentParameter& parameter) {
    QStringList lines;
    if (!parameter.metadata.source.trimmed().isEmpty()) {
        lines.append(parameter.metadata.source.trimmed());
    }
    lines.append(QStringLiteral(
        "客户端采集：V$DM_INI.PARA_VALUE（当前系统值）、SESS_VALUE（当前会话值）、"
        "FILE_VALUE（dm.ini 文件值）、DEFAULT_VALUE（版本默认值）。"));
    lines.append(QStringLiteral(
        "参数说明：优先 V$DM_INI_DESC.DESCRIPTION，视图不可用时回退 "
        "V$DM_INI.DESCRIPTION。"));
    lines.append(QStringLiteral("普通主备同步属性来自 V$DM_INI.PRIMARY_SYNC；特殊重演项参见 DM8 系统管理员手册“主备环境 INI 同步规则”。旧版未提供字段时显示未知。"));
    if (!parameter.module.trimmed().isEmpty()) {
        lines.append(QStringLiteral("数据库模块：V$DM_INI.PARA_MODULE = %1。")
            .arg(parameter.module.trimmed()));
    }
    lines.append(QStringLiteral(
        "说明：V$DM_INI 不返回 dm.ini 的绝对路径；实际文件位置由数据库实例目录决定。"));
    return lines.join(QLatin1Char('\n'));
}

QString DatabaseEnvironment::parameterModificationDescription(
        const EnvironmentParameter& parameter) {
    if (!parameter.metadata.modification.trimmed().isEmpty()) {
        return parameter.metadata.modification.trimmed();
    }
    const QString type = parameter.parameterType.trimmed().toUpper();
    const QString policy = parameter.metadata.changePolicy.trimmed().toLower();
    if (policy == QStringLiteral("recreate")) {
        return QStringLiteral(
            "该项是建库属性，不能通过通用 INI 参数过程在线修改；需要新建数据库或按参数"
            "专用迁移方案处理。修改前必须备份并验证数据迁移路径。");
    }
    if (type.contains(QStringLiteral("SESSION")) || policy == QStringLiteral("session")) {
        return QStringLiteral(
            "会话级参数：可只修改当前会话；如需改变系统默认值，再按当前版本支持情况写入"
            "系统配置。已有其他会话通常不会自动继承新值。");
    }
    if (type.contains(QStringLiteral("SYS")) || policy == QStringLiteral("dynamic")) {
        return QStringLiteral(
            "动态系统参数：使用 scope=1 同时更新内存与 dm.ini，通常立即影响当前会话和"
            "后续新会话；其他已建立会话是否继承应以参数类型和目标版本为准。");
    }
    if (type.contains(QStringLiteral("READ ONLY"))) {
        return QStringLiteral(
            "只读参数：客户端只提供来源和值的核查 SQL，不生成通用修改语句。应查阅当前"
            "服务端版本的参数专用说明；需要重建数据库或使用专用过程时，按独立变更执行。");
    }
    if (type.contains(QStringLiteral("IN FILE")) || policy == QStringLiteral("restart")) {
        return QStringLiteral(
            "文件／静态参数：使用 scope=2 只写入 dm.ini，重启实例后生效。当前版本拒绝"
            "过程修改时，应停止操作并按官方配置说明在停库、备份后修改文件。");
    }
    return QStringLiteral(
        "当前数据库没有返回可确定的修改类型。先核对 PARA_TYPE、取值范围和目标版本文档，"
        "不要直接套用动态或静态参数模板。");
}

QString DatabaseEnvironment::parameterModificationTemplate(
        const EnvironmentParameter& parameter) {
    if (!parameter.metadata.modificationTemplate.trimmed().isEmpty()) {
        return parameter.metadata.modificationTemplate.trimmed();
    }
    QString name = parameter.name;
    name.replace(QLatin1Char('\''), QStringLiteral("''"));
    const QString query = QStringLiteral(
        "-- 先复核来源、当前值、文件值和参数类型\n"
        "SELECT PARA_NAME, PARA_VALUE, SESS_VALUE, FILE_VALUE, DEFAULT_VALUE,\n"
        "       MIN_VALUE, MAX_VALUE, PARA_TYPE, PARA_MODULE, DESCRIPTION\n"
        "  FROM V$DM_INI\n"
        " WHERE PARA_NAME = '%1';").arg(name);
    const QString type = parameter.parameterType.trimmed().toUpper();
    const QString policy = parameter.metadata.changePolicy.trimmed().toLower();
    if (policy == QStringLiteral("recreate")) return query;
    if (type.contains(QStringLiteral("SESSION")) || policy == QStringLiteral("session")) {
        return query + QStringLiteral(
            "\n\n-- 仅修改当前会话；请将 <NEW_VALUE> 替换为合法值\n"
            "SF_SET_SESSION_PARA_VALUE('%1', <NEW_VALUE>);\n\n"
            "-- 如需修改系统默认值，先确认目标版本允许 scope=1\n"
            "SP_SET_INI_PARA_VALUE(1, '%1', <NEW_VALUE>);").arg(name);
    }
    if (type.contains(QStringLiteral("SYS")) || policy == QStringLiteral("dynamic")) {
        return query + QStringLiteral(
            "\n\n-- 动态参数：同时修改内存与 dm.ini\n"
            "SP_SET_INI_PARA_VALUE(1, '%1', <NEW_VALUE>);").arg(name);
    }
    if (type.contains(QStringLiteral("READ ONLY"))) return query;
    if (type.contains(QStringLiteral("IN FILE")) || policy == QStringLiteral("restart")) {
        return query + QStringLiteral(
            "\n\n-- 静态／文件参数：只修改 dm.ini，重启实例后生效\n"
            "SP_SET_INI_PARA_VALUE(2, '%1', <NEW_VALUE>);").arg(name);
    }
    return query;
}

QString DatabaseEnvironment::formatValue(const QString& format, const QString& value) {
    if (value.isEmpty()) return QStringLiteral("—");
    if (format == QStringLiteral("boolean")) {
        if (value == QStringLiteral("1")) return QStringLiteral("是（1）");
        if (value == QStringLiteral("0")) return QStringLiteral("否（0）");
    }
    if (format == QStringLiteral("unicode")) {
        if (value == QStringLiteral("1")) return QStringLiteral("UTF-8（1）");
        if (value == QStringLiteral("0")) return QStringLiteral("GB18030（0）");
    }
    if (format == QStringLiteral("compatible")) {
        if (value == QStringLiteral("0")) return QStringLiteral("DM（0）");
        if (value == QStringLiteral("1")) return QStringLiteral("SQL92（1）");
        if (value == QStringLiteral("2")) return QStringLiteral("Oracle（2）");
        if (value == QStringLiteral("3")) return QStringLiteral("SQL Server（3）");
        if (value == QStringLiteral("4")) return QStringLiteral("MySQL（4）");
        if (value == QStringLiteral("5")) return QStringLiteral("DM6（5）");
        if (value == QStringLiteral("6")) return QStringLiteral("Teradata（6）");
        if (value == QStringLiteral("7")) return QStringLiteral("PostgreSQL（7）");
        if (value == QStringLiteral("8")) return QStringLiteral("DB2（8）");
    }
    bool ok = false;
    const qlonglong number = value.toLongLong(&ok);
    if (ok && format == QStringLiteral("memoryMb")) {
        return number >= 1024 && number % 1024 == 0
            ? QStringLiteral("%1 GB（%2 MB）").arg(number / 1024).arg(number)
            : QStringLiteral("%1 MB").arg(QLocale().toString(number));
    }
    if (ok && format == QStringLiteral("pageSize")) {
        return number >= 1024 ? QStringLiteral("%1 KB").arg(number / 1024)
                              : QStringLiteral("%1 KB").arg(number);
    }
    if (ok && format == QStringLiteral("extentPages")) {
        return QStringLiteral("%1 Pages").arg(QLocale().toString(number));
    }
    if (format == QStringLiteral("percent")) return value + QStringLiteral("%");
    if (ok && format == QStringLiteral("integer")) return QLocale().toString(number);
    return value;
}

QString DatabaseEnvironment::archiveModeLabel(const QString& value) {
    const QString normalized = value.trimmed().toUpper();
    if (normalized == QStringLiteral("1") || normalized == QStringLiteral("Y")
            || normalized == QStringLiteral("YES") || normalized == QStringLiteral("TRUE")) {
        return QStringLiteral("已开启");
    }
    if (normalized == QStringLiteral("0") || normalized == QStringLiteral("N")
            || normalized == QStringLiteral("NO") || normalized == QStringLiteral("FALSE")) {
        return QStringLiteral("未开启");
    }
    return QStringLiteral("不可用");
}
