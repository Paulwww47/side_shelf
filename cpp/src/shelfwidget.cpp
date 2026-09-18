// -*- coding: utf-8 -*-
// Side Shelf（侧边暂存栏）—— C++20 + Qt6 移植版
// 逐段对应原 Python/PySide6 版 side_shelf.py 的行为。

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "autostart.h"
#include "glcanvas.h"
#include "itempreviewpanel.h"
#include "shelfwidget.h"
#include "uifont.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCryptographicHash>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QDrag>
#include <QEasingCurve>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLinearGradient>
#include <QLineF>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPropertyAnimation>
#include <QRegion>
#include <QScreen>
#include <QStringConverter>
#include <QTextStream>
#include <QTimer>
#include <QToolTip>
#include <QUrl>

#include <cmath>
#include <cwchar>

// ---- 颜色：灰蓝把手、暖白按钮与柔和中性阴影 ----
const QColor ShelfWidget::COLOR_GREEN(96, 202, 124);
const QColor ShelfWidget::COLOR_RED(232, 106, 90);
const QColor ShelfWidget::COLOR_NEUTRAL(152, 160, 172);

namespace {
const QColor BODY_TOP(97, 116, 139);       // #61748B
const QColor BODY_BOTTOM(97, 116, 139);    // 平涂灰蓝，与数字预览保持一致
const QColor BUTTON_FILL(255, 253, 248);  // #FFFDF8
const QColor OUTLINE(86, 100, 115, 64);    // 柔和的 1px 中性描边
const QColor ICON_COLOR(97, 116, 139);     // 暖白按钮上的灰蓝图标

// 把手胶囊路径（两端全圆角），cy 为窗口垂直中心
QPainterPath capsulePath(double cy)
{
    QPainterPath path;
    path.addRoundedRect(
        QRectF(ShelfWidget::FEATHER, cy - ShelfWidget::HANDLE_H / 2.0,
               ShelfWidget::HANDLE_W, ShelfWidget::HANDLE_H),
        ShelfWidget::HANDLE_W / 2.0, ShelfWidget::HANDLE_W / 2.0);
    return path;
}

// 使用数字实际字形的边界居中，避免不同字体的行高使数字看起来偏上或偏下。
void drawCount(QPainter &p, const QRectF &rect, int count, double reveal = 1.0)
{
    if (count <= 0 || reveal <= 0.001)
        return;
    const QString label = count > 99 ? QStringLiteral("99+")
                                     : QString::number(count);
    QFont font;
    UiFont::applyStyle(font);  // 微软雅黑 Light（缺失时按回退链）
    // 徽标是唯一刻意加粗的位置：Light 字重画在 11~13px 的灰蓝把手上偏细、
    // 远看发虚，这里单独用同字族的 Bold 字面（真 Bold，非合成加粗）；
    // 界面其余文字仍保持 Light。
    font.setWeight(QFont::Bold);
    font.setPixelSize(count > 99 ? 11 : 13);
    const QRectF ink = QFontMetricsF(font).tightBoundingRect(label);
    p.save();
    p.setFont(font);
    p.setPen(QColor(255, 255, 255, qRound(255 * reveal)));
    p.drawText(rect.center() - ink.center(), label);
    p.restore();
}

// 暂存项路径的唯一规范形式：绝对路径 + 原生分隔符（Windows 下 '\'）+ 小写。
// m_items / m_pathSet / 单项删除查找键必须全部经此规范化。历史 bug：addImage
// 直接存 QDir::absoluteFilePath()（Qt 内部统一使用 '/'），而 takePreviewItem 的
// 查找键经 toNativeSeparators() 变成 '\'，两者永不相等，于是剪贴板图片的右键
// 删除静默失效（本地文件条目两处都是 '\'，所以看不出问题）。
QString canonPath(const QString &path)
{
    if (path.isEmpty())
        return QString();
    return QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath())
        .toCaseFolded();
}

// filePath 与 image 是否为同一张图。按解码后的像素比较，不按字节：暂存副本是
// Qt 重新编码的 PNG，与源文件字节必然不同。用于同一次剪贴板里「%TEMP% 文件 +
// 位图」的跨类型去重（微信 / 浏览器复制图片的形态）。
bool sameImageAsFile(const QString &filePath, const QImage &image)
{
    if (image.isNull() || filePath.isEmpty())
        return false;
    const QFileInfo fi(filePath);
    if (!fi.isFile() || fi.size() <= 0)
        return false;
    constexpr qint64 kMaxBytes = 64LL * 1024 * 1024;  // 超大文件不值得解码对比
    if (fi.size() > kMaxBytes)
        return false;
    QImageReader reader(filePath);
    reader.setAutoTransform(true);
    const QImage other = reader.read();
    if (other.isNull())
        return false;
    return other.convertToFormat(QImage::Format_ARGB32)
        == image.convertToFormat(QImage::Format_ARGB32);
}
} // namespace

