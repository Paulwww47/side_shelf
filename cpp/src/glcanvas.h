// -*- coding: utf-8 -*-
// GLCanvas：共享的 GPU 渲染器——MSAA 离屏渲染 + 可选超采样。
// QPainter 绘制代码完全不变，只是渲染后端从软件光栅换成 OpenGL 多重采样
// （4× MSAA，边缘由多重采样覆盖，平滑度优于 1× 抗锯齿）。
// GL 不可用（无驱动 / 远程桌面等）时 available() 为 false，
// 调用方应回退到软件光栅超采样路径。

#pragma once

#include <QImage>
#include <QSize>

#include <functional>

class QPainter;
class QOpenGLContext;
class QOffscreenSurface;
class QOpenGLFramebufferObject;

class GLCanvas
{
public:
    static GLCanvas &instance();

    bool available() const { return m_ok; }

    // 以 4× MSAA 渲染到离屏 FBO（按 ss 倍超采样），解析为 QImage。
    // 渲染失败或 GL 不可用时返回空 QImage。
    QImage render(const QSize &logicalSize, int ss,
                  const std::function<void(QPainter &)> &paint);

private:
    GLCanvas();

    // 进程级单例，刻意不释放（QObject 在静态析构期删除有顺序风险，
    // 由操作系统在进程退出时统一回收）。
    QOpenGLContext *m_ctx = nullptr;
    QOffscreenSurface *m_surf = nullptr;
    QOpenGLFramebufferObject *m_fbo = nullptr;  // 按尺寸缓存复用
    QSize m_fboSize;
    bool m_ok = false;
};
