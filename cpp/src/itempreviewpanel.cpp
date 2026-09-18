// -*- coding: utf-8 -*-
// 暂存内容预览窗口实现：米白色平涂（无渐变、无文字），图片瀑布流排布，
// 保持原始宽高比；文件以系统图标展示。悬停时通过系统 Tooltip 显示名称、路径与操作提示。
// 左键点击条目进入放大视图（原尺寸，超出屏幕时按可用区域缩小铺满、避开任务栏），
// 其余条目在放大项右侧自上而下单列排布；再点放大项退回瀑布流，点列中其他项切换。
// 中键点击条目用系统默认程序打开文件。
// 右键点击条目从暂存中移除，底部提供 5 秒撤销。

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "glcanvas.h"
#include "itempreviewpanel.h"
#include "uifont.h"

#include <QDesktopServices>
#include <QDir>
#include <QEasingCurve>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QHideEvent>
#include <QImageReader>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegion>
#include <QScreen>
#include <QStandardPaths>
#include <QToolTip>
#include <QTimer>
#include <QUrl>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QVector>

#include <cmath>
#include <string>

// 路径在暂存时被 toCaseFolded() 统一成小写，展示前先还原磁盘上的
// 真实大小写（GetLongPathNameW 返回磁盘上实际保存的大小写形式）。
static QString displayPath(const QString &p)
{
    const std::wstring w = QDir::toNativeSeparators(p).toStdWString();
    const DWORD need = GetLongPathNameW(w.c_str(), nullptr, 0);
    if (need == 0)
        return p;
    std::wstring buf(need, L'\0');
    const DWORD got = GetLongPathNameW(w.c_str(), buf.data(), need);
    if (got == 0 || got >= need)
        return p;
    buf.resize(got);
    return QString::fromStdWString(buf);
}

static bool isImageSuffix(const QString &p)
{
    const QString suf = QFileInfo(p).suffix().toLower();
    return suf == QLatin1String("png") || suf == QLatin1String("jpg")
        || suf == QLatin1String("jpeg") || suf == QLatin1String("bmp")
        || suf == QLatin1String("gif") || suf == QLatin1String("webp");
}

static bool isImageItem(const ShelfItem &it)
{
    return it.kind == QLatin1String("image") || isImageSuffix(it.path);
}

#ifdef SIDESHELF_PERF_BASELINE
// ---- 性能基线对照（-DSIDESHELF_BASELINE=ON 时编译，仅测量用）----
// 原实现：按目标矩形精确尺寸解码 + 尺寸相关缓存键。QCache 默认
// maxCost=100，图片以 sizeInBytes 为 cost 的插入全部被拒，每次重绘
// 都重新读盘解码——过渡逐帧形变时还会逐帧整图解码。
static QSize clampedCacheSize(const QSize &size)
{
    return QSize(std::min(8192, size.width()),
                 std::min(4096, size.height()));
}

static QImage loadCachedImageBaseline(const ShelfItem &item, const QSize &size,
                                      QCache<QString, QImage> &cache)
{
    const QSize scaledSize = clampedCacheSize(size);
    if (!scaledSize.isValid() || scaledSize.isEmpty())
        return {};

    QFileInfo info(item.path);
    const QString key = QStringLiteral("%1|%2x%3|%4|%5")
        .arg(item.path, QString::number(scaledSize.width()),
             QString::number(scaledSize.height()),
             QString::number(info.lastModified().toMSecsSinceEpoch()),
             QString::number(info.size()));
    if (const QImage *cached = cache.object(key))
        return *cached;

    QImageReader reader(item.path);
    reader.setScaledSize(scaledSize);
    QImage image = reader.read();
    if (image.isNull())
        return {};

    const int cacheCost = static_cast<int>(std::max<qsizetype>(1, image.sizeInBytes()));
    cache.insert(key, new QImage(image), cacheCost);
    return image;
}
#endif

// ---- 图片解码缓存（两级、尺寸无关，见 itempreviewpanel.h 的说明）----
// cost 单位为解码后字节数；QCache 默认 maxCost=100 会拒绝一切图片插入
// （构造函数里已 setMaxCost），这里无需再关心预算。

QSize ItemPreviewPanel::levelFitSize(ImgLevel level)
{
    if (level == ImgLevel::Thumb) {
        // 瀑布流/右侧列卡片列宽 COL_TARGET（放大列 ZOOM_COL_W 同值），
        // 2× 覆盖 HiDPI 采样余量；高度给足长图空间
        return QSize(COL_TARGET * 2, COL_TARGET * 4);
    }
    const QRect g = QGuiApplication::primaryScreen()->availableGeometry();
    return QSize(std::min(4096, g.width() * 2), std::min(4096, g.height() * 2));
}

QString ItemPreviewPanel::imageCacheKey(const ShelfItem &item, ImgLevel level)
{
    QFileInfo info(item.path);
    return QStringLiteral("%1|%2|%3|%4")
        .arg(item.path,
             QString::number(info.lastModified().toMSecsSinceEpoch()),
             QString::number(info.size()),
             level == ImgLevel::Thumb ? QStringLiteral("T")
                                      : QStringLiteral("F"));
}

QImage ItemPreviewPanel::loadCachedImage(const ShelfItem &item, ImgLevel level,
                                         QCache<QString, QImage> &cache)
{
    const QString key = imageCacheKey(item, level);
    if (const QImage *cached = cache.object(key))
        return *cached;

    QImageReader reader(item.path);
    // 仅当原图超出该级别的 fit 尺寸时才缩小解码；小图按原尺寸，不放大气
    const QSize orig = reader.size();
    const QSize fit = levelFitSize(level);
    if (orig.isValid() && orig.width() > 0 && orig.height() > 0
        && (orig.width() > fit.width() || orig.height() > fit.height())) {
        QSize s = orig;
        s.scale(fit, Qt::KeepAspectRatio);
        reader.setScaledSize(s);
    }
    QImage image = reader.read();
    if (image.isNull())
        return {};

    cache.insert(key, new QImage(image), image.sizeInBytes());
    return image;
}

// ---- 全局鼠标钩子状态 ----
ItemPreviewPanel *ItemPreviewPanel::s_instance = nullptr;
HHOOK ItemPreviewPanel::s_hook = nullptr;
HWND ItemPreviewPanel::s_hookWnd = nullptr;
UINT ItemPreviewPanel::s_msgMouseDownOutside =
    RegisterWindowMessageW(L"SideShelf-Preview-OutsidePress");