ShelfWidget::ShelfWidget(const QString &stateFile, const QString &stashDir,
                         const QString &logFile, QWidget *parent)
    : QWidget(parent,
              Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
    , m_stateFile(stateFile)
    , m_stashDir(stashDir)
    , m_logFile(logFile)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_AlwaysShowToolTips, true);
    setAcceptDrops(true);
    setMouseTracking(true);
    setWindowTitle(QStringLiteral("Side Shelf"));
    setFixedSize(W, H);

    // 暂存内容不跨进程保留：启动时清掉上次遗留的剪贴板图片副本
    QDir().mkpath(m_stashDir);
    const QDir stash(m_stashDir);
    const QStringList leftovers = stash.entryList(QDir::Files);
    for (const QString &name : leftovers) {
        const QString p = stash.absoluteFilePath(name);
        if (QFileInfo::exists(p))
            QFile::remove(p);
    }

    loadState();

    QRect g = QGuiApplication::primaryScreen()->geometry();
    m_screenGeo = g;
    m_screenRight = g.right();
    if (m_savedY >= 0) {
        m_y = std::max(g.top(),
                       std::min(g.bottom() - H + 1, int(std::lround(m_savedY))));
    } else {
        m_y = g.top() + (g.height() - H) / 2;
    }
    m_xShown = m_screenRight - W + 1;
    m_xHidden = m_screenRight - HIDE + 1;
    setGeometry(m_xHidden, m_y, W, H);
    // 透明度不再作用于整窗（windowOpacity 会让描边/文字一起发灰），
    // 而是由 paintContents 把 m_opacity 映射为主体填充色的 alpha，
    // 描边、图标与数字始终保持不透明。
    // 注意：不再使用 setMask 二值遮罩（其折线化/像素量化会造成边缘锯齿），
    // 平滑边缘完全依靠 WA_TranslucentBackground 的逐像素透明 + 抗锯齿绘制；
    // paintEvent 内部用 shapePath() 裁剪，全透明像素由系统自动点击穿透。

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, [this] { poll(); });
    m_timer->start(40);

    // 自动监测剪贴板：每次启动默认关闭
    m_autoMonitor = false;
    m_clipTimer = new QTimer(this);
    connect(m_clipTimer, &QTimer::timeout, this, [this] { pollClipboard(); });
    m_clipTimer->start(400);
}

// ---------- 状态持久化（只存透明度与位置，不存暂存内容）----------

void ShelfWidget::loadState()
{
    m_opacity = 0.5;
    m_savedY = -1;
    QFile f(m_stateFile);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QByteArray raw = f.readAll();
    f.close();
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return;
    const QJsonObject stateData = doc.object();
    m_opacity = stateData.value(QStringLiteral("opacity")).toDouble(0.5);
    m_opacity = std::max(0.15, std::min(1.0, m_opacity));
    if (stateData.contains(QStringLiteral("y")))
        m_savedY = stateData.value(QStringLiteral("y")).toDouble(-1);
}

