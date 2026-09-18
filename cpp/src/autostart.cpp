// -*- coding: utf-8 -*-
// 开机自启：HKCU\Software\Microsoft\Windows\CurrentVersion\Run 键的读写。
// 值名 SideShelf，REG_SZ，数据为带引号的 exe 绝对路径。
// 全程宽字符 API：路径含中文 / 空格安全；HKCU 写入无需管理员权限。

#include "autostart.h"

#include <QCoreApplication>
#include <QDir>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

// 读取 REG_SZ 值；值不存在 / 类型不符 / 读失败返回空串
QString readStringValue(HKEY key, const wchar_t *name)
{
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes)
            != ERROR_SUCCESS
        || type != REG_SZ || bytes < sizeof(wchar_t))
        return QString();
    std::wstring buf(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(buf.data()), &bytes)
        != ERROR_SUCCESS)
        return QString();
    buf.back() = L'\0';  // 注册表值不保证 null 终止
    return QString::fromWCharArray(buf.c_str());
}

} // namespace

namespace AutoStart {

QString quotedCommand(const QString &exePath)
{
    return QStringLiteral("\"") + QDir::toNativeSeparators(exePath)
           + QStringLiteral("\"");
}

bool isEnabled(const std::wstring &subKey)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, KEY_READ, &key)
        != ERROR_SUCCESS)
        return false;  // 键不存在（如测试子键未创建）视为未启用
    const bool present =
        RegQueryValueExW(key, VALUE_NAME.c_str(), nullptr, nullptr, nullptr,
                         nullptr)
        == ERROR_SUCCESS;
    RegCloseKey(key);
    return present;
}

QString registeredPath(const std::wstring &subKey)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, KEY_READ, &key)
        != ERROR_SUCCESS)
        return QString();
    QString cmd = readStringValue(key, VALUE_NAME.c_str());
    RegCloseKey(key);
    // 去掉首尾空白与引号（本程序写入的格式是 "完整路径"）
    cmd = cmd.trimmed();
    if (cmd.size() >= 2 && cmd.startsWith(QLatin1Char('"'))
        && cmd.endsWith(QLatin1Char('"')))
        cmd = cmd.mid(1, cmd.size() - 2);
    return cmd;
}

bool setEnabled(bool on, QString *errMsg, const std::wstring &subKey)
{
    HKEY key = nullptr;
    const LSTATUS opened = RegCreateKeyExW(
        HKEY_CURRENT_USER, subKey.c_str(), 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, nullptr, &key, nullptr);
    if (opened != ERROR_SUCCESS) {
        if (errMsg)
            *errMsg = QStringLiteral("打开注册表失败（错误码 %1）")
                          .arg(int(opened));
        return false;
    }
    LSTATUS rc = ERROR_SUCCESS;
    if (on) {
        const std::wstring cmd =
            quotedCommand(QCoreApplication::applicationFilePath()).toStdWString();
        rc = RegSetValueExW(key, VALUE_NAME.c_str(), 0, REG_SZ,
                            reinterpret_cast<const BYTE *>(cmd.c_str()),
                            DWORD((cmd.size() + 1) * sizeof(wchar_t)));
        if (rc != ERROR_SUCCESS && errMsg)
            *errMsg = QStringLiteral("写入注册表失败（错误码 %1）").arg(int(rc));
    } else {
        rc = RegDeleteValueW(key, VALUE_NAME.c_str());
        if (rc == ERROR_FILE_NOT_FOUND)
            rc = ERROR_SUCCESS;  // 值本来就不存在：视为已关闭
        else if (rc != ERROR_SUCCESS && errMsg)
            *errMsg = QStringLiteral("删除注册表值失败（错误码 %1）").arg(int(rc));
    }
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

bool deleteSubkeyForTest(const std::wstring &subKey)
{
    return RegDeleteKeyW(HKEY_CURRENT_USER, subKey.c_str()) == ERROR_SUCCESS;
}

} // namespace AutoStart
