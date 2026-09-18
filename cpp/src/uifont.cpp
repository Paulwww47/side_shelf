// -*- coding: utf-8 -*-
// Side Shelf（侧边暂存栏）—— 全界面字体来源（实现见 uifont.h）

#include "uifont.h"

#include <QApplication>

namespace UiFont {

QStringList preferredFamilies()
{
    // 首选英文名（Qt 字体库在中文系统上同时登记了英文名与本地化名，两者都放进来
    // 以覆盖不同语言环境），最后以 Segoe UI 兜底。
    return {QStringLiteral("Microsoft YaHei"), QStringLiteral("微软雅黑"),
            QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Segoe UI")};
}

QFont::Weight preferredWeight()
{
    return QFont::Light;  // 300，命中「微软雅黑 Light」字面
}

void applyStyle(QFont &font)
{
    font.setFamilies(preferredFamilies());
    font.setWeight(preferredWeight());
}

QFont make(int pointSize, QFont::Weight weight)
{
    QFont font;
    applyStyle(font);
    if (pointSize > 0)
        font.setPointSize(pointSize);
    font.setWeight(weight);
    return font;
}

void applyToApplication()
{
    QFont font = QApplication::font();  // 保留系统默认字号
    applyStyle(font);
    QApplication::setFont(font);
}

QFont withWeight(QFont::Weight weight)
{
    QFont font = QApplication::font();  // 字族链与字号跟随应用级默认字体
    applyStyle(font);
    font.setWeight(weight);
    return font;
}

}  // namespace UiFont