void ShelfWidget::saveState()
{
    QJsonObject stateData;
    stateData.insert(QStringLiteral("opacity"), m_opacity);
    stateData.insert(QStringLiteral("y"), int(m_y));
    const QByteArray raw =
        QJsonDocument(stateData).toJson(QJsonDocument::Indented);

    QFile tmp(m_stateFile + QStringLiteral(".tmp"));
    if (!tmp.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    tmp.write(raw);
    tmp.close();
    const std::wstring tmpPath =
        (m_stateFile + QStringLiteral(".tmp")).toStdWString();
    const std::wstring statePath = m_stateFile.toStdWString();
    if (!MoveFileExW(tmpPath.c_str(), statePath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        QFile::remove(m_stateFile + QStringLiteral(".tmp"));
    }
}


int ShelfWidget::addFiles(const QStringList &paths)
{
    // 新内容写入前先提交上一项删除，避免同名 stash 副本被复用后
    // 已删除条目又因路径去重逻辑“复活”。
    commitPreviewDelete();
    int added = 0;
    for (const QString &p0 : paths) {
        // 对应 os.path.normcase(os.path.abspath(p))：Windows 上统一分隔符并小写
        const QString p = canonPath(p0);
        if (QFileInfo::exists(p) && QFileInfo(p).isFile()
            && !m_pathSet.contains(p)) {
            ShelfItem it;
            it.kind = QStringLiteral("file");
            it.path = p;
            m_items.append(it);
            m_pathSet.insert(p);
            ++added;
        }
    }
    if (added) {
        refreshPreview();  // 面板打开时原地刷新，避免展示过期列表
        update();
    }
    return added;
}

QStringList ShelfWidget::itemPaths() const
{
    // 自检用：按暂存顺序返回全部条目路径（路径为规范化小写形式）
    QStringList out;
    for (const ShelfItem &it : m_items)
        out.append(it.path);
    return out;
}

bool ShelfWidget::takePreviewItem(const QString &path, ShelfItem *outItem,
                                  int *outIndex)
{
    // 只允许一个待撤销项：再次删除时先提交上一项。
    commitPreviewDelete();

    const QString needle = canonPath(path);
    if (needle.isEmpty())
        return false;
    int found = -1;
    for (int i = 0; i < m_items.size(); ++i) {
        // 两侧都过 canonPath：任一条目若以其它形态（如 '/'）存过，也能被找到
        if (canonPath(m_items.at(i).path).compare(needle, Qt::CaseInsensitive)
            == 0) {
            found = i;
            break;
        }
    }
    if (found < 0)
        return false;

    const ShelfItem item = m_items.takeAt(found);
    m_pathSet.remove(item.path);
    if (!item.sha256.isEmpty())
        m_imageHashes.remove(item.sha256);
    m_pending.active = true;
    m_pending.item = item;
    m_pending.index = found;
    if (outItem)
        *outItem = item;
    if (outIndex)
        *outIndex = found;
    update();
    return true;
}

bool ShelfWidget::undoPreviewDelete(ShelfItem *outItem, int *outIndex)
{
    if (!m_pending.active)
        return false;

    const int index = qBound(0, m_pending.index, m_items.size());
    const ShelfItem item = m_pending.item;
    m_items.insert(index, item);
    m_pathSet.insert(item.path);
    if (!item.sha256.isEmpty())
        m_imageHashes.insert(item.sha256);
    m_pending = PendingDelete();
    if (outItem)
        *outItem = item;
    if (outIndex)
        *outIndex = index;
    update();
    return true;
}

void ShelfWidget::commitPreviewDelete()
{
    if (!m_pending.active)
        return;

    const QString path = m_pending.item.path;
    m_pending = PendingDelete();
    removeStashFileIfOwned(path);
    update();
}

bool ShelfWidget::isInStash(const QString &path) const
{
    if (path.isEmpty())
        return false;
    const QString p = QDir::fromNativeSeparators(
        QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    const QString stash = QDir::fromNativeSeparators(
        QDir::cleanPath(QFileInfo(m_stashDir).absoluteFilePath()));
    if (p.isEmpty() || stash.isEmpty()
        || p.compare(stash, Qt::CaseInsensitive) == 0) {
        return false;
    }
    return p.startsWith(stash + QLatin1Char('/'), Qt::CaseInsensitive);
}

void ShelfWidget::removeStashFileIfOwned(const QString &path)
{
    if (isInStash(path))
        QFile::remove(path);
}

int ShelfWidget::addImage(const QImage &image)
{
    if (image.isNull())
        return 0;
    commitPreviewDelete();
    QBuffer buf;
    buf.open(QIODevice::WriteOnly);
    image.save(&buf, "PNG");
    const QByteArray pngData = buf.data();
    const QString h = QString::fromLatin1(
        QCryptographicHash::hash(pngData, QCryptographicHash::Sha256).toHex());
    if (m_imageHashes.contains(h))
        return 0;
    const QString name = QStringLiteral("clip_%1_%2.png")
                             .arg(QDateTime::currentDateTime()
                                      .toString(QStringLiteral("yyyyMMdd_hhmmss")),
                                  h.left(6));
    const QString path = QDir(m_stashDir).absoluteFilePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        f.close();
        return 0;
    }
    f.write(pngData);
    f.close();
    m_imageHashes.insert(h);
    ShelfItem it;
    it.kind = QStringLiteral("image");
    // 与 addFiles 同一规范形式：删除查找键、m_pathSet、清空/提交删除都依赖它
    it.path = canonPath(path);
    it.sha256 = h;
    m_items.append(it);
    m_pathSet.insert(it.path);
    refreshPreview();
    update();
    return 1;
}

void ShelfWidget::clearItems(bool flash)
{
    commitPreviewDelete();
    // 只删除本程序保存的剪贴板图片副本（位于 stash 目录内），不动用户的原始文件
    for (const ShelfItem &it : m_items) {
        removeStashFileIfOwned(it.path);
    }
    m_items.clear();
    m_pathSet.clear();
    m_imageHashes.clear();
    refreshPreview();
    if (flash)
        this->flash(COLOR_RED);
    update();
}

void ShelfWidget::storeFromClipboard()
{
    // 点击把手：剪贴板中有文件 / 图片等非文字内容时暂存
    const QMimeData *md = QGuiApplication::clipboard()->mimeData();
    const int added = processClipboardContent(md);
    this->flash(added ? COLOR_GREEN : COLOR_NEUTRAL);
}

// ---------- 剪贴板内容处理 ----------

bool ShelfWidget::hasImageFormats(const QMimeData *md) const
{
    // Qt 内部用 application/x-qt-image 表示图片，外部程序常用 image/*（如 image/png）
    if (md->hasImage())
        return true;
    const QStringList fmts = md->formats();
    for (const QString &fmt : fmts) {
        if (fmt.startsWith(QStringLiteral("image/")))
            return true;
    }
    return false;
}

QImage ShelfWidget::extractImage(const QMimeData *md) const
{
    const QImage img = qvariant_cast<QImage>(md->imageData());
    if (!img.isNull())
        return img;
    const QImage img2 = QGuiApplication::clipboard()->image();
    if (!img2.isNull())
        return img2;
    return QImage();
}

int ShelfWidget::processClipboardContent(const QMimeData *md)
{
    // 文件与图片独立判断（互不排斥），返回新增数量。
    // 先取出位图（不落盘）：用于判断同一次复制里的文件是否就是这张图。
    QImage img;
    if (hasImageFormats(md))
        img = extractImage(md);

    // copyIntoStash 可能复用同名同大小副本，而该副本也许正是上一项 pending
    // 删除的对象；与 processDropContent 同理，复制前必须先提交旧 pending。
    if (md->hasUrls())
        commitPreviewDelete();

    int added = 0;
    if (md->hasUrls()) {
        QStringList plainFiles;
        const QList<QUrl> urls = md->urls();
        for (const QUrl &u : urls) {
            if (!u.isLocalFile())
                continue;
            const QString p = u.toLocalFile();
            // 微信等程序复制图片时同时给出 %TEMP% 文件路径与位图，两者是同一张图：
            // 只保留落在 stash 里的位图副本（持久、可右键删除），丢弃随后会被源
            // 程序清理的临时文件路径，避免出现两条一模一样的条目。
            if (sameImageAsFile(p, img))
                continue;
            // 临时目录里的文件（压缩软件里 Ctrl+C 等）先复制进 stash 再登记，
            // 否则源程序清理临时目录后该条目就是一条死路径。
            if (isTransientTempPath(p)) {
                const QString stashed = copyIntoStash(p);
                if (!stashed.isEmpty())
                    added += addFiles({stashed});
            } else {
                plainFiles.append(p);
            }
        }
        if (!plainFiles.isEmpty())
            added += addFiles(plainFiles);
    }
    if (!img.isNull())
        added += addImage(img);
    return added;
}

// ---------- 自动监测剪贴板 ----------

void ShelfWidget::setAutoMonitor(bool on)
{
    m_autoMonitor = on;
    if (m_autoMonitor) {
        // 从开启这一刻起只收集之后的新变化，不回溯处理当前剪贴板
        m_lastClipSeq = clipSequence();
        logClip(QStringList(), -1,
                QStringLiteral("monitor ON, seq=%1").arg(m_lastClipSeq));
    } else {
        logClip(QStringList(), -1, QStringLiteral("monitor OFF"));
    }
}

qint64 ShelfWidget::clipSequence() const
{
    try {
        return qint64(GetClipboardSequenceNumber());
    } catch (...) {
        return -1;
    }
}

void ShelfWidget::pollClipboard()
{
    if (!m_autoMonitor)
        return;
    const qint64 seq = clipSequence();
    if (seq < 0)
        return;
    if (m_fsHidden) {
        // 全屏期间暂停：持续同步序号，退出全屏后只收集新变化
        m_lastClipSeq = seq;
        return;
    }
    if (seq == m_lastClipSeq)
        return;
    m_lastClipSeq = seq;
    const QMimeData *md = QGuiApplication::clipboard()->mimeData();
    const QStringList formats = md->formats();
    const int added = processClipboardContent(md);
    logClip(formats, added);
    if (added)
        flash(COLOR_GREEN);
}

void ShelfWidget::logClip(const QStringList &formats, int added,
                          const QString &extra)
{
    try {
        QString line;
        if (!extra.isEmpty()) {
            line = QStringLiteral("%1 %2\n")
                       .arg(QDateTime::currentDateTime()
                                .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                            extra);
        } else {
            line = QStringLiteral("%1 seq=%2 formats=[%3] added=%4\n")
                       .arg(QDateTime::currentDateTime()
                                .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                       .arg(m_lastClipSeq)
                       .arg(formats.isEmpty() ? QStringLiteral("-")
                                              : formats.join(QLatin1Char(',')))
                       .arg(added);
        }
        QFile f(m_logFile);
        if (f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            f.write(line.toUtf8());
            f.close();
        }
        // 防止日志无限增长：超过 200KB 只保留最近约一半
        if (QFileInfo(m_logFile).size() > 200000) {
            QFile rf(m_logFile);
            QString tail;
            if (rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream ts(&rf);
                ts.setEncoding(QStringConverter::Utf8);
                tail = ts.readAll().right(100000);
                rf.close();
            }
            QFile wf(m_logFile);
            if (wf.open(QIODevice::WriteOnly | QIODevice::Truncate
                        | QIODevice::Text)) {
                wf.write(tail.toUtf8());
                wf.close();
            }
        }
    } catch (...) {
        // 日志失败不影响主功能
    }
}

// ---------- 拖出 ----------

void ShelfWidget::startDragOut()
{
    // 按住上方小圆拖动：把全部暂存内容作为文件拖出去（复制）
    QStringList paths;
    for (const ShelfItem &it : m_items) {
        if (QFileInfo::exists(it.path) && QFileInfo(it.path).isFile())
            paths.append(it.path);
    }
    if (paths.isEmpty()) {
        flash(COLOR_RED);
        QToolTip::showText(QCursor::pos(),
                           QStringLiteral("暂存区是空的"), this);
        return;
    }
    QMimeData *mime = new QMimeData;
    QList<QUrl> urls;
    for (const QString &p : paths)
        urls.append(QUrl::fromLocalFile(p));
    mime->setUrls(urls);
    // 默认动作为「复制」而不是「移动」
    const char preferredDropEffect[4] = {1, 0, 0, 0};
    mime->setData(
        QStringLiteral("application/x-qt-windows-mime;value=\"Preferred DropEffect\""),
        QByteArray(preferredDropEffect, 4));
    QDrag drag(this);
    drag.setMimeData(mime);  // QDrag 接管 mime 所有权
    const QPixmap pm = dragPixmap(paths.size());
    drag.setPixmap(pm);
    drag.setHotSpot(QPoint(pm.width() / 2, pm.height() / 2));
    drag.exec(Qt::DropAction::CopyAction | Qt::DropAction::MoveAction,
              Qt::DropAction::CopyAction);
}

QPixmap ShelfWidget::dragPixmap(int count) const
{
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    // 与主体一致的胶囊把手（宽度=小圆直径）+ 数量（Fluent 风格拖出鬼影）
    QLinearGradient g(0, 0, 0, 64);
    g.setColorAt(0.0, BODY_TOP);
    g.setColorAt(1.0, BODY_BOTTOM);
    p.setPen(QPen(OUTLINE, 1.0));
    p.setBrush(g);
    p.drawRoundedRect(QRectF(13, 6, 26, 52), 13.0, 13.0);
    drawCount(p, QRectF(13, 6, 26, 52), count);
    p.end();
    return pm;
}

// ---------- 边缘感应 / 全屏检测 / 滑入滑出 ----------

bool ShelfWidget::isFullscreenActive()
{
    // 前景窗口是否覆盖主显示器且处于全屏状态（非普通最大化）
    try {
        HWND hwnd = GetForegroundWindow();
        if (!hwnd)
            return false;
        const HWND wid = reinterpret_cast<HWND>(winId());
        if (hwnd == wid || hwnd == GetShellWindow()
            || hwnd == GetDesktopWindow()) {
            return false;
        }

        RECT r{};
        if (!GetWindowRect(hwnd, &r))
            return false;

        // 排除桌面 / 资源管理器桌面（Win+D 后焦点在桌面上）
        wchar_t cls[256] = {};
        GetClassNameW(hwnd, cls, 256);
        if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0)
            return false;

        const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        const LONG_PTR exstyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        // WS_MAXIMIZE / WS_EX_TOPMOST 为 windows.h 中已定义的宏

        const QRect g = QGuiApplication::primaryScreen()->geometry();
        const bool covers = (r.left <= g.left() + 2 && r.top <= g.top() + 2
                             && r.right >= g.right() - 2
                             && r.bottom >= g.bottom() - 2);
        // 覆盖全屏，且（不是普通最大化窗口 或 窗口置顶）：游戏 / 视频全屏
        return covers && (!(style & WS_MAXIMIZE) || (exstyle & WS_EX_TOPMOST));
    } catch (...) {
        return false;
    }
}

void ShelfWidget::poll()
{
    const bool fs = isFullscreenActive();

    const QRect g = QGuiApplication::primaryScreen()->geometry();
    if (g != m_screenGeo) {  // 分辨率 / 显示器变化
        m_screenGeo = g;
        m_screenRight = g.right();
        m_y = std::max(g.top(), std::min(g.bottom() - H + 1, m_y));
        m_xShown = m_screenRight - W + 1;
        m_xHidden = m_screenRight - HIDE + 1;
        setSlide(m_slide);
    }

    if (fs && !m_fsHidden) {
        m_fsHidden = true;
        m_shown = false;
        if (m_anim) {
            m_anim->stop();
            m_anim->deleteLater();
            m_anim = nullptr;
        }
        setSlide(0.0f);
        move(m_screenRight + 1, m_y);  // 完全移出屏幕
        hidePreview();
        logClip(QStringList(), -1,
                QStringLiteral("fullscreen ON, monitor paused"));
    } else if (!fs && m_fsHidden) {
        m_fsHidden = false;
        setSlide(0.0f);  // 回到收起位置
        logClip(QStringList(), -1,
                QStringLiteral("fullscreen OFF, monitor resumed"));
    }

    if (m_fsHidden)
        return;

    const int x = QCursor::pos().x();
    if (!m_shown && x >= m_screenRight - 1) {
        setShown(true);
    } else if (m_shown && !m_dragging && x < m_xShown - 80) {
        setShown(false);
    }
}

void ShelfWidget::setShown(bool shown)
{
    if (m_shown == shown)
        return;
    m_shown = shown;
    if (m_anim) {
        m_anim->stop();
        m_anim->deleteLater();
    }
    auto *anim = new QPropertyAnimation(this, "slide", this);
    anim->setDuration(190);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->setStartValue(m_slide);
    anim->setEndValue(shown ? 1.0f : 0.0f);
    anim->start();
    m_anim = anim;
}

void ShelfWidget::setSlide(float v)
{
    m_slide = v;
    const double x = m_xHidden + (m_xShown - m_xHidden) * double(v);
    move(qRound(x), m_y);
}

// ---------- 透明度 ----------

void ShelfWidget::setOpacity(double value)
{
    // 钳制 0.15–1.0；只影响主体填充色的 alpha（paintContents 内映射），
    // 不再调用 setWindowOpacity，描边/图标/数字不受透明度影响
    m_opacity = std::max(0.15, std::min(1.0, value));
    update();
    saveState();
}

void ShelfWidget::wheelEvent(QWheelEvent *e)
{
    const double step = e->angleDelta().y() > 0 ? 0.05 : -0.05;
    setOpacity(m_opacity + step);
    QToolTip::showText(
        e->globalPosition().toPoint(),
        QStringLiteral("透明度 %1%").arg(qRound(m_opacity * 100)),
        this);
}

// ---------- 外观 ----------

QHash<QString, QPointF> ShelfWidget::centers() const
{
    const double cy = height() / 2.0;
    const double hx = FEATHER + HANDLE_W / 2.0; // 把手垂直轴线
    QHash<QString, QPointF> c;
    c.insert(QStringLiteral("main"), QPointF(hx, cy));
    c.insert(QStringLiteral("top"),
             QPointF(hx, cy - HANDLE_H / 2.0 - GAP - R_SMALL));
    c.insert(QStringLiteral("bottom"),
             QPointF(hx, cy + HANDLE_H / 2.0 + GAP + R_SMALL));
    return c;
}

QString ShelfWidget::hitAt(const QPointF &pos) const
{
    const double cy = height() / 2.0;
    // 数字位于把手内部，点击数字仍执行把手操作。
    if (capsulePath(cy).contains(pos))
        return QStringLiteral("main");
    const QHash<QString, QPointF> c = centers();
    if (QLineF(pos, c.value(QStringLiteral("top"))).length() <= R_HIT)
        return QStringLiteral("top");
    if (QLineF(pos, c.value(QStringLiteral("bottom"))).length() <= R_HIT)
        return QStringLiteral("bottom");
    return QString();
}

QPainterPath ShelfWidget::shapePath() const
{
    const double cy = height() / 2.0;
    // 绘制裁剪区统一外扩 1px：让 1px 描边的外半边不被裁掉，
    // 三个形状的描边均以完整宽度均匀渲染（否则圆角处只剩半像素线，
    // 抗锯齿后呈稀疏散点）。仅作绘制裁剪，命中测试不受影响。
    QPainterPath path;
    path.addRoundedRect(
        QRectF(FEATHER - 1, cy - HANDLE_H / 2.0 - 1,
               HANDLE_W + 2, HANDLE_H + 2),
        HANDLE_W / 2.0 + 1, HANDLE_W / 2.0 + 1);
    const QHash<QString, QPointF> c = centers();
    path.addEllipse(c.value(QStringLiteral("top")),
                    R_SMALL + 1, R_SMALL + 1);
    path.addEllipse(c.value(QStringLiteral("bottom")),
                    R_SMALL + 1, R_SMALL + 1);
    return path;
}

void ShelfWidget::flash(const QColor &color)
{
    m_flashColor = color;
    m_pulse = 1.0f;
    if (m_pulseAnim) {
        m_pulseAnim->stop();
        m_pulseAnim->deleteLater();
    }
    auto *anim = new QPropertyAnimation(this, "pulse", this);
    anim->setDuration(450);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->setStartValue(1.0f);
    anim->setEndValue(0.0f);
    anim->start();
    m_pulseAnim = anim;
    update();
}

void ShelfWidget::setPulse(float v)
{
    m_pulse = v;
    update();
}

void ShelfWidget::setHoverP(float v)
{
    m_hoverP = v;
    update();
}

void ShelfWidget::startHoverAnim(float target)
{
    if (m_hoverAnim) {
        m_hoverAnim->stop();
        m_hoverAnim->deleteLater();
    }
    auto *anim = new QPropertyAnimation(this, "hoverP", this);
    anim->setDuration(120);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->setStartValue(m_hoverP);
    anim->setEndValue(target);
    anim->start();
    m_hoverAnim = anim;
}

void ShelfWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    // 优先 GPU 渲染（4× MSAA + 3× 超采样 + 边缘羽化）：
    // 边缘由多重采样覆盖并叠加羽化环，圆滑度最高；
    // GL 不可用或渲染失败时回退到软件光栅 3× 超采样
    QImage img = GLCanvas::instance().render(
        size(), 3, [this](QPainter &p) { paintContents(p); });
    if (img.isNull()) {
        constexpr int SS = 3;
        img = QImage(width() * SS, height() * SS,
                     QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter sp(&img);
        sp.scale(SS, SS);
        paintContents(sp);
        sp.end();
    }
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(rect(), img);
    p.end();
}

void ShelfWidget::paintContents(QPainter &p)
{
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    const double cy = height() / 2.0;
    const QHash<QString, QPointF> c = centers();
    const QPointF topC = c.value(QStringLiteral("top"));
    const QPointF botC = c.value(QStringLiteral("bottom"));

    const QPainterPath handle = capsulePath(cy);

    // 滑出进度：数字与功能小圆随 slide 淡入淡出（收起时窗口只露出把手
    // 左缘 10px，它们无法再像旧半圆布局那样靠悬出屏幕外隐藏）
    const double reveal = std::max(0.0, std::min(1.0, double(m_slide)));
    const bool showExtras = reveal > 0.001;
    const int n = m_items.size();

    // 主体填充 alpha：滚轮调节的"透明度"只作用于填充色，
    // 描边 / 图标 / 数字始终不透明
    const int bodyAlpha = qRound(255 * m_opacity);

    // 1) 柔和中性阴影（不裁剪，沿轮廓向外 3 层渐弱）——替代旧蓝色羽化光晕
    p.setClipping(false);
    p.setBrush(Qt::NoBrush);
    const int shAlphas[3] = {10, 16, 24};
    const double shWidths[3] = {4.0, 2.5, 1.5};
    for (int i = 0; i < 3; ++i) {
        p.setPen(QPen(QColor(10, 18, 35, shAlphas[i]), shWidths[i],
                      Qt::SolidLine, Qt::RoundCap));
        p.drawPath(handle);
        if (showExtras) {
            p.drawEllipse(topC, R_SMALL, R_SMALL);
            p.drawEllipse(botC, R_SMALL, R_SMALL);
        }
    }

    // 2) 其余内容裁剪到形状内绘制（形状之外保持全透明、点击穿透）
    p.setClipPath(shapePath());

    // 把手：纵向微渐变（仅微妙纵深）+ 1px 细描边
    QColor fillTop = BODY_TOP;
    QColor fillBottom = BODY_BOTTOM;
    fillTop.setAlpha(bodyAlpha);
    fillBottom.setAlpha(bodyAlpha);
    QLinearGradient body(0, cy - HANDLE_H / 2.0, 0, cy + HANDLE_H / 2.0);
    body.setColorAt(0.0, fillTop);
    body.setColorAt(1.0, fillBottom);
    p.setPen(QPen(OUTLINE, 1.0));
    p.setBrush(body);
    p.drawPath(handle);

    if (showExtras) {
        // 功能小圆：暖白底色 + 细描边
        QColor btn = BUTTON_FILL;
        btn.setAlpha(qRound(bodyAlpha * reveal));
        p.setPen(QPen(OUTLINE, 1.0));
        p.setBrush(btn);
        p.drawEllipse(topC, R_SMALL, R_SMALL);
        p.drawEllipse(botC, R_SMALL, R_SMALL);

    }

    // 悬停高亮：被悬停的部分叠加一层白，随 m_hoverP 平滑变亮
    const QString hoverPart = m_hover.isEmpty() ? m_hoverFade : m_hover;
    if (!hoverPart.isEmpty() && m_hoverP > 0.001f) {
        QPainterPath hoverPath;
        if (hoverPart == QLatin1String("main"))
            hoverPath = handle;
        else if (showExtras && hoverPart == QLatin1String("top"))
            hoverPath.addEllipse(topC, R_SMALL, R_SMALL);
        else if (showExtras && hoverPart == QLatin1String("bottom"))
            hoverPath.addEllipse(botC, R_SMALL, R_SMALL);
        if (!hoverPath.isEmpty()) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, qRound(34 * m_hoverP)));
            p.drawPath(hoverPath);
        }
    }

    // 存储/清空闪烁：罩上闪色，随 m_pulse 衰减
    if (m_pulse > 0.01f) {
        const float t = std::min(1.0f, m_pulse);
        QColor f = m_flashColor;
        f.setAlpha(qRound(160 * t));
        p.setPen(Qt::NoPen);
        p.setBrush(f);
        p.drawPath(handle);
        if (showExtras) {
            p.drawEllipse(topC, R_SMALL, R_SMALL);
            p.drawEllipse(botC, R_SMALL, R_SMALL);
        }
    }

    // 功能小圆图标：上=向上箭头（拖出），下=叉（清空）
    if (showExtras) {
        const QPen iconPen(ICON_COLOR, 1.8, Qt::SolidLine,
                           Qt::RoundCap, Qt::RoundJoin);
        p.setPen(iconPen);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(topC.x() - 5, topC.y() + 2.5),
                   QPointF(topC.x(), topC.y() - 3.5));
        p.drawLine(QPointF(topC.x(), topC.y() - 3.5),
                   QPointF(topC.x() + 5, topC.y() + 2.5));
        p.drawLine(QPointF(botC.x() - 4, botC.y() - 4),
                   QPointF(botC.x() + 4, botC.y() + 4));
        p.drawLine(QPointF(botC.x() - 4, botC.y() + 4),
                   QPointF(botC.x() + 4, botC.y() - 4));

        drawCount(p, handle.boundingRect(), n, reveal);
    }
}

