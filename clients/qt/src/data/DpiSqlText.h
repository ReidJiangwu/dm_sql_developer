#pragma once

#include "../core/SqlTextEncoding.h"
#include "BatchStatementCancellation.h"
#include <DPIext.h>
extern "C" {
#include <DPIucode.h>
}
#include <QByteArray>
#include <QString>
#include <cstdint>

namespace reid::data {

// Encoding failures happen before DPI sees SQL. Keep their diagnostics local
// to this worker thread and statement, and discard them with their SQL buffer.
struct SqlEncodingDiagnostic {
    dhandle statement = nullptr;
    const void* owner = nullptr;
    QString message;
};
// Keep the per-thread diagnostic behind a function-local TLS object. MinGW's
// linker can emit duplicate TLS initialization functions for an inline
// namespace-scope variable when this header is included by multiple targets.
inline SqlEncodingDiagnostic& sqlEncodingDiagnostic() {
    static thread_local SqlEncodingDiagnostic diagnostic;
    return diagnostic;
}

inline QString takeSqlEncodingError(dhandle statement) {
    auto& diagnostic = sqlEncodingDiagnostic();
    if (diagnostic.statement != statement) return {};
    const QString message = diagnostic.message;
    diagnostic = {};
    return message;
}

// Keep both buffers alive for the complete DPI operation, including async polling.
class DpiSqlText {
public:
    explicit DpiSqlText(const QString& text, const QString& encoding = QStringLiteral("UTF-8"))
        : text_(text), encoding_(reid::text::normalizeSqlTextEncoding(encoding)),
          encoded_(reid::text::encodeSqlText(text, encoding_)),
          wide_(encoding_ == QStringLiteral("UTF-16")) {}

    ~DpiSqlText() {
        auto& diagnostic = sqlEncodingDiagnostic();
        if (diagnostic.owner == this) diagnostic = {};
    }
    DpiSqlText(const DpiSqlText&) = delete;
    DpiSqlText& operator=(const DpiSqlText&) = delete;

    [[nodiscard]] const char* operationName() const { return operationName_; }

    DPIRETURN execute(dhstmt statement, const std::shared_ptr<BatchStatementCancellation>& cancellation = {}) {
        if (cancellation && cancellation->cancelled()) return DSQL_ERROR;
        auto active = cancellation ? cancellation->watch([statement]() { dpi_cancel(statement); })
            : BatchStatementCancellation::Guard{};
        const DPIRETURN configured = configure(statement);
        if (!DSQL_SUCCEEDED(configured)) return configured;
        if (cancellation && cancellation->cancelled()) return DSQL_ERROR;
        operationName_ = wide_ ? "dpi_exec_directW" : "dpi_exec_direct";
        return wide_ ? dpi_exec_directW(statement,
            reinterpret_cast<sdbyte*>(const_cast<ushort*>(text_.utf16())),
            static_cast<sdint4>(text_.size() * sizeof(ushort)))
            : dpi_exec_direct(statement, reinterpret_cast<sdbyte*>(encoded_.bytes.data()));
    }

    DPIRETURN prepare(dhstmt statement) {
        const DPIRETURN configured = configure(statement);
        if (!DSQL_SUCCEEDED(configured)) return configured;
        operationName_ = wide_ ? "dpi_prepareW" : "dpi_prepare";
        return wide_ ? dpi_prepareW(statement,
            reinterpret_cast<sdbyte*>(const_cast<ushort*>(text_.utf16())),
            static_cast<sdint4>(text_.size() * sizeof(ushort)))
            : dpi_prepare(statement, reinterpret_cast<sdbyte*>(encoded_.bytes.data()));
    }

private:
    DPIRETURN configure(dhstmt statement) {
        auto& diagnostic = sqlEncodingDiagnostic();
        if (!encoded_.error.isEmpty()) {
            operationName_ = "sql_text_encoding";
            diagnostic = {statement, this, encoded_.error};
            return DSQL_ERROR;
        }
        // Do not set attributes again while an asynchronous statement is running.
        if (configuredStatement_ == statement) return DSQL_SUCCESS;
        // The W entry points define their own UTF-16 input. Narrow entry points
        // require SQL_CHARSET to match their bytes; bindings retain LOCAL_CODE.
        if (!wide_) {
            const int code = encoding_ == QStringLiteral("GBK") ? PG_GBK
                : encoding_ == QStringLiteral("ISO-8859-1") ? PG_ISO_8859_1 : PG_UTF8;
            operationName_ = "dpi_set_stmt_attr(SQL_CHARSET)";
            const DPIRETURN result = dpi_set_stmt_attr(statement, DSQL_ATTR_SQL_CHARSET,
                reinterpret_cast<dpointer>(static_cast<intptr_t>(code)), 0);
            if (!DSQL_SUCCEEDED(result)) return result;
        }
        configuredStatement_ = statement;
        return DSQL_SUCCESS;
    }

    QString text_;
    QString encoding_;
    reid::text::EncodedSqlText encoded_;
    bool wide_;
    dhstmt configuredStatement_ = nullptr;
    const char* operationName_ = "sql_text_encoding";
};

inline DPIRETURN configureUtf8Environment(dhenv environment) {
    return dpi_set_env_attr(environment, DSQL_ATTR_LOCAL_CODE,
        reinterpret_cast<dpointer>(static_cast<intptr_t>(PG_UTF8)), 0);
}

} // namespace reid::data
