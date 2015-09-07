/****************************************************************************
**
** Copyright (C) 2014 Pier Luigi Fiorini <pierluigi.fiorini@gmail.com>
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the Qt Compositor.
**
** $QT_BEGIN_LICENSE:BSD$
** You may use this file under the terms of the BSD license as follows:
**
** "Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**   * Redistributions of source code must retain the above copyright
**     notice, this list of conditions and the following disclaimer.
**   * Redistributions in binary form must reproduce the above copyright
**     notice, this list of conditions and the following disclaimer in
**     the documentation and/or other materials provided with the
**     distribution.
**   * Neither the name of The Qt Company Ltd nor the names of its
**     contributors may be used to endorse or promote products derived
**     from this software without specific prior written permission.
**
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
** OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
** LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
** OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qwindowcompositor.h"

#include <QMouseEvent>
#include <QKeyEvent>
#include <QTouchEvent>
#include <QOpenGLFunctions>
#include <QOpenGLTexture>
#include <QGuiApplication>
#include <QCursor>
#include <QPixmap>
#include <QLinkedList>
#include <QScreen>
#include <QPainter>

#include <QtCompositor/qwaylandinput.h>
#include <QtCompositor/qwaylandbufferref.h>
#include <QtCompositor/qwaylandview.h>
#include <QtCompositor/qwaylandoutput.h>
#include <QtCompositor/qwaylandoutputspace.h>

#include <QtCompositor/QWaylandShellSurface>
#include <QtCompositor/private/qwlextendedsurface_p.h>

QT_BEGIN_NAMESPACE

class Surface : public QWaylandSurface
{
public:
    Surface(QWaylandCompositor *compositor, QWaylandClient *client, uint id, int version)
        : QWaylandSurface(compositor, client, id, version)
        , shellSurface(Q_NULLPTR)
        , extSurface(Q_NULLPTR)
        , hasSentOnScreen(false)
    { }

    QWaylandShellSurface *shellSurface;
    QtWayland::ExtendedSurface *extSurface;
    bool hasSentOnScreen;
};

class SurfaceView : public QWaylandView
{
public:
    SurfaceView()
        : QWaylandView(Q_NULLPTR)
        , m_texture(0)
    {}

    ~SurfaceView()
    {
        if (m_texture)
            glDeleteTextures(1, &m_texture);
    }

    GLuint updateTextureToCurrentBuffer()
    {
        if (advance()) {
            if (m_texture)
                glDeleteTextures(1, &m_texture);

            glGenTextures(1, &m_texture);
            glBindTexture(GL_TEXTURE_2D, m_texture);
            currentBuffer().bindToTexture();
        }
        return m_texture;
    }

private:
    GLuint m_texture;
};

QWindowCompositor::QWindowCompositor(CompositorWindow *window)
    : QWaylandCompositor()
    , m_window(window)
    , m_backgroundTexture(0)
    , m_textureBlitter(0)
    , m_renderScheduler(this)
    , m_draggingWindow(0)
    , m_dragKeyIsPressed(false)
    , m_cursorHotspotX(0)
    , m_cursorHotspotY(0)
    , m_modifiers(Qt::NoModifier)
    , m_shell(new QWaylandShell(this))
{
    m_window->makeCurrent();

    m_textureBlitter = new TextureBlitter();
    m_backgroundImage = makeBackgroundImage(QLatin1String(":/background.jpg"));

    QOpenGLFunctions *functions = m_window->context()->functions();
    functions->glGenFramebuffers(1, &m_surface_fbo);

    connect(m_shell, &QWaylandShell::createShellSurface, this, &QWindowCompositor::onCreateShellSurface);

    QtWayland::SurfaceExtensionGlobal *extSurfGlob = new QtWayland::SurfaceExtensionGlobal(this);
    connect(extSurfGlob, &QtWayland::SurfaceExtensionGlobal::extendedSurfaceReady, this, &QWindowCompositor::extendedSurfaceCreated);

}

QWindowCompositor::~QWindowCompositor()
{
    delete m_textureBlitter;
}

void QWindowCompositor::create()
{
    QWaylandCompositor::create();

    new QWaylandOutput(defaultOutputSpace(), m_window);

    m_renderScheduler.setSingleShot(true);
    connect(&m_renderScheduler, &QTimer::timeout, this, &QWindowCompositor::render);
    connect(this, &QWaylandCompositor::createSurface, this, &QWindowCompositor::onCreateSurface);
    connect(this, &QWaylandCompositor::surfaceCreated, this, &QWindowCompositor::onSurfaceCreated);
    connect(defaultInputDevice(), &QWaylandInputDevice::cursorSurfaceRequest, this, &QWindowCompositor::adjustCursorSurface);

    m_window->installEventFilter(this);

    setRetainedSelectionEnabled(true);
}

QImage QWindowCompositor::makeBackgroundImage(const QString &fileName)
{
    Q_ASSERT(m_window);

    int width = m_window->width();
    int height = m_window->height();
    QImage baseImage(fileName);
    QImage patternedBackground(width, height, baseImage.format());
    QPainter painter(&patternedBackground);

    QSize imageSize = baseImage.size();
    for (int y = 0; y < height; y += imageSize.height()) {
        for (int x = 0; x < width; x += imageSize.width()) {
            painter.drawImage(x, y, baseImage);
        }
    }

    return patternedBackground;
}

void QWindowCompositor::ensureKeyboardFocusSurface(QWaylandSurface *oldSurface)
{
    QWaylandSurface *kbdFocus = defaultInputDevice()->keyboardFocus();
    if (kbdFocus == oldSurface || !kbdFocus)
        defaultInputDevice()->setKeyboardFocus(m_visibleSurfaces.isEmpty() ? 0 : m_visibleSurfaces.last());
}

void QWindowCompositor::surfaceDestroyed()
{
    QWaylandSurface *surface = static_cast<QWaylandSurface *>(sender());
    ensureKeyboardFocusSurface(surface);
    m_renderScheduler.start(0);
}

void QWindowCompositor::surfaceMappedChanged()
{
    QWaylandSurface *surface = qobject_cast<QWaylandSurface *>(sender());
    if (surface->isMapped())
        surfaceMapped(surface);
    else
        surfaceUnmapped(surface);
}
void QWindowCompositor::surfaceMapped(QWaylandSurface *surface)
{
    Q_ASSERT(!m_visibleSurfaces.contains(surface));
    QWaylandShellSurface *shellSurface = QWaylandShellSurface::findIn(surface);
    QPoint pos;
    if (!shellSurface || (shellSurface->surfaceType() != QWaylandShellSurface::Popup)) {
        uint px = 0;
        uint py = 0;
        if (!QCoreApplication::arguments().contains(QLatin1String("-stickytopleft"))) {
            px = 1 + (qrand() % (m_window->width() - surface->size().width() - 2));
            py = 1 + (qrand() % (m_window->height() - surface->size().height() - 2));
        }
        pos = QPoint(px, py);
        QWaylandView *view = surface->views().first();
        view->setRequestedPosition(pos);
    }
    if (shellSurface && shellSurface->surfaceType() == QWaylandShellSurface::Popup) {
        QWaylandView *view = shellSurface->view();
        view->setRequestedPosition(shellSurface->transientParent()->views().first()->requestedPosition() + shellSurface->transientOffset());
    }

    m_visibleSurfaces.append(surface);

    if (!surface->isCursorSurface())
        defaultInputDevice()->setKeyboardFocus(surface);

    m_renderScheduler.start(0);
}

void QWindowCompositor::surfaceUnmapped(QWaylandSurface *surface)
{
    m_visibleSurfaces.removeOne(surface);

    ensureKeyboardFocusSurface(surface);
    m_renderScheduler.start(0);
}

void QWindowCompositor::surfaceCommittedSlot()
{
    QWaylandSurface *surface = qobject_cast<QWaylandSurface *>(sender());
    surfaceCommitted(surface);
}

void QWindowCompositor::surfacePosChanged()
{
    m_renderScheduler.start(0);
}

void QWindowCompositor::surfaceCommitted(QWaylandSurface *surface)
{
    Q_UNUSED(surface)
    m_renderScheduler.start(0);
}

void QWindowCompositor::onCreateSurface(QWaylandClient *client, uint id, int version)
{
    new Surface(this, client, id, version);
}

void QWindowCompositor::onSurfaceCreated(QWaylandSurface *surface)
{
    connect(surface, &QWaylandSurface::surfaceDestroyed, this, &QWindowCompositor::surfaceDestroyed);
    connect(surface, &QWaylandSurface::mappedChanged, this, &QWindowCompositor::surfaceMappedChanged);
    connect(surface, &QWaylandSurface::redraw, this, &QWindowCompositor::surfaceCommittedSlot);
}

void QWindowCompositor::onCreateShellSurface(QWaylandSurface *s, QWaylandClient *client, uint id)
{
    Surface *surface = static_cast<Surface *>(s);
    SurfaceView *newView = new SurfaceView();
    newView->setSurface(surface);
    newView->setOutput(output(m_window));
    QWaylandShellSurface *shellSurface = new QWaylandShellSurface(m_shell, surface, newView, client, id);
    surface->shellSurface = shellSurface;
    m_renderScheduler.start(0);
}

void QWindowCompositor::extendedSurfaceCreated(QtWayland::ExtendedSurface *extendedSurface, QWaylandSurface *surface)
{
    Surface *s = static_cast<Surface *>(surface);
    Q_ASSERT(!s->extSurface);
    s->extSurface = extendedSurface;
}

void QWindowCompositor::updateCursor(bool hasBuffer)
{
    Q_UNUSED(hasBuffer)
    if (!m_cursorView.surface())
        return;

    m_cursorView.advance();

    QImage image = m_cursorView.currentBuffer().image();

    QCursor cursor(QPixmap::fromImage(image), m_cursorHotspotX, m_cursorHotspotY);
    static bool cursorIsSet = false;
    if (cursorIsSet) {
        QGuiApplication::changeOverrideCursor(cursor);
    } else {
        QGuiApplication::setOverrideCursor(cursor);
        cursorIsSet = true;
    }
}

QPointF QWindowCompositor::toView(QWaylandView *view, const QPointF &pos) const
{
    return pos - view->requestedPosition();
}

void QWindowCompositor::adjustCursorSurface(QWaylandSurface *surface, int hotspotX, int hotspotY)
{
    if ((m_cursorView.surface() != surface)) {
        if (m_cursorView.surface())
            disconnect(m_cursorView.surface(), &QWaylandSurface::configure, this, &QWindowCompositor::updateCursor);
        if (surface)
            connect(surface, &QWaylandSurface::configure, this, &QWindowCompositor::updateCursor);
    }

    m_cursorView.setSurface(surface);
    m_cursorHotspotX = hotspotX;
    m_cursorHotspotY = hotspotY;
}

QWaylandView *QWindowCompositor::viewAt(const QPointF &point, QPointF *local)
{
    for (int i = m_visibleSurfaces.size() - 1; i >= 0; --i) {
        QWaylandSurface *surface = m_visibleSurfaces.at(i);
        foreach (QWaylandView *view, surface->views()) {
            QRectF geo(view->requestedPosition(), surface->size());
            if (geo.contains(point)) {
                if (local)
                    *local = toView(view, point);
                return view;
            }
        }
    }
    return 0;
}

void QWindowCompositor::render()
{
    m_window->makeCurrent();
    defaultOutput()->frameStarted();

    cleanupGraphicsResources();

    if (!m_backgroundTexture)
        m_backgroundTexture = new QOpenGLTexture(m_backgroundImage, QOpenGLTexture::DontGenerateMipMaps);

    m_textureBlitter->bind();
    // Draw the background image texture
    m_textureBlitter->drawTexture(m_backgroundTexture->textureId(),
                                  QRect(QPoint(0, 0), m_backgroundImage.size()),
                                  m_window->size(),
                                  0, false, true);

    foreach (QWaylandSurface *surface, m_visibleSurfaces) {
        if (!surface->isMapped())
            continue;
        drawSubSurface(QPoint(), surface);
    }

    m_textureBlitter->release();
    defaultOutput()->sendFrameCallbacks();

    // N.B. Never call glFinish() here as the busylooping with vsync 'feature' of the nvidia binary driver is not desirable.
    m_window->swapBuffers();
}

void QWindowCompositor::drawSubSurface(const QPoint &offset, QWaylandSurface *s)
{
    Surface *surface = static_cast<Surface *>(s);
    SurfaceView *view = Q_NULLPTR;
    if (surface->shellSurface && surface->shellSurface->view())
        view = static_cast<SurfaceView *>(surface->shellSurface->view());
    else if (surface->views().size())
        view = static_cast<SurfaceView *>(surface->views().first());

    if (!view)
        return;

    GLuint texture = view->updateTextureToCurrentBuffer();
    bool invert_y = view->currentBuffer().origin() == QWaylandSurface::OriginTopLeft;
    QPoint pos = view->requestedPosition().toPoint() + offset;
    QRect geo(pos, surface->size());
    bool onscreen = (QRect(QPoint(0, 0), m_window->size()).contains(pos));
    if (surface->extSurface && onscreen != surface->hasSentOnScreen) {
        surface->extSurface->sendOnScreenVisibilityChange(onscreen);
        surface->hasSentOnScreen = onscreen;
    }

    if (surface->isMapped() && onscreen) {
        m_textureBlitter->drawTexture(texture, geo, m_window->size(), 0, false, invert_y);
    }
}

bool QWindowCompositor::eventFilter(QObject *obj, QEvent *event)
{
    if (obj != m_window)
        return false;

    QWaylandInputDevice *input = defaultInputDevice();

    switch (event->type()) {
    case QEvent::FocusOut:
        m_dragKeyIsPressed = false;
    case QEvent::Expose:
        m_renderScheduler.start(0);
        if (m_window->isExposed()) {
            // Alt-tabbing away normally results in the alt remaining in
            // pressed state in the clients xkb state. Prevent this by sending
            // a release. This is not an issue in a "real" compositor but
            // is very annoying when running in a regular window on xcb.
            Qt::KeyboardModifiers mods = QGuiApplication::queryKeyboardModifiers();
            if (m_modifiers != mods && input->keyboardFocus()) {
                Qt::KeyboardModifiers stuckMods = m_modifiers ^ mods;
                if (stuckMods & Qt::AltModifier)
                    input->sendKeyReleaseEvent(64); // native scancode for left alt
                m_modifiers = mods;
            }
        }
        break;
    case QEvent::MouseButtonPress: {
        QPointF local;
        QMouseEvent *me = static_cast<QMouseEvent *>(event);
        QWaylandView *target = viewAt(me->localPos(), &local);
        if (m_dragKeyIsPressed && target) {
            m_draggingWindow = target;
            m_drag_diff = local;
        } else {
            if (target && input->keyboardFocus() != target->surface()) {
                input->setKeyboardFocus(target->surface());
                m_visibleSurfaces.removeOne(target->surface());
                m_visibleSurfaces.append(target->surface());
                m_renderScheduler.start(0);
            }
            input->sendMousePressEvent(me->button());
        }
        return true;
    }
    case QEvent::MouseButtonRelease: {
        QWaylandView *target = input->mouseFocus();
        if (m_draggingWindow) {
            m_draggingWindow = 0;
            m_drag_diff = QPointF();
        } else {
            QMouseEvent *me = static_cast<QMouseEvent *>(event);
            QPointF localPos;
            if (target)
                localPos = toView(target, me->localPos());
            input->sendMouseReleaseEvent(me->button());
        }
        return true;
    }
    case QEvent::MouseMove: {
        QMouseEvent *me = static_cast<QMouseEvent *>(event);
        if (m_draggingWindow) {
            m_draggingWindow->setRequestedPosition(me->localPos() - m_drag_diff);
            m_renderScheduler.start(0);
        } else {
            QPointF local;
            QWaylandView *target = viewAt(me->localPos(), &local);
            input->sendMouseMoveEvent(target, local, me->localPos());
        }
        break;
    }
    case QEvent::Wheel: {
        QWheelEvent *we = static_cast<QWheelEvent *>(event);
        input->sendMouseWheelEvent(we->orientation(), we->delta());
        break;
    }
    case QEvent::KeyPress: {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Meta || ke->key() == Qt::Key_Super_L) {
            m_dragKeyIsPressed = true;
        }
        m_modifiers = ke->modifiers();
        QWaylandSurface *targetSurface = input->keyboardFocus();
        if (targetSurface)
            input->sendKeyPressEvent(ke->nativeScanCode());
        break;
    }
    case QEvent::KeyRelease: {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Meta || ke->key() == Qt::Key_Super_L) {
            m_dragKeyIsPressed = false;
        }
        m_modifiers = ke->modifiers();
        QWaylandSurface *targetSurface = input->keyboardFocus();
        if (targetSurface)
            input->sendKeyReleaseEvent(ke->nativeScanCode());
        break;
    }
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    {
        QWaylandView *target = 0;
        QTouchEvent *te = static_cast<QTouchEvent *>(event);
        QList<QTouchEvent::TouchPoint> points = te->touchPoints();
        QPoint pointPos;
        if (!points.isEmpty()) {
            pointPos = points.at(0).pos().toPoint();
            target = viewAt(pointPos);
        }
        if (target && target != input->mouseFocus())
            input->sendMouseMoveEvent(target, pointPos, pointPos);
        if (input->mouseFocus())
            input->sendFullTouchEvent(te);
        break;
    }
    default:
        break;
    }
    return false;
}

QT_END_NAMESPACE