// ---------- 鼠标交互 ----------

void ShelfWidget::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::MiddleButton) {
        // 中键点击把手：弹出/查看暂存内容面板
        if (hitAt(e->position()) == QStringLiteral("main"))
            openPreview();
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    const QString hit = hitAt(e->position());
    if (hit.isEmpty())
        return;
    m_press = hit;
    m_pressPos = e->position();
    m_pressGlobal = e->globalPosition().toPoint();
    m_startY = m_y;
    m_dragging = false;
}

void ShelfWidget::mouseMoveEvent(QMouseEvent *e)
{
    const QString hit = hitAt(e->position());
    if (hit != m_hover) {
        if (!m_hover.isEmpty())
            m_hoverFade = m_hover;  // 记住刚离开的部分，让它的高亮淡出
        m_hover = hit;
        startHoverAnim(hit.isEmpty() ? 0.0f : 1.0f);
        update();
    }
    if (m_press == QStringLiteral("top")) {
        if ((e->position() - m_pressPos).manhattanLength()
            >= QApplication::startDragDistance()) {
            m_press.clear();
            startDragOut();
        }
    } else if (m_press == QStringLiteral("main")) {
        const QPoint d = e->globalPosition().toPoint() - m_pressGlobal;
        if (!m_dragging && d.manhattanLength() >= QApplication::startDragDistance())
            m_dragging = true;
        if (m_dragging) {
            const QRect g = QGuiApplication::primaryScreen()->geometry();
            const int y = std::max(g.top(),
                                   std::min(g.bottom() - H + 1, m_startY + d.y()));
            if (y != m_y) {
                m_y = y;
                setSlide(m_slide);
                saveState();
            }
        }
    }
}