// 钩子回调：只做 PostMessageW，纯 Win32、微秒级，不可能触发
// WH_MOUSE_LL 的超时移除；关闭判断全部在 GUI 线程 nativeEvent 中完成
LRESULT CALLBACK ItemPreviewPanel::llMouseProc(int nCode, WPARAM wParam,
                                               LPARAM lParam)
{
    if (nCode == HC_ACTION
        && (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN
            || wParam == WM_MBUTTONDOWN)) {
        const MSLLHOOKSTRUCT *ms =
            reinterpret_cast<const MSLLHOOKSTRUCT *>(lParam);
        if (s_hookWnd && ms) {
            PostMessageW(s_hookWnd, s_msgMouseDownOutside,
                         static_cast<WPARAM>(ms->pt.x),
                         static_cast<LPARAM>(ms->pt.y));
        }
    }
    return CallNextHookEx(s_hook, nCode, wParam, lParam);
}

ItemPreviewPanel::ItemPreviewPanel(QWidget *parent)
    : QWidget(parent,
              Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_AlwaysShowToolTips, true);
    setMouseTracking(true);
    setWindowTitle(QStringLiteral("Side Shelf 预览"));
    // 缓存预算（cost 单位 = 字节）。QCache 默认 maxCost=100，而图片 cost 是
    // sizeInBytes()（几十万起），不调大会导致 insert 直接拒绝、缓存永不命中，
    // 每次重绘都重新读盘解码——这是过渡动画卡顿的主因。
#ifndef SIDESHELF_PERF_BASELINE
    m_imageCache.setMaxCost(192 * 1024 * 1024);  // LRU 上限 192MB
    m_iconCache.setMaxCost(256);                 // 256 个图标槽（cost=1/个）
#endif
    m_profiling = !qEnvironmentVariableIsEmpty("SIDESHELF_PROF");
    profTrace(QStringLiteral("[prof] panel ctor, profiling=%1\n")
                  .arg(m_profiling ? QStringLiteral("1") : QStringLiteral("0")));
    m_undoTimer = new QTimer(this);
    m_undoTimer->setInterval(50);
    connect(m_undoTimer, &QTimer::timeout, this,
            &ItemPreviewPanel::tickUndoCountdown);
    s_instance = this;
    s_hookWnd = reinterpret_cast<HWND>(winId());  // 提前创建原生窗口以便收消息
    installGlobalClickClose();
}

ItemPreviewPanel::~ItemPreviewPanel()
{
    removeGlobalClickClose();
    if (s_instance == this)
        s_instance = nullptr;
}

void ItemPreviewPanel::installGlobalClickClose()
{
    if (!s_hook)
        s_hook = SetWindowsHookExW(WH_MOUSE_LL, llMouseProc,
                                   GetModuleHandleW(nullptr), 0);
}

void ItemPreviewPanel::removeGlobalClickClose()
{
    if (s_hook) {
        UnhookWindowsHookEx(s_hook);
        s_hook = nullptr;
    }
}

void ItemPreviewPanel::setItems(const QList<ShelfItem> &items)
{
    finishTransition();  // 内容变化（自动监测等）：中断进行中的过渡并跳到终点
    m_iconCache.clear();  // 同路径文件的图标可能已变化：一并失效
    const bool wasZoomed = (m_zoom >= 0) && isVisible();
    m_items = items;
    // 立即重建布局：面板已可见时（自动监测新增等）update() 会马上触发重绘，
    // 不能让 m_rects 处于空状态被 paintEvent 越界访问
    buildLayout(std::max(width(), 360));
    m_hover = -1;
    m_press = -1;
    m_scrollY = 0;
    m_zoom = -1;
    m_zoomRect = QRect();
    // 放大状态下内容变化（自动监测等）：退回瀑布流并恢复瀑布流窗口尺寸
    if (wasZoomed && isVisible()) {
        resize(panelSize());
        const QRect g = QGuiApplication::primaryScreen()->availableGeometry();
        move(g.center().x() - width() / 2, g.center().y() - height() / 2);
    }
    if (!m_owner || !m_owner->hasPendingDelete())
        clearUndoUi();
    update();
}

// 瀑布流布局：依次把每张卡片放进当前最短的列。
// 图片卡片高度 = 列宽 ×（原图高/原图宽），保持原始宽高比；文件卡片为固定高度。
void ItemPreviewPanel::buildLayout(int width)
{
    m_rects.clear();
    const int cols = std::max(
        1, (width - 2 * MARGIN + GUTTER) / (COL_TARGET + GUTTER));
    const int colW = (width - 2 * MARGIN - (cols - 1) * GUTTER) / cols;

    QVector<int> colY(cols, 0);
    for (const ShelfItem &it : m_items) {
        int h = FILE_H;
        if (isImageItem(it)) {
            const QImageReader r(it.path);  // 只读文件头，取原始尺寸
            const QSize s = r.size();
            if (s.isValid() && s.width() > 0 && s.height() > 0) {
                h = std::max(
                    40, int(std::lround(colW * double(s.height())
                                        / double(s.width()))));
            }
        }
        int c = 0;
        for (int j = 1; j < cols; ++j) {
            if (colY[j] < colY[c])
                c = j;
        }
        m_rects.append(
            QRect(MARGIN + c * (colW + GUTTER), MARGIN + colY[c], colW, h));
        colY[c] += h + GUTTER;
    }

    int content = 0;
    for (int j = 0; j < cols; ++j)
        content = std::max(content, colY[j]);
    m_contentH = MARGIN + (content > 0 ? content - GUTTER : 0) + 8;
}

// 条目“原尺寸”：图片取文件头里的原始宽高；文件用固定的放大卡片尺寸。
QSize ItemPreviewPanel::originalSize(const ShelfItem &it) const
{
    if (isImageItem(it)) {
        const QImageReader r(it.path);  // 只读文件头
        const QSize s = r.size();
        if (s.isValid() && s.width() > 0 && s.height() > 0)
            return s;
    }
    return QSize(ZOOM_FILE_W, ZOOM_FILE_H);
}

