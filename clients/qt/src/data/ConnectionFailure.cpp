#include "ConnectionFailure.h"

#include <QAbstractSocket>
#include <QTcpSocket>

#include <algorithm>
#include <utility>

namespace {

bool containsAny(const QString& text, const QStringList& terms) {
    return std::any_of(terms.cbegin(), terms.cend(), [&text](const QString& term) {
        return text.contains(term, Qt::CaseInsensitive);
    });
}

SessionError classified(SessionError error, QString category, QString summary,
        QString recovery) {
    const QString native = error.message.trimmed();
    const QString code = error.code == 0
        ? QStringLiteral("未提供错误码") : QString::number(error.code);
    error.category = std::move(category);
    error.message = QStringLiteral("%1\n原始错误（%2）：%3")
        .arg(summary, code, native.isEmpty()
            ? QStringLiteral("DPI 未返回详细错误信息。") : native);
    error.recovery = std::move(recovery);
    return error;
}

}  // namespace

EndpointProbeResult ConnectionFailure::probe(
        const QString& host, int port, int timeoutMs) {
    QTcpSocket socket;
    socket.connectToHost(host.trimmed(), static_cast<quint16>(port));
    if (socket.waitForConnected(qBound(250, timeoutMs, 10'000))) {
        socket.abort();
        return {EndpointProbeStatus::Reachable, {}};
    }
    const QString detail = socket.errorString().trimmed();
    switch (socket.error()) {
    case QAbstractSocket::HostNotFoundError:
        return {EndpointProbeStatus::HostNotFound, detail};
    case QAbstractSocket::ConnectionRefusedError:
        return {EndpointProbeStatus::ConnectionRefused, detail};
    case QAbstractSocket::SocketTimeoutError:
        return {EndpointProbeStatus::TimedOut, detail};
    case QAbstractSocket::NetworkError:
    case QAbstractSocket::SocketAddressNotAvailableError:
        return {EndpointProbeStatus::NetworkUnavailable, detail};
    case QAbstractSocket::SocketAccessError:
        return {EndpointProbeStatus::AccessDenied, detail};
    default:
        return {EndpointProbeStatus::Unknown, detail};
    }
}

SessionError ConnectionFailure::classify(SessionError error,
        const QString& host, int port, const EndpointProbeResult& probeResult) {
    const QString endpoint = QStringLiteral("%1:%2").arg(host).arg(port);
    const QString native = error.message.toCaseFolded();

    if (containsAny(native, {QStringLiteral("账号锁定"), QStringLiteral("账户锁定"),
            QStringLiteral("account locked"), QStringLiteral("user locked")})) {
        return classified(std::move(error), QStringLiteral("AccountLocked"),
            QStringLiteral("账号已锁定，数据库拒绝登录。"),
            QStringLiteral("请联系数据库管理员解锁账号，并检查失败登录次数限制。"));
    }
    if (containsAny(native, {QStringLiteral("密码过期"),
            QStringLiteral("password expired"), QStringLiteral("password expiry")})) {
        return classified(std::move(error), QStringLiteral("PasswordExpired"),
            QStringLiteral("密码已过期，数据库拒绝登录。"),
            QStringLiteral("请使用管理账号重置密码或调整账号密码有效期。"));
    }
    if (error.code == -70073 || containsAny(native, {QStringLiteral("kerberos"),
            QStringLiteral("证书"), QStringLiteral("certificate"),
            QStringLiteral("ukey")})) {
        return classified(std::move(error), QStringLiteral("AuthenticationMethod"),
            QStringLiteral("认证方式或认证材料不正确。"),
            QStringLiteral("检查 Kerberos、证书、UKey 和服务端认证配置。"));
    }
    const bool combinedCredential = containsAny(native, {
        QStringLiteral("用户名或密码"), QStringLiteral("用户或密码"),
        QStringLiteral("user or password"), QStringLiteral("username or password"),
        QStringLiteral("invalid user or password")});
    if (!combinedCredential && containsAny(native, {QStringLiteral("用户名不存在"),
            QStringLiteral("无效的用户名"), QStringLiteral("invalid user"),
            QStringLiteral("unknown user"), QStringLiteral("user does not exist")})) {
        return classified(std::move(error), QStringLiteral("Username"),
            QStringLiteral("用户名不正确或账号不存在。"),
            QStringLiteral("检查用户名、大小写和账号状态后重试。"));
    }
    if (!combinedCredential && containsAny(native, {QStringLiteral("密码错误"),
            QStringLiteral("无效的密码"), QStringLiteral("invalid password"),
            QStringLiteral("wrong password")})) {
        return classified(std::move(error), QStringLiteral("Password"),
            QStringLiteral("密码不正确。"),
            QStringLiteral("重新输入密码；注意大小写和特殊字符。"));
    }
    if (combinedCredential || containsAny(native, {QStringLiteral("authentication failed"),
            QStringLiteral("认证失败"), QStringLiteral("登录验证失败")})) {
        return classified(std::move(error), QStringLiteral("Authentication"),
            QStringLiteral("用户名或密码不正确。"),
            QStringLiteral("检查用户名、密码、大小写和账号状态后重试。"));
    }
    switch (probeResult.status) {
    case EndpointProbeStatus::HostNotFound:
        return classified(std::move(error), QStringLiteral("Host"),
            QStringLiteral("主机解析失败：无法找到 %1。").arg(host),
            QStringLiteral("检查主机名或 IP 地址；使用主机名时检查 DNS。"));
    case EndpointProbeStatus::ConnectionRefused:
        return classified(std::move(error), QStringLiteral("Port"),
            QStringLiteral("端口拒绝连接：%1 未接受 TCP 连接。").arg(endpoint),
            QStringLiteral("检查端口号、达梦服务是否启动及服务器防火墙监听规则。"));
    case EndpointProbeStatus::TimedOut:
        return classified(std::move(error), QStringLiteral("NetworkTimeout"),
            QStringLiteral("网络连接超时：无法在限定时间内连接 %1。").arg(endpoint),
            QStringLiteral("检查路由、VPN、安全组、防火墙和目标主机状态。"));
    case EndpointProbeStatus::NetworkUnavailable:
        return classified(std::move(error), QStringLiteral("Network"),
            QStringLiteral("网络不可达：当前计算机无法访问 %1。").arg(endpoint),
            QStringLiteral("检查本机网络、路由、VPN 和服务器网络配置。"));
    case EndpointProbeStatus::AccessDenied:
        return classified(std::move(error), QStringLiteral("NetworkPolicy"),
            QStringLiteral("本机安全策略阻止访问 %1。").arg(endpoint),
            QStringLiteral("检查本机防火墙、安全软件和网络访问策略。"));
    case EndpointProbeStatus::Reachable:
        if (containsAny(native, {QStringLiteral("最大连接"),
                QStringLiteral("max session"), QStringLiteral("too many connections")})) {
            return classified(std::move(error), QStringLiteral("SessionLimit"),
                QStringLiteral("数据库连接数已达到限制。"),
                QStringLiteral("等待空闲会话或请管理员检查 MAX_SESSIONS 和账号连接数限制。"));
        }
        if ((error.code == -70019 || error.code == -70028)
                && containsAny(native, {QStringLiteral("会话数"),
                    QStringLiteral("连接数"), QStringLiteral("许可证"),
                    QStringLiteral("session capacity"),
                    QStringLiteral("connection limit"),
                    QStringLiteral("license connection")})) {
            return classified(std::move(error), QStringLiteral("SessionCapacity"),
                QStringLiteral("端口可访问，但数据库未接受新的会话。"),
                QStringLiteral("优先关闭未使用的数据库窗口或重复启动的应用实例；"
                    "请管理员检查 MAX_SESSIONS、SESSION_PER_USER 和许可证连接数。"
                    "若会话容量充足，再检查该端口的达梦服务、登录加密配置和客户端版本。"));
        }
        if (error.code == -70030 || containsAny(native, {
                QStringLiteral("不支持的服务器版本"),
                QStringLiteral("unsupported server version")})) {
            return classified(std::move(error), QStringLiteral("DriverVersion"),
                QStringLiteral("达梦客户端驱动与服务器版本不兼容。"),
                QStringLiteral("使用与目标数据库服务器版本匹配的达梦客户端运行库。"));
        }
        return classified(std::move(error), QStringLiteral("DatabaseHandshake"),
            QStringLiteral("端口可访问，但达梦登录握手失败。"),
            QStringLiteral("确认该端口确为达梦服务，并检查用户名、密码、数据库状态、"
                "登录加密配置和客户端版本兼容性。"));
    case EndpointProbeStatus::Unknown:
        return classified(std::move(error), QStringLiteral("Network"),
            QStringLiteral("无法确认 %1 的网络状态：%2。")
                .arg(endpoint, probeResult.detail.isEmpty()
                    ? QStringLiteral("未知网络错误") : probeResult.detail),
            QStringLiteral("检查主机、端口、网络和本机安全策略后重试。"));
    case EndpointProbeStatus::NotChecked:
        break;
    }
    return classified(std::move(error), QStringLiteral("Connection"),
        QStringLiteral("数据库连接失败。"),
        QStringLiteral("根据原始错误码检查连接配置和数据库服务状态。"));
}