void ShelfWidget::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton || m_press.isEmpty())
        return;
    const QString hit = hitAt(e->position());
    if (hit == m_press && !m_dragging) {
        if (hit == QStringLiteral("main"))
            storeFromClipboard();
        else if (hit == QStringLiteral("bottom"))
            clearItems();
    }
    m_press.clear();
    m_dragging = false;
}

void ShelfWidget::leaveEvent(QEvent *e)
{
    Q_UNUSED(e);
    if (!m_hover.isEmpty()) {
        m_hoverFade = m_hover;
        m_hover.clear();
        startHoverAnim(0.0f);  // 高亮平滑淡出
    }
    update();
}

// ---------- 拖入 ----------

void ShelfWidget::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls() || hasImageFormats(e->mimeData()))
        e->acceptProposedAction();
}

void ShelfWidget::dragMoveEvent(QDragMoveEvent *e)
{
    if (e->mimeData()->hasUrls() || hasImageFormats(e->mimeData()))
        e->acceptProposedAction();
}

bool ShelfWidget::isTransientTempPath(const QString &path) const
{
    // 从压缩软件（7-Zip / WinRAR / Bandizip / 资源管理器 zip 视图）等拖出文件时，
    // 源程序把文件物化到系统临时目录再给出路径，随后即被源程序或系统清理——
    // 只记路径必然失效。检测方式：与各临时目录做前缀比较（大小写不敏感）。
    const QString p = QDir::toNativeSeparators(
                          QFileInfo(path).absoluteFilePath())
                          .toCaseFolded();
    if (p.isEmpty())
        return false;
    // 本程序 stash 目录内的文件（自检模式下 stash 就位于 temp 下）不算临时文件，
    // 否则会出现「把文件复制成它自己」的荒谬路径
    const QString stashReal = QDir::toNativeSeparators(
                                  QDir::cleanPath(
                                      QFileInfo(m_stashDir).absoluteFilePath()))
                                  .toCaseFolded();
    if (!stashReal.isEmpty() && p.startsWith(stashReal + QLatin1Char('\\')))
        return false;
    const QStringList candidates = {
        QDir::tempPath(),
        qEnvironmentVariable("TEMP"),
        qEnvironmentVariable("TMP"),
    };
    for (const QString &c0 : candidates) {
        if (c0.isEmpty())
            continue;
        const QString c = QDir::toNativeSeparators(QDir::cleanPath(c0))
                              .toCaseFolded();
        if (p.startsWith(c + QLatin1Char('\\')))
            return true;
    }
    return false;
}