// 放大状态布局：
//   - 放大项在左，原尺寸展示；超出“可用区域 − 留白 − 右侧列”时按比例缩小
//     （即铺满屏幕可用区域，避开任务栏，但保留面板边距与右侧单列，非真正全屏）。
//   - 其余条目在放大项右侧自上而下单列排布，超高时滚轮滚动。
//   - 填充 m_rects/m_colIdx/m_zoomRect/m_contentH 并返回目标窗口矩形
//     （按当前窗口位置钳制到可用区域内，但不应用几何——由调用方决定
//     瞬时应用还是交给过渡动画插值）。
QRect ItemPreviewPanel::computeZoomLayout()
{
    if (m_zoom < 0 || m_zoom >= m_items.size())
        return geometry();
    const QRect g = QGuiApplication::primaryScreen()->availableGeometry();
    const QRect avail =
        g.adjusted(ZOOM_EDGE_GAP, ZOOM_EDGE_GAP, -ZOOM_EDGE_GAP, -ZOOM_EDGE_GAP);

    // 放大项可用区域（扣除面板边距与右侧单列）
    const int maxImgW =
        std::max(80, avail.width() - 2 * MARGIN - GUTTER - ZOOM_COL_W);
    const int maxImgH = std::max(80, avail.height() - 2 * MARGIN);

    // 原尺寸优先，超出可用区域才按比例缩小（不放大）
    QSize s = originalSize(m_items.at(m_zoom));
    s.scale(QSize(std::min(s.width(), maxImgW), std::min(s.height(), maxImgH)),
            Qt::KeepAspectRatio);
    const int imgW = std::max(1, s.width());
    const int imgH = std::max(1, s.height());

    // 右侧单列：其余条目自上而下排布（图片保持宽高比，文件固定高度）
    m_rects.clear();
    m_colIdx.clear();
    int colH = 0;
    for (int i = 0; i < m_items.size(); ++i) {
        if (i == m_zoom)
            continue;
        int h = FILE_H;
        if (isImageItem(m_items.at(i))) {
            const QImageReader r(m_items.at(i).path);
            const QSize is = r.size();
            if (is.isValid() && is.width() > 0 && is.height() > 0) {
                h = std::max(
                    40, int(std::lround(ZOOM_COL_W * double(is.height())
                                        / double(is.width()))));
            }
        }
        m_colIdx.append(i);
        m_rects.append(QRect(MARGIN + imgW + GUTTER, MARGIN + colH,
                             ZOOM_COL_W, h));
        colH += h + GUTTER;
    }
    m_contentH = MARGIN + (colH > 0 ? colH - GUTTER : 0) + 8;

    // 窗口尺寸：放大项 + 右侧列（两高取大），再钳制到可用区域
    const int w = std::min(avail.width(),
                           MARGIN + imgW + GUTTER + ZOOM_COL_W + MARGIN);
    const int h = std::max(
        160, std::min(avail.height(),
                      2 * MARGIN + std::max(imgH, m_contentH - 2 * MARGIN)));

    // 放大项在内容区垂直居中（窗口比图片高、但右侧列更矮时），
    // 右侧列始终保持顶对齐（“由上至下”）
    const int contentH = h - 2 * MARGIN;
    const int zoomH = std::min(imgH, contentH);
    m_zoomRect = QRect(MARGIN, MARGIN + (contentH - zoomH) / 2, imgW, zoomH);

    // 钳制窗口位置（保持左上角不动，向右下屏外溢出时拉回），返回目标矩形
    const QPoint p = pos();
    QPoint np(p.x(), p.y());
    np.setX(std::max(avail.x(), std::min(np.x(), avail.right() - w + 1)));
    np.setY(std::max(avail.y(), std::min(np.y(), avail.bottom() - h + 1)));
    return QRect(np, QSize(w, h));
}

// ---- 放大/退回/切换的过渡动画 ----

bool ItemPreviewPanel::transitionRunning() const
{
    return m_zoomAnim
        && m_zoomAnim->state() == QAbstractAnimation::Running;
}

QImage ItemPreviewPanel::renderSnapshot()
{
    // 与 paintEvent 相同的渲染管线：GL 优先，失败回退软件光栅。
    // 快照本身在过渡中会被拉伸 + 淡出，1× 精度足够。
    QImage img = GLCanvas::instance().render(
        size(), 1, [this](QPainter &p) { paintContents(p); });
    if (!img.isNull())
        return img;
    QImage fallback(size(), QImage::Format_ARGB32_Premultiplied);
    fallback.fill(Qt::transparent);
    QPainter sp(&fallback);
    paintContents(sp);
    sp.end();
    return fallback;
}

void ItemPreviewPanel::startTransition(const QImage &snap,
                                       const QRect &fromGeo,
                                       const QRect &toGeo,
                                       const QRect &cardFrom,
                                       const QRect &cardTo, int cardIdx)
{
    m_animSnapshot = snap;
    m_animFromGeo = fromGeo;
    m_animToGeo = toGeo;
    m_animCardFrom = cardFrom;
    m_animCardTo = cardTo;
    m_animCardIdx = cardIdx;
    profTrace(QStringLiteral("[prof] startTransition %1->%2 snap=%3x%4\n")
                  .arg(fromGeo.width()).arg(fromGeo.height())
                  .arg(snap.width()).arg(snap.height()));

    if (m_zoomAnim) {
        m_zoomAnim->stop();
        m_zoomAnim->deleteLater();
    }
    auto *anim = new QVariantAnimation(this);
    anim->setDuration(240);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    connect(anim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &v) {
                const double t = v.toDouble();
                const QRectF from(m_animFromGeo), to(m_animToGeo);
                const QRectF r(from.x() + (to.x() - from.x()) * t,
                               from.y() + (to.y() - from.y()) * t,
                               from.width() + (to.width() - from.width()) * t,
                               from.height() + (to.height() - from.height()) * t);
                // 两端点都在屏幕可用区域内，凸组合保证中间帧也不越界
                setGeometry(r.toAlignedRect());
                update();
            });
    connect(anim, &QVariantAnimation::finished, this, [this]() {
        profTrace(QStringLiteral("[prof] anim finished\n"));
        profFlush();
        setGeometry(m_animToGeo);
        m_animSnapshot = QImage();
        m_animCardIdx = -1;
        if (m_zoomAnim) {
            m_zoomAnim->deleteLater();
            m_zoomAnim = nullptr;
        }
        update();
    });
    anim->start();
    m_zoomAnim = anim;
