// -*- coding: utf-8 -*-
// Side Shelf（侧边暂存栏）—— 全界面字体来源
//
// 界面文字统一使用「微软雅黑 Light」。
//
// 注意（实测）：`Microsoft YaHei Light` / `微软雅黑 Light` 在本机 Qt 里**不是**
// 独立字族，按该名字请求会静默回退到 Tahoma；Light 是 `Microsoft YaHei` 字族的
// 一个字面（QFontDatabase::styles("Microsoft YaHei") == Regular / Light / Bold），
// 必须用 QFont::Light(300) 才能命中真正的 Light 字形。因此这里把「字族链 + 字重」
// 一起收口，任何需要指定字体样式的地方都必须经由本模块，避免再次出现
// 「设了字体名却悄悄落到别的字体」的情况。

#pragma once

#include <QFont>
#include <QStringList>

namespace UiFont {

// 字族优选链（按顺序回退）：
// Microsoft YaHei → 微软雅黑 → Microsoft YaHei UI → Segoe UI
QStringList preferredFamilies();

// 统一字重：Light(300)。只有它是 Light 时才会命中微软雅黑 Light 字面。
QFont::Weight preferredWeight();

// 套用统一界面字体样式（字族链 + Light 字重），字号等其它设定保持不变。
void applyStyle(QFont &font);

// 新建一个使用统一界面字体样式的字体（pointSize 为磅值，<= 0 表示不指定）
QFont make(int pointSize, QFont::Weight weight = QFont::Light);

// 以应用级默认字体为基准，仅替换字重（字族链与字号不变）。
// 用于个别位置单独指定字重：例如右键菜单用 Regular、把手徽标用 Bold，
// 界面其余文字仍保持 Light。
QFont withWeight(QFont::Weight weight);

// 设置应用级默认字体：右键菜单、气泡提示（Tooltip）、消息框等所有控件跟随。
void applyToApplication();

}  // namespace UiFont