QString ShelfWidget::copyIntoStash(const QString &srcPath)
{
    // 把拖入的临时文件复制进 stash（随清空 / 退出一起删除）。保留原文件名：
    // 预览面板按文件名展示，原名最友好。同步复制在 dropEvent 内完成，早于
    // 拖拽源 DoDragDrop 返回，源程序事后清理临时目录也不受影响。
    const QFileInfo src(srcPath);
    if (!src.isFile())
        return QString();
    QDir().mkpath(m_stashDir);
    const QDir stash(m_stashDir);
    // 同名且同大小：视为同一文件（重复从压缩包拖出），复用现有副本，由
    // m_pathSet 去重不产生新条目——临时路径每次拖都不同，无法按路径去重
    const QString direct = stash.absoluteFilePath(src.fileName());
    if (QFileInfo::exists(direct) && QFileInfo(direct).size() == src.size())
        return direct;
    // 同名不同内容：仿资源管理器追加「 (2)」「 (3)」…
    QString target = direct;
    for (int n = 2; QFileInfo::exists(target); ++n) {
        const QString name =
            src.suffix().isEmpty()
                ? QStringLiteral("%1 (%2)").arg(src.completeBaseName()).arg(n)
                : QStringLiteral("%1 (%2).%3")
                      .arg(src.completeBaseName())
                      .arg(n)
                      .arg(src.suffix());
        target = stash.absoluteFilePath(name);
    }
    if (!QFile::copy(srcPath, target))
        return QString();
    return target;
}

