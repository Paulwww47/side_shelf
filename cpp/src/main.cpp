// -*- coding: utf-8 -*-
// Side Shelf（侧边暂存栏）—— C++20 + Qt6 移植版
// 入口：单实例互斥、自检模式、全局异常日志。

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <QApplication>
#include <QColor>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontInfo>
#include <QGuiApplication>
#include <QImage>
#include <QLinearGradient>
#include <QMimeData>
#include <QPainter>
#include <QRegion>
#include <QScreen>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#include <cmath>
#include <cstdio>
#include <exception>
#include <string>

#include "autostart.h"
#include "glcanvas.h"
#include "itempreviewpanel.h"
#include "shelfwidget.h"
#include "uifont.h"

static HANDLE g_mutexHandle = nullptr;
static bool g_devLog = false;  // 仅开发模式（带参数启动）时写诊断日志

// 诊断日志：纯 Win32 追加写入 %TEMP%\ss_test_log.txt（不依赖 Qt，不被 QFile 影响）
static void rawLog(const char *msg)
{
    if (!g_devLog)
        return;
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    wcscat_s(tmp, L"ss_test_log.txt");
    const HANDLE h = CreateFileW(
        tmp, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, msg, (DWORD)lstrlenA(msg), &written, nullptr);
        CloseHandle(h);
    }
}

// 未处理异常兜底：把异常码与地址写入 side_shelf_error.log。
// 只使用 Win32 API（不依赖 Qt），避免在堆已损坏时二次崩溃。
static LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS *ep)
{
    char buf[512] = {};
    const DWORD code = ep ? ep->ExceptionRecord->ExceptionCode : 0;
    const ULONG_PTR addr = ep
        ? reinterpret_cast<ULONG_PTR>(ep->ExceptionRecord->ExceptionAddress)
        : 0;
    wsprintfA(buf, "crash: exception 0x%08lX at address 0x%p\r\n", code,
              reinterpret_cast<void *>(addr));
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) > 0) {
        wchar_t *slash = wcsrchr(path, L'\\');
        if (slash)
            wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path),
                     L"side_shelf_error.log");
    }
    const HANDLE h = CreateFileW(
        path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, buf, (DWORD)lstrlenA(buf), &written, nullptr);
        CloseHandle(h);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// 用命名互斥量保证单实例；进程退出后互斥量自动释放，无残留锁问题
static bool acquireSingleInstance(const wchar_t *name)
{
    g_mutexHandle = CreateMutexW(nullptr, FALSE, name);
    return g_mutexHandle && GetLastError() != ERROR_ALREADY_EXISTS;
}

static QString appDir()
{
    return QCoreApplication::applicationDirPath();
}

static void writeErrorLog(const QString &dir, const QString &message)
{
    const QString path = dir + QStringLiteral("/side_shelf_error.log");
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(message.toUtf8());
        f.close();
    }
    // 控制台输出（若附加到了父控制台）
    std::fprintf(stderr, "启动失败，详情见 side_shelf_error.log\n");
}

