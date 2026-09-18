// -*- coding: utf-8 -*-
// 暂存内容预览窗口：屏幕中央弹出，米白色平涂（无渐变、无文字）。
// 图片以瀑布流方式排布并保持原始宽高比；文件显示系统图标。
//
// 窗口类型与主把手完全一致（Qt::Tool + 无边框 + 置顶 + 遮罩），
// 不依赖 Qt::Popup / 焦点机制。“点击窗口外关闭”通过 Windows 全局
// 低级鼠标钩子（WH_MOUSE_LL）实现：任何发生在窗口外的鼠标按下都会关闭它。

#pragma once

// 头文件中用到 HHOOK / CALLBACK / WPARAM 等 Win32 类型，直接包含 windows.h
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <QWidget>
#include <QList>
#include <QRect>
#include <QImage>
#include <QPixmap>
#include <QFileIconProvider>
#include <QCache>

#include "shelfwidget.h"  // ShelfItem

class QMouseEvent;
class QPaintEvent;
class QWheelEvent;
class QKeyEvent;
class QHelpEvent;
class QHideEvent;
class QPainter;
class QVariantAnimation;
class QTimer;

class ItemPreviewPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ItemPreviewPanel(QWidget *parent = nullptr);
    ~ItemPreviewPanel() override;

    void setItems(const QList<ShelfItem> &items);
    void setOwner(ShelfWidget *owner) { m_owner = owner; }
    QSize panelSize();    // 按当前屏幕尺寸重新计算布局后返回窗口尺寸
    void showCentered();  // 屏幕中央弹出（主显示器可用区域中心）
    // 开发用：直接进入放大状态（--render-preview 离屏渲染放大布局，无事件循环、瞬时完成）
    void enterZoomForTest(int idx) { enterZoomInstant(idx); }
    // 开发用：带过渡动画的放大/退回（--test-panel-heavy 性能测量，绕开输入注入）
    void animateZoomForTest(int idx) { enterZoom(idx); }
    void animateUnzoomForTest() { leaveZoom(); }
    // 开发用：强制显示底部撤销条（--render-preview 离屏输出 .panel.undo.png，
    // 用于核对「N 秒 / 撤销 / 文件名」所用字体）。label 为空即清除。
    void showUndoBarForTest(const QString &label, int remainingMs);

protected:
    void paintEvent(QPaintEvent *event) override;
    void paintContents(QPainter &p);  // 在逻辑坐标中绘制全部内容（供超采样渲染复用）
    void paintPanelBackground(QPainter &p);  // 圆角裁剪 + 米白底色
    void paintState(QPainter &p, int skipIdx);  // 瀑布流/放大状态内容（跳过 skipIdx 条目）
    void paintTransition(QPainter &p, double t);  // 放大切换过渡帧（快照淡出+新状态淡入+卡片形变）
    void drawCard(QPainter &p, const ShelfItem &it, const QRect &r,
                  bool hover, bool fileName,
                  int iconSizeOverride = -1, bool hiRes = false);  // 单张卡片（白底图片卡/文件图标卡）；
                                                // iconSizeOverride>0 时文件图标随
                                                // 过渡形变缩放并保持居中（不显示文件名）；
                                                // hiRes 时图片按全尺寸级别解码（放大项/形变卡）
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    bool event(QEvent *e) override;
    bool nativeEvent(const QByteArray &eventType, void *message,
                     qintptr *result) override;

