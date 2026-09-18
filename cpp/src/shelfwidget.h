// -*- coding: utf-8 -*-
// Side Shelf（侧边暂存栏）—— C++20 + Qt6 移植版
//
// 与原 Python/PySide6 版行为保持一致：
//   常驻屏幕右侧的半透明小把手，临时中转文件与剪贴板里的非文字内容。

#pragma once

#include <QWidget>
#include <QColor>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QList>
#include <QSet>
#include <QHash>
#include <QImage>
#include <QPixmap>
#include <QPainterPath>
#include <QMimeData>

class QPropertyAnimation;
class QTimer;
class QMouseEvent;
class QWheelEvent;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QContextMenuEvent;
class QCloseEvent;
class QPaintEvent;
class QPainter;
class ItemPreviewPanel;

// 暂存项：kind == "file" 时记录用户原始文件路径；kind == "image" 时是
// 本程序保存的剪贴板图片副本路径（位于 stash 目录内，退出时删除）。
struct ShelfItem
{
    QString kind;      // "file" | "image"
    QString path;
    QString sha256;    // 仅图片有
};

class ShelfWidget : public QWidget
{
    Q_OBJECT
    // 供 QPropertyAnimation 动画的 float 属性（对应 Python 版 Property(float, ...)）
    Q_PROPERTY(float slide READ slide WRITE setSlide)
    Q_PROPERTY(float pulse READ pulse WRITE setPulse)
    Q_PROPERTY(float hoverP READ hoverP WRITE setHoverP)

public:
    // ---- 几何常量（public 供 selftest 使用）----
    // 竖长胶囊把手：数量居中，与上下功能小圆共用同一轴线
    static constexpr int R_SMALL = 13;    // 功能小圆绘制半径
    static constexpr int HANDLE_W = 2 * R_SMALL; // 把手宽度 = 小圆直径（三元素同宽对齐）
    static constexpr int HANDLE_H = 64;   // 把手高度（两端全圆角）
    static constexpr int FEATHER = 12;    // 窗口左侧留白：容纳阴影/抗锯齿 bleed
    static constexpr int R_HIT = 15;      // 功能小圆点击半径
    static constexpr int GAP = 12;        // 把手与小圆之间的垂直空隙
    static constexpr int EDGE_PAD = 6;    // 整组元素与屏幕右缘的浮动留白
    static constexpr int HIDE = 10 + FEATHER; // 收起时留在屏幕内的宽度（把手左缘 10px）
    static constexpr int W = FEATHER + HANDLE_W + EDGE_PAD;
    static constexpr int H = HANDLE_H + 2 * GAP + 4 * (R_SMALL + 4);

    explicit ShelfWidget(const QString &stateFile,
                         const QString &stashDir,
                         const QString &logFile,
                         QWidget *parent = nullptr);
    ~ShelfWidget() override;

    // ---- 暂存逻辑（selftest 也直接调用这些方法）----
    int addFiles(const QStringList &paths);
    int addImage(const QImage &image);
    void clearItems(bool flash = true);
    void storeFromClipboard();

    // ---- 预览内单项删除（右键）+ 5 秒撤销 ----
    // takePreviewItem：提交已有 pending（真删上一个），再从 m_items/去重集合摘除
    // path 对应项并暂存为 pending，stash 副本延迟到 commit 才真删。返回是否找到。
    bool takePreviewItem(const QString &path, ShelfItem *outItem = nullptr,
                         int *outIndex = nullptr);
    // undoPreviewDelete：把 pending 按原下标插回 m_items/去重集合。返回是否有 pending。
    bool undoPreviewDelete(ShelfItem *outItem = nullptr, int *outIndex = nullptr);
    // commitPreviewDelete：真删 pending 的 stash 副本并清空 pending（幂等）。
    void commitPreviewDelete();
    bool hasPendingDelete() const { return m_pending.active; }
    // 预览面板重建/对账用：当前暂存快照（按暂存顺序）
    QList<ShelfItem> previewItems() const { return m_items; }

    // ---- 自动监测剪贴板 ----
    void setAutoMonitor(bool on);

    // ---- 拖出全部暂存内容（复制）----
    void startDragOut();

    // ---- 透明度 ----
    void setOpacity(double value);
    double opacity() const { return m_opacity; }

    // ---- 动画属性 ----
    float slide() const { return m_slide; }
    void setSlide(float v);
    float pulse() const { return m_pulse; }
    void setPulse(float v);
    float hoverP() const { return m_hoverP; }
    void setHoverP(float v);

    // ---- 几何（selftest 校验用）----
    int savedY() const { return m_y; }
    int xShown() const { return m_xShown; }
    int xHidden() const { return m_xHidden; }
    int screenRight() const { return m_screenRight; }
    bool autoMonitor() const { return m_autoMonitor; }
    int itemCount() const { return m_items.size(); }

    // 处理剪贴板 / 拖入内容：文件与图片独立判断（互不排斥），返回新增数量
    int processClipboardContent(const QMimeData *md);

