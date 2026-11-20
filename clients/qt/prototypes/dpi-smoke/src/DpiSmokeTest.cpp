#include <DPI.h>
#include <DPIext.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <conio.h>
#include <io.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace {

#ifndef _WIN32
class TerminalEchoGuard {
public:
    TerminalEchoGuard() {
        if (tcgetattr(STDIN_FILENO, &original_) != 0) {
            throw std::runtime_error("Cannot read terminal settings.");
        }
        termios hidden = original_;
        hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) != 0) {
            throw std::runtime_error("Cannot disable terminal echo.");
        }
        active_ = true;
    }

    ~TerminalEchoGuard() {
        if (active_) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
        }
    }

    TerminalEchoGuard(const TerminalEchoGuard&) = delete;
    TerminalEchoGuard& operator=(const TerminalEchoGuard&) = delete;

private:
    termios original_{};
    bool active_ = false;
};
#endif

std::string readPassword() {
#ifdef _WIN32
    if (!_isatty(_fileno(stdin))) {
#else
    if (!isatty(STDIN_FILENO)) {
#endif
        throw std::runtime_error("A terminal is required so the password can be read without echoing.");
    }

    std::cout << "DM password: " << std::flush;
    std::string password;
#ifdef _WIN32
    for (;;) {
        const int character = _getch();
        if (character == '\r' || character == '\n') break;
        if (character == '\b') {
            if (!password.empty()) password.pop_back();
            continue;
        }
        if (character == 0 || character == 224) {
            (void)_getch();
            continue;
        }
        password.push_back(static_cast<char>(character));
    }
#else
    TerminalEchoGuard guard;
    std::getline(std::cin, password);
#endif
    std::cout << '\n';
    if (password.empty()) {
        throw std::invalid_argument("Password must not be empty.");
    }
    return password;
}

int parsePort(const std::string& value) {
    std::size_t parsed = 0;
    int port = 0;
    try {
        port = std::stoi(value, &parsed);
    } catch (const std::exception&) {
        throw std::invalid_argument("Port must be a number.");
    }
    if (parsed != value.size() || port < 1 || port > 65535) {
        throw std::invalid_argument("Port must be between 1 and 65535.");
    }
    return port;
}

void printDiagnostic(sdint2 handleType, dhandle handle) {
    sdint4 errorCode = 0;
    sdint2 messageLength = 0;
    sdbyte message[2048]{};
    DPIRETURN result = dpi_get_diag_rec(
        handleType,
        handle,
        1,
        &errorCode,
        message,
        static_cast<sdint2>(sizeof(message)),
        &messageLength);
    if (DSQL_SUCCEEDED(result)) {
        std::cerr << "DPI_ERROR_CODE=" << errorCode << '\n';
        std::cerr << "DPI_ERROR_MESSAGE=" << reinterpret_cast<const char*>(message) << '\n';
    }
}

bool succeeded(DPIRETURN result, sdint2 handleType, dhandle handle) {
    if (DSQL_SUCCEEDED(result)) {
        return true;
    }
    printDiagnostic(handleType, handle);
    return false;
}

void executeSql(dhstmt statement, const char* sql, const char* errorMessage) {
    std::vector<sdbyte> query(sql, sql + std::char_traits<char>::length(sql) + 1);
    DPIRETURN result = dpi_exec_direct(statement, query.data());
    if (!succeeded(result, DSQL_HANDLE_STMT, statement)) {
        throw std::runtime_error(errorMessage);
    }
}

sdint4 queryInt(dhstmt statement, const char* sql) {
    executeSql(statement, sql, "Integer query failed.");
    sdint4 value = 0;
    slength indicator = 0;
    DPIRETURN result = dpi_bind_col(statement, 1, DSQL_C_SLONG, &value, sizeof(value), &indicator);
    if (DSQL_SUCCEEDED(result)) {
        ulength rowsFetched = 0;
        result = dpi_fetch(statement, &rowsFetched);
    }
    if (!DSQL_SUCCEEDED(result) || result == DSQL_NO_DATA) {
        printDiagnostic(DSQL_HANDLE_STMT, statement);
        throw std::runtime_error("Integer query returned no row.");
    }
    result = dpi_close_cursor(statement);
    if (!succeeded(result, DSQL_HANDLE_STMT, statement)) {
        throw std::runtime_error("Cannot close the integer query cursor.");
    }
    return value;
}

void dropProbeTable(dhstmt statement, const char* table) {
    const std::string sql = std::string("DROP TABLE ") + table;
    std::vector<sdbyte> query(sql.begin(), sql.end());
    query.push_back('\0');
    dpi_exec_direct(statement, query.data());
}

void dropProbeProcedure(dhstmt statement, const char* procedure) {
    const std::string sql = std::string("DROP PROCEDURE ") + procedure;
    std::vector<sdbyte> query(sql.begin(), sql.end());
    query.push_back('\0');
    dpi_exec_direct(statement, query.data());
}

}  // namespace