private:
    // ---- 布局常量 ----
    static constexpr int MARGIN = 14;        // 四周留白
    static constexpr int GUTTER = 10;        // 卡片间距
    static constexpr int COL_TARGET = 190;   // 目标列宽（按窗口宽度自动调整列数）
    static constexpr int FILE_H = 132;       // 非图片（文件）卡片高度
    static constexpr int RADIUS = 12;        // 窗口圆角
    static constexpr int ZOOM_COL_W = 190;   // 放大状态右侧单列宽度
    static constexpr int ZOOM_EDGE_GAP = 12; // 放大状态窗口与屏幕边缘的最小间隙
    static constexpr int ZOOM_FILE_W = 460;  // 放大状态文件卡片默认宽度
    static constexpr int ZOOM_FILE_H = 320;  // 放大状态文件卡片默认高度
    static constexpr int ZOOM_ICON = 128;    // 放大状态文件图标尺寸
    static constexpr int ZOOM_FILE_FONT = 13; // 放大状态文件名字号
    static constexpr int UNDO_BAR_H = 56;    // 底部撤销条高度
    static constexpr int UNDO_MS = 5000;     // 删除撤销时限

    void buildLayout(int width);             // 瀑布流布局
    QRect computeZoomLayout();               // 放大状态布局：填充 m_rects/m_colIdx/m_zoomRect，
                                             // 返回目标窗口矩形（不应用几何）
    QSize originalSize(const ShelfItem &it) const;  // 图片原始尺寸 / 文件默认卡片尺寸
    void enterZoom(int idx);                 // 放大第 idx 项（带窗口形变+卡片过渡动画）
    void enterZoomInstant(int idx);          // 放大第 idx 项（瞬时，离屏渲染用）
    void leaveZoom();                        // 回到瀑布流（带过渡动画）
    void startTransition(const QImage &snap, const QRect &fromGeo,
                         const QRect &toGeo, const QRect &cardFrom,
                         const QRect &cardTo, int cardIdx);
    void finishTransition();                 // 跳到进行中过渡的终点并清理
    QImage renderSnapshot();                 // 渲染当前状态为 1× 快照（GL/软件回退）
    bool transitionRunning() const;
    int maxScroll() const;
    int hitAt(const QPoint &pos) const;
    void deleteItemAt(int idx);
    void undoItem();
    void commitPendingDelete();
    void startUndoCountdown(const QString &label);
    void tickUndoCountdown();
    void clearUndoUi();
    bool undoVisible() const;
    QRect undoBarRect() const;
    bool undoBarAt(const QPoint &pos) const;
    void paintUndoBar(QPainter &p);

    // ---- 图片解码缓存（两级、尺寸无关）----
    // T = 缩略图级别（列宽 2× HiDPI 余量），F = 全尺寸级别（约 2× 屏显尺寸，
    // 长边封顶 4096）。键不含目标矩形尺寸：过渡中卡片逐帧形变，键含尺寸会
    // 每帧 miss 并触发整图重解码（卡顿主因）。
    enum class ImgLevel { Thumb, Full };
    static QSize levelFitSize(ImgLevel level);
    static QString imageCacheKey(const ShelfItem &item, ImgLevel level);
    static QImage loadCachedImage(const ShelfItem &item, ImgLevel level,
                                  QCache<QString, QImage> &cache);
    QPixmap cardIcon(const ShelfItem &it, int size);  // 带缓存的系统图标
    void profTrace(const QString &what);  // 性能探针：跟踪行（开发用）
    void profFlush();  // 性能探针：写出当前过渡的逐帧耗时统计（开发用）

    // ---- 全局鼠标钩子（点击窗口外 → 关闭）----
    // 钩子回调只做 PostMessageW（纯 Win32、绝无超时风险），
    // 关闭判断在 GUI 线程的 nativeEvent 中完成
    static ItemPreviewPanel *s_instance;
    static HHOOK s_hook;
    static HWND s_hookWnd;   // 接收钩子消息的窗口句柄（面板自身）
    static UINT s_msgMouseDownOutside;
    static LRESULT CALLBACK llMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    void installGlobalClickClose();
    void removeGlobalClickClose();

    QList<ShelfItem> m_items;
    mutable QCache<QString, QImage> m_imageCache;
    mutable QCache<QString, QPixmap> m_iconCache;  // 系统图标缓存（键 path|px）
    QList<QRect> m_rects;      // 瀑布流卡片矩形（未滚动坐标）；放大状态下为右侧单列
    QList<int> m_colIdx;       // 放大状态：m_rects[i] 对应 m_items 的下标
    int m_contentH = 0;        // 内容总高度
    QFileIconProvider m_icons;
    int m_hover = -1;          // 悬停的条目下标
    int m_press = -1;          // 按下的条目下标
    Qt::MouseButton m_pressBtn = Qt::NoButton;  // 按下的键（左/中/右键）
    int m_scrollY = 0;         // 内部滚动偏移（瀑布流/放大状态的右侧列共用）
    int m_zoom = -1;           // 放大状态：放大项下标；-1 = 瀑布流
    QRect m_zoomRect;          // 放大项显示矩形（窗口逻辑坐标）
    int m_waterfallScroll = 0; // 进入放大前保存的瀑布流滚动位置
    int m_waterfallWidth = 0;  // 进入放大前保存的瀑布流窗口宽度

    // ---- 单项删除后的 5 秒撤销条 ----
    ShelfWidget *m_owner = nullptr;
    QTimer *m_undoTimer = nullptr;
    int m_undoRemainingMs = 0;
    QString m_undoLabel;
    bool m_testUndo = false;    // 开发用：强制显示撤销条（离屏渲染），不涉真实待删除状态
    bool m_pressUndo = false;   // 左键按在撤销条上
    bool m_undoHover = false;   // 鼠标悬停在撤销条上

    // ---- 放大/退回/切换的过渡动画状态 ----
    // 方案：旧状态快照（随窗口形变拉伸淡出）+ 新状态淡入 + 共享卡片
    // （放大卡片）从旧矩形形变到新矩形（全不透明），窗口几何同步插值。
    QVariantAnimation *m_zoomAnim = nullptr;  // 过渡动画（父对象 this）
    QImage m_animSnapshot;      // 过渡前旧状态整窗快照（旧窗口坐标系）
    QRect m_animFromGeo;        // 起始窗口几何
    QRect m_animToGeo;          // 目标窗口几何
    QRect m_animCardFrom;       // 共享卡片起始矩形（旧窗口坐标系）
    QRect m_animCardTo;         // 共享卡片目标矩形（新窗口坐标系）
    int m_animCardIdx = -1;     // 共享卡片对应条目下标；-1 = 无

    // ---- 性能探针（SIDESHELF_PROF=1 启用，开发用）----
    bool m_profiling = false;
    QString m_animKind;         // 当前过渡类型：enter / switch / leave
    QList<double> m_profMs;     // 当前过渡逐帧 paintEvent 耗时（ms）
};
