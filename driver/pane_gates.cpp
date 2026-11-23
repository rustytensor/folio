// pane-gates: checks the draggable lines between Folio's panes in the real
// main window (Main.qml), offscreen, with synthetic mouse events.
//
// Usage: pane-gates <gate>, with a gate number from 3 to 12, or 15. Each run is one
// gate in a process of its own, with a private HOME and XDG folders made for
// it, so settings written by one gate never reach another and nothing of the
// user's own configuration, cache or data is read or written.
//
// The last line printed is `PASS`, or `FAIL: <what was seen, and what was
// expected>`; the exit status is 0 for a pass and 1 otherwise. Qt's own
// messages go to stderr.
//
// The lines are found only by their object names (sidebarDivider,
// splitDivider, columnDivider). Widths are read from each divider's
// `paneWidth`, cross-checked against where the divider's parent line sits on
// screen, and the stored values from the window's `sidebarWidth`,
// `columnWidth` and `splitShare` properties.

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJSValue>
#include <QPointer>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <unistd.h>

#include "backend.h"
#include "disks.h"
#include "dragpicture.h"
#include "foldermodel.h"
#include "kineticscroll.h"
#include "places.h"
#include "plugins.h"
#include "theme.h"
#include "thumbnails.h"
#include "translator.h"
#include "windows.h"

namespace {

// The window size every gate runs at: Main.qml's own default.
constexpr int windowWidth = 1280;
constexpr int windowHeight = 800;
// Main.qml's least width, for the narrow split.
constexpr int narrowWidth = 720;
// The least width either pane of a split keeps.
constexpr double splitLeast = 240;
// How long any one wait may take before the gate fails, and how long a whole
// gate may run before the watchdog ends it.
constexpr int waitLimitMs = 10000;
constexpr int gateLimitMs = 90000;

// What the gate is doing, for the watchdog's message.
QString currentStep = QStringLiteral("starting");

class GateFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const QString &what)
{
    throw GateFailure(what.toStdString());
}

void step(const QString &what)
{
    currentStep = what;
}

// Two widths are the same when they differ by less than this: item
// geometry is in doubles, and every expected width is a whole pixel.
constexpr double exact = 0.01;

QString px(double value)
{
    return QString::number(value, 'f', std::abs(value - std::round(value)) < exact ? 0 : 2);
}

void waitFor(const std::function<bool()> &condition, const QString &what)
{
    if (!QTest::qWaitFor(condition, waitLimitMs))
        fail(QStringLiteral("timed out after %1 ms waiting for %2").arg(waitLimitMs).arg(what));
}

// Lets bindings, layouts and Qt.callLater() run.
void settle(int ms = 150)
{
    QTest::qWait(ms);
}

// udisks stood in for: no disks, and no call ever reaches the system bus.
class NoDisks : public Disks {
public:
    QList<Disk> list() override { return {}; }
    QString unmount(const QString &) override { return refused(); }
    QString lock(const QString &) override { return refused(); }
    QString makeTable(const Disk &) override { return refused(); }
    QString addPartition(const Disk &, const QString &, const QString &, QString *) override { return refused(); }
    bool canFormat(const QString &) override { return false; }
    QString mount(const QString &) override { return refused(); }
    QString open(const Disk &, int *) override { return refused(); }
    QString eject(const Disk &) override { return refused(); }

private:
    static QString refused() { return QStringLiteral("no disks here"); }
};

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size())
        fail(QStringLiteral("could not write the fixture file %1").arg(path));
}

// The folders the gates open, inside the private home:
//   d1/d2/d3/d4/d5/d6  six folders deep, for the scrolled columns;
//   big/               400 files, a column long enough to scroll.
void makeFixture(const QString &home)
{
    if (!QDir(home).mkpath(QStringLiteral("d1/d2/d3/d4/d5/d6")))
        fail(QStringLiteral("could not make the fixture folders in %1").arg(home));
    for (int i = 0; i < 400; ++i)
        writeFile(home + QStringLiteral("/big/file-%1.txt").arg(i, 3, 10, QLatin1Char('0')), "x");
}

// A QML `var` property holding an object arrives as a QObject pointer or as
// a QJSValue wrapping one.
QObject *objectOf(const QVariant &value)
{
    if (value.metaType() == QMetaType::fromType<QJSValue>())
        return value.value<QJSValue>().toQObject();
    return value.value<QObject *>();
}

// Every item under `root` (itself included), in the visual tree.
void collect(QQuickItem *root, const std::function<void(QQuickItem *)> &each)
{
    if (!root)
        return;
    each(root);
    const auto children = root->childItems();
    for (QQuickItem *child : children)
        collect(child, each);
}

QList<QQuickItem *> namedItems(QQuickItem *root, const QString &name)
{
    QList<QQuickItem *> found;
    collect(root, [&](QQuickItem *item) {
        if (item->objectName() == name)
            found << item;
    });
    return found;
}

QPointF scenePos(QQuickItem *item)
{
    return item->mapToScene(QPointF(0, 0));
}

double numberProperty(QObject *object, const char *name, const QString &owner)
{
    const QVariant value = object->property(name);
    if (!value.isValid())
        fail(QStringLiteral("%1 has no property %2").arg(owner, QLatin1String(name)));
    bool ok = false;
    const double number = value.toDouble(&ok);
    if (!ok)
        fail(QStringLiteral("%1.%2 is not a number (%3)").arg(owner, QLatin1String(name), value.toString()));
    return number;
}

