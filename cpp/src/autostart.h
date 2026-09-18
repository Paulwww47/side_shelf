// -*- coding: utf-8 -*-
// 开机自启（HKCU Run 注册表键读写）。
//
// 设计：注册表是唯一事实来源——右键菜单每次打开时实时读取勾选状态，
// 不把该状态写进 side_shelf_state.json，避免与真实注册表状态漂移。

#pragma once

#include <QString>
#include <string>

namespace AutoStart {

// 真实 Run 键（HKCU 下相对路径）与注册值名
inline const std::wstring RUN_KEY =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline const std::wstring VALUE_NAME = L"SideShelf";

// 生成写入注册表的启动命令行：转 native 分隔符并整体加英文引号
//（路径可含空格 / 中文）
QString quotedCommand(const QString &exePath);

// Run 键下是否已存在 SideShelf 值（键 / 值不存在或读失败一律视为未启用）
bool isEnabled(const std::wstring &subKey = RUN_KEY);

// 解析注册值得到 exe 路径（去引号与空白，native 分隔符）；未启用返回空串。
// 供启动自愈与 selftest 校验使用。
QString registeredPath(const std::wstring &subKey = RUN_KEY);

// on=true：写入当前 exe 的带引号路径；on=false：删除值（值本不存在视为成功）。
// 失败返回 false 并在 *errMsg（非空时）给出中文错误说明。
bool setEnabled(bool on, QString *errMsg = nullptr,
                const std::wstring &subKey = RUN_KEY);

// 自检清理：删除 HKCU 下整个子键。仅供测试子键使用，勿传真实 Run 键。
bool deleteSubkeyForTest(const std::wstring &subKey);

} // namespace AutoStart