#ifndef SIDESHELF_PERF_BASELINE
    // 首帧预热（在动画第一帧之前完成，避免动画帧支付一次性尖峰）：
    // 1) 形变卡片的全尺寸解码（3000×2000 级 JPEG 首次解码 ~25ms）；
    // 2) 窗口放大时把 GL FBO 预增长到目标尺寸（MSAA 缓冲分配）。两者
    //    都发生在点击帧，动画帧保持稳定的小耗时（过渡期逐帧形变平滑）
    if (cardIdx >= 0 && cardIdx < m_items.size()
        && isImageItem(m_items.at(cardIdx))) {
        loadCachedImage(m_items.at(cardIdx), ImgLevel::Full, m_imageCache);
    }
    if (toGeo.width() * toGeo.height() > fromGeo.width() * fromGeo.height())
        GLCanvas::instance().render(toGeo.size(), 1, [](QPainter &) {});
#endif
    update();
}

void ItemPreviewPanel::finishTransition()
{
    // 过渡期间来了新操作（快速连点 / Esc / setItems / hide）：
    // 直接跳到动画终点并清理，保证状态一致后接下一段过渡
    if (transitionRunning()) {
        m_zoomAnim->stop();
        setGeometry(m_animToGeo);
        m_animSnapshot = QImage();
        m_animCardIdx = -1;
        m_zoomAnim->deleteLater();
        m_zoomAnim = nullptr;
        profFlush();
    }
}

void ItemPreviewPanel::profTrace(const QString &what)
{
    if (!m_profiling)
        return;
    const QString path =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/side_shelf_anim_prof.log");
    QFile f(path);
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write(what.toUtf8());
}

void ItemPreviewPanel::profFlush()
{
    if (!m_profiling)
        return;
    double sum = 0;
    double mx = 0;
    for (double v : m_profMs) {
        sum += v;
        mx = std::max(mx, v);
    }
    const QString line = QStringLiteral("[%1] frames=%2 avg=%3ms max=%4ms\n")
                             .arg(m_animKind,
                                  QString::number(m_profMs.size()),
                                  QString::number(sum / m_profMs.size(), 'f', 2),
                                  QString::number(mx, 'f', 2));
    const QString path =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/side_shelf_anim_prof.log");
    QFile f(path);
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write(line.toUtf8());
    m_profMs.clear();
}

void ItemPreviewPanel::enterZoomInstant(int idx)
{
    if (idx < 0 || idx >= m_items.size())
        return;
    if (m_zoom < 0) {
        m_waterfallScroll = m_scrollY;  // 记住瀑布流滚动位置，退出放大时恢复
        m_waterfallWidth = width();
    }
    m_zoom = idx;
    m_scrollY = 0;
    m_press = -1;
    m_hover = -1;
    setGeometry(computeZoomLayout());
    update();
}

void ItemPreviewPanel::enterZoom(int idx)
{
    if (idx < 0 || idx >= m_items.size())
        return;
    finishTransition();  // 上一段过渡未结束（快速连点）：先跳到其终点
    m_animKind = (m_zoom < 0) ? QStringLiteral("enter")
                              : QStringLiteral("switch");
    profTrace(QStringLiteral("[prof] enterZoom(%1) kind=%2\n")
                  .arg(idx).arg(m_animKind));

    // 旧状态快照与旧卡片矩形必须在状态变更前捕获
    const QRect fromGeo = geometry();
    QRect cardFrom;
    if (m_zoom >= 0) {
        cardFrom = m_zoomRect;  // 切换放大项：旧放大卡片
    } else if (idx < m_rects.size()) {
        cardFrom = m_rects.at(idx).translated(0, -m_scrollY);  // 点击的缩略图
    }
    const QImage snap = renderSnapshot();

    if (m_zoom < 0) {
        m_waterfallScroll = m_scrollY;
        m_waterfallWidth = width();
    }
    m_zoom = idx;
    m_scrollY = 0;
    m_press = -1;
    m_hover = -1;
    const QRect toGeo = computeZoomLayout();
    startTransition(snap, fromGeo, toGeo, cardFrom, m_zoomRect, idx);
}

void ItemPreviewPanel::leaveZoom()
{
    if (m_zoom < 0)
        return;
    finishTransition();
    m_animKind = QStringLiteral("leave");

    const QRect fromGeo = geometry();
    const int leavingIdx = m_zoom;
    const QRect cardFrom = m_zoomRect;
    const QImage snap = renderSnapshot();

    m_zoom = -1;
    m_zoomRect = QRect();
    buildLayout(std::max(m_waterfallWidth, 360));
    // 恢复瀑布流窗口尺寸（panelSize 会按屏幕重新计算宽高并钳制）
    const QSize ts = panelSize();
    const QRect g = QGuiApplication::primaryScreen()->availableGeometry();
    const QRect toGeo(g.center().x() - ts.width() / 2,
                      g.center().y() - ts.height() / 2, ts.width(),
                      ts.height());
    // 滚动恢复：钳制上限用目标窗口高度（与旧实现 resize 后再钳制等价）
    m_scrollY = std::max(
        0, std::min(m_waterfallScroll, std::max(0, m_contentH - ts.height())));
    m_press = -1;
    m_hover = -1;
    QRect cardTo;
    if (leavingIdx < m_rects.size())
        cardTo = m_rects.at(leavingIdx).translated(0, -m_scrollY);
    startTransition(snap, fromGeo, toGeo, cardFrom, cardTo, leavingIdx);
}

QSize ItemPreviewPanel::panelSize()
{
    const QRect g = QGuiApplication::primaryScreen()->availableGeometry();
    // 窗口宽：屏幕宽度的 55%，限制在 360~880 之间
    const int w = std::max(360, std::min(880, int(g.width() * 0.55)));
    buildLayout(w);
    // 窗口高：内容高度，最多占屏幕高度的 72%（超出部分滚轮滚动）
    const int maxH = std::max(200, int(g.height() * 0.72));
    const int h = std::max(160, std::min(m_contentH, maxH));
    return QSize(w, h);
}

void ItemPreviewPanel::showCentered()
{
    // 钩子若被系统移除（极少见），打开时重新安装
    if (!s_hook)
        installGlobalClickClose();
    commitPendingDelete();  // 重开前清掉上次可能残留的 pending
    clearUndoUi();
    finishTransition();  // 上次可能停在过渡中（快速关闭重开），先清理
    m_zoom = -1;  // 每次打开都从瀑布流开始
    m_zoomRect = QRect();
    resize(panelSize());
    // 与主把手一致：不使用二值遮罩，圆角由逐像素透明 + 抗锯齿绘制保证
    m_scrollY = 0;
    m_hover = -1;
    m_press = -1;
    const QRect g = QGuiApplication::primaryScreen()->availableGeometry();
    move(g.center().x() - width() / 2, g.center().y() - height() / 2);
    show();
}