// Folio's shared state, built as main.cpp's Session builds it, but with
// every folder it reads or writes inside the private home and no plugins.
class Folio {
public:
    explicit Folio(const QString &root)
        : m_home(root + QStringLiteral("/home")),
          m_translator(languageFile(root)),
          m_theme(root + QStringLiteral("/theme")),
          m_places(m_home),
          m_thumbnails(root + QStringLiteral("/thumbnails"), {root + QStringLiteral("/no-thumbnailers")}),
          m_plugins(QStringLiteral("en"), {root + QStringLiteral("/plugins")}, root + QStringLiteral("/plugins.conf")),
          m_windows(&m_engine, &m_plugins)
    {
        QCoreApplication::installTranslator(&m_translator);
        m_engine.addImageProvider(QStringLiteral("thumb"), new ThumbnailProvider(&m_thumbnails));
        m_engine.addImageProvider(QStringLiteral("plugin-icon"), new PluginIconProvider);
        QQmlContext *context = m_engine.rootContext();
        context->setContextProperty(QStringLiteral("translator"), &m_translator);
        context->setContextProperty(QStringLiteral("theme"), &m_theme);
        context->setContextProperty(QStringLiteral("places"), &m_places);
        context->setContextProperty(QStringLiteral("plugins"), &m_plugins);
    }

    QString home() const { return m_home; }

    // Opens a main window on `folder` (relative to the home) and waits until
    // it is on screen, settled, and its left pane lists that folder.
    QQuickWindow *open(const QString &folder)
    {
        const QString path = QDir::cleanPath(m_home + QLatin1Char('/') + folder);
        step(QStringLiteral("opening a window on %1").arg(folder));
        const QWindowList before = QGuiApplication::topLevelWindows();
        if (!m_windows.open(path))
            fail(QStringLiteral("Main.qml did not load (its errors are in the log)"));
        QQuickWindow *window = nullptr;
        const QWindowList after = QGuiApplication::topLevelWindows();
        for (QWindow *each : after)
            if (!before.contains(each))
                if (auto *quick = qobject_cast<QQuickWindow *>(each))
                    window = quick;
        if (!window)
            fail(QStringLiteral("no new QQuickWindow after opening Main.qml"));
        if (!QTest::qWaitForWindowExposed(window, waitLimitMs))
            fail(QStringLiteral("the window was never exposed"));
        if (window->size() != QSize(windowWidth, windowHeight))
            window->resize(windowWidth, windowHeight);
        waitFor([&] { return window->size() == QSize(windowWidth, windowHeight); }, QStringLiteral("the window size"));
        waitFor([&] { return window->property("settled").toBool(); }, QStringLiteral("the window to settle"));
        // Active, so keys and hover reach it as they would a real window.
        window->requestActivate();
        if (!QTest::qWaitForWindowActive(window, waitLimitMs))
            fail(QStringLiteral("the window never became active"));
        QObject *pane = objectOf(window->property("activePane"));
        if (!pane)
            fail(QStringLiteral("the window has no activePane"));
        waitForListing(pane, path);
        return window;
    }

    // Waits until a pane shows `path` and has finished listing it.
    static void waitForListing(QObject *pane, const QString &path)
    {
        waitFor(
            [&] {
                auto *backend = qobject_cast<Backend *>(objectOf(pane->property("paneBackend")));
                return backend && backend->path() == path && !backend->folder()->property("loading").toBool();
            },
            QStringLiteral("the pane to list %1").arg(path));
        settle(400);
    }

private:
    static QString languageFile(const QString &root)
    {
        // In a folder of its own: the translator watches the file's folder.
        const QString file = root + QStringLiteral("/language/language");
        writeFile(file, "en\n");
        return file;
    }

    QString m_home;
    Translator m_translator;
    Theme m_theme;
    Places m_places;
    Thumbnails m_thumbnails;
    Plugins m_plugins;
    QQmlEngine m_engine;
    Windows m_windows;
};

// One main window as the gates see it: its lines, the widths they report,
// and the pointer moves that drag them.
class Window {
public:
    explicit Window(QQuickWindow *window) : m_window(window)
    {
        m_leftPane = qobject_cast<QQuickItem *>(objectOf(window->property("activePane")));
        if (!m_leftPane)
            fail(QStringLiteral("the window's activePane is not an item"));
    }

    QQuickWindow *quick() const { return m_window; }

    static bool mirrored() { return QGuiApplication::layoutDirection() == Qt::RightToLeft; }

    double width() const { return m_window->contentItem()->width(); }

    // The divider with this name that is on screen; the gate fails unless
    // there is exactly one.
    QQuickItem *divider(const QString &name) const
    {
        QList<QQuickItem *> shown;
        for (QQuickItem *item : namedItems(m_window->contentItem(), name))
            if (item->isVisible())
                shown << item;
        if (shown.isEmpty())
            fail(QStringLiteral("no item named %1").arg(name));
        if (shown.size() > 1)
            fail(QStringLiteral("%1 visible items named %2, expected 1").arg(shown.size()).arg(name));
        return shown.first();
    }

    static QQuickItem *lineOf(QQuickItem *divider, const QString &name)
    {
        QQuickItem *line = divider->parentItem();
        if (!line)
            fail(QStringLiteral("%1 has no parent line").arg(name));
        return line;
    }

    static double paneWidth(QQuickItem *divider, const QString &name)
    {
        return numberProperty(divider, "paneWidth", name);
    }

    double stored(const char *name) const { return numberProperty(m_window, name, QStringLiteral("the window")); }