int ShelfWidget::processDropContent(const QMimeData *md, int *copiedCount)
{
    // 拖入：位于系统临时目录的文件（压缩软件拖出解压等）先复制进 stash 再
    // 暂存；其余位置的文件沿用「只记录路径」；图片位图仍走 addImage（互不排斥）。
    // copyIntoStash 可能复用同名同大小副本，因此必须先提交旧 pending。
    commitPreviewDelete();
    int added = 0;
    int copied = 0;
    if (md->hasUrls()) {
        QStringList plainFiles;
        const QList<QUrl> urls = md->urls();
        for (const QUrl &u : urls) {
            if (!u.isLocalFile())
                continue;
            const QString p = u.toLocalFile();
            if (isTransientTempPath(p)) {
                const QString stashed = copyIntoStash(p);
                if (!stashed.isEmpty()) {
                    const int one = addFiles({stashed});
                    added += one;
                    if (one)
                        ++copied;  // 该文件以「复制保存」方式暂存成功
                }
            } else {
                plainFiles.append(p);
            }
        }
        if (!plainFiles.isEmpty())
            added += addFiles(plainFiles);
    }
    if (hasImageFormats(md)) {
        const QImage img = extractImage(md);
        if (!img.isNull())
            added += addImage(img);
    }
    if (copiedCount)
        *copiedCount = copied;
    return added;
}