int ItemPreviewPanel::maxScroll() const
{
    // 瀑布流与放大状态共用：放大状态下 m_contentH 是右侧单列的总高
    return std::max(0, m_contentH - height());
}

int ItemPreviewPanel::hitAt(const QPoint &pos) const
{
    // 过渡动画期间几何与布局都在插值中：不参与命中（无悬停高亮、点击忽略）
    if (transitionRunning())
        return -1;
    if (m_zoom >= 0) {
        // 放大状态：先查放大项本身，再查右侧单列
        if (m_zoomRect.contains(pos))
            return m_zoom;
        for (int i = 0; i < m_rects.size(); ++i) {
            if (m_rects.at(i).translated(0, -m_scrollY).contains(pos))
                return m_colIdx.at(i);
        }
        return -1;
    }
    for (int i = 0; i < m_rects.size(); ++i) {
        if (m_rects.at(i).translated(0, -m_scrollY).contains(pos))
            return i;
    }
    return -1;
}

void ItemPreviewPanel::deleteItemAt(int idx)
{
    if (!m_owner || idx < 0 || idx >= m_items.size())
        return;

    const QString path = m_items.at(idx).path;
    const QString label = QFileInfo(displayPath(path)).fileName();
    const bool wasZoomed = m_zoom >= 0;
    const int oldZoom = m_zoom;

    finishTransition();
    if (!m_owner->takePreviewItem(path))
        return;

    m_items = m_owner->previewItems();
    m_iconCache.clear();
    m_hover = -1;
    m_press = -1;
    m_pressBtn = Qt::NoButton;
    m_pressUndo = false;
    m_undoHover = false;
    QToolTip::hideText();

    if (m_items.isEmpty()) {
        // 删空立即关闭；hideEvent 会提交 pending，不保留撤销。
        hide();
        return;
    }

    if (wasZoomed) {
        if (oldZoom == idx) {
            // 删除放大项：退回瀑布流。
            m_zoom = -1;
            m_zoomRect = QRect();
            resize(panelSize());
            const QRect g = QGuiApplication::primaryScreen()->availableGeometry();
            move(g.center().x() - width() / 2,
                 g.center().y() - height() / 2);
            m_scrollY =
                std::max(0, std::min(m_waterfallScroll, maxScroll()));
        } else {
            // 删除右侧条目：修正放大项下标并重排右侧单列。
            m_zoom = oldZoom - (idx < oldZoom ? 1 : 0);
            setGeometry(computeZoomLayout());
            m_scrollY = std::max(0, std::min(m_scrollY, maxScroll()));
        }
    } else {
        // 瀑布流：重排后保持面板居中。
        m_zoom = -1;
        m_zoomRect = QRect();
        resize(panelSize());
        const QRect g = QGuiApplication::primaryScreen()->availableGeometry();
        move(g.center().x() - width() / 2,
             g.center().y() - height() / 2);
        m_scrollY = std::max(0, std::min(m_scrollY, maxScroll()));
    }

    startUndoCountdown(label);
    update();
}

void ItemPreviewPanel::undoItem()
{
    if (!m_owner || !m_owner->hasPendingDelete()) {
        clearUndoUi();
        return;
    }
    ShelfItem item;
    int index = -1;
    if (!m_owner->undoPreviewDelete(&item, &index)) {
        clearUndoUi();
        return;
    }
    setItems(m_owner->previewItems());
    clearUndoUi();
}

void ItemPreviewPanel::commitPendingDelete()
{
    if (m_owner)
        m_owner->commitPreviewDelete();
    clearUndoUi();
}

void ItemPreviewPanel::startUndoCountdown(const QString &label)
{
    m_undoLabel = label;
    m_undoRemainingMs = UNDO_MS;
    m_pressUndo = false;
    m_undoHover = false;
    if (m_undoTimer)
        m_undoTimer->start();
    update();
}

void ItemPreviewPanel::tickUndoCountdown()
{
    if (!undoVisible())
        return;
    const int step = m_undoTimer ? std::max(1, m_undoTimer->interval()) : 50;
    m_undoRemainingMs = std::max(0, m_undoRemainingMs - step);
    if (m_undoRemainingMs == 0) {
        commitPendingDelete();
        return;
    }
    update();
}

void ItemPreviewPanel::clearUndoUi()
{
    if (m_undoTimer)
        m_undoTimer->stop();
    m_undoRemainingMs = 0;
    m_undoLabel.clear();
    m_pressUndo = false;
    m_undoHover = false;
    update();
}

// 开发用：--render-preview 离屏输出撤销条（label 为空表示清除）
void ItemPreviewPanel::showUndoBarForTest(const QString &label, int remainingMs)
{
    m_testUndo = !label.isEmpty() && remainingMs > 0;
    m_undoLabel = m_testUndo ? label : QString();
    m_undoRemainingMs = m_testUndo ? remainingMs : 0;
    m_pressUndo = false;
    m_undoHover = false;
    update();
}

bool ItemPreviewPanel::undoVisible() const
{
    if (m_testUndo)  // 开发用：离屏渲染撤销条时绕过 owner 的待删除状态
        return m_undoRemainingMs > 0 && !m_undoLabel.isEmpty();
    return m_owner && m_owner->hasPendingDelete()
        && m_undoRemainingMs > 0 && !m_undoLabel.isEmpty();
}

QRect ItemPreviewPanel::undoBarRect() const
{
    const int h = std::min(UNDO_BAR_H, height());
    return QRect(0, std::max(0, height() - h), width(), h);
}

bool ItemPreviewPanel::undoBarAt(const QPoint &pos) const
{
    return undoVisible() && undoBarRect().contains(pos);
}