    // The sidebar's width: its divider's paneWidth, which must match where
    // the sidebar's line is (the line follows the sidebar directly).
    double sidebar() const
    {
        QQuickItem *divider = this->divider(QStringLiteral("sidebarDivider"));
        const double reported = paneWidth(divider, QStringLiteral("sidebarDivider"));
        const double lineX = scenePos(lineOf(divider, QStringLiteral("sidebarDivider"))).x();
        const double onScreen = mirrored() ? width() - lineX - 1 : lineX;
        if (std::abs(reported - onScreen) > exact)
            fail(QStringLiteral("sidebarDivider.paneWidth is %1 but the sidebar is %2 px on screen")
                     .arg(px(reported), px(onScreen)));
        return reported;
    }

    QQuickItem *rightPane() const { return qobject_cast<QQuickItem *>(objectOf(m_window->property("rightPane"))); }

    void openSplit()
    {
        step(QStringLiteral("opening the split"));
        if (!QMetaObject::invokeMethod(m_window, "openSplit"))
            fail(QStringLiteral("the window's openSplit() could not be called"));
        waitFor([&] { return rightPane() && rightPane()->width() > 0; }, QStringLiteral("the right pane"));
        Folio::waitForListing(rightPane(), qobject_cast<Backend *>(objectOf(m_leftPane->property("paneBackend")))->path());
    }

    struct Split {
        double left;
        double right;
        double room;
    };

    // The two folder panes of a split: the left pane (the one before the
    // line in layout direction) from its divider's paneWidth, cross-checked
    // with the line positions and the pane's own width, and the right pane
    // from its item. The room is the row less the sidebar and both lines.
    Split split() const
    {
        const double sidebarWidth = sidebar();
        QQuickItem *sidebarLine = lineOf(divider(QStringLiteral("sidebarDivider")), QStringLiteral("sidebarDivider"));
        QQuickItem *divider = this->divider(QStringLiteral("splitDivider"));
        QQuickItem *line = lineOf(divider, QStringLiteral("splitDivider"));
        const double reported = paneWidth(divider, QStringLiteral("splitDivider"));
        const double sidebarLineX = scenePos(sidebarLine).x();
        const double lineX = scenePos(line).x();
        const double between = mirrored() ? sidebarLineX - lineX - 1 : lineX - sidebarLineX - 1;
        if (std::abs(reported - between) > exact || std::abs(reported - m_leftPane->width()) > exact)
            fail(QStringLiteral("splitDivider.paneWidth is %1 but the left pane is %2 px between the lines and %3 px wide")
                     .arg(px(reported), px(between), px(m_leftPane->width())));
        QQuickItem *right = rightPane();
        if (!right)
            fail(QStringLiteral("the window is not split"));
        const double after = mirrored() ? lineX : width() - lineX - 1;
        if (std::abs(after - right->width()) > exact)
            fail(QStringLiteral("the right pane is %1 px wide but %2 px lie past the split line")
                     .arg(px(right->width()), px(after)));
        return {reported, right->width(), width() - sidebarWidth - 2};
    }

    // The left pane's column lines, in layout order (from the side the
    // columns grow from).
    QList<QQuickItem *> columnDividers() const
    {
        QList<QQuickItem *> found;
        for (QQuickItem *item : namedItems(m_leftPane, QStringLiteral("columnDivider")))
            if (item->isVisible())
                found << item;
        if (found.isEmpty())
            fail(QStringLiteral("no item named columnDivider"));
        std::sort(found.begin(), found.end(), [](QQuickItem *a, QQuickItem *b) {
            const double ax = scenePos(a->parentItem() ? a->parentItem() : a).x();
            const double bx = scenePos(b->parentItem() ? b->parentItem() : b).x();
            return mirrored() ? ax > bx : ax < bx;
        });
        return found;
    }

    // The first column line inside the columns' viewport: with the split
    // open the columns may be scrolled so the first column is out of sight.
    QQuickItem *firstColumnDividerInView() const
    {
        const QList<QQuickItem *> dividers = columnDividers();
        QQuickItem *flick = columnsFlickable(dividers.first());
        const double left = scenePos(flick).x();
        for (QQuickItem *divider : dividers) {
            const double x = scenePos(divider->parentItem()).x();
            if (x >= left && x < left + flick->width())
                return divider;
        }
        fail(QStringLiteral("no column line is in view"));
    }

    // The Flickable the columns scroll in, found by walking up from a
    // column line.
    static QQuickItem *columnsFlickable(QQuickItem *divider)
    {
        for (QQuickItem *item = divider->parentItem(); item; item = item->parentItem())
            if (item->inherits("QQuickFlickable"))
                return item;
        fail(QStringLiteral("no Flickable encloses the columnDivider"));
    }

    static QQuickItem *contentItemOf(QQuickItem *flickable)
    {
        auto *content = qobject_cast<QQuickItem *>(objectOf(flickable->property("contentItem")));
        if (!content)
            fail(QStringLiteral("the columns' Flickable has no contentItem"));
        return content;
    }

    // A line's x in the columns' content, where the first column starts at
    // 0 (left to right) or ends at the content's width (right to left).
    static double contentX(QQuickItem *line)
    {
        QQuickItem *content = contentItemOf(columnsFlickable(line));
        return line->mapToItem(content, QPointF(0, 0)).x();
    }

