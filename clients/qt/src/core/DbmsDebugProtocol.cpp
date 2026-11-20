#include "DbmsDebugProtocol.h"

#include <QRegularExpression>
#include <stdexcept>

QString DbmsDebugProtocol::initializeSql() {
    return QStringLiteral("SELECT DBMS_DEBUG.INITIALIZE FROM DUAL");
}

QString DbmsDebugProtocol::debugOnSql() {
    return QStringLiteral("BEGIN DBMS_DEBUG.DEBUG_ON; END;");
}

QString DbmsDebugProtocol::debugOffSql() {
    return QStringLiteral("BEGIN DBMS_DEBUG.DEBUG_OFF; END;");
}

QString DbmsDebugProtocol::attachSql() {
    return QStringLiteral("BEGIN DBMS_DEBUG.ATTACH_SESSION(?); END;");
}

QString DbmsDebugProtocol::detachSql() {
    return QStringLiteral("BEGIN DBMS_DEBUG.DETACH_SESSION; END;");
}

QString DbmsDebugProtocol::setBreakpointSql() {
    return QStringLiteral(
        "DECLARE V_BP INTEGER; V_RET INTEGER; BEGIN "
        "V_RET := DBMS_DEBUG.SET_BREAKPOINT(NULL, ?, V_BP); "
        "DBMS_OUTPUT.PUT_LINE('REID_DEBUG_RET=' || V_RET); "
        "DBMS_OUTPUT.PUT_LINE('REID_DEBUG_BREAKPOINT=' || V_BP); END;");
}

QString DbmsDebugProtocol::continueSql(int breakFlags) {
    if (breakFlags != 0 && breakFlags != 12 && breakFlags != 32) {
        throw std::invalid_argument("Unsupported DBMS_DEBUG break flags.");
    }
    return QStringLiteral(
        "DECLARE V_RUN_INFO DBMS_DEBUG.RUNTIME_INFO; V_RET INTEGER; BEGIN "
        "V_RET := DBMS_DEBUG.\"CONTINUE\"(V_RUN_INFO, %1, "
        "0 + DBMS_DEBUG.info_getBreakpoint + DBMS_DEBUG.info_getLineinfo "
        "+ DBMS_DEBUG.info_getOerInfo); "
        "DBMS_OUTPUT.PUT_LINE('REID_DEBUG_RET=' || V_RET); "
        "DBMS_OUTPUT.PUT_LINE('REID_DEBUG_BREAKPOINT=' || V_RUN_INFO.Breakpoint); "
        "DBMS_OUTPUT.PUT_LINE('REID_DEBUG_LINE=' || V_RUN_INFO.Line#); "
        "DBMS_OUTPUT.PUT_LINE('REID_DEBUG_OWNER=' || V_RUN_INFO.Program.Owner); "
        "DBMS_OUTPUT.PUT_LINE('REID_DEBUG_OBJECT=' || V_RUN_INFO.Program.Name); END;")
        .arg(breakFlags);
}

DbmsDebugContinueResult DbmsDebugProtocol::parseOutput(const QString& output) {
    const auto value = [&output](QStringView name) {
        const QRegularExpression expression(
            QStringLiteral("(?:^|\\n)%1=([^\\r\\n]*)").arg(name.toString()),
            QRegularExpression::CaseInsensitiveOption);
        const auto match = expression.match(output);
        return match.hasMatch() ? match.captured(1).trimmed() : QString();
    };
    DbmsDebugContinueResult result;
    bool returnCodeValid = false;
    const int returnCode = value(u"REID_DEBUG_RET").toInt(&returnCodeValid);
    result.returnCode = returnCodeValid ? returnCode : -1;
    result.breakpoint = value(u"REID_DEBUG_BREAKPOINT").toInt();
    result.line = value(u"REID_DEBUG_LINE").toInt();
    result.owner = value(u"REID_DEBUG_OWNER");
    result.objectName = value(u"REID_DEBUG_OBJECT");
    return result;
}