void ItemPreviewPanel::paintUndoBar(QPainter &p)
{
    if (!undoVisible())
        return;

    const QRect r = undoBarRect();
    p.save();
    p.fillRect(r, QColor(49, 44, 35, 232));
    p.setPen(QPen(QColor(255, 255, 255, 34), 1.0));
    p.drawLine(r.topLeft(), r.topRight());

    const int pad = 14;
    const int undoW = 62;
    const int countW = 52;
    const int gap = 10;
    const QRect countRect(r.right() - pad - countW + 1, r.top() + 1,
                          countW, r.height() - 1);
    const QRect undoRect(countRect.left() - gap - undoW, r.top() + 7,
                         undoW, r.height() - 14);
    if (m_undoHover) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 38));
        p.drawRoundedRect(undoRect, 6, 6);
    }

    QFont font = UiFont::make(10);  // 微软雅黑 Light
    p.setFont(font);
    const QRect nameRect(r.left() + pad, r.top(), undoRect.left() - pad - 10,
                         r.height());
    const QString name = p.fontMetrics().elidedText(
        m_undoLabel, Qt::ElideMiddle, std::max(0, nameRect.width()));
    p.setPen(QColor(255, 255, 255, 225));
    p.drawText(nameRect, Qt::AlignVCenter | Qt::AlignLeft, name);
    p.setPen(QColor(255, 255, 255, 245));
    p.drawText(undoRect, Qt::AlignCenter, QStringLiteral("撤销"));

    const int seconds = std::max(1, (m_undoRemainingMs + 999) / 1000);
    p.setPen(QColor(255, 255, 255, 190));
    p.drawText(countRect, Qt::AlignCenter,
               QStringLiteral("%1 秒").arg(seconds));
    p.restore();
}

void ItemPreviewPanel::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    // 性能探针（开发用）：过渡帧测量整帧绘制耗时（GL 渲染 + 回读 + 贴图）
    QElapsedTimer profTimer;
    if (m_profiling && transitionRunning())
        profTimer.start();
    // 优先 GPU 渲染（4× MSAA）：圆角边缘由多重采样覆盖；
    // GL 不可用或渲染失败时回退到软件光栅 2× 超采样
    QImage img = GLCanvas::instance().render(
        size(), 1, [this](QPainter &p) { paintContents(p); });
    if (img.isNull()) {
        constexpr int SS = 2;
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
    if (profTimer.isValid())
        m_profMs.append(profTimer.nsecsElapsed() / 1e6);
}

void ItemPreviewPanel::paintContents(QPainter &p)
{
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    if (transitionRunning()) {
        paintTransition(p, m_zoomAnim->currentValue().toDouble());
    } else {
        paintPanelBackground(p);
        paintState(p, -1);
    }
    if (undoVisible()) {
        // 过渡中 paintPanelBackground 的圆角裁剪会随 save/restore 结束，
        // 这里单独建立裁剪，保证撤销条也贴合面板圆角。
        p.save();
        QPainterPath clip;
        clip.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                            RADIUS, RADIUS);
        p.setClipPath(clip);
        paintUndoBar(p);
        p.restore();
    }
}

void ItemPreviewPanel::paintPanelBackground(QPainter &p)
{
    QPainterPath panelPath;
    panelPath.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                             RADIUS, RADIUS);
    p.setClipPath(panelPath);

    // 米白色平涂（无渐变）
    p.setPen(QPen(QColor(80, 70, 50, 60), 1.0));
    p.setBrush(QColor(247, 243, 232));
    p.drawPath(panelPath);
}

// 带缓存的系统图标：过渡动画每帧都会重绘文件卡片，直接走
// QFileIconProvider 会反复触发 shell 取图；按 path|px 缓存 QPixmap。
QPixmap ItemPreviewPanel::cardIcon(const ShelfItem &it, int size)
{
    const QString key = it.path + QStringLiteral("|%1").arg(size);
    if (const QPixmap *cached = m_iconCache.object(key))
        return *cached;
    QPixmap pm = m_icons.icon(QFileInfo(it.path)).pixmap(size, size);
    if (!pm.isNull())
        m_iconCache.insert(key, new QPixmap(pm), 1);
    return pm;
}

// 单张卡片：白底图片卡（图片按比例铺入卡片居中）或米深底文件卡（系统图标）
void ItemPreviewPanel::drawCard(QPainter &p, const ShelfItem &it, const QRect &r,
                                bool hover, bool fileName, int iconSizeOverride,
                                bool hiRes)
{
    const QRectF rf = QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath card;
    card.addRoundedRect(rf, 8, 8);

    p.save();
    p.setClipPath(card);
    const bool imgItem = isImageItem(it);
    if (imgItem) {
        // 白底卡片，图片按原比例缩放到卡片尺寸居中
        p.fillPath(card, QColor(255, 255, 255));
#ifdef SIDESHELF_PERF_BASELINE
        // 原实现：按卡片矩形精确尺寸解码（基线对照用）
        const QImage img = loadCachedImageBaseline(it, r.size(), m_imageCache);
        if (!img.isNull()) {
            const QPointF tl(r.x() + (r.width() - img.width()) / 2.0,
                             r.y() + (r.height() - img.height()) / 2.0);
            p.drawImage(tl, img);
        } else {
            const QPixmap pm =
                m_icons.icon(QFileInfo(it.path)).pixmap(48, 48);
            p.drawPixmap(r.center().x() - pm.width() / 2,
                         r.center().y() - pm.height() / 2, pm);
        }
#else
        // 解码尺寸与卡片矩形不再要求 1:1（缓存键尺寸无关）：过渡中卡片
        // 逐帧形变，按矩形尺寸解码会每帧 miss 并触发整图重解码
        const QImage img = loadCachedImage(
            it, hiRes ? ImgLevel::Full : ImgLevel::Thumb, m_imageCache);
        if (!img.isNull()) {
            QSize ds = img.size();
            ds.scale(r.size(), Qt::KeepAspectRatio);
            const QRectF dr(r.x() + (r.width() - ds.width()) / 2.0,
                            r.y() + (r.height() - ds.height()) / 2.0,
                            ds.width(), ds.height());
            p.drawImage(dr, img);
        } else {
            const QPixmap pm = cardIcon(it, 48);
            p.drawPixmap(r.center().x() - pm.width() / 2,
                         r.center().y() - pm.height() / 2, pm);
        }
#endif
    } else {
        // 文件：米白深一点的底 + 系统图标（放大状态额外显示文件名）
        p.fillPath(card, QColor(240, 236, 222));
        const bool hasOverride = iconSizeOverride > 0;
        const int iconSize =
            hasOverride ? iconSizeOverride : (fileName ? ZOOM_ICON : 48);
#ifndef SIDESHELF_PERF_BASELINE
        const QPixmap pm = cardIcon(it, iconSize);
#else
        const QPixmap pm =
            m_icons.icon(QFileInfo(it.path)).pixmap(iconSize, iconSize);
#endif
        // 过渡形变中的文件卡片：图标随卡片一起缩放并保持居中，暂不显示文件名
        const int iconY = (fileName && !hasOverride)
                              ? r.y() + (r.height() - iconSize - 44) / 2
                              : r.center().y() - pm.height() / 2;
        p.drawPixmap(r.center().x() - pm.width() / 2, iconY, pm);
        if (fileName && !hasOverride) {
            // 文件名只会出现在放大状态（用户确认：该状态大图标+文件名）
            const QString name = QFileInfo(it.path).fileName();
            p.setFont(UiFont::make(ZOOM_FILE_FONT));
            p.setPen(QColor(60, 52, 38));
            const QRect textRect(r.x() + 14, r.y() + r.height() - 40,
                                 r.width() - 28, 26);
            p.drawText(textRect, Qt::AlignCenter,
                       p.fontMetrics().elidedText(name, Qt::ElideMiddle,
                                                  textRect.width()));
        }
    }
    p.restore();

    // 卡片描边与悬停亮层
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(80, 70, 50, hover ? 110 : 55), 1.0));
    p.drawPath(card);
    if (hover) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(60, 50, 30, 18));
        p.drawPath(card);
    }
}