    // Every column's width, one per line: each divider's paneWidth,
    // cross-checked with the room between its line and the one before.
    QList<double> columns() const
    {
        const QList<QQuickItem *> dividers = columnDividers();
        QQuickItem *content = contentItemOf(columnsFlickable(dividers.first()));
        QList<double> widths;
        double edge = mirrored() ? content->width() : 0;
        for (int i = 0; i < dividers.size(); ++i) {
            QQuickItem *line = lineOf(dividers[i], QStringLiteral("columnDivider"));
            const double lineX = line->mapToItem(content, QPointF(0, 0)).x();
            const double onScreen = mirrored() ? edge - lineX - 1 : lineX - edge;
            const double reported = paneWidth(dividers[i], QStringLiteral("columnDivider"));
            if (std::abs(reported - onScreen) > exact)
                fail(QStringLiteral("columnDivider %1 has paneWidth %2 but its column is %3 px on screen")
                         .arg(i).arg(px(reported), px(onScreen)));
            widths << reported;
            edge = mirrored() ? lineX : lineX + 1;
        }
        return widths;
    }

    // Where to press on a line: on its own pixel column, halfway down it
    // (which is below the 60 px top bar for the split's full-height line).
    static QPoint grip(QQuickItem *divider)
    {
        QQuickItem *line = divider->parentItem();
        const QPointF at = line->mapToScene(QPointF(0, line->height() / 2));
        return QPoint(qRound(at.x()), qRound(at.y()));
    }

    // Presses the left button at `from`, moves the pointer horizontally by
    // `travel` in small steps (`stride` px each), and releases there unless
    // asked not to.
    void drag(QPoint from, int travel, bool release = true, int stride = 2)
    {
        QTest::mouseMove(m_window, from);
        QTest::mousePress(m_window, Qt::LeftButton, {}, from);
        const int sign = travel < 0 ? -1 : 1;
        for (int moved = stride; moved < std::abs(travel); moved += stride)
            QTest::mouseMove(m_window, from + QPoint(sign * moved, 0));
        const QPoint to = from + QPoint(travel, 0);
        QTest::mouseMove(m_window, to);
        if (release)
            QTest::mouseRelease(m_window, Qt::LeftButton, {}, to);
        settle();
        trace(QStringLiteral("dragged from %1,%2 by %3%4")
                  .arg(from.x())
                  .arg(from.y())
                  .arg(travel)
                  .arg(release ? QString() : QStringLiteral(" (still pressed)")));
    }

    // With PANE_GATES_TRACE set, each pointer action is logged to stderr
    // with the widths seen after it, to see where a gate went wrong.
    void trace(const QString &what) const
    {
        if (qEnvironmentVariableIsEmpty("PANE_GATES_TRACE"))
            return;
        QStringList seen;
        for (const char *name : {"sidebarDivider", "splitDivider"})
            for (QQuickItem *item : namedItems(m_window->contentItem(), QLatin1String(name)))
                seen << QStringLiteral("%1 %2 at x %3").arg(QLatin1String(name), item->property("paneWidth").toString())
                            .arg(px(scenePos(item->parentItem()).x()));
        for (QQuickItem *item : namedItems(m_leftPane, QStringLiteral("columnDivider")))
            seen << QStringLiteral("columnDivider %1 at x %2").arg(item->property("paneWidth").toString(),
                                                                  px(scenePos(item->parentItem()).x()));
        std::fprintf(stderr, "trace: %s; %s\n", qPrintable(what), qPrintable(seen.join(QStringLiteral(", "))));
    }

    // Resizes the window to `width` (at the usual height) and waits for the
    // panes to be laid out again.
    void resize(int width)
    {
        m_window->resize(width, windowHeight);
        waitFor([&] { return m_window->width() == width; }, QStringLiteral("the window to be %1 px wide").arg(width));
        settle(400);
    }

    void doubleClick(QPoint at)
    {
        QTest::mouseMove(m_window, at);
        QTest::mouseDClick(m_window, Qt::LeftButton, {}, at, 10);
        settle();
    }

private:
    QQuickWindow *m_window;
    QQuickItem *m_leftPane;
};

void expectEqual(double seen, double expected, const QString &what, double tolerance = exact)
{
    if (std::abs(seen - expected) > tolerance)
        fail(QStringLiteral("%1 is %2, expected %3%4")
                 .arg(what, px(seen), px(expected),
                      tolerance > exact ? QStringLiteral(" within %1 px").arg(px(tolerance)) : QString()));
}

void expectColumns(const Window &window, double expected, const QString &when)
{
    const QList<double> widths = window.columns();
    for (int i = 0; i < widths.size(); ++i)
        expectEqual(widths[i], expected, QStringLiteral("column %1 %2").arg(i).arg(when));
}

void expectEvenSplit(const Window &window, const QString &when)
{
    const Window::Split split = window.split();
    if (std::abs(split.left - split.right) > 1 + exact)
        fail(QStringLiteral("split panes %1 and %2 px %3, expected equal within 1 px")
                 .arg(px(split.left), px(split.right), when));
}

void expectFilled(const Window &window, const QString &when)
{
    const Window::Split split = window.split();
    const double sum = window.sidebar() + 1 + split.left + 1 + split.right;
    expectEqual(sum, window.width(), QStringLiteral("sidebar + lines + both panes %1").arg(when));
}

void expectStoredZero(const Window &window, const QString &when)
{
    for (const char *name : {"sidebarWidth", "columnWidth", "splitShare"})
        expectEqual(window.stored(name), 0, QStringLiteral("the window's %1 %2").arg(QLatin1String(name), when));
}

// ---- The gates -----------------------------------------------------------