static int run(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("SideShelf"));
    // 全界面字体统一由 UiFont 收口：微软雅黑字族链 + Light(300) 字重
    // （「微软雅黑 Light」并非独立字族，细节见 uifont.h）。
    // 右键菜单 / 气泡提示 / 消息框等控件都跟随应用级默认字体，
    // 因此在事件循环开始前统一设定。
    UiFont::applyToApplication();
    if (g_devLog) {
        // 开发模式留证：记录实际解析到的字族与字面（字族名写错会静默落到别处，
        // 例如把「微软雅黑 Light」当字族名会退化成 Tahoma）
        const QFontInfo uiInfo(QApplication::font());
        rawLog(QStringLiteral("ui-font: %1 / %2\r\n")
                   .arg(uiInfo.family(), uiInfo.styleName())
                   .toUtf8()
                   .constData());
    }

    const QStringList args = app.arguments();
    const bool selftest = args.contains(QStringLiteral("--selftest"));
    // 开发用：--render-preview <out.png> 离屏渲染界面预览图后退出（不进入事件循环）
    const int previewIdx = args.indexOf(QStringLiteral("--render-preview"));
    const bool preview = previewIdx >= 0 && previewIdx + 1 < args.size();
    const QString previewOut = preview ? args.at(previewIdx + 1) : QString();
    // 开发用：覆盖把手示例数量，便于检查空态、两位数和 99+ 的字形。
    const int previewCountIdx = args.indexOf(QStringLiteral("--preview-count"));
    const int previewCount = previewCountIdx >= 0
        ? qBound(0, args.value(previewCountIdx + 1).toInt(), 1000) : 3;
    // --test-panel-keep：与 --test-panel 相同的示例内容与自动打开，但不注入
    // 脚本化点击、存活 20s，供外部脚本按自己的节奏驱动（如删除/撤销/超时
    // 提交等交互验证）
    const bool panelTestKeep =
        args.contains(QStringLiteral("--test-panel-keep"));
    // 开发用：--test-panel 塞入示例内容后自动打开预览窗口（复现/验证用）
    const bool panelTest = panelTestKeep
        || args.contains(QStringLiteral("--test-panel"))
        || args.contains(QStringLiteral("--test-panel-hover"))
        || args.contains(QStringLiteral("--test-panel-noopen"))
        || args.contains(QStringLiteral("--test-panel-filesonly"))
        || args.contains(QStringLiteral("--test-panel-edge"))
        || args.contains(QStringLiteral("--test-panel-heavy"));
    // --test-panel-heavy 塞入 4 张 3000×2000 JPEG（真实照片量级），
    // 并自动点击：1.5s 进放大、3s 退回——配合 SIDESHELF_PROF=1 测量过渡逐帧耗时
    const bool panelTestHeavy =
        args.contains(QStringLiteral("--test-panel-heavy"));
    // --test-panel-hover 额外把鼠标移到屏幕中央，触发面板悬停/Tooltip
    const bool panelTestHover =
        args.contains(QStringLiteral("--test-panel-hover"));
    // --test-panel-noopen 不打开面板（二分：排除面板显示路径）
    const bool panelTestNoOpen =
        args.contains(QStringLiteral("--test-panel-noopen"));
    // --test-panel-filesonly 只加文件不加图片（二分：排除 QImageReader/PNG 路径）
    const bool panelTestFilesOnly =
        args.contains(QStringLiteral("--test-panel-filesonly"));
    // --test-panel-edge 在打开面板前把鼠标移到屏幕右边缘（触发把手滑出动画，
    // 验证"父窗口移动 + 子窗口显示"并发路径）
    const bool panelTestEdge =
        args.contains(QStringLiteral("--test-panel-edge"));
    const bool devMode = selftest || preview || panelTest;

    QString stateFile;
    QString stashDir;
    QString logFile;
    if (devMode) {
        const QString tmp = QDir::tempPath();
        stateFile = tmp + QStringLiteral("/side_shelf_selftest_state.json");
        stashDir = tmp + QStringLiteral("/side_shelf_selftest_stash");
        logFile = tmp + QStringLiteral("/side_shelf_selftest_clip.log");
        FILE *outRedirect = nullptr;
        FILE *errRedirect = nullptr;
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
            freopen_s(&outRedirect, "cpp_stdout.txt", "w", stdout);
            freopen_s(&errRedirect, "cpp_stderr.txt", "w", stderr);
        } else {
            freopen_s(&outRedirect, "CONOUT$", "w", stdout);
            freopen_s(&errRedirect, "CONOUT$", "w", stderr);
        }
    } else {
        stateFile = appDir() + QStringLiteral("/side_shelf_state.json");
        stashDir = appDir() + QStringLiteral("/stash");
        logFile = appDir() + QStringLiteral("/clip_monitor.log");
    }

    const QString mutexName =
        QStringLiteral("Local\\SideShelf-SingleInstance")
        + (devMode ? QStringLiteral("-Dev") : QString());
    const std::wstring mutexNameW = mutexName.toStdWString();
    if (!acquireSingleInstance(mutexNameW.c_str())) {
        rawLog("LOCK_FAIL\r\n");
        if (selftest) {
            std::fprintf(stderr, "lock failed\n");
            std::fflush(stderr);
        }
        return 0;  // 已在运行：静默退出，避免"已经在运行了"弹窗滞留
    }
    rawLog("LOCK_OK\r\n");

    // 开机自启自愈（仅正式模式）：已启用但注册的 exe 路径已不存在
    //（程序目录被移动）时，把注册值更新为当前路径；注册路径仍有效则
    // 不覆盖（尊重用户手动运行的另一份拷贝）
    if (!devMode && AutoStart::isEnabled()) {
        const QString reg = AutoStart::registeredPath();
        const QString cur = QCoreApplication::applicationFilePath();
        if (!reg.isEmpty() && !QFileInfo::exists(reg) && QFileInfo::exists(cur))
            AutoStart::setEnabled(true);
    }

    ShelfWidget w(stateFile, stashDir, logFile);
    if (!preview)
        w.show();

    if (preview) {
        rawLog(GLCanvas::instance().available() ? "renderer: GL\r\n"
                                                : "renderer: raster\r\n");
        // 放入 3 个示例项，让预览图能看到数量徽标
        const QStringList tmpFiles = {
            stashDir + QStringLiteral("/preview_a.txt"),
            stashDir + QStringLiteral("/preview_b.txt"),
            stashDir + QStringLiteral("/preview_c.txt"),
        };
        for (const QString &f : tmpFiles) {
            QFile qf(f);
            if (qf.open(QIODevice::WriteOnly))
                qf.write("preview");
            qf.close();
        }
        QStringList countFiles = tmpFiles;
        for (qsizetype i = countFiles.size(); i < previewCount; ++i) {
            const QString path = stashDir + QStringLiteral("/preview_count_%1.txt").arg(i);
            QFile file(path);
            if (file.open(QIODevice::WriteOnly))
                file.write("preview");
            countFiles.append(path);
        }
        w.addFiles(countFiles.mid(0, previewCount));

        // 预览需要呈现完整展开态：把滑出进度设为 1（徽标/功能小圆随
        // slide 淡入，否则静止态只画把手）
        w.setSlide(1.0f);

        // 先渲染到透明图层，再合成到浅色桌面渐变上；
        // 主体透明度已内嵌到填充色 alpha（paintContents），无需再整层叠加
        QImage overlay(ShelfWidget::W, ShelfWidget::H,
                       QImage::Format_ARGB32_Premultiplied);
        overlay.fill(Qt::transparent);
        QPainter op(&overlay);
        w.render(&op);
        op.end();

        QImage img(ShelfWidget::W, ShelfWidget::H, QImage::Format_ARGB32);
        QPainter cp(&img);
        QLinearGradient bgG(0, 0, 0, ShelfWidget::H);
        bgG.setColorAt(0.0, QColor(242, 246, 252));
        bgG.setColorAt(1.0, QColor(216, 225, 239));
        cp.fillRect(img.rect(), bgG);
        cp.drawImage(0, 0, overlay);
        cp.end();

        QImage big = img.scaled(ShelfWidget::W * 2, ShelfWidget::H * 2,
                                Qt::KeepAspectRatio, Qt::SmoothTransformation);
        big.save(previewOut, "PNG");

        // 预览面板：同样离屏渲染，输出 <out>.panel.png
        {
            // 三张不同宽高比的示例图（纯色渐变、无文字），验证瀑布流布局
            struct Sample {
                QString name;
                int w;
                int h;
                QColor a;
                QColor b;
            };
            const Sample samples[] = {
                {QStringLiteral("/preview_thumb.png"), 64, 44, QColor(90, 140, 240),
                 QColor(60, 200, 160)},
                {QStringLiteral("/preview_portrait.png"), 40, 72, QColor(240, 150, 90),
                 QColor(220, 80, 140)},
                {QStringLiteral("/preview_wide.png"), 140, 40, QColor(120, 200, 90),
                 QColor(40, 140, 200)},
                {QStringLiteral("/preview_big.png"), 900, 600, QColor(160, 120, 220),
                 QColor(60, 60, 160)},
            };
            for (const Sample &s : samples) {
                QImage t(s.w, s.h, QImage::Format_ARGB32);
                QLinearGradient tg(0, 0, s.w, s.h);
                tg.setColorAt(0.0, s.a);
                tg.setColorAt(1.0, s.b);
                QPainter tp(&t);
                tp.fillRect(t.rect(), tg);
                tp.end();
                t.save(stashDir + s.name, "PNG");
            }

            QList<ShelfItem> panelItems;
            for (int i = 0; i < 3; ++i) {
                ShelfItem it;
                it.kind = QStringLiteral("file");
                it.path = tmpFiles.at(i);
                panelItems.append(it);
            }
            for (const Sample &s : samples) {
                ShelfItem it;
                it.kind = QStringLiteral("image");
                it.path = stashDir + s.name;
                panelItems.append(it);
            }
            // 高面板用例：复制条目把面板顶到屏幕高度上限（72%），
            // 验证 GL 离屏渲染在远大于此前尺寸时的完整性（截断类问题）
            for (int dup = 0; dup < 24; ++dup) {
                for (const Sample &s : samples) {
                    ShelfItem it;
                    it.kind = QStringLiteral("image");
                    it.path = stashDir + s.name;
                    panelItems.append(it);
                }
            }

            ItemPreviewPanel panel;
            panel.setItems(panelItems);
            const QSize ps = panel.panelSize();
            panel.resize(ps);
            QImage pov(ps, QImage::Format_ARGB32_Premultiplied);
            pov.fill(Qt::transparent);
            QPainter pop(&pov);
            panel.render(&pop);
            pop.end();

            QImage pimg(ps, QImage::Format_ARGB32);
            QPainter pcp(&pimg);
            QLinearGradient pbg(0, 0, 0, ps.height());
            pbg.setColorAt(0.0, QColor(242, 246, 252));
            pbg.setColorAt(1.0, QColor(216, 225, 239));
            pcp.fillRect(pimg.rect(), pbg);
            pcp.drawImage(0, 0, pov);
            pcp.end();
            const QString panelOut = QFileInfo(previewOut).dir().filePath(
                QFileInfo(previewOut).completeBaseName()
                + QStringLiteral(".panel.png"));
            pimg.save(panelOut, "PNG");

            // 撤销条（右键删除后的 5 秒倒计时）：同一布局再渲染一张，
            // 输出 <out>.panel.undo.png，用于核对倒计时 / 撤销 / 文件名所用字体
            {
                panel.showUndoBarForTest(
                    QFileInfo(tmpFiles.at(0)).fileName(), 5000);
                QImage uov(ps, QImage::Format_ARGB32_Premultiplied);
                uov.fill(Qt::transparent);
                QPainter uop(&uov);
                panel.render(&uop);
                uop.end();

                QImage uimg(ps, QImage::Format_ARGB32);
                QPainter ucp(&uimg);
                ucp.fillRect(uimg.rect(), pbg);
                ucp.drawImage(0, 0, uov);
                ucp.end();
                const QString undoOut = QFileInfo(previewOut).dir().filePath(
                    QFileInfo(previewOut).completeBaseName()
                    + QStringLiteral(".panel.undo.png"));
                uimg.save(undoOut, "PNG");
                panel.showUndoBarForTest(QString(), 0);  // 清除，避免影响后续放大图
            }

            // 放大状态（大图）：放大最后一张 900×600 示例图，输出 <out>.panel.zoom.png
            // 放大状态（文件）：放大第一个文件条目，输出 <out>.panel.zoomfile.png
            const auto renderZoom = [&](int idx, const QString &suffix) {
                panel.enterZoomForTest(idx);
                const QSize zs = panel.size();
                QImage zov(zs, QImage::Format_ARGB32_Premultiplied);
                zov.fill(Qt::transparent);
                QPainter zop(&zov);
                panel.render(&zop);
                zop.end();
                QImage zimg(zs, QImage::Format_ARGB32);
                QPainter zcp(&zimg);
                QLinearGradient zbg(0, 0, 0, zs.height());
                zbg.setColorAt(0.0, QColor(242, 246, 252));
                zbg.setColorAt(1.0, QColor(216, 225, 239));
                zcp.fillRect(zimg.rect(), zbg);
                zcp.drawImage(0, 0, zov);
                zcp.end();
                const QString zoomOut = QFileInfo(previewOut).dir().filePath(
                    QFileInfo(previewOut).completeBaseName() + suffix);
                zimg.save(zoomOut, "PNG");
            };
            renderZoom(panelItems.size() - 1, QStringLiteral(".panel.zoom.png"));
            renderZoom(0, QStringLiteral(".panel.zoomfile.png"));

            for (const Sample &s : samples)
                QFile::remove(stashDir + s.name);
        }

        w.clearItems(false);
        for (const QString &f : tmpFiles)
            QFile::remove(f);
        return 0;
    }

    if (panelTest) {
        rawLog("panelTest-block-start\r\n");
        // 时间戳日志（写到 exe 目录，便于核对各阶段实际执行时刻）
        const auto stageLog = [](const QString &msg) {
            rawLog(QStringLiteral("[%1] %2\r\n")
                       .arg(QDateTime::currentMSecsSinceEpoch())
                       .arg(msg)
                       .toUtf8()
                       .constData());
        };
        stageLog(QStringLiteral("block-start"));
        // 复现/验证用：塞入示例内容，600ms 后自动打开预览窗口，
        // 1.2s 后把鼠标移到屏幕中央（触发面板悬停与 Tooltip），5s 后自动退出
        QStringList tmpFiles;
        if (panelTestHeavy) {
            // 重内容变体：4 张 3000×2000 JPEG。第 1 张固定落在瀑布流左上角
            // （MARGIN,CARD0），供下方脚本点击触发 enter/leave 过渡动画
            for (int i = 0; i < 4; ++i) {
                const QString f = stashDir
                    + QStringLiteral("/preview_heavy_%1.jpg").arg(i);
                QImage t(3000, 2000, QImage::Format_ARGB32);
                QLinearGradient tg(0, 0, 3000, 2000);
                tg.setColorAt(0.0, QColor(30 * i, 120 + 30 * i, 240 - 30 * i));
                tg.setColorAt(1.0, QColor(240 - 20 * i, 60 + 40 * i, 160));
                QPainter tp(&t);
                tp.fillRect(t.rect(), tg);
                tp.end();
                const bool ok = t.save(f, "JPEG", 85);
                if (!ok)
                    stageLog(QStringLiteral("heavy-save-FAIL %1").arg(f));
                tmpFiles.append(f);
            }
            const int added = w.addFiles(tmpFiles);
            stageLog(QStringLiteral("heavy-added=%1 stashDir=%2")
                         .arg(added).arg(stashDir));
        } else {
            const QStringList txtFiles = {
                stashDir + QStringLiteral("/preview_a.txt"),
                stashDir + QStringLiteral("/preview_b.txt"),
                stashDir + QStringLiteral("/preview_c.txt"),
            };
            for (const QString &f : txtFiles) {
                QFile qf(f);
                if (qf.open(QIODevice::WriteOnly))
                    qf.write("preview");
                qf.close();
            }
            tmpFiles = txtFiles;
            w.addFiles(tmpFiles);
            if (!panelTestFilesOnly) {
                const int widths[] = {64, 40, 140};
                const int heights[] = {44, 72, 40};
                for (int i = 0; i < 3; ++i) {
                    QImage t(widths[i], heights[i], QImage::Format_ARGB32);
                    QLinearGradient tg(0, 0, widths[i], heights[i]);
                    tg.setColorAt(0.0,
                                  QColor(90 + i * 40, 140, 240));
                    tg.setColorAt(1.0,
                                  QColor(60, 200 - i * 40, 160));
                    QPainter tp(&t);
                    tp.fillRect(t.rect(), tg);
                    tp.end();
                    w.addImage(t);
                }
            }
        }
        if (panelTestEdge) {
            // 打开面板前 300ms 把鼠标放到屏幕最右边缘：触发把手滑出动画，
            // 让面板显示与父窗口移动并发发生
            QTimer::singleShot(300, &w, []() {
                const QRect g =
                    QGuiApplication::primaryScreen()->geometry();
                QCursor::setPos(g.right() - 2, g.center().y());
            });
        }
        if (!panelTestNoOpen)
            QTimer::singleShot(600, &w, [&w, stageLog]() {
                w.openPreviewForTest();
                stageLog(w.previewVisibleForTest() ? QStringLiteral("opened")
                                                   : QStringLiteral("open-failed"));
            });
        if (panelTestHover) {
            QTimer::singleShot(1200, &w, []() {
                const QRect g =
                    QGuiApplication::primaryScreen()->availableGeometry();
                QCursor::setPos(g.center());
            });
        }
        if (args.contains(QStringLiteral("--test-panel")) && !panelTestKeep) {
            // 点击语义自检：面板内点击（应保持打开）→ 面板外点击（应关闭）
            QTimer::singleShot(1500, &w, [&w]() {
                const QRect pr = w.previewGeometryForTest();
                if (pr.isNull())
                    return;
                // 面板内：顶部边缘中点（MARGIN 留白区，不会命中条目）
                QCursor::setPos(pr.x() + pr.width() / 2, pr.y() + 4);
                INPUT in[2] = {};
                in[0].type = INPUT_MOUSE;
                in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                in[1].type = INPUT_MOUSE;
                in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                SendInput(2, in, sizeof(INPUT));
            });
            QTimer::singleShot(2000, &w, [&w, stageLog]() {
                stageLog(w.previewVisibleForTest()
                             ? QStringLiteral("INSIDE_OK")
                             : QStringLiteral("INSIDE_FAIL"));
            });
            QTimer::singleShot(2500, &w, []() {
                QCursor::setPos(20, 20);  // 屏幕左上角，面板外
                INPUT in[2] = {};
                in[0].type = INPUT_MOUSE;
                in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                in[1].type = INPUT_MOUSE;
                in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                SendInput(2, in, sizeof(INPUT));
            });
            QTimer::singleShot(3000, &w, [&w, stageLog]() {
                stageLog(w.previewVisibleForTest()
                             ? QStringLiteral("OUTSIDE_FAIL")
                             : QStringLiteral("OUTSIDE_OK"));
            });
        }
        if (panelTestHeavy) {
            // 动画测量脚本（代码直触发，绕开输入注入；无头会话中 SendInput
            // 不一定落在面板窗口上）：1.6s 进入放大（enter），3.1s 退回瀑布流
            // （leave）。过渡逐帧耗时由面板在 SIDESHELF_PROF=1 时写到
            // %TEMP%\side_shelf_anim_prof.log
            QTimer::singleShot(1600, &w, [&w, stageLog]() {
                w.animatePreviewZoomForTest(0);
                stageLog(QStringLiteral("enter-triggered"));
            });
            QTimer::singleShot(3100, &w, [&w, stageLog]() {
                w.animatePreviewUnzoomForTest();
                stageLog(QStringLiteral("leave-triggered"));
            });
            // 状态取证：2500ms 时若已进入放大态，窗口应为 2244×1372 量级
            // （瀑布流为 880×160 量级）；据此判断过渡是否真正生效
            QTimer::singleShot(2500, &w, [&w, stageLog]() {
                const QRect pr = w.previewGeometryForTest();
                stageLog(QStringLiteral("mid-geo=%1x%2@%3,%4")
                             .arg(pr.width()).arg(pr.height())
                             .arg(pr.x()).arg(pr.y()));
            });
        }
        QTimer::singleShot(panelTestKeep ? 20000 : 5000, &app,
                           [&app, stageLog]() {
            stageLog(QStringLiteral("quit"));
            app.quit();
        });
        return app.exec();
    }

    if (!selftest)
        return app.exec();

    // ---------- 自检（逐项对应 Python 版 --selftest）----------
    bool ok = true;
    // 每项检查失败时写诊断日志，便于定位
    const auto chk = [&ok](bool cond, const char *name) {
        if (!cond) {
            char buf[160] = {};
            wsprintfA(buf, "SELFTEST-FAIL: %s\r\n", name);
            rawLog(buf);
            ok = false;
        }
    };
    // 不使用二值窗口遮罩：平滑边缘依靠每像素透明（WA_TranslucentBackground），
    // 全透明像素由系统自动点击穿透（Windows 分层窗口行为）
    chk(w.testAttribute(Qt::WA_TranslucentBackground), "translucent");
    chk(w.mask().isEmpty(), "mask-empty");
    const QRect g = QGuiApplication::primaryScreen()->geometry();
    chk(g.top() <= w.savedY() && w.savedY() <= g.bottom() - ShelfWidget::H + 1,
        "y-clamp");

    const QString t1 = stashDir + QStringLiteral("/selftest_a.txt");
    const QString t2 = stashDir + QStringLiteral("/selftest_b.txt");
    {
        QFile f(t1);
        f.open(QIODevice::WriteOnly);
        f.write("a");
        f.close();
        QFile f2(t2);
        f2.open(QIODevice::WriteOnly);
        f2.write("b");
        f2.close();
    }
    chk(w.addFiles({t1, t2}) == 2, "add-files");
    chk(w.addFiles({t1}) == 0, "add-dedup");  // 去重
    chk(w.itemCount() == 2, "count-2");
    w.clearItems();
    chk(w.itemCount() == 0, "clear-count");
    chk(!QFileInfo::exists(t1) && !QFileInfo::exists(t2), "clear-files");

    w.setOpacity(5.0);
    chk(std::abs(w.opacity() - 1.0) < 1e-6, "opacity-max");
    w.setOpacity(0.0);
    chk(std::abs(w.opacity() - 0.15) < 1e-6, "opacity-min");

    // 滑入滑出几何位置
    chk(w.xShown() == w.screenRight() - ShelfWidget::W + 1, "x-shown");
    chk(w.xHidden() == w.screenRight() - ShelfWidget::HIDE + 1, "x-hidden");

    // 自动监测：开启后同步当前序号，无新变化则不新增
    w.setAutoMonitor(true);
    chk(w.autoMonitor(), "automon-on");
    w.pollClipboardForTest();
    chk(w.itemCount() == 0, "automon-no-add");
    w.setAutoMonitor(false);
    chk(!w.autoMonitor(), "automon-off");

    // 文件与图片同时存在时应都暂存（修复浏览器复制图片漏检）
    const QString t4 = stashDir + QStringLiteral("/selftest_d.txt");
    {
        QFile f(t4);
        f.open(QIODevice::WriteOnly);
        f.write("d");
        f.close();
    }
    QMimeData md;
    md.setUrls({QUrl::fromLocalFile(t4)});
    QImage img(8, 8, QImage::Format_ARGB32);
    img.fill(QColor(10, 20, 30));
    md.setImageData(img);
    chk(w.processClipboardContent(&md) == 2, "clip-file-plus-image");
    chk(w.itemCount() == 2, "clip-count");
    w.clearItems();
    chk(w.itemCount() == 0, "clip-clear");

    // 拖入临时目录文件（压缩软件拖出解压场景）：应复制进 stash 再暂存；
    // 重复拖入同名同大小文件不重复添加；普通位置文件只记录路径不复制；
    // 清空时 stash 副本随暂存删除，源文件与普通文件都不受影响。
    {
        const QString archDir =
            QDir::tempPath() + QStringLiteral("/side_shelf_selftest_arch");
        QDir().mkpath(archDir);
        const QString archFile =
            archDir + QStringLiteral("/selftest_arch_doc.txt");
        {
            QFile f(archFile);
            f.open(QIODevice::WriteOnly);
            f.write("archived");
            f.close();
        }
        QMimeData mdDrop;
        mdDrop.setUrls({QUrl::fromLocalFile(archFile)});
        int copied = -1;
        chk(w.processDropContent(&mdDrop, &copied) == 1 && copied == 1,
            "drop-temp-copy");
        const QStringList p1 = w.itemPaths();
        chk(p1.size() == 1
                && p1.first().endsWith(
                       QStringLiteral("selftest_arch_doc.txt"),
                       Qt::CaseInsensitive),
            "drop-temp-item");
        const QString stashCopy =
            stashDir + QStringLiteral("/selftest_arch_doc.txt");
        {
            QFile f(stashCopy);
            chk(f.open(QIODevice::ReadOnly), "drop-temp-open");
            chk(f.readAll() == QByteArray("archived"), "drop-temp-content");
            f.close();
        }
        chk(QFileInfo::exists(archFile), "drop-temp-src-kept");
        chk(w.processDropContent(&mdDrop) == 0, "drop-temp-dedup");
        chk(w.itemCount() == 1, "drop-temp-dedup-count");

        // 普通位置文件：只记录路径，不复制进 stash
        QString plainDir = appDir();
        if (w.isTransientTempPath(plainDir))  // exe 目录恰在 temp 下时换个地方
            plainDir = QDir::homePath();
        const QString plainFile =
            plainDir + QStringLiteral("/side_shelf_selftest_plain.txt");
        {
            QFile f(plainFile);
            f.open(QIODevice::WriteOnly);
            f.write("plain");
            f.close();
        }
        QMimeData mdPlain;
        mdPlain.setUrls({QUrl::fromLocalFile(plainFile)});
        chk(w.processDropContent(&mdPlain) == 1, "drop-plain-add");
        chk(w.itemCount() == 2, "drop-plain-count");
        chk(!QFileInfo::exists(stashDir
                               + QStringLiteral("/side_shelf_selftest_plain.txt")),
            "drop-plain-nocopy");

        w.clearItems();
        chk(w.itemCount() == 0, "drop-clear-count");
        chk(!QFileInfo::exists(stashCopy), "drop-clear-copy-gone");  // 副本随清空删除
        chk(QFileInfo::exists(plainFile), "drop-clear-plain-kept");  // 普通文件不动
        chk(QFileInfo::exists(archFile), "drop-clear-src-kept");     // 源临时文件不动
        QFile::remove(plainFile);
        QFile::remove(archFile);
        QDir(archDir).removeRecursively();
    }

    // 预览内单项删除 + 5 秒撤销：pending 阶段保留文件；撤销恢复原下标；
    // 提交只删除 stash 副本，普通外部文件不动；新操作会提交旧 pending。
    {
        const QString delStash =
            stashDir + QStringLiteral("/selftest_delete_stash.txt");
        const QString keepExternal =
            appDir() + QStringLiteral("/selftest_delete_external.txt");
        {
            QFile f(delStash);
            f.open(QIODevice::WriteOnly);
            f.write("stash-delete");
            f.close();
            QFile f2(keepExternal);
            f2.open(QIODevice::WriteOnly);
            f2.write("external-delete");
            f2.close();
        }
        const QString delNorm =
            QDir::toNativeSeparators(QFileInfo(delStash).absoluteFilePath())
                .toCaseFolded();
        const QString keepNorm =
            QDir::toNativeSeparators(QFileInfo(keepExternal).absoluteFilePath())
                .toCaseFolded();
        chk(w.addFiles({delStash, keepExternal}) == 2, "delete-add");
        const QStringList original = {delNorm, keepNorm};
        chk(w.itemPaths() == original, "delete-order");

        ShelfItem taken;
        int takenIndex = -1;
        chk(w.takePreviewItem(delNorm, &taken, &takenIndex), "delete-take");
        chk(taken.path == delNorm && takenIndex == 0, "delete-take-index");
        chk(w.hasPendingDelete(), "delete-pending");
        chk(w.itemCount() == 1 && w.itemPaths() == QStringList{keepNorm},
            "delete-remove-state");
        chk(QFileInfo::exists(delStash), "delete-file-deferred");

        ShelfItem undone;
        int undoneIndex = -1;
        chk(w.undoPreviewDelete(&undone, &undoneIndex), "delete-undo");
        chk(undone.path == delNorm && undoneIndex == 0, "delete-undo-index");
        chk(!w.hasPendingDelete(), "delete-undo-pending");
        chk(w.itemPaths() == original, "delete-undo-order");
        chk(w.addFiles({delNorm}) == 0, "delete-undo-pathset");

        chk(w.takePreviewItem(delNorm), "delete-take-again");
        w.commitPreviewDelete();
        chk(!w.hasPendingDelete(), "delete-commit-pending");
        chk(!QFileInfo::exists(delStash), "delete-stash-gone");
        chk(QFileInfo::exists(keepExternal), "delete-external-kept");
        chk(w.itemCount() == 1 && w.itemPaths() == QStringList{keepNorm},
            "delete-commit-state");

        const QString newExternal =
            appDir() + QStringLiteral("/selftest_delete_new.txt");
        {
            QFile f(newExternal);
            f.open(QIODevice::WriteOnly);
            f.write("new");
            f.close();
        }
        const QString newNorm =
            QDir::toNativeSeparators(QFileInfo(newExternal).absoluteFilePath())
                .toCaseFolded();
        chk(w.takePreviewItem(keepNorm), "delete-auto-take");
        chk(w.addFiles({newExternal}) == 1, "delete-auto-add");
        chk(!w.hasPendingDelete(), "delete-auto-committed");
        chk(QFileInfo::exists(keepExternal), "delete-auto-external-kept");
        chk(w.itemPaths() == QStringList{newNorm}, "delete-auto-state");

        const QString secondExternal =
            appDir() + QStringLiteral("/selftest_delete_second.txt");
        {
            QFile f(secondExternal);
            f.open(QIODevice::WriteOnly);
            f.write("second");
            f.close();
        }
        const QString secondNorm = QDir::toNativeSeparators(
            QFileInfo(secondExternal).absoluteFilePath()).toCaseFolded();
        chk(w.addFiles({secondExternal}) == 1, "delete-second-add");
        chk(w.takePreviewItem(newNorm), "delete-second-take");
        chk(w.takePreviewItem(secondNorm), "delete-second-take-commit-old");
        chk(w.itemCount() == 0 && w.hasPendingDelete(),
            "delete-second-pending-state");
        chk(QFileInfo::exists(newExternal) && QFileInfo::exists(secondExternal),
            "delete-second-external-kept");
        w.commitPreviewDelete();

        w.clearItems();
        QFile::remove(keepExternal);
        QFile::remove(newExternal);
        QFile::remove(secondExternal);
    }

    // 剪贴板图片条目必须能右键删除：条目路径与删除查找键必须是同一种规范形式。
    // 历史 bug：图片条路径存 '/'、查找键用 '\'，比较永不相等，右键静默无效
    //（本地文件条目两处都是 '\'，所以只有剪贴板图片删不掉）。
    {
        QImage delImg(6, 6, QImage::Format_ARGB32);
        delImg.fill(QColor(1, 2, 3));
        chk(w.addImage(delImg) == 1, "image-delete-add");
        chk(w.itemCount() == 1, "image-delete-count");
        const QString imgPath = w.itemPaths().value(0);
        chk(QFileInfo::exists(imgPath), "image-delete-exists");
        QString slashForm = imgPath;
        slashForm.replace(QLatin1Char('\\'), QLatin1Char('/'));
        chk(w.takePreviewItem(slashForm), "image-delete-slash-form");
        chk(w.hasPendingDelete(), "image-delete-pending");
        chk(w.itemCount() == 0, "image-delete-removed");
        chk(QFileInfo::exists(imgPath), "image-delete-deferred");
        w.commitPreviewDelete();
        chk(!QFileInfo::exists(imgPath), "image-delete-gone");
        chk(w.addImage(delImg) == 1, "image-delete-readd");
        w.clearItems();
        chk(w.itemCount() == 0, "image-delete-clear");
    }

    // 同一次剪贴板同时给出「临时文件 + 位图」（微信复制图片）时只留一条：保留
    // stash 里的持久副本，丢弃随后会被源程序清理的 %TEMP% 路径。位图是 Qt 重新
    // 编码的 PNG，字节与源文件不同，因此必须按解码后的像素判等。
    {
        const QString pairDir =
            QDir::tempPath() + QStringLiteral("/side_shelf_selftest_pair");
        QDir().mkpath(pairDir);
        const QString pairFile = pairDir + QStringLiteral("/wechat_img.png");
        QImage src(10, 10, QImage::Format_ARGB32);
        src.fill(QColor(200, 100, 50));
        chk(src.save(pairFile, "PNG"), "clip-pair-file");
        const QString stashCanon =
            QDir::toNativeSeparators(QFileInfo(stashDir).absoluteFilePath())
                .toCaseFolded();
        QMimeData mdPair;
        mdPair.setUrls({QUrl::fromLocalFile(pairFile)});
        mdPair.setImageData(src);
        chk(w.processClipboardContent(&mdPair) == 1, "clip-pair-dedup");
        chk(w.itemCount() == 1, "clip-pair-count");
        const QStringList pairPaths = w.itemPaths();
        chk(pairPaths.size() == 1
                && pairPaths.first().startsWith(
                    stashCanon + QLatin1Char('\\')),
            "clip-pair-stashed");
        chk(!QFileInfo::exists(stashDir + QStringLiteral("/wechat_img.png")),
            "clip-pair-nofilecopy");
        w.clearItems();
        chk(QFileInfo::exists(pairFile), "clip-pair-src-kept");

        // 只有临时文件、没有位图（压缩软件里 Ctrl+C 等）：同样复制进 stash 再
        // 登记，清空时副本随暂存删除，源文件不动。
        const QString tmpDoc = pairDir + QStringLiteral("/wechat_doc.txt");
        {
            QFile f(tmpDoc);
            f.open(QIODevice::WriteOnly);
            f.write("tmp-doc");
            f.close();
        }
        QMimeData mdTmpClip;
        mdTmpClip.setUrls({QUrl::fromLocalFile(tmpDoc)});
        chk(w.processClipboardContent(&mdTmpClip) == 1, "clip-temp-copy");
        const QStringList tmpPaths = w.itemPaths();
        chk(tmpPaths.size() == 1
                && tmpPaths.first().startsWith(
                    stashCanon + QLatin1Char('\\')),
            "clip-temp-stashed");
        w.clearItems();
        chk(!QFileInfo::exists(stashDir + QStringLiteral("/wechat_doc.txt")),
            "clip-temp-copy-gone");
        chk(QFileInfo::exists(tmpDoc), "clip-temp-src-kept");
        QFile::remove(pairFile);
        QFile::remove(tmpDoc);
        QDir(pairDir).removeRecursively();
    }

    // 退出即清空：closeEvent 应清掉暂存
    const QString t3 = stashDir + QStringLiteral("/selftest_c.txt");
    {
        QFile f(t3);
        f.open(QIODevice::WriteOnly);
        f.write("c");
        f.close();
    }
    w.addFiles({t3});
    chk(w.itemCount() == 1, "exit-clear-count");
    w.close();
    chk(w.itemCount() == 0 && !QFileInfo::exists(t3), "exit-clear-files");

    // 开机自启注册表读写（只用专用测试子键，绝不触碰真实 Run 键）
    {
        const std::wstring testKey = L"Software\\SideShelfSelfTestRun";
        AutoStart::deleteSubkeyForTest(testKey);  // 清理上次残留
        chk(AutoStart::quotedCommand(QStringLiteral("C:/a b/side_shelf.exe"))
                == QStringLiteral("\"C:\\a b\\side_shelf.exe\""),
            "autostart-quote");
        QString err;
        chk(AutoStart::setEnabled(true, &err, testKey), "autostart-set");
        chk(AutoStart::isEnabled(testKey), "autostart-enabled");
        chk(AutoStart::registeredPath(testKey)
                == QDir::toNativeSeparators(
                    QCoreApplication::applicationFilePath()),
            "autostart-path");
        chk(AutoStart::setEnabled(false, nullptr, testKey), "autostart-off");
        chk(!AutoStart::isEnabled(testKey), "autostart-disabled");
        chk(AutoStart::deleteSubkeyForTest(testKey), "autostart-cleanup");
    }

    QFile::remove(stateFile);

    rawLog(ok ? "SELFTEST OK\r\n" : "SELFTEST FAILED\r\n");
    std::printf("SELFTEST %s\n", ok ? "OK" : "FAILED");
    std::fflush(stdout);
    return ok ? 0 : 2;
}

int main(int argc, char *argv[])
{
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
    g_devLog = (argc > 1);  // 只有开发模式（--selftest/--test-panel/--render-preview）写诊断日志
    rawLog("== main-enter ==\r\n");
    try {
        return run(argc, argv);
    } catch (const std::exception &ex) {
        writeErrorLog(appDir(), QString::fromUtf8(ex.what()));
        return 1;
    } catch (...) {
        writeErrorLog(appDir(), QStringLiteral("unknown exception"));
        return 1;
    }
}