// 当前状态的常规绘制（供正常重绘与过渡中的“新状态淡入”复用）。
// skipIdx >= 0 时跳过该条目的卡片（由 paintTransition 以形变矩形单独绘制）。
void ItemPreviewPanel::paintState(QPainter &p, int skipIdx)
{
    if (m_zoom >= 0) {
        // ---- 放大状态：放大项在左，其余条目在右侧自上而下单列排布 ----
        // 防御：布局与条目数不一致时跳过，避免越界
        if (m_rects.size() != m_colIdx.size() || m_zoomRect.isEmpty()
            || m_zoom >= m_items.size()) {
            return;
        }
        // 先画右侧单列（普通尺寸卡片，悬停/中键/左键切换语义一致）
        for (int i = 0; i < m_rects.size(); ++i) {
            const QRect r = m_rects.at(i).translated(0, -m_scrollY);
            if (r.bottom() < 0 || r.top() > height())
                continue;
            const int idx = m_colIdx.at(i);
            if (idx == skipIdx)
                continue;
            drawCard(p, m_items.at(idx), r, idx == m_hover, false);
        }
        // 再画放大项（全尺寸级别解码：放大显示需要原图细节）
        if (m_zoom != skipIdx) {
            drawCard(p, m_items.at(m_zoom), m_zoomRect, m_zoom == m_hover,
                     !isImageItem(m_items.at(m_zoom)), -1, true);
        }
        return;
    }

    // ---- 瀑布流状态 ----
    // 防御：布局与条目数不一致时跳过，避免越界
    if (m_rects.size() != m_items.size())
        return;
    for (int i = 0; i < m_items.size(); ++i) {
        if (i == skipIdx)
            continue;
        const QRect r = m_rects.at(i).translated(0, -m_scrollY);
        if (r.bottom() < 0 || r.top() > height())
            continue;
        drawCard(p, m_items.at(i), r, i == m_hover, false);
    }
}

// 过渡帧：旧状态快照（随窗口形变拉伸）淡出 + 新状态淡入 +
// 共享卡片（被放大/切换的条目）全不透明地从旧位置形变到新位置。
void ItemPreviewPanel::paintTransition(QPainter &p, double t)
{
    // 1) 旧状态快照：拉伸铺满当前窗口，透明度 1-t 淡出
    if (!m_animSnapshot.isNull() && !m_animFromGeo.isEmpty()) {
        p.save();
        p.setOpacity(1.0 - t);
        p.drawImage(QRectF(rect()), m_animSnapshot,
                    QRectF(m_animSnapshot.rect()));
        p.restore();
    }
    // 2) 新状态：整体淡入（背景 + 除共享卡片外的全部内容）
    p.save();
    p.setOpacity(t);
    paintPanelBackground(p);
    paintState(p, m_animCardIdx);
    p.restore();
    // 3) 共享卡片：把旧矩形按“窗口形变比例”映射到当前坐标系后与目标矩形
    //    插值。t=0 时与快照中的卡片完全重合，实现无缝展开/收缩/切换
    if (m_animCardIdx >= 0 && m_animCardIdx < m_items.size()
        && !m_animCardTo.isEmpty() && m_animFromGeo.width() > 0
        && m_animFromGeo.height() > 0) {
        const double sx = width() / double(m_animFromGeo.width());
        const double sy = height() / double(m_animFromGeo.height());
        const QRectF from(m_animCardFrom.x() * sx, m_animCardFrom.y() * sy,
                          m_animCardFrom.width() * sx,
                          m_animCardFrom.height() * sy);
        const QRectF to(m_animCardTo);
        const QRectF r(from.x() + (to.x() - from.x()) * t,
                       from.y() + (to.y() - from.y()) * t,
                       from.width() + (to.width() - from.width()) * t,
                       from.height() + (to.height() - from.height()) * t);
        const bool zoomStyle = (m_zoom >= 0 && m_zoom == m_animCardIdx);
        const ShelfItem &it = m_items.at(m_animCardIdx);
        // 文件卡片：图标随形变在 48↔128 之间连续缩放并保持居中
        // （放大态目标→128，退回瀑布流目标→48），文件名由快照/终态淡入淡出
        int overrideSize = -1;
        if (!isImageItem(it)) {
            const double k = zoomStyle ? t : 1.0 - t;
            const int grow = qRound(48.0 + (ZOOM_ICON - 48) * k);
            const int fit =
                std::max(48, std::min(int(r.height()) - 12, ZOOM_ICON));
            overrideSize = std::min(grow, fit);
        }
        // 形变卡片用全尺寸级别解码：矩形逐帧变化，尺寸相关键会每帧重解码
        drawCard(p, it, r.toAlignedRect(), false, false, overrideSize, true);
    }
}

void ItemPreviewPanel::mousePressEvent(QMouseEvent *e)
{
    profTrace(QStringLiteral("[prof] press btn=%1 pos=%2,%3 hit=%4\n")
                  .arg(int(e->button()))
                  .arg(int(e->position().x()))
                  .arg(int(e->position().y()))
                  .arg(hitAt(e->position().toPoint())));
    const QPoint pos = e->position().toPoint();
    m_pressUndo = (e->button() == Qt::LeftButton && undoBarAt(pos));
    if (m_pressUndo) {
        m_press = -1;
        m_pressBtn = Qt::LeftButton;
        return;
    }
    if (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton
        || e->button() == Qt::RightButton) {
        m_press = hitAt(pos);
        m_pressBtn = e->button();
    } else {
        m_press = -1;
        m_pressBtn = Qt::NoButton;
    }
}