// 3: nothing dragged, the window is laid out as before.
void defaultsGate(Folio &folio)
{
    Window window(folio.open(QStringLiteral("d1")));
    step(QStringLiteral("reading the default widths"));
    expectEqual(window.sidebar(), 190, QStringLiteral("the sidebar"));
    expectColumns(window, 260, QStringLiteral("with nothing dragged"));
    window.openSplit();
    expectEvenSplit(window, QStringLiteral("with nothing dragged"));
    expectFilled(window, QStringLiteral("with nothing dragged"));
}

// 4: each line dragged sizes its pane by the pointer's travel.
void dragGate(Folio &folio)
{
    Window window(folio.open(QStringLiteral("d1")));
    step(QStringLiteral("dragging the sidebar line by +60"));
    window.drag(Window::grip(window.divider(QStringLiteral("sidebarDivider"))), 60);
    expectEqual(window.sidebar(), 250, QStringLiteral("the sidebar after dragging its line +60"));

    step(QStringLiteral("dragging the first column's line by +40"));
    window.drag(Window::grip(window.columnDividers().first()), 40);
    expectColumns(window, 300, QStringLiteral("after dragging the first column's line +40"));

    window.openSplit();
    step(QStringLiteral("dragging the split line by +100"));
    const double before = window.split().left;
    window.drag(Window::grip(window.divider(QStringLiteral("splitDivider"))), 100);
    expectEqual(window.split().left, before + 100, QStringLiteral("the left pane after dragging the split line +100"), 1);
    expectFilled(window, QStringLiteral("after dragging the split line"));
}

// 5: a double-click on a line puts its pane back to the default.
void resetGate(Folio &folio)
{
    Window window(folio.open(QStringLiteral("d1")));
    window.openSplit();
    step(QStringLiteral("dragging every line"));
    window.drag(Window::grip(window.divider(QStringLiteral("sidebarDivider"))), 60);
    window.drag(Window::grip(window.firstColumnDividerInView()), 40);
    window.drag(Window::grip(window.divider(QStringLiteral("splitDivider"))), 100);
    if (std::abs(window.sidebar() - 190) < exact || std::abs(window.columns().first() - 260) < exact)
        fail(QStringLiteral("dragging changed nothing (sidebar %1, first column %2), so there is nothing to reset")
                 .arg(px(window.sidebar()), px(window.columns().first())));

    step(QStringLiteral("double-clicking the sidebar line"));
    window.doubleClick(Window::grip(window.divider(QStringLiteral("sidebarDivider"))));
    expectEqual(window.sidebar(), 190, QStringLiteral("the sidebar after a double-click on its line"));
    expectEqual(window.stored("sidebarWidth"), 0, QStringLiteral("sidebarWidth after a double-click"));

    step(QStringLiteral("double-clicking a column line"));
    window.doubleClick(Window::grip(window.firstColumnDividerInView()));
    expectColumns(window, 260, QStringLiteral("after a double-click on a column line"));
    expectEqual(window.stored("columnWidth"), 0, QStringLiteral("columnWidth after a double-click"));

    step(QStringLiteral("double-clicking the split line"));
    window.doubleClick(Window::grip(window.divider(QStringLiteral("splitDivider"))));
    expectEvenSplit(window, QStringLiteral("after a double-click on the split line"));
    expectEqual(window.stored("splitShare"), 0, QStringLiteral("splitShare after a double-click"));
}

// 6: the dragged widths are kept, so the next window has them.
void keptGate(Folio &folio)
{
    Window first(folio.open(QStringLiteral("d1")));
    step(QStringLiteral("dragging every line in the first window"));
    first.drag(Window::grip(first.divider(QStringLiteral("sidebarDivider"))), 60);
    first.drag(Window::grip(first.columnDividers().first()), 40);
    first.openSplit();
    first.drag(Window::grip(first.divider(QStringLiteral("splitDivider"))), 100);
    const double sidebar = first.sidebar();
    const double column = first.columns().first();
    const double left = first.split().left;
    expectEqual(sidebar, 250, QStringLiteral("the first window's sidebar after dragging"));
    expectEqual(column, 300, QStringLiteral("the first window's columns after dragging"));
    // Settings writes its values shortly after they change.
    settle(1500);

    Window second(folio.open(QStringLiteral("d1")));
    step(QStringLiteral("reading the second window"));
    expectEqual(second.sidebar(), sidebar, QStringLiteral("the second window's sidebar"));
    expectColumns(second, column, QStringLiteral("in the second window"));
    second.openSplit();
    expectEqual(second.split().left, left, QStringLiteral("the second window's left pane"), 1);
}