    // 处理拖入内容：位于系统临时目录的文件（压缩软件拖出解压等场景）先复制进
    // stash 再暂存（副本随清空一起删除），其余文件沿用「只记录路径」；图片位图
    // 仍走 addImage。返回新增数量；copiedCount（可选）返回其中复制保存的个数。
    int processDropContent(const QMimeData *md, int *copiedCount = nullptr);
    // 路径是否位于系统临时目录（%TEMP% / TEMP / TMP 之下）且不在本程序 stash
    // 目录内。从压缩软件（7-Zip / WinRAR / Bandizip / 资源管理器 zip 视图）拖出
    // 文件时，源程序会把内容物化到临时目录再给出路径，随后即被清理。
    bool isTransientTempPath(const QString &path) const;
    // 自检用：当前全部暂存项路径（按暂存顺序）
    QStringList itemPaths() const;

    // 自检用：手动触发一次剪贴板轮询（对应 Python 版 w._poll_clipboard()）
    void pollClipboardForTest() { pollClipboard(); }
    // 开发用：直接弹出预览窗口（对应 --test-panel）
    void openPreviewForTest() { openPreview(); }
    // 开发用：预览窗口当前是否可见（点击语义自检用；定义在 cpp 中，
    // 因为此处只有 ItemPreviewPanel 的前向声明，不能调用其成员）
    bool previewVisibleForTest() const;
    // 开发用：预览窗口当前全局几何（点击语义自检用）
    QRect previewGeometryForTest() const;
    // 开发用：带过渡动画的放大/退回（--test-panel-heavy 性能测量，
    // 代码直触发，绕开输入注入；定义在 cpp 中，此处只有前向声明）
    void animatePreviewZoomForTest(int idx);
    void animatePreviewUnzoomForTest();

protected:
    void paintEvent(QPaintEvent *event) override;
    void paintContents(QPainter &p);  // 在逻辑坐标中绘制全部内容（供超采样渲染复用）
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    bool event(QEvent *e) override;
    void closeEvent(QCloseEvent *e) override;

private:
    // ---- 状态持久化 ----
    void loadState();
    void saveState();

    // ---- 剪贴板 ----
    bool hasImageFormats(const QMimeData *md) const;
    QImage extractImage(const QMimeData *md) const;
    qint64 clipSequence() const;
    void pollClipboard();
    void logClip(const QStringList &formats, int added, const QString &extra = QString());

    // 把拖入的临时文件复制进 stash 目录（保留原文件名；同名同大小视为重复直接
    // 复用现有副本；同名不同大小仿资源管理器追加「 (2)」…）。返回 stash 内目标
    // 路径；失败返回空 QString。
    QString copyIntoStash(const QString &srcPath);

    // ---- 边缘感应 / 全屏检测 / 滑入滑出 ----
    bool isFullscreenActive();
    void poll();
    void setShown(bool shown);

    // ---- 外观 ----
    QHash<QString, QPointF> centers() const;
    QString hitAt(const QPointF &pos) const;
    QPainterPath shapePath() const;
    void startHoverAnim(float target);
    void flash(const QColor &color);
    QPixmap dragPixmap(int count) const;
    void openPreview();    // 中键：屏幕中央弹出暂存内容预览窗口
    void hidePreview();
    void refreshPreview(); // 面板打开时：内容变化后原地刷新（空则关闭）

    // ---- 颜色 ----
    static const QColor COLOR_GREEN;
    static const QColor COLOR_RED;
    static const QColor COLOR_NEUTRAL;

    QString m_stateFile;
    QString m_stashDir;
    QString m_logFile;

    QList<ShelfItem> m_items;
    QSet<QString> m_pathSet;
    QSet<QString> m_imageHashes;

    // ---- 单项删除 pending（仅一项，Q2A）----
    struct PendingDelete {
        bool active = false;
        ShelfItem item;
        int index = -1;
    };
    PendingDelete m_pending;
    bool isInStash(const QString &path) const;
    void removeStashFileIfOwned(const QString &path);

    double m_opacity = 0.75;
    double m_savedY = -1;    // -1 表示状态文件里没有位置
    int m_y = 0;
    float m_slide = 0.0f;    // 0 = 收起，1 = 滑出
    bool m_shown = false;
    bool m_fsHidden = false; // 全屏禁用中
    QString m_hover;         // "main" / "top" / "bottom" / 空
    QString m_press;
    QPointF m_pressPos;
    QPoint m_pressGlobal;
    int m_startY = 0;
    bool m_dragging = false; // 正在上下拖动把手
    float m_pulse = 0.0f;    // 存储/清空时的颜色闪烁
    QColor m_flashColor = COLOR_NEUTRAL;
    float m_hoverP = 0.0f;   // 悬停高亮过渡进度（0→1 变亮）
    QString m_hoverFade;     // 正在淡出高亮的部分（"main"/"top"/"bottom"）
    QPropertyAnimation *m_anim = nullptr;
    QPropertyAnimation *m_pulseAnim = nullptr;
    QPropertyAnimation *m_hoverAnim = nullptr;
    ItemPreviewPanel *m_previewPanel = nullptr;  // 中键预览面板（懒创建）
    QRect m_screenGeo;
    int m_screenRight = 0;
    int m_xShown = 0;
    int m_xHidden = 0;

    bool m_autoMonitor = false; // 每次启动默认关闭
    qint64 m_lastClipSeq = -1;  // 与 Python 版 None 对应：未同步

    QTimer *m_timer = nullptr;      // 40ms 边缘/全屏轮询
    QTimer *m_clipTimer = nullptr;  // 400ms 剪贴板轮询
};