int main(int argc, char* argv[]) {
    const bool hasProbeMode = argc == 5
        && (std::string(argv[4]) == "--transaction-probe"
            || std::string(argv[4]) == "--batch-probe"
            || std::string(argv[4]) == "--mixed-batch-probe"
            || std::string(argv[4]) == "--fetch-probe"
            || std::string(argv[4]) == "--lob-probe"
            || std::string(argv[4]) == "--ddl-probe"
            || std::string(argv[4]) == "--isolation-probe"
            || std::string(argv[4]) == "--metadata-probe"
            || std::string(argv[4]) == "--program-metadata-probe"
            || std::string(argv[4]) == "--dbms-output-probe"
            || std::string(argv[4]) == "--sql-compatibility-probe"
            || std::string(argv[4]) == "--cancel-capability"
            || std::string(argv[4]) == "--cancel-probe"
            || std::string(argv[4]) == "--stage-one-probe"
            || std::string(argv[4]) == "--stage-two-probe"
            || std::string(argv[4]) == "--stage-three-probe");
    if (argc != 4 && !hasProbeMode) {
        std::cerr << "Usage: dpi-smoke <host> <port> <user> "
                     "[--transaction-probe|--batch-probe|--mixed-batch-probe|--fetch-probe|"
                     "--lob-probe|--ddl-probe|--isolation-probe|--metadata-probe|"
                     "--program-metadata-probe|--dbms-output-probe|--sql-compatibility-probe|"
                     "--cancel-capability|--cancel-probe|--stage-one-probe|--stage-two-probe|"
                     "--stage-three-probe]\n";
        return 2;
    }

    const bool cancelCapabilityProbe = argc == 5 && std::string(argv[4]) == "--cancel-capability";
    const bool stageOneProbe = argc == 5 && std::string(argv[4]) == "--stage-one-probe";
    const bool stageTwoProbe = argc == 5 && std::string(argv[4]) == "--stage-two-probe";
    const bool stageThreeProbe = argc == 5 && std::string(argv[4]) == "--stage-three-probe";
    const bool cancelProbe = argc == 5
        && (std::string(argv[4]) == "--cancel-probe" || stageOneProbe);
    const bool batchProbe = argc == 5 && std::string(argv[4]) == "--batch-probe";
    const bool mixedBatchProbe = argc == 5 && std::string(argv[4]) == "--mixed-batch-probe";
    const bool fetchProbe = argc == 5
        && (std::string(argv[4]) == "--fetch-probe" || stageOneProbe);
    const bool lobProbe = argc == 5 && std::string(argv[4]) == "--lob-probe";
    const bool transactionOnlyProbe = argc == 5 && std::string(argv[4]) == "--transaction-probe";
    const bool transactionProbe = transactionOnlyProbe || batchProbe || mixedBatchProbe || fetchProbe || lobProbe;
    const bool ddlProbe = argc == 5 && std::string(argv[4]) == "--ddl-probe";
    const bool isolationProbe = argc == 5 && std::string(argv[4]) == "--isolation-probe";
    const bool metadataProbe = argc == 5
        && (std::string(argv[4]) == "--metadata-probe" || stageTwoProbe);
    const bool programMetadataProbe = argc == 5
        && (std::string(argv[4]) == "--program-metadata-probe" || stageTwoProbe);
    const bool dbmsOutputProbe = argc == 5 && std::string(argv[4]) == "--dbms-output-probe";
    const bool sqlCompatibilityProbe = argc == 5 && std::string(argv[4]) == "--sql-compatibility-probe";

    dhenv environment = nullptr;
    dhcon connection = nullptr;
    dhcon secondConnection = nullptr;
    dhstmt statement = nullptr;
    bool loggedIn = false;
    bool secondLoggedIn = false;
    int exitCode = 1;

    try {
        const std::string host = argv[1];
        const int port = parsePort(argv[2]);
        std::string server = host + ":" + std::to_string(port);
        std::string user = argv[3];

        DPIRETURN result = dpi_alloc_env(&environment);
        if (!succeeded(result, DSQL_HANDLE_ENV, environment)) {
            throw std::runtime_error("dpi_alloc_env failed.");
        }

        result = dpi_alloc_con(environment, &connection);
        if (!succeeded(result, DSQL_HANDLE_DBC, connection)) {
            throw std::runtime_error("dpi_alloc_con failed.");
        }

        std::string password = readPassword();
        result = dpi_login(
            connection,
            reinterpret_cast<sdbyte*>(server.data()),
            reinterpret_cast<sdbyte*>(user.data()),
            reinterpret_cast<sdbyte*>(password.data()));
        if (!succeeded(result, DSQL_HANDLE_DBC, connection)) {
            std::fill(password.begin(), password.end(), '\0');
            throw std::runtime_error("dpi_login failed.");
        }
        loggedIn = true;

        if (stageOneProbe || stageThreeProbe) {
            result = dpi_alloc_con(environment, &secondConnection);
            if (DSQL_SUCCEEDED(result)) {
                result = dpi_login(
                    secondConnection,
                    reinterpret_cast<sdbyte*>(server.data()),
                    reinterpret_cast<sdbyte*>(user.data()),
                    reinterpret_cast<sdbyte*>(password.data()));
            }
            if (!succeeded(result, DSQL_HANDLE_DBC, secondConnection)) {
                std::fill(password.begin(), password.end(), '\0');
                throw std::runtime_error("Cannot create the second DPI connection.");
            }
            secondLoggedIn = true;
        }
        std::fill(password.begin(), password.end(), '\0');

        result = dpi_alloc_stmt(connection, &statement);
        if (!succeeded(result, DSQL_HANDLE_DBC, connection)) {
            throw std::runtime_error("dpi_alloc_stmt failed.");
        }

        sdbyte query[] = "SELECT 1";
        result = dpi_exec_direct(statement, query);
        if (!succeeded(result, DSQL_HANDLE_STMT, statement)) {
            throw std::runtime_error("dpi_exec_direct failed.");
        }

        sdint4 value = 0;
        slength indicator = 0;
        result = dpi_bind_col(statement, 1, DSQL_C_SLONG, &value, sizeof(value), &indicator);
        if (!succeeded(result, DSQL_HANDLE_STMT, statement)) {
            throw std::runtime_error("dpi_bind_col failed.");
        }

        ulength rowsFetched = 0;
        result = dpi_fetch(statement, &rowsFetched);
        if (!DSQL_SUCCEEDED(result) || result == DSQL_NO_DATA || value != 1) {
            if (result != DSQL_NO_DATA) {
                printDiagnostic(DSQL_HANDLE_STMT, statement);
            }
            throw std::runtime_error("SELECT 1 returned an unexpected result.");
        }
        result = dpi_close_cursor(statement);
        if (!succeeded(result, DSQL_HANDLE_STMT, statement)) {
            throw std::runtime_error("Cannot close the SELECT 1 cursor.");
        }

        if (cancelCapabilityProbe) {
            udint4 asyncMode = DSQL_AM_NONE;
            result = dpi_get_con_attr(connection, DSQL_ASYNC_MODE, &asyncMode, 0, nullptr);
            if (!succeeded(result, DSQL_HANDLE_DBC, connection)) {
                throw std::runtime_error("Cannot read the DPI asynchronous execution capability.");
            }
            std::cout << "ASYNC_MODE=" << asyncMode << '\n';
            std::cout << "CANCEL_CAPABILITY=PASS\n";
        }

        if (cancelProbe) {
            dhstmt cancelStatement = nullptr;
            result = dpi_alloc_stmt(connection, &cancelStatement);
            if (DSQL_SUCCEEDED(result)) {
                result = dpi_set_stmt_attr(cancelStatement, DSQL_ATTR_ASYNC_ENABLE,
                    reinterpret_cast<dpointer>(DSQL_ASYNC_ENABLE_ON), 0);
            }
            sdbyte sleepStatement[] = "CALL DBMS_LOCK.SLEEP(10)";
            if (DSQL_SUCCEEDED(result)) {
                result = dpi_exec_direct(cancelStatement, sleepStatement);
            }
            if (result != DSQL_STILL_EXECUTING) {
                if (!DSQL_SUCCEEDED(result)) {
                    printDiagnostic(DSQL_HANDLE_STMT, cancelStatement);
                }
                dpi_free_stmt(cancelStatement);
                throw std::runtime_error("The controlled sleep statement did not enter asynchronous execution.");
            }
            result = dpi_cancel(cancelStatement);
            if (!succeeded(result, DSQL_HANDLE_STMT, cancelStatement)) {
                dpi_free_stmt(cancelStatement);
                throw std::runtime_error("dpi_cancel failed for the controlled sleep statement.");
            }
            for (int attempt = 0; attempt < 100; ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                result = dpi_exec_direct(cancelStatement, sleepStatement);
                if (result != DSQL_STILL_EXECUTING) {
                    break;
                }
            }
            if (result == DSQL_STILL_EXECUTING) {
                dpi_free_stmt(cancelStatement);
                throw std::runtime_error("Cancelled statement did not complete within five seconds.");
            }
            dpi_free_stmt(cancelStatement);

            dhstmt validationStatement = nullptr;
            result = dpi_alloc_stmt(connection, &validationStatement);
            sdbyte validationQuery[] = "SELECT 1";
            if (DSQL_SUCCEEDED(result)) {
                result = dpi_exec_direct(validationStatement, validationQuery);
            }
            if (!succeeded(result, DSQL_HANDLE_STMT, validationStatement)) {
                if (validationStatement != nullptr) {
                    dpi_free_stmt(validationStatement);
                }
                throw std::runtime_error("Connection was unusable after dpi_cancel.");
            }
            dpi_close_cursor(validationStatement);
            dpi_free_stmt(validationStatement);
            std::cout << "CANCEL_RUNNING_STATEMENT=PASS\n";
            std::cout << "CANCEL_CONNECTION_REUSE=PASS\n";
        }

        if (ddlProbe) {
            dropProbeTable(statement, "REID_DPI_PROBE_DDL_TRIGGER");
            dropProbeTable(statement, "REID_DPI_PROBE_DDL_MARKER");
            try {
                executeSql(statement, "CREATE TABLE REID_DPI_PROBE_DDL_MARKER (ID INT PRIMARY KEY)",
                    "Cannot create the DDL marker table.");
                executeSql(statement, "INSERT INTO REID_DPI_PROBE_DDL_MARKER VALUES (1)",
                    "Cannot insert the DDL marker row.");
                executeSql(statement, "CREATE TABLE REID_DPI_PROBE_DDL_TRIGGER (ID INT)",
                    "Cannot create the DDL trigger table.");
                result = dpi_rollback(connection);
                if (!succeeded(result, DSQL_HANDLE_DBC, connection)
                        || queryInt(statement, "SELECT COUNT(*) FROM REID_DPI_PROBE_DDL_MARKER") != 1) {
                    throw std::runtime_error("DDL implicit-commit behavior differed.");
                }
            } catch (...) {
                dropProbeTable(statement, "REID_DPI_PROBE_DDL_TRIGGER");
                dropProbeTable(statement, "REID_DPI_PROBE_DDL_MARKER");
                throw;
            }
            dropProbeTable(statement, "REID_DPI_PROBE_DDL_TRIGGER");
            dropProbeTable(statement, "REID_DPI_PROBE_DDL_MARKER");
            std::cout << "DDL_IMPLICIT_COMMIT=PASS\n";
        }

        if (isolationProbe) {
            udint4 originalIsolation = 0;
            result = dpi_get_con_attr(connection, DSQL_ATTR_TXN_ISOLATION,
                &originalIsolation, sizeof(originalIsolation), nullptr);
            if (!succeeded(result, DSQL_HANDLE_DBC, connection)) {
                throw std::runtime_error("Cannot read the transaction isolation level.");
            }
            for (udint4 isolation : {static_cast<udint4>(ISO_LEVEL_READ_COMMITTED),
                    static_cast<udint4>(ISO_LEVEL_SERIALIZABLE)}) {
                result = dpi_set_con_attr(connection, DSQL_ATTR_TXN_ISOLATION,
                    reinterpret_cast<dpointer>(static_cast<std::uintptr_t>(isolation)), 0);
                if (!succeeded(result, DSQL_HANDLE_DBC, connection)) {
                    throw std::runtime_error("Cannot set a required transaction isolation level.");
                }
                udint4 actualIsolation = 0;
                result = dpi_get_con_attr(connection, DSQL_ATTR_TXN_ISOLATION,
                    &actualIsolation, sizeof(actualIsolation), nullptr);
                if (!succeeded(result, DSQL_HANDLE_DBC, connection) || actualIsolation != isolation) {
                    throw std::runtime_error("Transaction isolation mapping differed.");
                }
            }
            dpi_set_con_attr(connection, DSQL_ATTR_TXN_ISOLATION,
                reinterpret_cast<dpointer>(static_cast<std::uintptr_t>(originalIsolation)), 0);
            std::cout << "ISOLATION_DEFAULT=" << originalIsolation << '\n';
            std::cout << "ISOLATION_MAPPING=PASS\n";
        }

        if (metadataProbe) {
            const sdint4 objectCount = queryInt(statement, "SELECT COUNT(*) FROM ALL_OBJECTS");
            if (objectCount <= 0) {
                throw std::runtime_error("Metadata query returned no visible objects.");
            }
            std::cout << "METADATA_OBJECT_COUNT=" << objectCount << '\n';
            std::cout << "METADATA_ENUMERATION=PASS\n";
        }

        if (programMetadataProbe) {
            dropProbeProcedure(statement, "REID_DPI_PROBE_INVALID");
            dropProbeProcedure(statement, "REID_DPI_PROBE_PROGRAM");
            try {
                executeSql(statement,
                    "CREATE OR REPLACE PROCEDURE REID_DPI_PROBE_PROGRAM AS "
                    "V_VALUE INT; BEGIN V_VALUE := 1; END;",
                    "Cannot create the DPI program metadata fixture.");
                if (queryInt(statement, "SELECT COUNT(*) FROM ALL_SOURCE "
                        "WHERE NAME = 'REID_DPI_PROBE_PROGRAM' AND TYPE = 'PROCEDURE'") <= 0) {
                    throw std::runtime_error("Program source was not visible through ALL_SOURCE.");
                }
                sdbyte invalidProgram[] =
                    "CREATE OR REPLACE PROCEDURE REID_DPI_PROBE_INVALID AS "
                    "BEGIN SELECT FROM; END;";
                result = dpi_exec_direct(statement, invalidProgram);
                if (DSQL_SUCCEEDED(result)) {
                    throw std::runtime_error("Invalid DPI procedure compilation was not rejected.");
                }
                printDiagnostic(DSQL_HANDLE_STMT, statement);
            } catch (...) {
                dropProbeProcedure(statement, "REID_DPI_PROBE_INVALID");
                dropProbeProcedure(statement, "REID_DPI_PROBE_PROGRAM");
                throw;
            }
            dropProbeProcedure(statement, "REID_DPI_PROBE_INVALID");
            dropProbeProcedure(statement, "REID_DPI_PROBE_PROGRAM");
            std::cout << "PROGRAM_SOURCE_METADATA=PASS\n";
            std::cout << "PROGRAM_COMPILE_ERROR=PASS\n";
            if (stageTwoProbe) {
                for (const char* view : {"ALL_TABLES", "ALL_VIEWS", "ALL_TAB_COLUMNS",
                        "ALL_CONSTRAINTS", "ALL_CONS_COLUMNS", "ALL_INDEXES",
                        "ALL_IND_COLUMNS", "ALL_SEQUENCES", "ALL_TRIGGERS",
                        "ALL_SYNONYMS", "ALL_PROCEDURES", "ALL_ARGUMENTS",
                        "ALL_SOURCE", "ALL_DEPENDENCIES"}) {
                    const std::string sql = std::string("SELECT COUNT(*) FROM ") + view;
                    (void)queryInt(statement, sql.c_str());
                }
                std::cout << "STAGE_TWO_DPI_METADATA_VIEWS=PASS\n";
            }
        }

        if (dbmsOutputProbe) {
            executeSql(statement,
                "BEGIN DBMS_OUTPUT.ENABLE(1000000); "
                "DBMS_OUTPUT.PUT_LINE('GzydDmTools-DPI'); "
                "DBMS_OUTPUT.PUT_LINE('中文输出'); END;",
                "DPI DBMS_OUTPUT statement failed.");
            std::vector<char> printInfo(1024 * 1024 + 1, '\0');
            slength printLength = 0;
            result = dpi_get_diag_field(DSQL_HANDLE_STMT, statement, 1,
                DSQL_DIAG_PRINT_INFO, printInfo.data(), printInfo.size(), &printLength);
            if (!succeeded(result, DSQL_HANDLE_STMT, statement) || printLength <= 0) {
                throw std::runtime_error("DPI DBMS_OUTPUT print information was empty.");
            }
            const std::string output(printInfo.data(), static_cast<std::size_t>(printLength));
            if (output.find("GzydDmTools-DPI") == std::string::npos
                || output.find("中文输出") == std::string::npos) {
                throw std::runtime_error("DPI DBMS_OUTPUT print information differed.");
            }
            std::cout << "DBMS_OUTPUT_CAPTURE=PASS\n";
        }

        if (sqlCompatibilityProbe) {
            const sdint4 pageValue = queryInt(statement,
                "SELECT ID FROM (SELECT 1 ID UNION ALL SELECT 2 UNION ALL SELECT 3) T "
                "ORDER BY ID LIMIT 1 OFFSET 1");
            const sdint4 withUrValue = queryInt(statement, "SELECT 1 WITH UR");
            if (pageValue != 2 || withUrValue != 1) {
                throw std::runtime_error("Pagination or WITH UR behavior differed.");
            }
            std::cout << "PAGINATION_LIMIT_OFFSET=PASS\n";
            std::cout << "WITH_UR=PASS\n";
        }

        if (transactionProbe) {
            result = dpi_set_con_attr(
                connection, DSQL_ATTR_AUTOCOMMIT, reinterpret_cast<dpointer>(DSQL_AUTOCOMMIT_OFF), 0);
            if (!succeeded(result, DSQL_HANDLE_DBC, connection)) {
                throw std::runtime_error("Cannot disable auto-commit.");
            }

            sdbyte createTable[] =
                "CREATE TEMPORARY TABLE REID_DPI_PROBE_TX "
                "(ID INT PRIMARY KEY, TEXT_VALUE CLOB, BINARY_VALUE BLOB) ON COMMIT PRESERVE ROWS";
            result = dpi_exec_direct(statement, createTable);
            if (!succeeded(result, DSQL_HANDLE_STMT, statement)) {
                throw std::runtime_error("Cannot create the transaction probe temporary table.");
            }

            if (lobProbe) {
                dhstmt lobStatement = nullptr;
                result = dpi_alloc_stmt(connection, &lobStatement);
                if (!succeeded(result, DSQL_HANDLE_DBC, connection)) {
                    throw std::runtime_error("Cannot allocate the LOB insert statement.");
                }
                sdbyte insertLob[] =
                    "INSERT INTO REID_DPI_PROBE_TX (ID, TEXT_VALUE, BINARY_VALUE) VALUES (?, ?, ?)";
                sdint4 lobId = 1;
                std::string textValue(16 * 1024, 'c');
                std::vector<sdbyte> binaryValue(64 * 1024);
                for (std::size_t index = 0; index < binaryValue.size(); ++index) {
                    binaryValue[index] = static_cast<sdbyte>(index % 251);
                }
                slength idIndicator = sizeof(lobId);
                slength textIndicator = static_cast<slength>(textValue.size());
                slength binaryIndicator = static_cast<slength>(binaryValue.size());
                result = dpi_prepare(lobStatement, insertLob);
                if (DSQL_SUCCEEDED(result)) {
                    result = dpi_bind_param(lobStatement, 1, DSQL_PARAM_INPUT, DSQL_C_SLONG,
                        DSQL_INT, sizeof(lobId), 0, &lobId, sizeof(lobId), &idIndicator);
                }
                if (DSQL_SUCCEEDED(result)) {
                    result = dpi_bind_param(lobStatement, 2, DSQL_PARAM_INPUT, DSQL_C_NCHAR,
                        DSQL_CLOB, textValue.size(), 0, textValue.data(), textValue.size(), &textIndicator);
                }
                if (DSQL_SUCCEEDED(result)) {
                    result = dpi_bind_param(lobStatement, 3, DSQL_PARAM_INPUT, DSQL_C_BINARY,
                        DSQL_BLOB, binaryValue.size(), 0, binaryValue.data(), binaryValue.size(), &binaryIndicator);
                }
                if (DSQL_SUCCEEDED(result)) {
                    result = dpi_exec(lobStatement);
                }
                if (!succeeded(result, DSQL_HANDLE_STMT, lobStatement)) {
                    dpi_free_stmt(lobStatement);
                    throw std::runtime_error("Cannot insert the LOB probe row.");
                }
                dpi_free_stmt(lobStatement);

                dhloblctr textLocator = nullptr;
                dhloblctr binaryLocator = nullptr;
                result = dpi_alloc_lob_locator(statement, &textLocator);
                if (DSQL_SUCCEEDED(result)) {
                    result = dpi_alloc_lob_locator(statement, &binaryLocator);
                }
                sdbyte selectLob[] = "SELECT TEXT_VALUE, BINARY_VALUE FROM REID_DPI_PROBE_TX WHERE ID = 1";
                if (DSQL_SUCCEEDED(result)) {
                    result = dpi_exec_direct(statement, selectLob);
                }
                slength textResultIndicator = 0;
                slength binaryResultIndicator = 0;
                if (DSQL_SUCCEEDED(result)) {
                    result = dpi_bind_col(statement, 1, DSQL_C_LOB_HANDLE, &textLocator,
                        sizeof(textLocator), &textResultIndicator);
                }
                if (DSQL_SUCCEEDED(result)) {
                    result = dpi_bind_col(statement, 2, DSQL_C_LOB_HANDLE, &binaryLocator,
                        sizeof(binaryLocator), &binaryResultIndicator);
                }
                if (DSQL_SUCCEEDED(result)) {
                    result = dpi_fetch(statement, &rowsFetched);
                }
                slength textLength = 0;
                slength binaryLength = 0;
                if (DSQL_SUCCEEDED(result) && result != DSQL_NO_DATA) {
                    result = dpi_lob_get_length(textLocator, &textLength);
                }
                if (DSQL_SUCCEEDED(result)) {
                    result = dpi_lob_get_length(binaryLocator, &binaryLength);
                }
                constexpr slength chunkSize = 8 * 1024;
                std::vector<sdbyte> readText;
                std::vector<sdbyte> readBinary;
                std::array<sdbyte, chunkSize> chunk{};
                for (slength offset = 1; DSQL_SUCCEEDED(result) && offset <= textLength;) {
                    slength readLength = 0;
                    const slength requested = std::min(chunkSize, textLength - offset + 1);
                    result = dpi_lob_read(textLocator, offset, DSQL_C_NCHAR, requested, chunk.data(),
                        chunk.size(), &readLength);
                    readText.insert(readText.end(), chunk.begin(), chunk.begin() + readLength);
                    offset += readLength;
                }
                for (slength offset = 1; DSQL_SUCCEEDED(result) && offset <= binaryLength;) {
                    slength readLength = 0;
                    const slength requested = std::min(chunkSize, binaryLength - offset + 1);
                    result = dpi_lob_read(binaryLocator, offset, DSQL_C_BINARY, requested, chunk.data(),
                        chunk.size(), &readLength);
                    readBinary.insert(readBinary.end(), chunk.begin(), chunk.begin() + readLength);
                    offset += readLength;
                }
                const bool lobMatches = DSQL_SUCCEEDED(result)
                    && readText.size() == textValue.size()
                    && readBinary.size() == binaryValue.size()
                    && std::equal(textValue.begin(), textValue.end(), readText.begin())
                    && std::equal(binaryValue.begin(), binaryValue.end(), readBinary.begin());
                dpi_close_cursor(statement);
                dpi_free_lob_locator(textLocator);
                dpi_free_lob_locator(binaryLocator);
                if (!lobMatches) {
                    throw std::runtime_error("CLOB or BLOB locator read returned unexpected data.");
                }
            } else if (batchProbe || fetchProbe) {
                dhstmt batchStatement = nullptr;
                result = dpi_alloc_stmt(connection, &batchStatement);
                if (!succeeded(result, DSQL_HANDLE_DBC, connection)) {
                    throw std::runtime_error("Cannot allocate the batch probe statement.");
                }
                sdbyte insertRow[] = "INSERT INTO REID_DPI_PROBE_TX (ID) VALUES (?)";
                const ulength batchRows = fetchProbe ? 10000 : 500;
                std::vector<sdint4> batchValues(batchRows);
                std::vector<slength> batchIndicators(batchRows);
                for (ulength index = 0; index < batchRows; ++index) {
                    batchValues[index] = static_cast<sdint4>(index + 1);
                    batchIndicators[index] = sizeof(batchValues[index]);
                }
                result = dpi_set_stmt_attr(batchStatement, DSQL_ATTR_PARAMSET_SIZE,
                    reinterpret_cast<dpointer>(batchRows), sizeof(batchRows));
                if (DSQL_SUCCEEDED(result)) {
                    result = dpi_prepare(batchStatement, insertRow);
                }
                if (succeeded(result, DSQL_HANDLE_STMT, batchStatement)) {
                    result = dpi_bind_param(batchStatement, 1, DSQL_PARAM_INPUT, DSQL_C_SLONG,
                        DSQL_INT, sizeof(batchValues[0]), 0, batchValues.data(),
                        sizeof(batchValues[0]), batchIndicators.data());
                }
                if (DSQL_SUCCEEDED(result)) {
                    result = dpi_exec(batchStatement);
                }
                if (!succeeded(result, DSQL_HANDLE_STMT, batchStatement)) {
                    dpi_free_stmt(batchStatement);
                    throw std::runtime_error("Parameter batch failed.");
                }
                dpi_free_stmt(batchStatement);
                if (fetchProbe) {
                    result = dpi_set_stmt_attr(statement, DSQL_ATTR_FETCH_PACKAGE_SIZE,
                        reinterpret_cast<dpointer>(static_cast<std::uintptr_t>(256)), 0);
                    if (!succeeded(result, DSQL_HANDLE_STMT, statement)) {
                        throw std::runtime_error("Cannot set the DPI fetch package size.");
                    }
                    executeSql(statement, "SELECT ID FROM REID_DPI_PROBE_TX ORDER BY ID",
                        "Cannot execute the 10,000-row fetch query.");
                    sdint4 fetchedValue = 0;
                    slength fetchedIndicator = 0;
                    result = dpi_bind_col(statement, 1, DSQL_C_SLONG, &fetchedValue,
                        sizeof(fetchedValue), &fetchedIndicator);
                    if (!succeeded(result, DSQL_HANDLE_STMT, statement)) {
                        throw std::runtime_error("Cannot bind the fetch probe column.");
                    }
                    sdint4 fetchedRows = 0;
                    for (;;) {
                        result = dpi_fetch(statement, &rowsFetched);
                        if (result == DSQL_NO_DATA) {
                            break;
                        }
                        if (!DSQL_SUCCEEDED(result) || fetchedValue != fetchedRows + 1) {
                            throw std::runtime_error("DPI fetch probe row ordering differed.");
                        }
                        ++fetchedRows;
                    }
                    if (fetchedRows != 10000) {
                        throw std::runtime_error("DPI fetch probe returned an unexpected row count.");
                    }
                    result = dpi_close_cursor(statement);
                    if (!succeeded(result, DSQL_HANDLE_STMT, statement)) {
                        throw std::runtime_error("Cannot close the DPI fetch cursor.");
                    }
                }
            } else if (mixedBatchProbe) {
                executeSql(statement, "INSERT INTO REID_DPI_PROBE_TX (ID) VALUES (1)",
                    "Cannot insert the mixed-batch seed row.");
                executeSql(statement, "UPDATE REID_DPI_PROBE_TX SET ID = 10 WHERE ID = 1",
                    "Cannot update the mixed-batch seed row.");
                executeSql(statement, "INSERT INTO REID_DPI_PROBE_TX (ID) VALUES (2)",
                    "Cannot insert the mixed-batch delete row.");
                executeSql(statement, "DELETE FROM REID_DPI_PROBE_TX WHERE ID = 2",
                    "Cannot delete the mixed-batch row.");
                if (queryInt(statement, "SELECT COUNT(*) FROM REID_DPI_PROBE_TX WHERE ID = 10") != 1) {
                    throw std::runtime_error("Mixed insert/update/delete produced unexpected rows.");
                }
            } else {
                sdbyte insertRow[] = "INSERT INTO REID_DPI_PROBE_TX (ID) VALUES (1)";
                result = dpi_exec_direct(statement, insertRow);
                if (!succeeded(result, DSQL_HANDLE_STMT, statement)) {
                    throw std::runtime_error("Cannot insert the transaction probe row.");
                }
            }

            sdbyte countRows[] = "SELECT COUNT(*) FROM REID_DPI_PROBE_TX";
            result = dpi_exec_direct(statement, countRows);
            if (!succeeded(result, DSQL_HANDLE_STMT, statement)) {
                throw std::runtime_error("Cannot query the transaction probe table.");
            }
            value = -1;
            result = dpi_bind_col(statement, 1, DSQL_C_SLONG, &value, sizeof(value), &indicator);
            if (!succeeded(result, DSQL_HANDLE_STMT, statement)) {
                throw std::runtime_error("Cannot bind the transaction probe row count.");
            }
            result = dpi_fetch(statement, &rowsFetched);
            const sdint4 expectedRows = fetchProbe ? 10000 : (batchProbe ? 500 : 1);
            if (!DSQL_SUCCEEDED(result) || value != expectedRows) {
                throw std::runtime_error("Transaction probe inserted an unexpected number of rows.");
            }
            result = dpi_close_cursor(statement);
            if (!succeeded(result, DSQL_HANDLE_STMT, statement)) {
                throw std::runtime_error("Cannot close the transaction probe row count cursor.");
            }
            if (mixedBatchProbe) {
                sdbyte duplicateRow[] = "INSERT INTO REID_DPI_PROBE_TX (ID) VALUES (10)";
                result = dpi_exec_direct(statement, duplicateRow);
                if (DSQL_SUCCEEDED(result)) {
                    throw std::runtime_error("Duplicate-key row was not rejected.");
                }
                printDiagnostic(DSQL_HANDLE_STMT, statement);
                std::cout << "BATCH_ROW_ERROR=PASS\n";
            }
            result = dpi_rollback(connection);
            if (!succeeded(result, DSQL_HANDLE_DBC, connection)) {
                throw std::runtime_error("Transaction probe rollback failed.");
            }

            sdbyte dropTable[] = "DROP TABLE REID_DPI_PROBE_TX";
            result = dpi_exec_direct(statement, dropTable);
            if (!succeeded(result, DSQL_HANDLE_STMT, statement)) {
                throw std::runtime_error("Cannot remove the transaction probe temporary table.");
            }
            std::cout << "TRANSACTION_ROLLBACK=PASS\n";
            if (batchProbe) {
                std::cout << "BATCH_500=PASS\n";
            }
            if (mixedBatchProbe) {
                std::cout << "MIXED_BATCH=PASS\n";
            }
            if (fetchProbe) {
                std::cout << "FETCH_10000=PASS\n";
            }
            if (lobProbe) {
                std::cout << "CLOB_STREAM_READ=PASS\n";
                std::cout << "BLOB_STREAM_READ=PASS\n";
            }
        }

        if (stageOneProbe) {
            result = dpi_set_con_attr(
                secondConnection, DSQL_ATTR_AUTOCOMMIT,
                reinterpret_cast<dpointer>(DSQL_AUTOCOMMIT_OFF), 0);
            if (!succeeded(result, DSQL_HANDLE_DBC, secondConnection)) {
                throw std::runtime_error("Cannot disable auto-commit on the second DPI connection.");
            }

            executeSql(statement,
                "CREATE TEMPORARY TABLE IF NOT EXISTS REID_DPI_PROBE_TX "
                "(ID INT PRIMARY KEY) ON COMMIT PRESERVE ROWS",
                "Cannot create the DPI commit probe table.");
            executeSql(statement, "INSERT INTO REID_DPI_PROBE_TX VALUES (10001)",
                "Cannot insert the DPI commit probe row.");
            result = dpi_commit(connection);
            if (!succeeded(result, DSQL_HANDLE_DBC, connection)
                    || queryInt(statement,
                        "SELECT COUNT(*) FROM REID_DPI_PROBE_TX WHERE ID = 10001") != 1) {
                throw std::runtime_error("DPI commit did not preserve the inserted row.");
            }
            executeSql(statement, "DROP TABLE REID_DPI_PROBE_TX",
                "Cannot remove the DPI commit probe table.");
            std::cout << "TRANSACTION_COMMIT=PASS\n";

            dhstmt errorStatement = nullptr;
            result = dpi_alloc_stmt(connection, &errorStatement);
            if (!succeeded(result, DSQL_HANDLE_DBC, connection)) {
                throw std::runtime_error("Cannot allocate the DPI SQL error statement.");
            }
            sdbyte invalidSql[] = "SELECT * FROM REID_STAGE_ONE_MISSING_OBJECT";
            result = dpi_exec_direct(errorStatement, invalidSql);
            if (DSQL_SUCCEEDED(result)) {
                dpi_free_stmt(errorStatement);
                throw std::runtime_error("Invalid DPI SQL unexpectedly succeeded.");
            }
            sdint4 sqlErrorCode = 0;
            sdint2 sqlMessageLength = 0;
            std::array<sdbyte, 2048> sqlMessage{};
            const DPIRETURN diagnosticResult = dpi_get_diag_rec(
                DSQL_HANDLE_STMT, errorStatement, 1, &sqlErrorCode, sqlMessage.data(),
                static_cast<sdint2>(sqlMessage.size()), &sqlMessageLength);
            if (!DSQL_SUCCEEDED(diagnosticResult) || sqlErrorCode != -2106) {
                printDiagnostic(DSQL_HANDLE_STMT, errorStatement);
                dpi_free_stmt(errorStatement);
                throw std::runtime_error("Missing-table DPI SQL returned an unexpected error code.");
            }
            dpi_free_stmt(errorStatement);
            std::cout << "SQL_ERROR_CODE=" << sqlErrorCode << '\n';
            std::cout << "SQL_ERROR_REPORTING=PASS\n";

            dhstmt secondStatement = nullptr;
            result = dpi_alloc_stmt(secondConnection, &secondStatement);
            if (!succeeded(result, DSQL_HANDLE_DBC, secondConnection)) {
                throw std::runtime_error("Cannot allocate the second DPI statement.");
            }
            try {
                dropProbeTable(statement, "REID_DPI_PROBE_ISOLATION");
                executeSql(statement,
                    "CREATE TABLE REID_DPI_PROBE_ISOLATION (ID INT PRIMARY KEY, VALUE_NUMBER INT)",
                    "Cannot create the DPI isolation table.");
                result = dpi_commit(connection);
                if (!succeeded(result, DSQL_HANDLE_DBC, connection)) {
                    throw std::runtime_error("Cannot commit the DPI isolation fixture.");
                }
                executeSql(statement, "INSERT INTO REID_DPI_PROBE_ISOLATION VALUES (1, 10)",
                    "Cannot insert the uncommitted DPI isolation row.");
                if (queryInt(secondStatement,
                        "SELECT COUNT(*) FROM REID_DPI_PROBE_ISOLATION") != 0) {
                    throw std::runtime_error("Second DPI connection observed an uncommitted insert.");
                }
                result = dpi_rollback(connection);
                if (!succeeded(result, DSQL_HANDLE_DBC, connection)) {
                    throw std::runtime_error("Cannot rollback the first DPI isolation transaction.");
                }
                executeSql(statement, "INSERT INTO REID_DPI_PROBE_ISOLATION VALUES (2, 20)",
                    "Cannot insert the committed DPI isolation row.");
                result = dpi_commit(connection);
                if (!succeeded(result, DSQL_HANDLE_DBC, connection)
                        || queryInt(secondStatement,
                            "SELECT COUNT(*) FROM REID_DPI_PROBE_ISOLATION WHERE ID = 2") != 1) {
                    throw std::runtime_error("Second DPI connection did not observe a committed insert.");
                }
                executeSql(secondStatement,
                    "DELETE FROM REID_DPI_PROBE_ISOLATION WHERE ID = 2",
                    "Cannot delete the DPI isolation row on the second connection.");
                result = dpi_rollback(secondConnection);
                if (!succeeded(result, DSQL_HANDLE_DBC, secondConnection)
                        || queryInt(statement,
                            "SELECT COUNT(*) FROM REID_DPI_PROBE_ISOLATION WHERE ID = 2") != 1) {
                    throw std::runtime_error("Second DPI rollback affected the first connection.");
                }
                dpi_rollback(secondConnection);
                dropProbeTable(statement, "REID_DPI_PROBE_ISOLATION");
            } catch (...) {
                dpi_rollback(connection);
                dpi_rollback(secondConnection);
                dropProbeTable(statement, "REID_DPI_PROBE_ISOLATION");
                dpi_free_stmt(secondStatement);
                throw;
            }
            dpi_free_stmt(secondStatement);
            std::cout << "TWO_PHYSICAL_CONNECTIONS=PASS\n";
            std::cout << "TRANSACTION_ISOLATION=PASS\n";
            std::cout << "STAGE_ONE_DPI=PASS\n";
        }

        if (stageThreeProbe) {
            result = dpi_set_con_attr(connection, DSQL_ATTR_AUTOCOMMIT,
                reinterpret_cast<dpointer>(DSQL_AUTOCOMMIT_OFF), 0);
            if (DSQL_SUCCEEDED(result)) {
                result = dpi_set_con_attr(secondConnection, DSQL_ATTR_AUTOCOMMIT,
                    reinterpret_cast<dpointer>(DSQL_AUTOCOMMIT_OFF), 0);
            }
            if (!succeeded(result, DSQL_HANDLE_DBC, connection)) {
                throw std::runtime_error("Cannot disable auto-commit for stage three.");
            }
            dropProbeTable(statement, "REID_STAGE_THREE_NO_KEY");
            dropProbeTable(statement, "REID_STAGE_THREE_DATA");
            try {
                executeSql(statement,
                    "CREATE TABLE REID_STAGE_THREE_DATA "
                    "(ID INT PRIMARY KEY, NAME VARCHAR(200) DEFAULT 'ROW')",
                    "Cannot create the stage-three data table.");
                executeSql(statement,
                    "CREATE TABLE REID_STAGE_THREE_NO_KEY (ID INT, NAME VARCHAR(200))",
                    "Cannot create the stage-three no-key table.");
                executeSql(statement, "INSERT INTO REID_STAGE_THREE_NO_KEY VALUES (1, 'NO-KEY')",
                    "Cannot insert the no-key row.");

                dhstmt batchStatement = nullptr;
                result = dpi_alloc_stmt(connection, &batchStatement);
                const ulength batchRows = 10000;
                std::vector<sdint4> values(batchRows);
                std::vector<slength> indicators(batchRows, sizeof(sdint4));
                for (ulength index = 0; index < batchRows; ++index) {
                    values[index] = static_cast<sdint4>(index + 1);
                }
                sdbyte insert[] = "INSERT INTO REID_STAGE_THREE_DATA (ID) VALUES (?)";
                if (DSQL_SUCCEEDED(result)) result = dpi_set_stmt_attr(batchStatement,
                    DSQL_ATTR_PARAMSET_SIZE, reinterpret_cast<dpointer>(batchRows), sizeof(batchRows));
                if (DSQL_SUCCEEDED(result)) result = dpi_prepare(batchStatement, insert);
                if (DSQL_SUCCEEDED(result)) result = dpi_bind_param(batchStatement, 1,
                    DSQL_PARAM_INPUT, DSQL_C_SLONG, DSQL_INT, sizeof(sdint4), 0, values.data(),
                    sizeof(sdint4), indicators.data());
                if (DSQL_SUCCEEDED(result)) result = dpi_exec(batchStatement);
                if (!succeeded(result, DSQL_HANDLE_STMT, batchStatement)) {
                    dpi_free_stmt(batchStatement);
                    throw std::runtime_error("Cannot insert the stage-three 10,000-row fixture.");
                }
                dpi_free_stmt(batchStatement);
                result = dpi_commit(connection);
                if (!succeeded(result, DSQL_HANDLE_DBC, connection)) {
                    throw std::runtime_error("Cannot commit the stage-three fixture.");
                }

                if (queryInt(statement, "SELECT COUNT(*) FROM REID_STAGE_THREE_DATA") != 10000) {
                    throw std::runtime_error("Stage-three fixture row count differed.");
                }
                if (queryInt(statement,
                        "SELECT COUNT(*) FROM ALL_CONSTRAINTS WHERE TABLE_NAME="
                        "'REID_STAGE_THREE_DATA' AND CONSTRAINT_TYPE='P'") != 1
                    || queryInt(statement,
                        "SELECT COUNT(*) FROM ALL_CONSTRAINTS WHERE TABLE_NAME="
                        "'REID_STAGE_THREE_NO_KEY' AND CONSTRAINT_TYPE IN ('P','U')") != 0) {
                    throw std::runtime_error("Stage-three stable-key metadata differed.");
                }

                executeSql(statement, "SELECT * FROM REID_STAGE_THREE_DATA WHERE ID=1 FOR UPDATE",
                    "Cannot execute the stage-three FOR UPDATE query.");
                result = dpi_fetch(statement, &rowsFetched);
                if (!DSQL_SUCCEEDED(result)) {
                    throw std::runtime_error("Cannot fetch the locked stage-three row.");
                }
                dpi_close_cursor(statement);

                dhstmt updateStatement = nullptr;
                result = dpi_alloc_stmt(connection, &updateStatement);
                sdbyte update[] = "UPDATE REID_STAGE_THREE_DATA SET NAME=? WHERE ID=?";
                std::string updated = "UPDATED";
                sdint4 id = 1;
                slength nameIndicator = static_cast<slength>(updated.size());
                slength idIndicator = sizeof(id);
                if (DSQL_SUCCEEDED(result)) result = dpi_prepare(updateStatement, update);
                if (DSQL_SUCCEEDED(result)) result = dpi_bind_param(updateStatement, 1,
                    DSQL_PARAM_INPUT, DSQL_C_NCHAR, DSQL_VARCHAR, updated.size(), 0,
                    updated.data(), updated.size(), &nameIndicator);
                if (DSQL_SUCCEEDED(result)) result = dpi_bind_param(updateStatement, 2,
                    DSQL_PARAM_INPUT, DSQL_C_SLONG, DSQL_INT, sizeof(id), 0,
                    &id, sizeof(id), &idIndicator);
                if (DSQL_SUCCEEDED(result)) result = dpi_exec(updateStatement);
                sdint8 affected = -1;
                if (DSQL_SUCCEEDED(result)) result = dpi_row_count(updateStatement, &affected);
                const bool updatedSuccessfully = succeeded(
                    result, DSQL_HANDLE_STMT, updateStatement) && affected == 1;
                dpi_free_stmt(updateStatement);
                if (!updatedSuccessfully) {
                    throw std::runtime_error("Parameterized stage-three update differed.");
                }
                dpi_commit(connection);
                if (queryInt(statement,
                        "SELECT COUNT(*) FROM REID_STAGE_THREE_DATA WHERE ID=1 AND NAME='UPDATED'") != 1) {
                    throw std::runtime_error("Committed stage-three update was not visible.");
                }

                dhstmt deleteStatement = nullptr;
                result = dpi_alloc_stmt(connection, &deleteStatement);
                sdbyte deleteSql[] = "DELETE FROM REID_STAGE_THREE_DATA WHERE ID=?";
                sdint4 deleteId = 4;
                slength deleteIdIndicator = sizeof(deleteId);
                if (DSQL_SUCCEEDED(result)) result = dpi_prepare(deleteStatement, deleteSql);
                if (DSQL_SUCCEEDED(result)) result = dpi_bind_param(deleteStatement, 1,
                    DSQL_PARAM_INPUT, DSQL_C_SLONG, DSQL_INT, sizeof(deleteId), 0,
                    &deleteId, sizeof(deleteId), &deleteIdIndicator);
                if (DSQL_SUCCEEDED(result)) result = dpi_exec(deleteStatement);
                affected = -1;
                if (DSQL_SUCCEEDED(result)) result = dpi_row_count(deleteStatement, &affected);
                const bool deletedSuccessfully = succeeded(
                    result, DSQL_HANDLE_STMT, deleteStatement) && affected == 1;
                dpi_free_stmt(deleteStatement);
                if (!deletedSuccessfully) {
                    throw std::runtime_error("Parameterized stage-three delete differed.");
                }
                dpi_commit(connection);
                if (queryInt(statement,
                        "SELECT COUNT(*) FROM REID_STAGE_THREE_DATA WHERE ID=4") != 0) {
                    throw std::runtime_error("Committed stage-three delete was not visible.");
                }
                executeSql(statement,
                    "UPDATE REID_STAGE_THREE_DATA SET NAME='ROLLBACK' WHERE ID=2",
                    "Cannot execute the stage-three rollback update.");
                dpi_rollback(connection);
                if (queryInt(statement,
                        "SELECT COUNT(*) FROM REID_STAGE_THREE_DATA WHERE ID=2 AND NAME='ROW'") != 1) {
                    throw std::runtime_error("Stage-three rollback did not restore the row.");
                }
                std::cout << "STAGE_THREE_DPI_FOR_UPDATE=PASS\n";
                std::cout << "STAGE_THREE_DPI_PARAMETER_UPDATE=PASS\n";
                std::cout << "STAGE_THREE_DPI_PARAMETER_DELETE=PASS\n";
                std::cout << "STAGE_THREE_DPI_ROLLBACK=PASS\n";
                std::cout << "STAGE_THREE_DPI_FETCH_10000=PASS\n";
                std::cout << "STAGE_THREE_DPI_NO_KEY_READ_ONLY=PASS\n";
            } catch (...) {
                dpi_rollback(connection);
                dropProbeTable(statement, "REID_STAGE_THREE_NO_KEY");
                dropProbeTable(statement, "REID_STAGE_THREE_DATA");
                throw;
            }
            dropProbeTable(statement, "REID_STAGE_THREE_NO_KEY");
            dropProbeTable(statement, "REID_STAGE_THREE_DATA");
            if (queryInt(statement,
                    "SELECT COUNT(*) FROM ALL_OBJECTS WHERE OBJECT_NAME LIKE 'REID_STAGE_THREE_%'") != 0) {
                throw std::runtime_error("Stage-three DPI cleanup left objects behind.");
            }
            std::cout << "STAGE_THREE_DPI_CLEANUP=PASS\n";
        }

        std::cout << "ENDPOINT=" << host << ':' << port << '\n';
        std::cout << "QUERY_SELECT_1=PASS\n";
        std::cout << "RESULT=PASS\n";
        exitCode = 0;
    } catch (const std::exception& error) {
        std::cerr << "RESULT=FAIL\n";
        std::cerr << "ERROR=" << error.what() << '\n';
    }

    if (statement != nullptr) {
        dpi_free_stmt(statement);
    }
    if (loggedIn) {
        dpi_logout(connection);
    }
    if (secondLoggedIn) {
        dpi_logout(secondConnection);
    }
    if (secondConnection != nullptr) {
        dpi_free_con(secondConnection);
    }
    if (connection != nullptr) {
        dpi_free_con(connection);
    }
    if (environment != nullptr) {
        dpi_free_env(environment);
    }
    return exitCode;
}