// 7: dragging a column line while earlier columns are scrolled out of
// sight keeps those columns out of sight and the line under the pointer.
void scrolledGate(Folio &folio)
{
    Window window(folio.open(QStringLiteral("d1/d2/d3/d4/d5/d6")));
    QList<QQuickItem *> dividers = window.columnDividers();
    if (dividers.size() < 6)
        fail(QStringLiteral("%1 column lines in a folder six deep, expected at least 6").arg(dividers.size()));
    expectColumns(window, 260, QStringLiteral("before dragging"));
    QQuickItem *flick = Window::columnsFlickable(dividers.first());

    // Two whole columns scrolled past the starting edge.
    step(QStringLiteral("scrolling back to the third column"));
    const int past = 2;
    const double most = std::max(0.0, flick->property("contentWidth").toDouble() - flick->width());
    const double offset = past * (260 + 1);
    if (offset >= most)
        fail(QStringLiteral("the columns scroll only %1 px, too little to scroll %2 columns back").arg(px(most)).arg(past));
    flick->setProperty("contentX", Window::mirrored() ? most - offset : offset);
    settle(400);
    const double scrolled = flick->property("contentX").toDouble();
    expectEqual(scrolled, offset, QStringLiteral("contentX after scrolling back"));

    // The line after the fourth column, the second one in view.
    QQuickItem *divider = window.columnDividers().at(3);
    QQuickItem *line = divider->parentItem();
    const double lineBefore = scenePos(line).x();
    const QPoint from = Window::grip(divider);
    if (from.x() < scenePos(flick).x() || from.x() >= scenePos(flick).x() + flick->width())
        fail(QStringLiteral("the fourth column's line is not in view after scrolling"));

    // Checked while the line is still held, once the columns have been laid
    // out again (a Row places its children at the next frame), and again
    // after the release, which by itself must move nothing.
    const auto check = [&](const QString &when) {
        const QList<double> widths = window.columns();
        const double width = widths.first();
        for (int i = 0; i < widths.size(); ++i)
            expectEqual(widths[i], width, QStringLiteral("column %1 %2 (all columns share one width)").arg(i).arg(when));
        if (std::abs(width - 260) < exact)
            fail(QStringLiteral("the columns are still 260 px %1").arg(when));
        const double now = flick->property("contentX").toDouble();
        const double mostNow = std::max(0.0, flick->property("contentWidth").toDouble() - flick->width());
        const double offsetNow = Window::mirrored() ? mostNow - now : now;
        expectEqual(offsetNow, past * (width + 1),
                    QStringLiteral("the scroll offset %1 (%2 columns of %3 px)").arg(when).arg(past).arg(px(width)), 1);
        if (std::abs(offsetNow - mostNow) < 1)
            fail(QStringLiteral("the view jumped to the end %1 (scroll offset %2 of %3)")
                     .arg(when, px(offsetNow), px(mostNow)));
        expectEqual(scenePos(line).x(), lineBefore + 30,
                    QStringLiteral("the grabbed line's x %1 (the pointer moved +30)").arg(when), 2);
    };
    step(QStringLiteral("dragging the fourth column's line by +30"));
    window.drag(from, 30, false);
    settle(400);
    check(QStringLiteral("while the line is held"));
    QTest::mouseRelease(window.quick(), Qt::LeftButton, {}, from + QPoint(30, 0));
    settle(400);
    check(QStringLiteral("after the release"));
}

// 8: right to left, the pane before a line is on its right, and dragging
// the line left grows it.
void mirroredGate(Folio &folio)
{
    QGuiApplication::setLayoutDirection(Qt::RightToLeft);
    Window window(folio.open(QStringLiteral("d1")));
    if (!Window::mirrored())
        fail(QStringLiteral("the application did not stay right to left"));
    step(QStringLiteral("right to left: reading the defaults"));
    expectEqual(window.sidebar(), 190, QStringLiteral("the mirrored sidebar"));
    expectColumns(window, 260, QStringLiteral("mirrored, before dragging"));

    step(QStringLiteral("right to left: dragging the sidebar line by -60"));
    window.drag(Window::grip(window.divider(QStringLiteral("sidebarDivider"))), -60);
    expectEqual(window.sidebar(), 250, QStringLiteral("the mirrored sidebar after dragging its line 60 px left"), 1);

    step(QStringLiteral("right to left: dragging the first column's line by -40"));
    window.drag(Window::grip(window.columnDividers().first()), -40);
    expectColumns(window, 300, QStringLiteral("mirrored, after dragging the first column's line 40 px left"));

    window.openSplit();
    step(QStringLiteral("right to left: dragging the split line by -100"));
    const double before = window.split().left;
    window.drag(Window::grip(window.divider(QStringLiteral("splitDivider"))), -100);
    expectEqual(window.split().left, before + 100,
                QStringLiteral("the mirrored left pane after dragging the split line 100 px left"), 1);
    expectFilled(window, QStringLiteral("mirrored, after dragging the split line"));
}

// 9: dragging far out stops at the limits.
void limitsGate(Folio &folio)
{
    Window window(folio.open(QStringLiteral("d1")));
    step(QStringLiteral("dragging the sidebar line far in and out"));
    window.drag(Window::grip(window.divider(QStringLiteral("sidebarDivider"))), -120);
    expectEqual(window.sidebar(), 140, QStringLiteral("the sidebar dragged 120 px narrower"));
    window.drag(Window::grip(window.divider(QStringLiteral("sidebarDivider"))), 400);
    expectEqual(window.sidebar(), 400, QStringLiteral("the sidebar dragged 400 px wider"));

    step(QStringLiteral("dragging the first column's line far in and out"));
    window.drag(Window::grip(window.columnDividers().first()), -200);
    expectColumns(window, 180, QStringLiteral("after dragging the first column's line 200 px narrower"));
    window.drag(Window::grip(window.columnDividers().first()), 400);
    expectColumns(window, 480, QStringLiteral("after dragging the first column's line 400 px wider"));

    window.openSplit();
    step(QStringLiteral("dragging the split line far in and out"));
    window.drag(Window::grip(window.divider(QStringLiteral("splitDivider"))), -500);
    Window::Split split = window.split();
    expectEqual(split.left, splitLeast,
                QStringLiteral("the left pane dragged 500 px narrower (room %1)").arg(px(split.room)));
    window.drag(Window::grip(window.divider(QStringLiteral("splitDivider"))), 600);
    split = window.split();
    expectEqual(split.left, split.room - splitLeast,
                QStringLiteral("the left pane dragged 600 px wider (room %1 less %2)").arg(px(split.room), px(splitLeast)));
}

