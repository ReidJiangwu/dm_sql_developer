#pragma once
#include "ProductFeatures.h"

#include <QString>
#include <QVector>

struct BuiltInSnippet {
    QString id;
    QString category;
    QString name;
    QString trigger;
    QString sql;
    QString placeholder;
};

inline const QVector<BuiltInSnippet>& builtInSnippets() {
    static const QVector<BuiltInSnippet> snippets{
        {QStringLiteral("builtin-output"), QStringLiteral("输出与调试"),
            QStringLiteral("DBMS_OUTPUT.PUT_LINE：输出文本"), QStringLiteral("putline"),
            QStringLiteral("DBMS_OUTPUT.PUT_LINE('value_');"), QStringLiteral("value_")},
        {QStringLiteral("builtin-output-label"), QStringLiteral("输出与调试"),
            QStringLiteral("DBMS_OUTPUT.PUT_LINE：标签与文本"), QStringLiteral("putlabel"),
            QStringLiteral("DBMS_OUTPUT.PUT_LINE('value: ' || 'value_');"), QStringLiteral("value_")},
        {QStringLiteral("builtin-select"), QStringLiteral("SQL 查询"),
            QStringLiteral("查询前 100 行"), QStringLiteral("sel100"),
            QStringLiteral("SELECT * FROM schema_name.table_name LIMIT 100;"), QStringLiteral("schema_name.table_name")},
        {QStringLiteral("builtin-count"), QStringLiteral("SQL 查询"),
            QStringLiteral("统计行数"), QStringLiteral("count"),
            QStringLiteral("SELECT COUNT(*) FROM schema_name.table_name;"), QStringLiteral("schema_name.table_name")},
        {QStringLiteral("builtin-select-into"), QStringLiteral("SQL 查询"),
            QStringLiteral("SELECT INTO：读取单行"), QStringLiteral("selinto"),
            QStringLiteral("SELECT column_name\n  INTO value_\n  FROM table_name\n WHERE condition_;"), QStringLiteral("column_name")},
        {QStringLiteral("builtin-execute-into"), QStringLiteral("SQL 查询"),
            QStringLiteral("EXECUTE IMMEDIATE：动态查询"), QStringLiteral("execinto"),
            QStringLiteral("EXECUTE IMMEDIATE 'sql_' INTO value_;"), QStringLiteral("sql_")},
        {QStringLiteral("builtin-crypto-hash"), QStringLiteral("SQL 查询"),
            QStringLiteral("DBMS_CRYPTO.HASH：计算摘要"), QStringLiteral("cryptohash"),
            QStringLiteral("SELECT DBMS_CRYPTO.HASH('value_', 4) FROM DUAL;"), QStringLiteral("value_")},
        {QStringLiteral("builtin-if"), QStringLiteral("流程控制"),
            QStringLiteral("IF THEN：条件分支"), QStringLiteral("ifthen"),
            QStringLiteral("IF condition_ THEN\n    NULL;\nEND IF;"), QStringLiteral("condition_")},
        {QStringLiteral("builtin-if-else"), QStringLiteral("流程控制"),
            QStringLiteral("IF ELSE：双分支"), QStringLiteral("ifelse"),
            QStringLiteral("IF condition_ THEN\n    NULL;\nELSE\n    NULL;\nEND IF;"), QStringLiteral("condition_")},
        {QStringLiteral("builtin-for"), QStringLiteral("流程控制"),
            QStringLiteral("FOR LOOP：计数循环"), QStringLiteral("forloop"),
            QStringLiteral("FOR index_ IN 1..count_ LOOP\n    NULL;\nEND LOOP;"), QStringLiteral("count_")},
        {QStringLiteral("builtin-while"), QStringLiteral("流程控制"),
            QStringLiteral("WHILE LOOP：条件循环"), QStringLiteral("whileloop"),
            QStringLiteral("WHILE condition_ LOOP\n    NULL;\nEND LOOP;"), QStringLiteral("condition_")},
        {QStringLiteral("builtin-block"), QStringLiteral("程序结构"),
            QStringLiteral("DECLARE BEGIN：匿名块"), QStringLiteral("declare"),
            QStringLiteral("DECLARE\n    value_ VARCHAR2(4000);\nBEGIN\n    NULL;\nEND;"), QStringLiteral("value_")},
        {QStringLiteral("builtin-exception"), QStringLiteral("程序结构"),
            QStringLiteral("BEGIN EXCEPTION：异常处理块"), QStringLiteral("exception"),
            QStringLiteral("BEGIN\n    NULL;\nEXCEPTION\n    WHEN OTHERS THEN\n        DBMS_OUTPUT.PUT_LINE(SQLERRM);\n        RAISE;\nEND;"), QStringLiteral("NULL")},
    };
    return snippets;
}
