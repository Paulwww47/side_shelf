// -*- coding: utf-8 -*-
#include "glcanvas.h"

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLPaintDevice>
#include <QPainter>
#include <QSurfaceFormat>

GLCanvas &GLCanvas::instance()
{
    static GLCanvas s;
    return s;
}

GLCanvas::GLCanvas()
{
    QSurfaceFormat fmt;
    fmt.setSamples(4);          // 4× 多重采样
    fmt.setStencilBufferSize(8); // QPainter 的路径裁剪需要模板缓冲
    fmt.setAlphaBufferSize(8);
    m_ctx = new QOpenGLContext;
    m_ctx->setFormat(fmt);
    if (!m_ctx->create())
        return;
    m_surf = new QOffscreenSurface;
    m_surf->setFormat(m_ctx->format());
    m_surf->create();
    if (!m_surf->isValid())
        return;
    if (!m_ctx->makeCurrent(m_surf))
        return;
    m_ctx->doneCurrent();
    m_ok = true;
}

QImage GLCanvas::render(const QSize &logicalSize, int ss,
                        const std::function<void(QPainter &)> &paint)
{
    if (!m_ok || logicalSize.isEmpty() || ss < 1 || ss > 4)
        return QImage();
    if (!m_ctx->makeCurrent(m_surf))
        return QImage();

    const QSize pxSize = logicalSize * ss;
#ifdef SIDESHELF_PERF_BASELINE
    // 原实现（基线对照用）：FBO 尺寸精确匹配，窗口逐帧形变时每帧重建
    if (!m_fbo || m_fboSize != pxSize) {
        delete m_fbo;
        m_fbo = nullptr;
        QOpenGLFramebufferObjectFormat f;
        f.setSamples(4);
        f.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        m_fbo = new QOpenGLFramebufferObject(pxSize, f);
        m_fboSize = pxSize;
    }
#else
    // FBO 增长式分配（宽高向上取整到 256 的倍数，缩小超过一半才回收）：
    // 预览窗格过渡动画逐帧改变窗口尺寸，精确匹配会导致每帧重建
    // MSAA FBO（渲染缓冲反复分配）。绘制仍用 pxSize 的视口，内容落在
    // FBO 左下角区域，toImage 后裁回精确尺寸，返回值语义不变。
    const bool needGrow = !m_fbo || pxSize.width() > m_fboSize.width()
        || pxSize.height() > m_fboSize.height();
    const bool needShrink =
        m_fbo && pxSize.width() * 2 < m_fboSize.width()
        && pxSize.height() * 2 < m_fboSize.height();
    if (needGrow || needShrink) {
        const QSize want((std::max(pxSize.width(), 256) + 255) / 256 * 256,
                         (std::max(pxSize.height(), 256) + 255) / 256 * 256);
        const QOpenGLFramebufferObjectFormat fmt = [this] {
            QOpenGLFramebufferObjectFormat f;
            f.setSamples(4);
            f.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
            return f;
        }();
        delete m_fbo;
        m_fbo = new QOpenGLFramebufferObject(want, fmt);
        m_fboSize = m_fbo->isValid() ? want : pxSize;
        if (!m_fbo->isValid()) {
            // 带内边距的分配失败（极少见）：退回精确尺寸重试一次
            delete m_fbo;
            m_fbo = new QOpenGLFramebufferObject(pxSize, fmt);
        }
    }
#endif

    QImage out;
    if (m_fbo->isValid()) {
        // QPainter 不会自己清空 FBO：先清成全透明，
        // 否则形状之外的区域会残留上一帧内容（出现“鬼影”）
        m_fbo->bind();
        QOpenGLFunctions *gl = QOpenGLContext::currentContext()->functions();
        gl->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        gl->glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        // Qt6：QPainter 通过 QOpenGLPaintDevice 绘制到当前绑定的 FBO。
        // 引擎把视口设为设备尺寸 pxSize——在 GL 坐标里即 FBO 的“底部”
        // h 行；toImage() 翻转整个缓冲后，内容位于 QImage 的底部区域。
        QOpenGLPaintDevice pd(pxSize);
        QPainter p(&pd);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.scale(ss, ss);
        paint(p);
        p.end();
        m_fbo->release();
        out = m_fbo->toImage();  // 解析多重采样 FBO 为普通位图
        // 有内边距时从“底部”裁出内容区（GL 原点在左下，翻转后内容贴底）
        if (!out.isNull() && out.size() != pxSize)
            out = out.copy(0, out.height() - pxSize.height(),
                           pxSize.width(), pxSize.height());
    }
    m_ctx->doneCurrent();
    return out;
}