void ShelfWidget::dropEvent(QDropEvent *e)
{
    int copied = 0;
    const int added = processDropContent(e->mimeData(), &copied);
    flash(added ? COLOR_GREEN : COLOR_NEUTRAL);
    if (added && copied > 0) {
        QToolTip::showText(
            QCursor::pos(),
            QStringLiteral("已暂存 %1 项（%2 个来自压缩包等的文件已复制保存，清空时将删除）")
                .arg(added)
                .arg(copied),
            this);
    }
}

// ---------- 右键菜单 / 提示 / 关闭 ----------

void ShelfWidget::contextMenuEvent(QContextMenuEvent *e)
{
    QMenu menu(this);
    // 右键菜单单独用 Regular 字重（界面其余文字是 Light，把手徽标是 Bold）。
    // 实测：子菜单不会继承父菜单的 setFont（会落回应用级字体 Light），
    // 所以主菜单与「透明度」子菜单都要显式设置。
    const QFont menuFont = UiFont::withWeight(QFont::Normal);
    menu.setFont(menuFont);
    QMenu *sub = menu.addMenu(QStringLiteral("透明度"));
    sub->setFont(menuFont);
    const int current = qRound(m_opacity * 100);
    const int values[] = {15, 25, 35, 50, 65, 80, 100};
    for (int v : values) {
        QAction *act = sub->addAction(QStringLiteral("%1%").arg(v));
        act->setCheckable(true);
        act->setChecked(v == current);
        connect(act, &QAction::triggered, this,
                [this, v](bool) { setOpacity(v / 100.0); });
    }
    menu.addSeparator();
    QAction *actMon =
        menu.addAction(QStringLiteral("自动监测剪贴板（文件 / 图片）"));
    actMon->setCheckable(true);
    actMon->setChecked(m_autoMonitor);
    connect(actMon, &QAction::toggled, this, &ShelfWidget::setAutoMonitor);
    // 开机自启：注册表 Run 键是唯一事实来源，每次打开菜单实时读取勾选状态
    QAction *actAuto = menu.addAction(QStringLiteral("开机自启"));
    actAuto->setCheckable(true);
    actAuto->setChecked(AutoStart::isEnabled());
    connect(actAuto, &QAction::triggered, this, [this](bool on) {
        QString err;
        if (!AutoStart::setEnabled(on, &err))
            QMessageBox::warning(this, QStringLiteral("开机自启"), err);
    });
    QAction *actClear = menu.addAction(
        QStringLiteral("清空暂存（%1 项）").arg(m_items.size()));
    connect(actClear, &QAction::triggered, this,
            [this](bool) { clearItems(); });
    menu.addSeparator();
    QAction *actQuit = menu.addAction(QStringLiteral("退出"));
    connect(actQuit, &QAction::triggered, qApp, &QApplication::quit);
    menu.exec(e->globalPos());
}

bool ShelfWidget::event(QEvent *e)
{
    if (e->type() == QEvent::ToolTip) {
        auto *he = static_cast<QHelpEvent *>(e);
        const QString hit = hitAt(QPointF(he->pos()));
        const QHash<QString, QString> tips = {
            {QStringLiteral("main"),
             QStringLiteral("已暂存 %1 项\n"
                            "点击：暂存剪贴板里的文件 / 图片\n"
                            "中键：查看暂存的内容（图片缩略图 / 文件列表）\n"
                            "按住拖动：上下移动位置\n"
                            "把文件拖到这里：暂存（压缩包里拖出的文件会复制保存）")
                 .arg(m_items.size())},
            {QStringLiteral("top"),
             QStringLiteral("按住并拖动：把全部暂存内容拖出去（复制）")},
            {QStringLiteral("bottom"),
             QStringLiteral("点击：清空暂存")},
        };
        if (!hit.isEmpty())
            QToolTip::showText(he->globalPos(), tips.value(hit), this);
        else
            QToolTip::hideText();
        return true;
    }
    return QWidget::event(e);
}

void ShelfWidget::closeEvent(QCloseEvent *e)
{
    hidePreview();
    commitPreviewDelete();
    clearItems(false);  // 退出即清空
    saveState();
    QWidget::closeEvent(e);
}

ShelfWidget::~ShelfWidget()
{
    delete m_previewPanel;  // 面板无父窗口，需显式释放（含全局钩子卸载）
    m_previewPanel = nullptr;
}

// ---------- 中键预览面板 ----------

void ShelfWidget::openPreview()
{
    if (m_items.isEmpty()) {
        flash(COLOR_RED);
        QToolTip::showText(QCursor::pos(), QStringLiteral("暂存区是空的"), this);
        return;
    }
    if (!m_previewPanel)
        m_previewPanel = new ItemPreviewPanel(nullptr);  // 无父窗口：独立顶层窗口
    m_previewPanel->setOwner(this);
    if (m_previewPanel->isVisible()) {
        // 面板已打开：再按一次中键 = 关闭
        m_previewPanel->hide();
        return;
    }
    m_previewPanel->setItems(m_items);
    m_previewPanel->showCentered();
}

void ShelfWidget::hidePreview()
{
    if (m_previewPanel)
        m_previewPanel->hide();
    commitPreviewDelete();
}

bool ShelfWidget::previewVisibleForTest() const
{
    return m_previewPanel && m_previewPanel->isVisible();
}

QRect ShelfWidget::previewGeometryForTest() const
{
    return m_previewPanel ? m_previewPanel->frameGeometry() : QRect();
}

void ShelfWidget::animatePreviewZoomForTest(int idx)
{
    if (m_previewPanel && m_previewPanel->isVisible())
        m_previewPanel->animateZoomForTest(idx);
}

void ShelfWidget::animatePreviewUnzoomForTest()
{
    if (m_previewPanel && m_previewPanel->isVisible())
        m_previewPanel->animateUnzoomForTest();
}

void ShelfWidget::refreshPreview()
{
    // 面板打开时：内容变化后原地刷新；空了就关闭
    if (m_previewPanel && m_previewPanel->isVisible()) {
        if (m_items.isEmpty())
            m_previewPanel->hide();
        else
            m_previewPanel->setItems(m_items);
    }
}