void ItemPreviewPanel::mouseMoveEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    const bool undoHover = undoBarAt(pos);
    const int h = undoHover ? -1 : hitAt(pos);
    bool changed = false;
    if (undoHover != m_undoHover) {
        m_undoHover = undoHover;
        changed = true;
    }
    if (h != m_hover) {
        m_hover = h;
        changed = true;
    }
    if (undoHover)
        QToolTip::hideText();
    if (changed)
        update();
}

void ItemPreviewPanel::mouseReleaseEvent(QMouseEvent *e)
{
    profTrace(QStringLiteral("[prof] release btn=%1 pos=%2,%3 press=%4\n")
                  .arg(int(e->button()))
                  .arg(int(e->position().x()))
                  .arg(int(e->position().y()))
                  .arg(m_press));
    if (e->button() != Qt::LeftButton && e->button() != Qt::MiddleButton
        && e->button() != Qt::RightButton)
        return;
    if (m_pressBtn != e->button()) {
        m_press = -1;
        m_pressBtn = Qt::NoButton;
        m_pressUndo = false;
        return;
    }
    const QPoint pos = e->position().toPoint();
    if (e->button() == Qt::LeftButton && m_pressUndo) {
        if (undoBarAt(pos))
            undoItem();
        m_press = -1;
        m_pressBtn = Qt::NoButton;
        m_pressUndo = false;
        return;
    }
    const int h = undoBarAt(pos) ? -1 : hitAt(pos);
    if (m_press >= 0 && h == m_press && m_press < m_items.size()) {
        if (e->button() == Qt::RightButton) {
            // 右键：按下与松开命中同一条目才删除
            deleteItemAt(m_press);
        } else if (e->button() == Qt::MiddleButton) {
            // 中键：用系统默认程序打开（窗口保持打开，点击窗口外才会关闭）
            QDesktopServices::openUrl(
                QUrl::fromLocalFile(m_items.at(m_press).path));
        } else {
            // 左键：瀑布流 → 放大该项；放大状态 → 点放大项退回瀑布流，
            // 点右侧列中其他条目则切换放大该项
            if (m_zoom >= 0) {
                if (m_press == m_zoom)
                    leaveZoom();
                else
                    enterZoom(m_press);
            } else {
                enterZoom(m_press);
            }
        }
    }
    m_press = -1;
    m_pressBtn = Qt::NoButton;
    m_pressUndo = false;
}

void ItemPreviewPanel::leaveEvent(QEvent *e)
{
    Q_UNUSED(e);
    if (m_hover != -1 || m_undoHover) {
        m_hover = -1;
        m_undoHover = false;
        update();
    }
}

void ItemPreviewPanel::hideEvent(QHideEvent *e)
{
    // 窗外点击 / 再按中键关闭：立即终止进行中的过渡并跳到终点，
    // 快照及时释放；下次打开由 showCentered 重置一切
    finishTransition();
    QToolTip::hideText();
    commitPendingDelete();  // 关闭面板 = 提交删除
    clearUndoUi();
    QWidget::hideEvent(e);
}

void ItemPreviewPanel::wheelEvent(QWheelEvent *e)
{
    const int step = 48;
    const int delta = e->angleDelta().y();
    m_scrollY = std::max(
        0, std::min(maxScroll(), m_scrollY - (delta / 120) * step));
    update();
}

void ItemPreviewPanel::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape) {
        // 放大状态下先退回瀑布流，再按一次才关闭窗口
        if (m_zoom >= 0)
            leaveZoom();
        else
            hide();
        return;
    }
    QWidget::keyPressEvent(e);
}

bool ItemPreviewPanel::event(QEvent *e)
{
    if (e->type() == QEvent::ToolTip) {
        // 窗口本身不显示任何文字；悬停时用系统 Tooltip 标明文件名称与路径
        auto *he = static_cast<QHelpEvent *>(e);
        if (undoBarAt(he->pos())) {
            QToolTip::hideText();
            return true;
        }
       const int idx = hitAt(he->pos());
       if (idx >= 0 && idx < m_items.size()) {
           const QString p0 = displayPath(m_items.at(idx).path);
           QToolTip::showText(
               he->globalPos(),
                QStringLiteral("%1\n%2\n右键删除 · 中键打开 · 左键放大").arg(QFileInfo(p0).fileName(), p0),
               this);
        } else {
            QToolTip::hideText();
        }
        return true;
    }
    return QWidget::event(e);
}

// 钩子消息的 GUI 线程处理：任意一次鼠标按下，若发生在窗口外则关闭。
// 所有 Qt 操作都在这里完成，钩子回调本身保持极简（见 llMouseProc）。
bool ItemPreviewPanel::nativeEvent(const QByteArray &eventType, void *message,
                                   qintptr *result)
{
    if (eventType == "windows_generic_MSG") {
        const MSG *msg = static_cast<const MSG *>(message);
        if (msg->message == s_msgMouseDownOutside) {
            if (isVisible()) {
                // 钩子坐标是 Win32 物理屏幕像素；frameGeometry() 是 Qt 逻辑
                // 坐标，高 DPI 下直接比对会把面板内点击误判为窗外，导致右键
                // 按下先被 hide 吃掉、mousePress 收不到。用原生 GetWindowRect
                //（同为物理像素）在同一坐标系下判定。
                const POINT pt{static_cast<LONG>(msg->wParam),
                               static_cast<LONG>(msg->lParam)};
                bool outside = true;
                const HWND hwnd = reinterpret_cast<HWND>(winId());
                RECT r{};
                if (hwnd && GetWindowRect(hwnd, &r)) {
                    outside = (PtInRect(&r, pt) == FALSE);
                } else {
                    const QPoint gp(static_cast<int>(msg->wParam),
                                    static_cast<int>(msg->lParam));
                    outside = !frameGeometry().contains(gp);
                }
                if (outside)
                    hide();
            }
            if (result)
                *result = 0;
            return true;
        }
    }
    return QWidget::nativeEvent(eventType, message, result);
}