// 15: in a window too narrow for two 240 px panes, the split stays even
// whatever is dragged; widened again, the left pane is within its limits.
void narrowSplitGate(Folio &folio)
{
    Window window(folio.open(QStringLiteral("d1")));
    window.resize(narrowWidth);
    step(QStringLiteral("dragging the sidebar line to 400 in a %1 px window").arg(narrowWidth));
    window.drag(Window::grip(window.divider(QStringLiteral("sidebarDivider"))), 400 - qRound(window.sidebar()));
    expectEqual(window.sidebar(), 400, QStringLiteral("the sidebar dragged to 400 in a %1 px window").arg(narrowWidth));
    window.openSplit();
    expectEqual(window.split().room, narrowWidth - 400 - 2, QStringLiteral("the split's room"));

    step(QStringLiteral("dragging the narrow split line by +100"));
    window.drag(Window::grip(window.divider(QStringLiteral("splitDivider"))), 100);
    expectEvenSplit(window, QStringLiteral("after dragging the split line +100 in a room of 318 px"));
    step(QStringLiteral("dragging the narrow split line by -100"));
    window.drag(Window::grip(window.divider(QStringLiteral("splitDivider"))), -100);
    expectEvenSplit(window, QStringLiteral("after dragging the split line -100 in a room of 318 px"));

    step(QStringLiteral("widening the window to %1 px").arg(windowWidth));
    window.resize(windowWidth);
    const Window::Split split = window.split();
    if (split.left < splitLeast - exact || split.left > split.room - splitLeast + exact)
        fail(QStringLiteral("the left pane is %1 px after widening to %2 px (room %3), expected %4 to %5")
                 .arg(px(split.left))
                 .arg(windowWidth)
                 .arg(px(split.room), px(splitLeast), px(split.room - splitLeast)));
}

// 10: a press that moves less than the drag distance is not a drag.
void nudgeGate(Folio &folio)
{
    Window window(folio.open(QStringLiteral("d1")));
    window.openSplit();
    const int distance = QGuiApplication::styleHints()->startDragDistance();
    const int nudge = std::max(1, distance - 1);
    const double sidebar = window.sidebar();
    const QList<double> columns = window.columns();
    const double left = window.split().left;

    step(QStringLiteral("nudging each line by %1 px").arg(nudge));
    window.drag(Window::grip(window.divider(QStringLiteral("sidebarDivider"))), nudge, true, 1);
    window.drag(Window::grip(window.firstColumnDividerInView()), nudge, true, 1);
    window.drag(Window::grip(window.divider(QStringLiteral("splitDivider"))), nudge, true, 1);
    const QString when = QStringLiteral("after a %1 px nudge (drag distance %2)").arg(nudge).arg(distance);
    expectEqual(window.sidebar(), sidebar, QStringLiteral("the sidebar %1").arg(when));
    const QList<double> after = window.columns();
    if (after != columns)
        fail(QStringLiteral("the columns changed %1").arg(when));
    expectEqual(window.split().left, left, QStringLiteral("the left pane %1").arg(when));
    expectStoredZero(window, when);
}

// 11: stored values that are out of range or not numbers.
void badSettingsGate(Folio &folio, const QString &configHome)
{
    writeFile(configHome + QStringLiteral("/folio/folio.conf"),
              "[General]\nsidebarWidth=99999\ncolumnWidth=-5\nsplitShare=abc\n");
    Window window(folio.open(QStringLiteral("d1")));
    step(QStringLiteral("reading a window opened on bad settings"));
    expectEqual(window.sidebar(), 400, QStringLiteral("the sidebar with sidebarWidth=99999"));
    expectColumns(window, 260, QStringLiteral("with columnWidth=-5"));
    window.openSplit();
    expectEvenSplit(window, QStringLiteral("with splitShare=abc"));
}

// 12: the column's own scroll bar, just before its line, still scrolls the
// column rather than sizing it.
void scrollBarGate(Folio &folio)
{
    Window window(folio.open(QStringLiteral("big")));
    const QList<QQuickItem *> dividers = window.columnDividers();
    if (dividers.size() < 2)
        fail(QStringLiteral("%1 column lines with big/ open, expected 2").arg(dividers.size()));
    // big/ is the second column; its line is the second one.
    QQuickItem *line = dividers.at(1)->parentItem();
    const double lineX = scenePos(line).x();
    const double sidebar = window.sidebar();

    step(QStringLiteral("finding big/'s column"));
    QQuickItem *list = nullptr;
    collect(Window::contentItemOf(Window::columnsFlickable(dividers.at(1))), [&](QQuickItem *item) {
        if (list || !item->inherits("QQuickListView") || !item->isVisible())
            return;
        const double left = scenePos(item).x();
        const double edge = Window::mirrored() ? left - 1 : left + item->width();
        if (std::abs(edge - lineX) < exact)
            list = item;
    });
    if (!list)
        fail(QStringLiteral("no list ends at big/'s column line"));
    waitFor([&] { return list->property("count").toInt() == 400; }, QStringLiteral("big/'s 400 files"));
    settle(300);
    QQuickItem *bar = nullptr;
    for (QQuickItem *child : list->childItems())
        if (child->inherits("QQuickScrollBar") && child->property("orientation").toInt() == Qt::Vertical)
            bar = child;
    if (!bar)
        fail(QStringLiteral("big/'s column has no vertical scroll bar"));
    const double size = bar->property("size").toDouble();
    if (size <= 0 || size >= 1)
        fail(QStringLiteral("big/'s column does not scroll (scroll bar size %1)").arg(size));

    // The middle of the handle, which sits at the top before scrolling.
    const double padding = bar->property("topPadding").toDouble();
    const double track = bar->height() - padding - bar->property("bottomPadding").toDouble();
    const QPointF barAt = scenePos(bar);
    const QPoint handle(qRound(barAt.x() + bar->width() / 2),
                        qRound(barAt.y() + padding + track * (bar->property("position").toDouble() + size / 2)));
    const bool beforeLine = Window::mirrored() ? handle.x() > lineX : handle.x() < lineX;
    if (!beforeLine)
        fail(QStringLiteral("the scroll bar's middle (x %1) is not before the line (x %2)").arg(handle.x()).arg(px(lineX)));
    const double scrolledBefore = list->property("contentY").toDouble();

    step(QStringLiteral("dragging big/'s scroll bar down"));
    QTest::mouseMove(window.quick(), handle);
    settle(100);
    QTest::mousePress(window.quick(), Qt::LeftButton, {}, handle);
    for (int moved = 4; moved <= 200; moved += 4)
        QTest::mouseMove(window.quick(), handle + QPoint(0, moved));
    QTest::mouseRelease(window.quick(), Qt::LeftButton, {}, handle + QPoint(0, 200));
    settle(300);
    const double scrolledAfter = list->property("contentY").toDouble();
    if (scrolledAfter <= scrolledBefore + 50)
        fail(QStringLiteral("the column scrolled from %1 to %2 px, expected it to scroll down")
                 .arg(px(scrolledBefore), px(scrolledAfter)));
    expectColumns(window, 260, QStringLiteral("after dragging the scroll bar"));
    expectEqual(window.sidebar(), sidebar, QStringLiteral("the sidebar after dragging the scroll bar"));
    expectStoredZero(window, QStringLiteral("after dragging the scroll bar"));
}

// Runs one gate and prints its result. Folio's windows are left open: the
// process ends right after, and tearing them down is not what is checked.
int run(int gate, const QString &root)
{
    try {
        Disks::replace(std::make_shared<NoDisks>());
        makeFixture(root + QStringLiteral("/home"));
        Folio *folio = new Folio(root);
        switch (gate) {
        case 3: defaultsGate(*folio); break;
        case 4: dragGate(*folio); break;
        case 5: resetGate(*folio); break;
        case 6: keptGate(*folio); break;
        case 7: scrolledGate(*folio); break;
        case 8: mirroredGate(*folio); break;
        case 9: limitsGate(*folio); break;
        case 10: nudgeGate(*folio); break;
        case 11: badSettingsGate(*folio, root + QStringLiteral("/home/.config")); break;
        case 12: scrollBarGate(*folio); break;
        case 15: narrowSplitGate(*folio); break;
        default: fail(QStringLiteral("no gate %1 here (3 to 12, 15)").arg(gate));
        }
    } catch (const GateFailure &failure) {
        std::printf("FAIL: %s\n", failure.what());
        std::fflush(stdout);
        return 1;
    }
    std::printf("PASS\n");
    std::fflush(stdout);
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    bool ok = false;
    const int gate = argc == 2 ? QByteArray(argv[1]).toInt(&ok) : 0;
    if (!ok) {
        std::fprintf(stderr, "usage: pane-gates <gate: 3 to 12, or 15>\n");
        return 2;
    }

    // A private home and XDG folders, set before Qt reads any of them
    // (QSettings keeps the first paths it sees for the whole process).
    QTemporaryDir root;
    if (!root.isValid()) {
        std::printf("FAIL: could not make a temporary folder\n");
        return 1;
    }
    const QByteArray home = QFile::encodeName(root.path() + QStringLiteral("/home"));
    QDir().mkpath(root.path() + QStringLiteral("/home"));
    qputenv("HOME", home);
    qputenv("XDG_CONFIG_HOME", home + "/.config");
    qputenv("XDG_CACHE_HOME", home + "/.cache");
    qputenv("XDG_DATA_HOME", home + "/.local/share");
    qputenv("XDG_STATE_HOME", home + "/.local/state");
    qunsetenv("XDG_CONFIG_DIRS");
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // No Hyprland, no GVfs daemons and no desktop settings of the user's.
    qunsetenv("HYPRLAND_INSTANCE_SIGNATURE");
    qputenv("GIO_USE_VFS", "local");
    qputenv("GSETTINGS_BACKEND", "memory");

    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("folio"));
    app.setOrganizationName(QStringLiteral("folio"));
    QQuickStyle::setStyle(QStringLiteral("Material"));
    qmlRegisterType<FolderModel>("Folio", 1, 0, "FolderModel");
    qmlRegisterType<DragPicture>("Folio", 1, 0, "DragPicture");
    qmlRegisterType<KineticScroll>("Folio", 1, 0, "KineticScroll");

    // Nothing may hang: a gate that runs too long fails and ends here.
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [] {
        std::printf("FAIL: the gate ran longer than %d s (at: %s)\n", gateLimitMs / 1000, qPrintable(currentStep));
        std::fflush(stdout);
        _exit(1);
    });
    watchdog.start(gateLimitMs);

    const int status = run(gate, root.path());
    // Ends without destroying the open windows (see run()), after removing
    // the private folders, which _exit would otherwise leave behind.
    watchdog.stop();
    root.remove();
    std::fflush(stderr);
    _exit(status);
}
