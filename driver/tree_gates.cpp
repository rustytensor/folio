// tree-gates: checks the hierarchy lines of Folio's list view in the real
// main window (Main.qml), offscreen.
//
// Usage:
//   tree-gates <gate> [arg]      a gate from 4 to 16, 20 to 22 or 25 (gate 4
//                                takes the PNG to write, gate 15 the stored
//                                value to try, gate 21 its part: idle, rtl,
//                                big or insert, run on the counting copy of
//                                FolderList.qml, as gate 25 is)
//   tree-gates measure           the cost of one row inserted in 10,000
//                                rows, per style (measure.sh; on the
//                                counting copy, prints one line per style)
//   tree-gates listshot <png>    the list of the fixture window, style off,
//                                cropped to the list (gate 4's other half)
//   tree-gates compare <a> <b>   whether two PNGs are pixel-identical
//   tree-gates shots <dir>       the six screenshots of gate 17, plus the
//                                two in style off and probe.txt
//   tree-gates verify-shots <dir>  gate 17's checks on those files
//   tree-gates verify-overlays <dir>  gate 23's checks on the same files
//
// Each run is one gate in a process of its own, with a private HOME and XDG
// folders made for it, so settings written by one gate never reach another
// and nothing of the user's own configuration, cache or data is read.
//
// The last line printed is `PASS`, or `FAIL: <what was seen, and what was
// expected>`; the exit status is 0 for a pass and 1 otherwise. Qt's own
// messages go to stderr.
//
// Every expected number comes from the contract's formulas, applied to the
// rows Folio's model lists (their depth, whether each is a folder and
// whether it is open). The item under test is found only by its object name
// (treeLines) and read only through what the drawing itself uses (contract
// revision 7): its style and spec, and its QtObject child holding guides,
// route and the dots' paths.
//
// With FOLIO_GATES_QML_DIR set, QML is loaded from that folder instead of
// the compiled-in resources (files missing there still come from the
// resources): gate 4 loads main's QML this way, and the self-tests load
// deliberately wrong stand-ins.

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QIdentityProxyModel>
#include <QImage>
#include <QJSValue>
#include <QProcess>
#include <QQmlAbstractUrlInterceptor>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <memory>
#include <stdexcept>
#include <unistd.h>

#include "backend.h"
#include "disks.h"
#include "dragpicture.h"
#include "fileops.h"
#include "foldermodel.h"
#include "places.h"
#include "plugins.h"
#include "theme.h"
#include "thumbnails.h"
#include "translator.h"
#include "windows.h"

namespace {

// Main.qml's own default size; gate 17's screenshots are this size.
constexpr int windowWidth = 1280;
constexpr int windowHeight = 800;
// How long any one wait may take, and how long a whole run may take before
// the watchdog ends it.
constexpr int waitLimitMs = 10000;
constexpr int gateLimitMs = 150000;
// The contract's tolerance for every position.
constexpr double within = 0.5;
// Widths and opacities are exact numbers in the contract.
constexpr double exact = 0.001;
// Gate 13's limit for computing the guides of 2000 rows.
constexpr int computeLimitMs = 1000;
// Gate 17: how far a screenshot pixel may be from the blended line colour
// (eight-bit rounding and antialiasing), per channel.
constexpr int colourSlack = 10;

// The fixture's rows, from fixture.sh tree.
constexpr int fixtureRows = 18;
constexpr int fixtureGuides = 5;
constexpr int fixtureCurves = 14;

QString currentStep = QStringLiteral("starting");
// Warnings Qt printed that came from QML files, for gate 13.
QStringList qmlWarnings;
QtMessageHandler defaultHandler = nullptr;

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

QString num(double value)
{
    return QString::number(value, 'f', std::abs(value - std::round(value)) < exact ? 0 : 2);
}

void waitFor(const std::function<bool()> &condition, const QString &what, int limit = waitLimitMs)
{
    if (!QTest::qWaitFor(condition, limit))
        fail(QStringLiteral("timed out after %1 ms waiting for %2").arg(limit).arg(what));
}

// Lets bindings, layouts and Qt.callLater() run.
void settle(int ms = 300)
{
    QTest::qWait(ms);
}

// JavaScript's Math.round, which the contract's formulas use.
int jsRound(double value)
{
    return int(std::floor(value + 0.5));
}

void keepMessage(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (type != QtDebugMsg && type != QtInfoMsg) {
        const QString file = QString::fromUtf8(context.file ? context.file : "");
        if (file.endsWith(QLatin1String(".qml")) || message.contains(QLatin1String(".qml:")))
            qmlWarnings << message;
    }
    defaultHandler(type, context, message);
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

// Loads QML from a folder in place of the resources: qrc:/X becomes the
// folder's X when it has one, and a file the folder lacks goes back to the
// resources (so a folder may hold only some of the files).
class QmlFolder : public QQmlAbstractUrlInterceptor {
public:
    explicit QmlFolder(const QString &folder) : m_folder(QDir(folder).absolutePath()) {}

    QUrl intercept(const QUrl &url, DataType) override
    {
        if (url.scheme() == QLatin1String("qrc")) {
            const QString relative = url.path().mid(1);
            const QString local = m_folder + QLatin1Char('/') + relative;
            if (!relative.isEmpty() && QFileInfo(local).isFile())
                return QUrl::fromLocalFile(local);
        } else if (url.isLocalFile()) {
            const QString local = url.toLocalFile();
            if (local.startsWith(m_folder + QLatin1Char('/')) && !QFileInfo::exists(local))
                return QUrl(QStringLiteral("qrc:/") + local.mid(m_folder.size() + 1));
        }
        return url;
    }

private:
    QString m_folder;
};

// Gate 21's counting wrapper: stands between the list (and so TreeLines,
// which reads its view's model) and Folio's model. An identity proxy passes
// every row signal on unchanged; the model's own properties and calls are
// passed on by hand, and every call of get() is counted.
class CountingModel : public QIdentityProxyModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool expandable READ expandable NOTIFY expandableChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString query READ query NOTIFY queryChanged)
    Q_PROPERTY(QString path READ path NOTIFY pathChanged)

public:
    explicit CountingModel(FolderModel *source) : QIdentityProxyModel(source), m_source(source)
    {
        setSourceModel(source);
        connect(source, SIGNAL(countChanged()), this, SIGNAL(countChanged()));
        connect(source, SIGNAL(expandableChanged()), this, SIGNAL(expandableChanged()));
        connect(source, SIGNAL(loadingChanged()), this, SIGNAL(loadingChanged()));
        connect(source, SIGNAL(queryChanged()), this, SIGNAL(queryChanged()));
        connect(source, SIGNAL(pathChanged()), this, SIGNAL(pathChanged()));
    }

    // Calls of get() and of outline() since the gate last set them to 0, and
    // reads of either kind since the start.
    int gets = 0;
    int outlines = 0;
    int total = 0;

    int count() const { return rowCount(); }
    bool expandable() const { return m_source->expandable(); }
    bool loading() const { return m_source->property("loading").toBool(); }
    QString query() const { return m_source->property("query").toString(); }
    QString path() const { return m_source->property("path").toString(); }

    Q_INVOKABLE QVariantMap get(int row)
    {
        ++gets;
        ++total;
        return m_source->get(row);
    }

    // Revision 5's accessor, passed on to Folio's model when it has one
    // (found at run time, so the driver also builds against a FolderModel
    // without it). With FOLIO_GATES_OUTLINE_FALLBACK set, a model without
    // it is stood in for here, from its rows, without counting get(): the
    // self-test's stand-ins run on main's C++ that way.
    Q_INVOKABLE QVariantMap outline()
    {
        ++outlines;
        ++total;
        QVariantMap result;
        if (m_source->metaObject()->indexOfMethod("outline()") >= 0 &&
            QMetaObject::invokeMethod(m_source, "outline", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, result)))
            return result;
        if (qEnvironmentVariableIsSet("FOLIO_GATES_OUTLINE_FALLBACK")) {
            // Two roles read straight from the model, as a C++ accessor
            // would, not a whole get() map per row.
            const QHash<int, QByteArray> names = m_source->roleNames();
            const int depthRole = names.key("depth", -1);
            const int dirRole = names.key("isDir", -1);
            QList<int> depths;
            QList<bool> folders;
            for (int row = 0; row < m_source->rowCount(); ++row) {
                const QModelIndex at = m_source->index(row, 0);
                depths << m_source->data(at, depthRole).toInt();
                folders << m_source->data(at, dirRole).toBool();
            }
            result.insert(QStringLiteral("depths"), QVariant::fromValue(depths));
            result.insert(QStringLiteral("folders"), QVariant::fromValue(folders));
        }
        return result;
    }
    Q_INVOKABLE int parentRow(int row) const { return m_source->parentRow(row); }
    Q_INVOKABLE bool isExpanded(int row) const { return m_source->isExpanded(row); }
    Q_INVOKABLE int indexOfPath(const QString &path) const { return m_source->indexOfPath(path); }
    Q_INVOKABLE int indexOf(const QString &name) const { return m_source->indexOf(name); }
    Q_INVOKABLE void setExpanded(int row, bool expanded) { m_source->setExpanded(row, expanded); }
    Q_INVOKABLE bool isSelected(int row) const { return m_source->isSelected(row); }

    // A reset of the list's model, as a re-read that resets it would give.
    void simulateReset()
    {
        beginResetModel();
        endResetModel();
    }

signals:
    void countChanged();
    void expandableChanged();
    void loadingChanged();
    void queryChanged();
    void pathChanged();

private:
    FolderModel *m_source;
};

// Hands out one CountingModel per Folio model; the counting copy of
// FolderList.qml calls gatesCounting.wrap(backend.folder) for its model.
class CountingFactory : public QObject {
    Q_OBJECT

public:
    Q_INVOKABLE QObject *wrap(QObject *source)
    {
        auto *model = qobject_cast<FolderModel *>(source);
        if (!model)
            return source;
        if (!m_models.contains(model)) {
            auto *counting = new CountingModel(model);
            QQmlEngine::setObjectOwnership(counting, QQmlEngine::CppOwnership);
            m_models.insert(model, counting);
        }
        return m_models.value(model);
    }

    CountingModel *of(FolderModel *model) const { return m_models.value(model); }

private:
    QHash<FolderModel *, CountingModel *> m_models;
};

CountingFactory &counting()
{
    static CountingFactory factory;
    return factory;
}

// CPU time of the calling thread (the GUI thread, where QML runs), in ms.
double threadCpuMs()
{
    timespec now{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return now.tv_sec * 1000.0 + now.tv_nsec / 1e6;
}

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size())
        fail(QStringLiteral("could not write %1").arg(path));
}

// Runs fixture.sh (beside the driver's sources, found through
// FOLIO_GATES_DIR) to build one of its trees in `folder`.
void makeFixture(const QString &kind, const QString &folder)
{
    const QString script = qEnvironmentVariable("FOLIO_GATES_DIR") + QStringLiteral("/fixture.sh");
    if (!QFileInfo(script).isFile())
        fail(QStringLiteral("fixture.sh not found (set FOLIO_GATES_DIR to the gates folder)"));
    QProcess process;
    process.setProcessChannelMode(QProcess::ForwardedChannels);
    process.start(QStringLiteral("bash"), {script, kind, folder});
    if (!process.waitForFinished(60000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        fail(QStringLiteral("fixture.sh %1 failed").arg(kind));
}

// The two Omarchy themes the gates and screenshots use.
struct Palette {
    const char *name;
    bool light;
    const char *background, *foreground, *brightForeground, *accent, *magenta, *cyan, *orange;
};

const Palette darkPalette{"dark", false, "#1a1b26", "#a9b1d6", "#c0caf5", "#7aa2f7", "#bb9af7", "#7dcfff", "#ff9e64"};
const Palette lightPalette{"light", true, "#eff1f5", "#4c4f69", "#232536", "#1e66f5", "#8839ef", "#04a5e5", "#fe640b"};

QByteArray colorsToml(const Palette &p)
{
    QByteArray text;
    const auto line = [&](const char *key, const char *value) {
        text += QByteArray(key) + " = \"" + value + "\"\n";
    };
    line("background", p.background);
    line("foreground", p.foreground);
    line("bright_foreground", p.brightForeground);
    line("accent", p.accent);
    line("blue", p.accent);
    line("magenta", p.magenta);
    line("cyan", p.cyan);
    line("orange", p.orange);
    if (p.light)
        text += "mode = \"light\"\n";
    return text;
}

QVariant plain(const QVariant &value)
{
    if (value.metaType() == QMetaType::fromType<QJSValue>())
        return value.value<QJSValue>().toVariant();
    return value;
}

QObject *objectOf(const QVariant &value)
{
    if (value.metaType() == QMetaType::fromType<QJSValue>())
        return value.value<QJSValue>().toQObject();
    return value.value<QObject *>();
}

void collect(QQuickItem *root, const std::function<void(QQuickItem *)> &each)
{
    if (!root)
        return;
    each(root);
    const auto children = root->childItems();
    for (QQuickItem *child : children)
        collect(child, each);
}

QList<QQuickItem *> namedItems(QQuickItem *root, const QString &name, bool visibleOnly = false)
{
    QList<QQuickItem *> found;
    collect(root, [&](QQuickItem *item) {
        if (item->objectName() == name && (!visibleOnly || item->isVisible()))
            found << item;
    });
    return found;
}

double toNumber(const QVariant &value, const QString &what)
{
    bool ok = false;
    const double number = plain(value).toDouble(&ok);
    if (!value.isValid() || !ok)
        fail(QStringLiteral("%1 is not a number (%2)").arg(what, plain(value).toString()));
    return number;
}

QColor toColor(const QVariant &value, const QString &what)
{
    const QVariant v = plain(value);
    QColor color;
    if (v.metaType() == QMetaType::fromType<QColor>())
        color = v.value<QColor>();
    else if (v.canConvert<QString>())
        color = QColor::fromString(v.toString());
    if (!color.isValid())
        fail(QStringLiteral("%1 is not a colour (%2)").arg(what, v.toString()));
    return color;
}

QVariantList toList(const QVariant &value, const QString &what)
{
    const QVariant v = plain(value);
    if (!value.isValid())
        fail(QStringLiteral("%1 does not exist").arg(what));
    if (!v.canConvert<QVariantList>())
        fail(QStringLiteral("%1 is not an array (%2)").arg(what, QString::fromUtf8(v.typeName())));
    return v.toList();
}

QVariant field(const QVariant &object, const char *name, const QString &what)
{
    const QVariantMap map = plain(object).toMap();
    if (!map.contains(QLatin1String(name)))
        fail(QStringLiteral("%1 has no field %2").arg(what, QLatin1String(name)));
    return map.value(QLatin1String(name));
}

QString rgb(const QColor &c)
{
    return c.name(QColor::HexRgb);
}

bool sameRgb(const QColor &a, const QColor &b)
{
    return std::abs(a.red() - b.red()) <= 1 && std::abs(a.green() - b.green()) <= 1 && std::abs(a.blue() - b.blue()) <= 1;
}

// Folio's shared state, built as main.cpp's Session builds it, but with
// every folder it reads or writes inside the private root and no plugins.
class Folio {
public:
    explicit Folio(const QString &root)
        : m_root(root),
          m_home(root + QStringLiteral("/home")),
          m_translator(languageFile(root)),
          m_theme(themeFolder(root)),
          m_places(m_home),
          m_thumbnails(root + QStringLiteral("/thumbnails"), {root + QStringLiteral("/no-thumbnailers")}),
          m_plugins(QStringLiteral("en"), {root + QStringLiteral("/plugins")}, root + QStringLiteral("/plugins.conf")),
          m_hooked(hook(m_engine)),
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
        // Used only by the counting copy of FolderList.qml (gate 21).
        context->setContextProperty(QStringLiteral("gatesCounting"), &counting());
    }

    QString home() const { return m_home; }
    Theme &theme() { return m_theme; }

    // Writes the palette as the Omarchy theme and waits until Folio's Theme
    // has read it.
    void setTheme(const Palette &palette)
    {
        step(QStringLiteral("switching to the %1 theme").arg(QLatin1String(palette.name)));
        writeFile(m_root + QStringLiteral("/theme/theme/colors.toml"), colorsToml(palette));
        QMetaObject::invokeMethod(&m_theme, "reload");
        waitFor([&] { return m_theme.light() == palette.light && rgb(m_theme.accent()) == QLatin1String(palette.accent); },
                QStringLiteral("the %1 theme to be read").arg(QLatin1String(palette.name)));
        settle(300);
    }

    // Opens a main window on `folder` (relative to the home) and waits until
    // it is on screen, settled, and its pane lists that folder.
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
        window->requestActivate();
        // Offscreen windows may never report active; the wait is a courtesy.
        (void)QTest::qWaitForWindowActive(window, 2000);
        QObject *pane = objectOf(window->property("activePane"));
        if (!pane)
            fail(QStringLiteral("the window has no activePane"));
        waitForListing(pane, path);
        return window;
    }

    static Backend *backendOf(QObject *pane)
    {
        return qobject_cast<Backend *>(objectOf(pane->property("paneBackend")));
    }

    static void waitForListing(QObject *pane, const QString &path)
    {
        waitFor(
            [&] {
                Backend *backend = backendOf(pane);
                return backend && backend->path() == path && !backend->folder()->property("loading").toBool();
            },
            QStringLiteral("the pane to list %1").arg(path));
        settle(400);
    }

private:
    static QString languageFile(const QString &root)
    {
        const QString file = root + QStringLiteral("/language/language");
        writeFile(file, "en\n");
        return file;
    }

    // The dark theme is written before Theme reads it, so every window
    // starts in it.
    static QString themeFolder(const QString &root)
    {
        writeFile(root + QStringLiteral("/theme/theme/colors.toml"), colorsToml(darkPalette));
        return root + QStringLiteral("/theme");
    }

    static bool hook(QQmlEngine &engine)
    {
        const QString folder = qEnvironmentVariable("FOLIO_GATES_QML_DIR");
        if (folder.isEmpty())
            return false;
        // Lives as long as the process; the engine is never torn down.
        static QmlFolder interceptor(folder);
        engine.addUrlInterceptor(&interceptor);
        return true;
    }

    QString m_root;
    QString m_home;
    Translator m_translator;
    Theme m_theme;
    Places m_places;
    Thumbnails m_thumbnails;
    Plugins m_plugins;
    QQmlEngine m_engine;
    bool m_hooked;
    Windows m_windows;
};

// One row of the list as the model has it.
struct Row {
    QString name;
    QString path;
    int depth = 0;
    bool isDir = false;
    bool expanded = false;
};

struct Curve {
    int row = -1;
    double y = 0;
    double endX = 0;
};

// One entry of treeLines.guides (revision 2: no colour, width or opacity
// in the entry; those are in treeLines.spec).
struct Guide {
    int row = -1;
    int depth = 0;
    double x = 0, y0 = 0, y1 = 0;
    QList<Curve> curves;
};

struct Dot {
    double x = 0, y = 0, r = 0;
};

// The contract's sizes at a zoom.
struct Metrics {
    int picture, rowHeight, indent;
    explicit Metrics(double zoom)
        : picture(jsRound(20 * zoom)), rowHeight(jsRound(32 * zoom)), indent(jsRound(picture / 2.0) + 10)
    {
    }
    double left(int depth) const { return 4 + depth * indent; }
    double centre(int row) const { return row * rowHeight + rowHeight / 2.0; }
    // Revision 2: the start is tied to the row's centre (the open arrow),
    // the ends to the child's indent; at zoom 1 they are 22, 20, 22 and 37.
    double start(int row) const { return centre(row) + 6; }
    double folderEnd(int depth, bool graph) const { return left(depth) + indent + (graph ? 0 : 2); }
    double fileEnd(int depth) const { return left(depth) + indent + 17; }
};

// The guides the contract asks for, from the rows: one per open folder with
// at least one child, in row order.
QList<Guide> expectedGuides(const QList<Row> &rows, const QString &style, double zoom, double width, bool mirrored)
{
    const Metrics m(zoom);
    const bool graph = style == QLatin1String("graph");
    const auto flip = [&](double x) { return mirrored ? width - x : x; };
    QList<Guide> guides;
    for (int i = 0; i < rows.size(); ++i) {
        const Row &row = rows[i];
        if (!row.expanded || i + 1 >= rows.size() || rows[i + 1].depth <= row.depth)
            continue;
        Guide g;
        g.row = i;
        g.depth = row.depth;
        const double left = m.left(row.depth);
        g.x = flip(left + (graph ? 9.25 : 9.5));
        g.y0 = m.start(i);
        for (int j = i + 1; j < rows.size() && rows[j].depth > row.depth; ++j) {
            if (rows[j].depth != row.depth + 1)
                continue;
            Curve c;
            c.row = j;
            c.y = m.centre(j);
            c.endX = flip(rows[j].isDir ? m.folderEnd(row.depth, graph) : m.fileEnd(row.depth));
            g.curves << c;
        }
        g.y1 = m.centre(g.curves.last().row) - 8;
        guides << g;
    }
    return guides;
}

// One main window with the fixture's list, as the gates see it.
class Lines {
public:
    explicit Lines(QQuickWindow *window) : m_window(window) {}

    QQuickWindow *quick() const { return m_window; }
    QObject *pane() const { return objectOf(m_window->property("activePane")); }
    Backend *backend() const { return Folio::backendOf(pane()); }
    FolderModel *model() const { return backend()->folder(); }

    // The list view of the active pane: the visible ListView under it that
    // has FolderList's rowHeight.
    QQuickItem *list() const
    {
        auto *paneItem = qobject_cast<QQuickItem *>(pane());
        QList<QQuickItem *> found;
        collect(paneItem, [&](QQuickItem *item) {
            if (item->inherits("QQuickListView") && item->isVisible() && item->property("rowHeight").isValid())
                found << item;
        });
        if (found.size() != 1)
            fail(QStringLiteral("%1 visible list views in the pane, expected 1 (is the view mode list?)").arg(found.size()));
        return found.first();
    }

    QQuickItem *contentItem() const
    {
        return qobject_cast<QQuickItem *>(objectOf(list()->property("contentItem")));
    }

    // The one item named treeLines, which the contract puts in the list's
    // contentItem.
    QQuickItem *treeLines() const
    {
        const QList<QQuickItem *> all = namedItems(m_window->contentItem(), QStringLiteral("treeLines"));
        QQuickItem *content = contentItem();
        QList<QQuickItem *> inList;
        for (QQuickItem *item : all)
            if (item->parentItem() == content)
                inList << item;
        if (inList.isEmpty())
            fail(all.isEmpty() ? QStringLiteral("no item named treeLines (src/TreeLines.qml missing or not instantiated)")
                               : QStringLiteral("%1 items named treeLines, none in the list's contentItem").arg(all.size()));
        if (inList.size() > 1)
            fail(QStringLiteral("%1 items named treeLines in the list's contentItem, expected 1").arg(inList.size()));
        return inList.first();
    }

    void setStyle(const QString &style)
    {
        step(QStringLiteral("setting lineStyle to %1").arg(style));
        if (!m_window->property("lineStyle").isValid())
            fail(QStringLiteral("the window has no lineStyle property"));
        m_window->setProperty("lineStyle", style);
        settle(300);
    }

    QString style() const
    {
        const QVariant value = treeLines()->property("style");
        if (!value.isValid())
            fail(QStringLiteral("treeLines has no style property"));
        return value.toString();
    }

    double width() const { return list()->width(); }
    double zoom() const { return toNumber(list()->property("zoom"), QStringLiteral("the list's zoom")); }

    QList<Row> rows() const
    {
        QList<Row> rows;
        FolderModel *m = model();
        for (int r = 0; r < m->rowCount(); ++r) {
            const QVariantMap item = m->get(r);
            rows << Row{item.value(QStringLiteral("name")).toString(), item.value(QStringLiteral("path")).toString(),
                        item.value(QStringLiteral("depth")).toInt(), item.value(QStringLiteral("isDir")).toBool(),
                        item.value(QStringLiteral("expanded")).toBool()};
        }
        return rows;
    }

    int rowOf(const QString &relative) const
    {
        const QString path = QDir::cleanPath(backend()->path() + QLatin1Char('/') + relative);
        const int row = model()->indexOfPath(path);
        if (row < 0)
            fail(QStringLiteral("%1 is not listed").arg(relative));
        return row;
    }

    // Opens a folder in place and waits until its rows are listed.
    void expand(const QString &relative)
    {
        step(QStringLiteral("opening %1 in place").arg(relative));
        const int row = rowOf(relative);
        const int depth = rows().at(row).depth;
        model()->setExpanded(row, true);
        waitFor(
            [&] {
                const int r = rowOf(relative);
                return r + 1 < model()->rowCount() && model()->get(r + 1).value(QStringLiteral("depth")).toInt() > depth;
            },
            QStringLiteral("%1 to open in place").arg(relative));
    }

    void collapse(const QString &relative)
    {
        step(QStringLiteral("closing %1").arg(relative));
        model()->setExpanded(rowOf(relative), false);
        settle(300);
    }

    // The contract's fixture with docs, api, v1, schemas and guides open.
    void openFixture()
    {
        for (const char *folder : {"docs", "docs/api", "docs/api/v1", "docs/api/v1/schemas", "docs/guides"})
            expand(QLatin1String(folder));
        waitFor([&] { return model()->rowCount() == fixtureRows; },
                QStringLiteral("the fixture's %1 rows (%2 listed)").arg(fixtureRows).arg(model()->rowCount()));
        settle(400);
    }

    void setCursor(int row)
    {
        list()->setProperty("currentIndex", row);
        settle(300);
    }

    // Revision 7: what the drawing is made from, kept by TreeLines in its
    // one QtObject child (id built): the guides and the trace route that
    // layout() works from, and the dots' paths as they are drawn. Found by
    // its properties, as it has no object name.
    QObject *built() const
    {
        QQuickItem *item = treeLines();
        QList<QObject *> found;
        const auto children = item->children();
        for (QObject *child : children) {
            if (qobject_cast<QQuickItem *>(child))
                continue;
            const QMetaObject *meta = child->metaObject();
            bool all = true;
            for (const char *name : {"guides", "route", "dotPaths", "routeDotPath"})
                all = all && meta->indexOfProperty(name) >= 0;
            if (all)
                found << child;
        }
        if (found.size() != 1)
            fail(QStringLiteral("%1 child objects of treeLines hold guides, route, dotPaths and routeDotPath, expected 1 "
                                "(the QtObject the drawing is made from)").arg(found.size()));
        return found.first();
    }

    QList<Guide> guides() const
    {
        const QVariantList list = toList(built()->property("guides"), QStringLiteral("built.guides"));
        QList<Guide> guides;
        for (int i = 0; i < list.size(); ++i) {
            const QString what = QStringLiteral("guides[%1]").arg(i);
            const QVariant &v = list[i];
            Guide g;
            g.row = int(toNumber(field(v, "row", what), what + QStringLiteral(".row")));
            g.depth = int(toNumber(field(v, "depth", what), what + QStringLiteral(".depth")));
            g.x = toNumber(field(v, "x", what), what + QStringLiteral(".x"));
            g.y0 = toNumber(field(v, "y0", what), what + QStringLiteral(".y0"));
            g.y1 = toNumber(field(v, "y1", what), what + QStringLiteral(".y1"));
            const QVariantList curves = toList(field(v, "curves", what), what + QStringLiteral(".curves"));
            for (int k = 0; k < curves.size(); ++k) {
                const QString cw = what + QStringLiteral(".curves[%1]").arg(k);
                Curve c;
                c.row = int(toNumber(field(curves[k], "row", cw), cw + QStringLiteral(".row")));
                c.y = toNumber(field(curves[k], "y", cw), cw + QStringLiteral(".y"));
                c.endX = toNumber(field(curves[k], "endX", cw), cw + QStringLiteral(".endX"));
                g.curves << c;
            }
            guides << g;
        }
        return guides;
    }

    // The length of guides, read without converting every entry.
    int guideCount() const
    {
        const QVariant value = built()->property("guides");
        if (value.metaType() == QMetaType::fromType<QJSValue>())
            return value.value<QJSValue>().property(QStringLiteral("length")).toInt();
        return int(toList(value, QStringLiteral("built.guides")).size());
    }

    // The dots as drawn (revision 7): every circle in built.dotPaths (one
    // path per colour) and built.routeDotPath, each written as TreeLines'
    // circle() writes it, "M<x-r> <y>a<r> <r> 0 1 0 <2r> 0a<r> <r> 0 1 0
    // <-2r> 0Z". Anything else in those paths fails the gate.
    QList<Dot> dots() const
    {
        QObject *b = built();
        QStringList paths;
        for (const QVariant &v : toList(b->property("dotPaths"), QStringLiteral("built.dotPaths")))
            paths << v.toString();
        paths << b->property("routeDotPath").toString();
        static const QString number = QStringLiteral("(-?\\d+(?:\\.\\d+)?(?:e[-+]?\\d+)?)");
        static const QString gap = QStringLiteral("[\\s,]+");
        static const QRegularExpression circle(
            QStringLiteral("M\\s*") + number + gap + number + QStringLiteral("\\s*a\\s*") + number + gap + number + gap +
            QStringLiteral("0") + gap + QStringLiteral("1") + gap + QStringLiteral("0") + gap + number + gap + QStringLiteral("0\\s*a\\s*") +
            number + gap + number + gap + QStringLiteral("0") + gap + QStringLiteral("1") + gap + QStringLiteral("0") + gap + number + gap +
            QStringLiteral("0\\s*Z"));
        QList<Dot> dots;
        for (int i = 0; i < paths.size(); ++i) {
            const QString which = i < paths.size() - 1 ? QStringLiteral("built.dotPaths[%1]").arg(i) : QStringLiteral("built.routeDotPath");
            QString rest = paths[i];
            auto it = circle.globalMatch(paths[i]);
            while (it.hasNext()) {
                const QRegularExpressionMatch m = it.next();
                const double r = m.captured(3).toDouble();
                if (m.captured(4).toDouble() != r || std::abs(m.captured(5).toDouble() - 2 * r) > exact ||
                    m.captured(6).toDouble() != r || m.captured(7).toDouble() != r || std::abs(m.captured(8).toDouble() + 2 * r) > exact)
                    fail(QStringLiteral("%1 holds %2, not a circle as circle() writes it").arg(which, m.captured(0)));
                dots << Dot{m.captured(1).toDouble() + r, m.captured(2).toDouble(), r};
                rest.replace(m.captured(0), QString());
            }
            if (!rest.trimmed().isEmpty())
                fail(QStringLiteral("%1 holds \"%2\" besides circles").arg(which, rest.trimmed().left(80)));
        }
        return dots;
    }

    QList<int> route() const
    {
        QList<int> rows;
        for (const QVariant &v : toList(built()->property("route"), QStringLiteral("built.route")))
            rows << int(toNumber(v, QStringLiteral("a route entry")));
        return rows;
    }

    // Waits until the guides have `count` entries (they may be computed a
    // moment after the rows change).
    void waitForGuides(int count, const QString &when)
    {
        if (!QTest::qWaitFor([&] { return guideCount() == count; }, 3000))
            fail(QStringLiteral("%1 guides %2, expected %3").arg(guideCount()).arg(when).arg(count));
    }

    QString nameOf(int row) const
    {
        const QList<Row> all = rows();
        return row >= 0 && row < all.size() ? all[row].name : QStringLiteral("row %1").arg(row);
    }

private:
    QQuickWindow *m_window;
};

// Compares the guides' positions with the contract's.
void expectGeometry(Lines &lines, const QString &style, const QString &when, bool mirrored = false)
{
    const QList<Row> rows = lines.rows();
    const QList<Guide> want = expectedGuides(rows, style, lines.zoom(), lines.width(), mirrored);
    const QList<Guide> got = lines.guides();
    if (got.size() != want.size())
        fail(QStringLiteral("%1 guides %2, expected %3").arg(got.size()).arg(when).arg(want.size()));
    const auto off = [](double a, double b) { return std::abs(a - b) > within; };
    for (int i = 0; i < want.size(); ++i) {
        const Guide &w = want[i];
        const Guide &g = got[i];
        const QString which = QStringLiteral("%1: guide of %2 (row %3)").arg(when, rows[w.row].name).arg(w.row);
        if (g.row != w.row)
            fail(QStringLiteral("%1 has row %2, expected %3").arg(which).arg(g.row).arg(w.row));
        if (g.depth != w.depth)
            fail(QStringLiteral("%1 has depth %2, expected %3").arg(which).arg(g.depth).arg(w.depth));
        if (off(g.x, w.x))
            fail(QStringLiteral("%1 x %2, expected %3").arg(which, num(g.x), num(w.x)));
        if (off(g.y0, w.y0))
            fail(QStringLiteral("%1 y0 %2, expected %3").arg(which, num(g.y0), num(w.y0)));
        if (off(g.y1, w.y1))
            fail(QStringLiteral("%1 y1 %2, expected %3").arg(which, num(g.y1), num(w.y1)));
        if (g.curves.size() != w.curves.size())
            fail(QStringLiteral("%1 has %2 curves, expected %3").arg(which).arg(g.curves.size()).arg(w.curves.size()));
        for (int k = 0; k < w.curves.size(); ++k) {
            const Curve &wc = w.curves[k];
            const Curve &gc = g.curves[k];
            const QString cw = QStringLiteral("%1, curve to %2 (row %3)").arg(which, rows[wc.row].name).arg(wc.row);
            if (gc.row != wc.row)
                fail(QStringLiteral("%1 has row %2, expected %3").arg(cw).arg(gc.row).arg(wc.row));
            if (off(gc.y, wc.y))
                fail(QStringLiteral("%1 y %2, expected %3").arg(cw, num(gc.y), num(wc.y)));
            if (off(gc.endX, wc.endX))
                fail(QStringLiteral("%1 endX %2, expected %3").arg(cw, num(gc.endX), num(wc.endX)));
            if (mirrored && !(gc.endX < g.x))
                fail(QStringLiteral("%1 turns right (endX %2 past the guide at %3), expected left").arg(cw, num(gc.endX), num(g.x)));
        }
    }
}

int totalCurves(const QList<Guide> &guides)
{
    int total = 0;
    for (const Guide &g : guides)
        total += int(g.curves.size());
    return total;
}

const QStringList styles{QStringLiteral("quiet"), QStringLiteral("graph"), QStringLiteral("trace")};

// Writes folio.conf as a real one looks after the first launch (revision 4):
// it always holds viewMode and zoom, so those are added (list, 1) when
// `general` does not give them.
void writeSettings(const QString &home, QByteArray general)
{
    const auto has = [&](const QByteArray &key) {
        return general.startsWith(key + '=') || general.contains('\n' + key + '=');
    };
    if (!has("viewMode"))
        general.prepend("viewMode=list\n");
    if (!has("zoom"))
        general.append("zoom=1\n");
    writeFile(home + QStringLiteral("/.config/folio/folio.conf"), "[General]\n" + general);
}

QQuickWindow *openFixtureWindow(Folio &folio, const QByteArray &settings = {})
{
    writeSettings(folio.home(), settings);
    makeFixture(QStringLiteral("tree"), folio.home());
    return folio.open(QStringLiteral("project"));
}

// The list's own rectangle in the window, for cropping screenshots.
QRect listRect(Lines &lines)
{
    QQuickItem *list = lines.list();
    return QRectF(list->mapToScene(QPointF(0, 0)), QSizeF(list->width(), list->height())).toAlignedRect();
}

QImage grab(Lines &lines)
{
    settle(500);
    QImage image = lines.quick()->grabWindow();
    if (image.isNull())
        fail(QStringLiteral("the window could not be grabbed"));
    return image.convertToFormat(QImage::Format_RGB32);
}

void save(const QImage &image, const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (!image.save(path, "PNG"))
        fail(QStringLiteral("could not write %1").arg(path));
}

// The fixture window for gate 4 and its main counterpart: list view, dark
// theme, fixture open, cursor on migrating.md; no style is set.
Lines fixtureForShot(Folio &folio)
{
    Lines lines(openFixtureWindow(folio));
    lines.openFixture();
    lines.setCursor(lines.rowOf(QStringLiteral("docs/guides/migrating.md")));
    return lines;
}

// 4: style off by default: nothing computed, and the list draws as on main
// (the comparison is made by gates.sh with `compare`).
void offGate(Folio &folio, const QString &png)
{
    if (png.isEmpty())
        fail(QStringLiteral("gate 4 needs the PNG to write"));
    Lines lines = fixtureForShot(folio);
    step(QStringLiteral("reading treeLines with the default style"));
    const QString style = lines.style();
    if (style != QLatin1String("off"))
        fail(QStringLiteral("treeLines.style is %1 by default, expected off").arg(style));
    const QVariant stored = lines.quick()->property("lineStyle");
    if (!stored.isValid())
        fail(QStringLiteral("the window has no lineStyle property"));
    if (stored.toString() != QLatin1String("off"))
        fail(QStringLiteral("the window's lineStyle is %1 by default, expected off").arg(stored.toString()));
    const int guides = int(lines.guides().size());
    const int dots = int(lines.dots().size());
    if (guides != 0 || dots != 0)
        fail(QStringLiteral("%1 guides and %2 dots in style off, expected none").arg(guides).arg(dots));
    save(grab(lines).copy(listRect(lines)), png);
}

void listShot(Folio &folio, const QString &png)
{
    Lines lines = fixtureForShot(folio);
    save(grab(lines).copy(listRect(lines)), png);
}

// 5: the guides' positions in each style.
void geometryGate(Folio &folio)
{
    Lines lines(openFixtureWindow(folio));
    lines.openFixture();
    for (const QString &style : styles) {
        lines.setStyle(style);
        const QString seen = lines.style();
        if (seen != style)
            fail(QStringLiteral("treeLines.style is %1 with lineStyle %2").arg(seen, style));
        lines.waitForGuides(fixtureGuides, QStringLiteral("in style %1").arg(style));
        const int curves = totalCurves(lines.guides());
        if (curves != fixtureCurves)
            fail(QStringLiteral("%1 curves in style %2, expected %3").arg(curves).arg(style).arg(fixtureCurves));
        expectGeometry(lines, style, QStringLiteral("style %1").arg(style));
    }
}

// The contract's table, for one style in one theme.
struct Look {
    double width;
    double opacity;
};

Look lookOf(const QString &style, bool light)
{
    if (style == QLatin1String("quiet"))
        return {1, light ? 0.30 : 0.18};
    if (style == QLatin1String("graph"))
        return {1.5, 0.78};
    return {1, light ? 0.24 : 0.14};
}

QColor depthColor(Theme &theme, int depth)
{
    switch (depth % 4) {
    case 0: return theme.accent();
    case 1: return theme.magenta();
    case 2: return theme.cyan();
    default: return theme.orange();
    }
}

// Revision 2: the table is read from treeLines.spec, the object the
// drawing binds to: { width, opacity, colors: [..], route: {..} or null }.
void expectLook(Lines &lines, Theme &theme, const QString &style, const QString &themeName)
{
    const Look look = lookOf(style, theme.light());
    const QString which = QStringLiteral("spec in style %1, %2 theme").arg(style, themeName);
    const QVariant spec = plain(lines.treeLines()->property("spec"));
    if (!spec.isValid() || spec.isNull())
        fail(QStringLiteral("treeLines has no spec property"));
    const double width = toNumber(field(spec, "width", which), which + QStringLiteral(".width"));
    const double opacity = toNumber(field(spec, "opacity", which), which + QStringLiteral(".opacity"));
    if (std::abs(width - look.width) > exact)
        fail(QStringLiteral("%1: width %2, expected %3").arg(which, num(width), num(look.width)));
    if (std::abs(opacity - look.opacity) > exact)
        fail(QStringLiteral("%1: opacity %2, expected %3").arg(which, num(opacity), num(look.opacity)));
    const QVariantList colors = toList(field(spec, "colors", which), which + QStringLiteral(".colors"));
    QList<QColor> want;
    if (style == QLatin1String("graph"))
        for (int d = 0; d < 4; ++d)
            want << depthColor(theme, d);
    else
        want << theme.foreground();
    if (colors.size() != want.size())
        fail(QStringLiteral("%1: %2 colours, expected %3").arg(which).arg(colors.size()).arg(want.size()));
    for (int i = 0; i < want.size(); ++i) {
        const QColor seen = toColor(colors[i], which + QStringLiteral(".colors[%1]").arg(i));
        if (!sameRgb(seen, want[i]))
            fail(QStringLiteral("%1: colors[%2] %3, expected %4").arg(which).arg(i).arg(rgb(seen), rgb(want[i])));
    }
    const QVariant route = plain(field(spec, "route", which));
    if (style != QLatin1String("trace")) {
        if (route.isValid() && !route.isNull())
            fail(QStringLiteral("%1: route is set, expected null").arg(which));
        return;
    }
    if (!route.isValid() || route.isNull())
        fail(QStringLiteral("%1: route is null, expected { width, opacity, color }").arg(which));
    const QString rw = which + QStringLiteral(".route");
    const double routeWidth = toNumber(field(route, "width", rw), rw + QStringLiteral(".width"));
    const double routeOpacity = toNumber(field(route, "opacity", rw), rw + QStringLiteral(".opacity"));
    const QColor routeColor = toColor(field(route, "color", rw), rw + QStringLiteral(".color"));
    if (!sameRgb(routeColor, theme.accent()) || std::abs(routeWidth - 1.5) > exact || std::abs(routeOpacity - 0.9) > exact)
        fail(QStringLiteral("%1: colour %2, width %3, opacity %4; expected %5, 1.5, 0.9")
                 .arg(rw, rgb(routeColor), num(routeWidth), num(routeOpacity), rgb(theme.accent())));
}

// 6: width, colour and opacity of each style, dark and light.
void lookGate(Folio &folio)
{
    Lines lines(openFixtureWindow(folio));
    lines.openFixture();
    lines.setCursor(lines.rowOf(QStringLiteral("docs/guides/migrating.md")));
    for (const Palette *palette : {&darkPalette, &lightPalette}) {
        folio.setTheme(*palette);
        if (folio.theme().light() != palette->light)
            fail(QStringLiteral("theme.light did not follow the %1 theme").arg(QLatin1String(palette->name)));
        for (const QString &style : styles) {
            lines.setStyle(style);
            lines.waitForGuides(fixtureGuides, QStringLiteral("in style %1").arg(style));
            expectLook(lines, folio.theme(), style, QLatin1String(palette->name));
        }
    }
}

QString dotsText(QList<Dot> dots)
{
    std::sort(dots.begin(), dots.end(), [](const Dot &a, const Dot &b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
    QStringList parts;
    for (const Dot &d : dots)
        parts << QStringLiteral("(%1, %2) r %3").arg(num(d.x), num(d.y), num(d.r));
    return parts.join(QStringLiteral(", "));
}

// Every wanted dot is matched by one seen dot within the tolerance.
void expectDots(const QList<Dot> &got, const QList<Dot> &want, const QString &when)
{
    if (got.size() != want.size())
        fail(QStringLiteral("%1 dots %2, expected %3 (seen: %4)").arg(got.size()).arg(when).arg(want.size()).arg(dotsText(got)));
    QList<Dot> left = got;
    for (const Dot &w : want) {
        const auto match = std::find_if(left.begin(), left.end(), [&](const Dot &g) {
            return std::abs(g.x - w.x) <= within && std::abs(g.y - w.y) <= within && std::abs(g.r - w.r) <= exact;
        });
        if (match == left.end())
            fail(QStringLiteral("no dot at (%1, %2) r %3 %4 (seen: %5)").arg(num(w.x), num(w.y), num(w.r), when, dotsText(got)));
        left.erase(match);
    }
}

// 7: the dots of each style.
void dotsGate(Folio &folio)
{
    Lines lines(openFixtureWindow(folio));
    lines.openFixture();
    const int cursor = lines.rowOf(QStringLiteral("docs/guides/migrating.md"));
    lines.setCursor(cursor);
    const Metrics m(lines.zoom());

    lines.setStyle(QStringLiteral("quiet"));
    lines.waitForGuides(fixtureGuides, QStringLiteral("in style quiet"));
    expectDots(lines.dots(), {}, QStringLiteral("in style quiet"));

    lines.setStyle(QStringLiteral("graph"));
    lines.waitForGuides(fixtureGuides, QStringLiteral("in style graph"));
    const QList<Row> rows = lines.rows();
    QList<Dot> want;
    for (const Guide &g : expectedGuides(rows, QStringLiteral("graph"), lines.zoom(), lines.width(), false)) {
        want << Dot{g.x, g.y0 + 2.5, 2};
        for (const Curve &c : g.curves)
            if (rows[c.row].isDir)
                want << Dot{c.endX, c.y, 2.5};
    }
    if (want.size() != 10)
        fail(QStringLiteral("the fixture gives %1 graph dots, expected 10 (the fixture is not as the contract)").arg(want.size()));
    expectDots(lines.dots(), want, QStringLiteral("in style graph"));

    lines.setStyle(QStringLiteral("trace"));
    lines.waitForGuides(fixtureGuides, QStringLiteral("in style trace"));
    const int parent = lines.rowOf(QStringLiteral("docs/guides"));
    const double end = m.fileEnd(rows[parent].depth);
    expectDots(lines.dots(), {Dot{end, m.centre(cursor), 2.5}}, QStringLiteral("in style trace with the cursor on migrating.md"));
}

QString rowsText(Lines &lines, const QList<int> &rows)
{
    QStringList names;
    for (int r : rows)
        names << QStringLiteral("%1 (%2)").arg(lines.nameOf(r)).arg(r);
    return names.isEmpty() ? QStringLiteral("empty") : names.join(QStringLiteral(", "));
}

void expectRoute(Lines &lines, const QList<int> &want, const QString &when)
{
    QList<int> got;
    for (int i = 0; i < 30; ++i) {
        got = lines.route();
        if (got == want)
            return;
        settle(100);
    }
    fail(QStringLiteral("route %1 is %2, expected %3").arg(when, rowsText(lines, got), rowsText(lines, want)));
}

// 8: trace's route follows the cursor.
void routeGate(Folio &folio)
{
    Lines lines(openFixtureWindow(folio));
    lines.openFixture();
    lines.setStyle(QStringLiteral("trace"));
    const auto row = [&](const char *path) { return lines.rowOf(QLatin1String(path)); };

    lines.setCursor(row("docs/guides/migrating.md"));
    expectRoute(lines, {row("docs"), row("docs/guides"), row("docs/guides/migrating.md")},
                QStringLiteral("with the cursor on migrating.md"));
    lines.setCursor(row("docs/api/v1/schemas/order.json"));
    expectRoute(lines,
                {row("docs"), row("docs/api"), row("docs/api/v1"), row("docs/api/v1/schemas"),
                 row("docs/api/v1/schemas/order.json")},
                QStringLiteral("with the cursor on order.json"));
    lines.setCursor(row("README.md"));
    expectRoute(lines, {}, QStringLiteral("with the cursor on the top-level README.md"));
    lines.setCursor(row("docs"));
    expectRoute(lines, {}, QStringLiteral("with the cursor on the top-level docs"));
    lines.setCursor(-1);
    expectRoute(lines, {}, QStringLiteral("with no cursor"));
}

// 9: closing v1 takes its guide (and that of schemas, inside it) away and
// moves the guides below up by the rows it hid; opening v1 and schemas
// again gives the first guides back.
void collapseGate(Folio &folio)
{
    Lines lines(openFixtureWindow(folio));
    lines.openFixture();
    lines.setStyle(QStringLiteral("quiet"));
    lines.waitForGuides(fixtureGuides, QStringLiteral("before closing v1"));
    const QList<Guide> before = lines.guides();
    const int rowsBefore = lines.model()->rowCount();
    const int guidesRow = lines.rowOf(QStringLiteral("docs/guides"));
    const double rowHeight = Metrics(lines.zoom()).rowHeight;

    lines.collapse(QStringLiteral("docs/api/v1"));
    const int hidden = rowsBefore - lines.model()->rowCount();
    if (hidden != 5)
        fail(QStringLiteral("closing v1 hid %1 rows, expected 5 (the fixture is not as the contract)").arg(hidden));
    lines.waitForGuides(fixtureGuides - 2, QStringLiteral("after closing v1 (v1 and schemas inside it)"));
    expectGeometry(lines, QStringLiteral("quiet"), QStringLiteral("after closing v1"));
    const QList<Guide> after = lines.guides();
    const auto guideAt = [](const QList<Guide> &guides, int row) {
        const auto it = std::find_if(guides.begin(), guides.end(), [&](const Guide &g) { return g.row == row; });
        if (it == guides.end())
            fail(QStringLiteral("no guide for row %1").arg(row));
        return *it;
    };
    const Guide was = guideAt(before, guidesRow);
    const Guide now = guideAt(after, guidesRow - hidden);
    const double moved = was.y0 - now.y0;
    if (std::abs(moved - hidden * rowHeight) > within || std::abs((was.y1 - now.y1) - hidden * rowHeight) > within)
        fail(QStringLiteral("the guide of guides moved up %1 px (y1 %2 px), expected %3")
                 .arg(num(moved), num(was.y1 - now.y1), num(hidden * rowHeight)));

    lines.expand(QStringLiteral("docs/api/v1"));
    lines.expand(QStringLiteral("docs/api/v1/schemas"));
    lines.waitForGuides(fixtureGuides, QStringLiteral("after opening v1 and schemas again"));
    expectGeometry(lines, QStringLiteral("quiet"), QStringLiteral("after opening v1 again"));
    const QList<Guide> again = lines.guides();
    for (int i = 0; i < before.size(); ++i)
        if (again[i].row != before[i].row || std::abs(again[i].y0 - before[i].y0) > within ||
            std::abs(again[i].y1 - before[i].y1) > within)
            fail(QStringLiteral("guide %1 after opening v1 again differs from before closing it").arg(i));
}

QPoint centreOf(QQuickItem *item)
{
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
}

QQuickItem *visibleOne(QQuickWindow *window, const QString &name)
{
    const QList<QQuickItem *> shown = namedItems(window->contentItem(), name, true);
    if (shown.size() != 1)
        fail(QStringLiteral("%1 visible items named %2, expected 1").arg(shown.size()).arg(name));
    return shown.first();
}

// Opens the lines menu with a click on its button and waits for an entry.
void openLinesMenu(QQuickWindow *window)
{
    step(QStringLiteral("clicking linesButton"));
    QQuickItem *button = visibleOne(window, QStringLiteral("linesButton"));
    QTest::mouseClick(window, Qt::LeftButton, {}, centreOf(button));
    if (!QTest::qWaitFor([&] { return !namedItems(window->contentItem(), QStringLiteral("lines-off"), true).isEmpty(); }, 3000))
        fail(QStringLiteral("clicking linesButton showed no visible lines-off entry (linesMenu did not open)"));
    settle(400);
}

void closeMenu(QQuickWindow *window)
{
    QTest::keyClick(window, Qt::Key_Escape);
    settle(400);
}

// 10: the menu sets lineStyle, checks its entry, a second window follows,
// and the setting is stored.
void menuGate(Folio &folio)
{
    QQuickWindow *window = openFixtureWindow(folio);
    Lines lines(window);
    lines.openFixture();
    const QString conf = folio.home() + QStringLiteral("/.config/folio/folio.conf");
    const QStringList order{QStringLiteral("quiet"), QStringLiteral("graph"), QStringLiteral("off"), QStringLiteral("trace")};
    for (const QString &style : order) {
        openLinesMenu(window);
        step(QStringLiteral("choosing lines-%1").arg(style));
        QTest::mouseClick(window, Qt::LeftButton, {}, centreOf(visibleOne(window, QStringLiteral("lines-") + style)));
        settle(400);
        const QString seen = window->property("lineStyle").toString();
        if (seen != style)
            fail(QStringLiteral("lineStyle is %1 after choosing lines-%2").arg(seen.isEmpty() ? QStringLiteral("unset") : seen, style));
        openLinesMenu(window);
        for (const QString &other : {QStringLiteral("off"), QStringLiteral("quiet"), QStringLiteral("graph"), QStringLiteral("trace")}) {
            const bool checked = visibleOne(window, QStringLiteral("lines-") + other)->property("checked").toBool();
            if (checked != (other == style))
                fail(QStringLiteral("lines-%1 is %2 after choosing lines-%3").arg(other, checked ? QStringLiteral("checked") : QStringLiteral("not checked"), style));
        }
        closeMenu(window);
    }
    step(QStringLiteral("waiting for the setting file"));
    if (!QTest::qWaitFor([&] {
            QFile file(conf);
            return file.open(QIODevice::ReadOnly) && file.readAll().contains("lineStyle=trace");
        }, waitLimitMs))
        fail(QStringLiteral("folio.conf does not hold lineStyle=trace after choosing it"));
    QQuickWindow *second = folio.open(QStringLiteral("project"));
    const QString secondStyle = second->property("lineStyle").toString();
    if (secondStyle != QLatin1String("trace"))
        fail(QStringLiteral("a second window opened after has lineStyle %1, expected trace").arg(secondStyle));
}

// Gate 26's helpers: the active pane's keyboard state, a press of L or
// Shift+L, and the lines menu's checked entry.
bool paneHasKeys(QObject *pane)
{
    return pane->property("keys").toBool();
}

// Gives the pane the keyboard back if a menu or a field left it elsewhere,
// as a click on the files would.
void focusPane(QQuickWindow *window, QObject *pane)
{
    if (!paneHasKeys(pane))
        QMetaObject::invokeMethod(pane, "forceActiveFocus");
    window->requestActivate();
    waitFor([&] { return paneHasKeys(pane); }, QStringLiteral("the pane to have the keys"));
}

void pressL(QQuickWindow *window, bool shift)
{
    QTest::keyClick(window, Qt::Key_L, shift ? Qt::ShiftModifier : Qt::NoModifier);
    settle(250);
}

QString lineStyleOf(QQuickWindow *window)
{
    return window->property("lineStyle").toString();
}

// Only `style`'s entry of the lines menu is checked.
void expectChecked(QQuickWindow *window, QObject *pane, const QString &style, const QString &when)
{
    openLinesMenu(window);
    for (const QString &entry : {QStringLiteral("off"), QStringLiteral("quiet"), QStringLiteral("graph"), QStringLiteral("trace")}) {
        const bool checked = visibleOne(window, QStringLiteral("lines-") + entry)->property("checked").toBool();
        if (checked != (entry == style))
            fail(QStringLiteral("lines-%1 is %2 %3, expected only lines-%4 checked")
                     .arg(entry, checked ? QStringLiteral("checked") : QStringLiteral("not checked"), when, style));
    }
    closeMenu(window);
    focusPane(window, pane);
}

// S steps the sort order (and Shift+S back): proof that a letter key
// reaches the pane's shortcuts, so that L doing nothing means something.
void expectSortKeys(QQuickWindow *window, Lines &lines, const QString &where)
{
    FolderModel *model = lines.model();
    const QString before = model->property("sortMode").toString();
    QTest::keyClick(window, Qt::Key_S);
    if (!QTest::qWaitFor([&] { return model->property("sortMode").toString() != before; }, 2000))
        fail(QStringLiteral("S did not change the sort order %1 (the keys do not reach the pane, so the L checks would prove nothing)").arg(where));
    QTest::keyClick(window, Qt::Key_S, Qt::ShiftModifier);
    waitFor([&] { return model->property("sortMode").toString() == before; }, QStringLiteral("Shift+S to give the sort order back %1").arg(where));
    settle(250);
}

// While a text field has the keyboard, L and Shift+L type and change
// nothing. `open` puts the keyboard in the field.
void expectTyping(QQuickWindow *window, QObject *pane, const QString &field, const std::function<void()> &open)
{
    const QString style = lineStyleOf(window);
    open();
    QQuickItem *input = nullptr;
    if (!QTest::qWaitFor([&] {
            input = window->activeFocusItem();
            return input && input->inherits("QQuickTextInput");
        }, 2000))
        fail(QStringLiteral("the %1 did not take the keyboard").arg(field));
    settle(250);
    if (lineStyleOf(window) != style)
        fail(QStringLiteral("opening the %1 changed lineStyle from %2 to %3").arg(field, style, lineStyleOf(window)));
    const QString text = input->property("text").toString();
    pressL(window, false);
    pressL(window, true);
    const QString typed = input->property("text").toString();
    if (lineStyleOf(window) != style)
        fail(QStringLiteral("L and Shift+L in the %1 changed lineStyle from %2 to %3, expected no change").arg(field, style, lineStyleOf(window)));
    // The field may have selected its text (Ctrl+L does), so the letters
    // replace it; either way they end the text. QTest types Shift+L as a
    // key with the Shift modifier, whose text may come out in either case.
    if (typed == text || !typed.toLower().endsWith(QStringLiteral("ll")))
        fail(QStringLiteral("L and Shift+L in the %1 turned its text \"%2\" into \"%3\", expected it to end with the two letters")
                 .arg(field, text, typed));
    for (int i = 0; i < 3 && !paneHasKeys(pane); ++i) {
        QTest::keyClick(window, Qt::Key_Escape);
        settle(300);
    }
    focusPane(window, pane);
}

// 26 (revision 6): L and Shift+L step the line style in the list view.
void keysGate(Folio &folio)
{
    QQuickWindow *window = openFixtureWindow(folio);
    Lines lines(window);
    lines.openFixture();
    QObject *pane = lines.pane();
    focusPane(window, pane);
    expectSortKeys(window, lines, QStringLiteral("in the list"));
    if (lineStyleOf(window) != QLatin1String("off"))
        fail(QStringLiteral("lineStyle is %1 on a first launch, expected off").arg(lineStyleOf(window)));

    const auto stepTo = [&](bool shift, const QString &want, const QString &from) {
        pressL(window, shift);
        const QString key = shift ? QStringLiteral("Shift+L") : QStringLiteral("L");
        if (!QTest::qWaitFor([&] { return lineStyleOf(window) == want; }, 2000))
            fail(QStringLiteral("%1 from %2 in the list gave lineStyle %3, expected %4").arg(key, from, lineStyleOf(window), want));
        if (lines.style() != want)
            fail(QStringLiteral("%1 from %2: treeLines.style is %3, expected %4").arg(key, from, lines.style(), want));
    };
    const QStringList order{QStringLiteral("off"), QStringLiteral("quiet"), QStringLiteral("graph"), QStringLiteral("trace")};
    for (int i = 1; i <= 4; ++i) {
        stepTo(false, order[i % 4], order[i - 1]);
        if (order[i % 4] == QLatin1String("graph"))
            expectChecked(window, pane, QStringLiteral("graph"), QStringLiteral("after L to graph"));
    }
    for (int i = 4; i >= 1; --i) {
        stepTo(true, order[i - 1], order[i % 4]);
        if (order[i - 1] == QLatin1String("trace"))
            expectChecked(window, pane, QStringLiteral("trace"), QStringLiteral("after Shift+L to trace"));
    }

    // A stored style this build does not know steps as off.
    window->setProperty("lineStyle", QStringLiteral("banana"));
    settle(250);
    stepTo(false, QStringLiteral("quiet"), QStringLiteral("banana"));
    window->setProperty("lineStyle", QStringLiteral("banana"));
    settle(250);
    stepTo(true, QStringLiteral("trace"), QStringLiteral("banana"));
    expectChecked(window, pane, QStringLiteral("trace"), QStringLiteral("after Shift+L from banana"));

    // Columns and grid: the keys reach the pane (S works) and L does nothing.
    lines.setStyle(QStringLiteral("quiet"));
    for (const QString &mode : {QStringLiteral("grid"), QStringLiteral("columns")}) {
        step(QStringLiteral("switching to %1").arg(mode));
        window->setProperty("viewMode", mode);
        waitFor([&] { return pane->property("viewMode").toString() == mode; }, QStringLiteral("the pane's viewMode %1").arg(mode));
        settle(500);
        focusPane(window, pane);
        expectSortKeys(window, lines, QStringLiteral("in the %1 view").arg(mode));
        pressL(window, false);
        pressL(window, true);
        pressL(window, false);
        if (lineStyleOf(window) != QLatin1String("quiet"))
            fail(QStringLiteral("L and Shift+L in the %1 view changed lineStyle from quiet to %2, expected no change").arg(mode, lineStyleOf(window)));
    }
    step(QStringLiteral("switching back to the list"));
    window->setProperty("viewMode", QStringLiteral("list"));
    waitFor([&] { return pane->property("viewMode").toString() == QLatin1String("list"); }, QStringLiteral("the pane's viewMode list"));
    settle(500);
    focusPane(window, pane);

    // Typing: Ctrl+L still opens the path field, and in it (and in the
    // search field, opened with /) L and Shift+L are letters.
    step(QStringLiteral("typing in the path field"));
    expectTyping(window, pane, QStringLiteral("path field (Ctrl+L)"), [&] { QTest::keyClick(window, Qt::Key_L, Qt::ControlModifier); });
    step(QStringLiteral("typing in the search field"));
    expectTyping(window, pane, QStringLiteral("search field (/)"), [&] { QTest::keyClick(window, Qt::Key_Slash); });
    // And afterwards L works again in the list.
    stepTo(false, QStringLiteral("graph"), QStringLiteral("quiet, after typing"));
}

// 28 (revision 6): a folder opened just before a re-read gets its lines.
// With a style on and nothing open, docs is opened and the folder reloaded
// in the same turn of the event loop: the reload starts a new generation,
// so the opening's own listing is dropped, and docs' items arrive with the
// reload's listing (which lists the folders marked open) instead of as rows
// inserted under docs. Once the rows have settled the guides are the
// contract's, in each style.
void reloadGate(Folio &folio)
{
    Lines lines(openFixtureWindow(folio));
    FolderModel *model = lines.model();
    const int closedRows = model->rowCount();
    for (const QString &style : styles) {
        if (lines.rows().at(lines.rowOf(QStringLiteral("docs"))).expanded) {
            lines.collapse(QStringLiteral("docs"));
            waitFor([&] { return model->rowCount() == closedRows; }, QStringLiteral("docs to close"));
        }
        lines.setStyle(style);
        lines.waitForGuides(0, QStringLiteral("in style %1 with nothing open").arg(style));
        settle(500);

        step(QStringLiteral("opening docs and reloading at once, style %1").arg(style));
        model->setExpanded(lines.rowOf(QStringLiteral("docs")), true);
        if (!QMetaObject::invokeMethod(model, "reload"))
            fail(QStringLiteral("FolderModel has no reload()"));
        // docs holds api, guides and README.md.
        waitFor([&] { return model->rowCount() == closedRows + 3 && !model->property("loading").toBool(); },
                QStringLiteral("docs' 3 items after the reload (%1 rows listed)").arg(model->rowCount()));
        settle(800);
        lines.waitForGuides(1, QStringLiteral("in style %1 after docs was opened and the folder reloaded at once "
                                              "(expected the guide of docs)").arg(style));
        expectGeometry(lines, style, QStringLiteral("style %1, docs opened just before a reload").arg(style));
    }
}

// 11: the button shows only in the list view.
void buttonGate(Folio &folio)
{
    QQuickWindow *window = openFixtureWindow(folio);
    const auto shown = [&] { return namedItems(window->contentItem(), QStringLiteral("linesButton"), true).size(); };
    if (shown() != 1)
        fail(QStringLiteral("%1 visible linesButton in the list view, expected 1").arg(shown()));
    for (const QString &mode : {QStringLiteral("grid"), QStringLiteral("columns")}) {
        step(QStringLiteral("switching to %1").arg(mode));
        window->setProperty("viewMode", mode);
        QObject *pane = objectOf(window->property("activePane"));
        waitFor([&] { return pane->property("viewMode").toString() == mode; }, QStringLiteral("the pane's viewMode %1").arg(mode));
        settle(500);
        if (shown() != 0)
            fail(QStringLiteral("%1 visible linesButton in the %2 view, expected 0").arg(shown()).arg(mode));
    }
    window->setProperty("viewMode", QStringLiteral("list"));
    settle(500);
    if (shown() != 1)
        fail(QStringLiteral("%1 visible linesButton back in the list view, expected 1").arg(shown()));
}

// 12: the geometry at zoom 1.5.
void zoomGate(Folio &folio)
{
    Lines lines(openFixtureWindow(folio, "zoom=1.5\n"));
    lines.openFixture();
    const double zoom = lines.zoom();
    if (std::abs(zoom - 1.5) > exact)
        fail(QStringLiteral("the list's zoom is %1 with zoom=1.5 stored").arg(num(zoom)));
    for (const QString &style : styles) {
        lines.setStyle(style);
        lines.waitForGuides(fixtureGuides, QStringLiteral("in style %1 at zoom 1.5").arg(style));
        expectGeometry(lines, style, QStringLiteral("style %1 at zoom 1.5").arg(style));
    }
}

// 13: 2000 rows, 8 levels deep.
void deepGate(Folio &folio)
{
    writeSettings(folio.home(), {});
    makeFixture(QStringLiteral("deep"), folio.home());
    Lines lines(folio.open(QStringLiteral("deep")));
    QString path;
    for (int level = 1; level <= 8; ++level) {
        path += (level > 1 ? QStringLiteral("/") : QString()) + QStringLiteral("l%1").arg(level);
        lines.expand(path);
    }
    waitFor([&] { return lines.model()->rowCount() == 2000; },
            QStringLiteral("2000 rows (%1 listed)").arg(lines.model()->rowCount()));
    settle(500);
    lines.treeLines();
    qmlWarnings.clear();

    step(QStringLiteral("timing the guides of 2000 rows"));
    QElapsedTimer timer;
    timer.start();
    lines.quick()->setProperty("lineStyle", QStringLiteral("quiet"));
    const bool done = QTest::qWaitFor([&] { return lines.guideCount() == 8; }, 5000);
    const qint64 took = timer.elapsed();
    if (!done)
        fail(QStringLiteral("%1 guides 5 s after choosing quiet with 2000 rows, expected 8").arg(lines.guideCount()));
    if (took > computeLimitMs)
        fail(QStringLiteral("the guides of 2000 rows took %1 ms, expected at most %2").arg(took).arg(computeLimitMs));
    // Revision 7: the item's height is not read (nothing draws with it).
    expectGeometry(lines, QStringLiteral("quiet"), QStringLiteral("2000 rows"));
    settle(300);
    if (!qmlWarnings.isEmpty())
        fail(QStringLiteral("%1 QML warnings with 2000 rows (first: %2)").arg(qmlWarnings.size()).arg(qmlWarnings.first()));
}

// 14: no guides where folders do not open in place.
void placesGate(Folio &folio)
{
    makeFixture(QStringLiteral("trash"), folio.home() + QStringLiteral("/.local/share"));
    Lines lines(openFixtureWindow(folio, "lineStyle=quiet\n"));
    lines.openFixture();
    // Guides show here first, so the empty lists below say something.
    lines.waitForGuides(fixtureGuides, QStringLiteral("in the fixture folder before leaving it"));

    step(QStringLiteral("searching for md"));
    lines.model()->setProperty("query", QStringLiteral("md"));
    waitFor([&] { return !lines.model()->property("loading").toBool() && lines.model()->rowCount() > 0; },
            QStringLiteral("search results"));
    settle(500);
    if (lines.model()->expandable())
        fail(QStringLiteral("the folder still opens in place during a search"));
    if (const int n = lines.guideCount(); n != 0)
        fail(QStringLiteral("%1 guides in a search, expected none").arg(n));
    lines.model()->setProperty("query", QString());
    settle(500);

    step(QStringLiteral("going to Recents"));
    lines.backend()->navigate(FolderModel::recentPath());
    Folio::waitForListing(lines.pane(), FolderModel::recentPath());
    if (const int n = lines.guideCount(); n != 0)
        fail(QStringLiteral("%1 guides in Recents, expected none").arg(n));

    step(QStringLiteral("going to the trash"));
    const QString trash = QDir::cleanPath(FileOps::homeTrash() + QStringLiteral("/files"));
    lines.backend()->navigate(trash);
    Folio::waitForListing(lines.pane(), trash);
    if (lines.model()->rowCount() < 1)
        fail(QStringLiteral("the trash lists nothing (the fixture's trashed folder is missing)"));
    lines.model()->setExpanded(0, true);
    settle(500);
    if (const int n = lines.guideCount(); n != 0)
        fail(QStringLiteral("%1 guides in the trash, expected none").arg(n));
}

// 15 (revision 4): a stored value that is not a style reads as off where it
// is used (style off, only "No lines" checked, nothing drawn), and the window
// keeps the stored text: the settings file still holds it.
void badValueGate(Folio &folio, const QString &value)
{
    const QByteArray stored = "lineStyle=" + value.toUtf8();
    QQuickWindow *window = openFixtureWindow(folio, stored + "\n");
    Lines lines(window);
    lines.openFixture();
    lines.setCursor(lines.rowOf(QStringLiteral("docs/guides/migrating.md")));
    const QString shown = value.isEmpty() ? QStringLiteral("an empty value") : value;
    const QString style = lines.style();
    if (style != QLatin1String("off"))
        fail(QStringLiteral("treeLines.style is %1 with lineStyle=%2 stored, expected off").arg(style, shown));
    const int guides = int(lines.guides().size());
    const int dots = int(lines.dots().size());
    if (guides != 0 || dots != 0)
        fail(QStringLiteral("%1 guides and %2 dots with lineStyle=%3 stored, expected none").arg(guides).arg(dots).arg(shown));

    // Revision 4: the menu shows what is in effect, so "No lines" is checked.
    openLinesMenu(window);
    for (const QString &entry : {QStringLiteral("off"), QStringLiteral("quiet"), QStringLiteral("graph"), QStringLiteral("trace")}) {
        const bool checked = visibleOne(window, QStringLiteral("lines-") + entry)->property("checked").toBool();
        if (checked != (entry == QLatin1String("off")))
            fail(QStringLiteral("lines-%1 is %2 with lineStyle=%3 stored, expected only lines-off checked")
                     .arg(entry, checked ? QStringLiteral("checked") : QStringLiteral("not checked"), shown));
    }
    closeMenu(window);

    // Settings may write some time after a change; give it that time.
    settle(1500);
    const QString conf = folio.home() + QStringLiteral("/.config/folio/folio.conf");
    QFile file(conf);
    const QList<QByteArray> fileLines = file.open(QIODevice::ReadOnly) ? file.readAll().split('\n') : QList<QByteArray>();
    if (!fileLines.contains(stored)) {
        QByteArray seen = "no lineStyle line";
        for (const QByteArray &line : fileLines)
            if (line.startsWith("lineStyle"))
                seen = line;
        fail(QStringLiteral("after the window opened folio.conf holds %1, expected %2 (the stored text kept)")
                 .arg(QString::fromUtf8(seen), QString::fromUtf8(stored)));
    }

    // Nothing drawn: the list is as it is with lineStyle set to off.
    const QImage before = grab(lines).copy(listRect(lines));
    lines.setStyle(QStringLiteral("off"));
    const QImage after = grab(lines).copy(listRect(lines));
    int differ = 0;
    for (int y = 0; y < before.height(); ++y)
        for (int x = 0; x < before.width(); ++x)
            differ += before.pixel(x, y) != after.pixel(x, y);
    if (differ > 0)
        fail(QStringLiteral("%1 list pixels change when lineStyle=%2 is replaced by off, expected 0 (lines were drawn)").arg(differ).arg(shown));
}

// 16: right to left, every x mirrored and each curve turning left.
void mirroredGate(Folio &folio)
{
    QGuiApplication::setLayoutDirection(Qt::RightToLeft);
    Lines lines(openFixtureWindow(folio));
    lines.openFixture();
    for (const QString &style : styles) {
        lines.setStyle(style);
        lines.waitForGuides(fixtureGuides, QStringLiteral("right to left, style %1").arg(style));
        expectGeometry(lines, style, QStringLiteral("right to left, style %1").arg(style), true);
    }
}

// The visible delegate of a row.
QQuickItem *delegateOf(Lines &lines, int row)
{
    const auto children = lines.contentItem()->childItems();
    for (QQuickItem *item : children)
        if (item->isVisible() && item->property("depth").isValid() && item->property("index").toInt() == row)
            return item;
    fail(QStringLiteral("row %1 (%2) has no visible delegate").arg(row).arg(lines.nameOf(row)));
}

// Where a row's arrow glyph and picture really are, in the list's content
// coordinates: the FileRow items themselves, mapped, not the formulas.
struct RowParts {
    QPointF arrowCentre;
    double arrowEdge;   // the glyph's visible left edge: its centre less 3
    double pictureLeft; // the left edge of the item named picture
};

RowParts partsOf(Lines &lines, int row)
{
    QQuickItem *delegate = delegateOf(lines, row);
    const QList<QQuickItem *> pictures = namedItems(delegate, QStringLiteral("picture"));
    if (pictures.size() != 1)
        fail(QStringLiteral("row %1 has %2 items named picture, expected 1").arg(row).arg(pictures.size()));
    QQuickItem *picture = pictures.first();
    // The arrow is the item just before the picture in the row's layout.
    const QList<QQuickItem *> siblings = picture->parentItem()->childItems();
    const qsizetype at = siblings.indexOf(picture);
    if (at < 1)
        fail(QStringLiteral("row %1 has no arrow item before its picture").arg(row));
    QQuickItem *glyph = siblings.at(at - 1)->childItems().value(0);
    if (!glyph)
        fail(QStringLiteral("row %1's arrow item holds no glyph").arg(row));
    QQuickItem *content = lines.contentItem();
    const QPointF centre = glyph->mapToItem(content, QPointF(glyph->width() / 2, glyph->height() / 2));
    return {centre, centre.x() - 3, picture->mapToItem(content, QPointF(0, 0)).x()};
}

// 20 (revision 2): at each zoom the lines meet the real arrows and pictures.
void realZoomGate(Folio &folio)
{
    QQuickWindow *window = openFixtureWindow(folio);
    Lines lines(window);
    lines.openFixture();
    // Tall enough that every row has a delegate at zoom 2.
    window->resize(windowWidth, 1500);
    waitFor([&] { return window->height() == 1500; }, QStringLiteral("the taller window"));
    settle(400);
    const auto inRange = [](double v, double low, double high) { return v >= low - 0.01 && v <= high + 0.01; };
    QStringList problems;
    for (const double zoom : {0.75, 1.0, 1.25, 1.5, 2.0}) {
        step(QStringLiteral("zoom %1").arg(zoom));
        window->setProperty("zoom", zoom);
        waitFor([&] { return lines.list()->property("rowHeight").toInt() == jsRound(32 * zoom); },
                QStringLiteral("the list at zoom %1").arg(zoom));
        settle(500);
        const QList<Row> rows = lines.rows();
        for (const QString &style : {QStringLiteral("quiet"), QStringLiteral("graph")}) {
            lines.setStyle(style);
            lines.waitForGuides(fixtureGuides, QStringLiteral("at zoom %1, style %2").arg(zoom).arg(style));
            settle(200);
            const QList<Guide> guides = lines.guides();
            const QList<Dot> dots = style == QLatin1String("graph") ? lines.dots() : QList<Dot>();
            const QString at = QStringLiteral("zoom %1, %2").arg(zoom).arg(style);
            for (const Guide &g : guides) {
                const RowParts folder = partsOf(lines, g.row);
                const double under = g.y0 - folder.arrowCentre.y();
                if (!inRange(under, 4, 8))
                    problems << QStringLiteral("%1: the guide of %2 starts %3 px under its arrow's centre, expected 4 to 8")
                                    .arg(at, rows[g.row].name, num(under));
                for (const Curve &c : g.curves) {
                    const RowParts child = partsOf(lines, c.row);
                    if (rows[c.row].isDir) {
                        const double gap = child.arrowEdge - c.endX;
                        if (!inRange(gap, 2, 6))
                            problems << QStringLiteral("%1: the line to %2 ends %3 px short of its arrow, expected 2 to 6")
                                            .arg(at, rows[c.row].name, num(gap));
                        if (style == QLatin1String("graph")) {
                            const auto dot = std::find_if(dots.begin(), dots.end(), [&](const Dot &d) {
                                return std::abs(d.y - c.y) <= within && std::abs(d.x - c.endX) <= within;
                            });
                            if (dot == dots.end())
                                problems << QStringLiteral("%1: no dot at the end of the line to %2").arg(at, rows[c.row].name);
                            else if (child.arrowEdge - (dot->x + dot->r) < 1 - 0.01)
                                problems << QStringLiteral("%1: the dot at %2 is %3 px clear of its arrow, expected at least 1")
                                                .arg(at, rows[c.row].name, num(child.arrowEdge - (dot->x + dot->r)));
                        }
                    } else {
                        const double gap = child.pictureLeft - c.endX;
                        if (!inRange(gap, 3, 8))
                            problems << QStringLiteral("%1: the line to %2 ends %3 px short of its picture, expected 3 to 8")
                                            .arg(at, rows[c.row].name, num(gap));
                    }
                }
            }
        }
    }
    if (!problems.isEmpty())
        fail(problems.mid(0, 3).join(QStringLiteral("; ")) +
             (problems.size() > 3 ? QStringLiteral("; and %1 more").arg(problems.size() - 3) : QString()));
}

// The counting wrapper the list uses, or a failure saying why there is none.
CountingModel *countingFor(Lines &lines)
{
    CountingModel *model = counting().of(lines.model());
    if (!model || objectOf(lines.list()->property("model")) != model)
        fail(QStringLiteral("the list's model is not the counting wrapper (FolderList.qml was not given "
                            "gatesCounting.wrap(backend.folder); run gate 21 through gates.sh)"));
    return model;
}

// Sets both counts of the wrapper to 0.
void zeroReads(CountingModel *model)
{
    model->gets = 0;
    model->outlines = 0;
}

// The wrapper's counts since zeroReads, as the failure messages give them.
QString readsText(CountingModel *model)
{
    return QStringLiteral("get() %1 times and outline() %2 times").arg(model->gets).arg(model->outlines);
}

// 21 (revision 5): what a change to the list costs. One part per run:
// idle, rtl, big or insert.
void costGate(Folio &folio, const QString &part)
{
    if (part == QLatin1String("idle")) {
        Lines lines(openFixtureWindow(folio, "lineStyle=quiet\n"));
        lines.treeLines();
        CountingModel *model = countingFor(lines);
        settle(500);
        step(QStringLiteral("resetting the model with no folder open"));
        zeroReads(model);
        model->simulateReset();
        settle(500);
        if (model->gets != 0 || model->outlines != 0)
            fail(QStringLiteral("a model reset with no folder open called %1, expected 0 of either").arg(readsText(model)));
        step(QStringLiteral("adding a file with no folder open"));
        const int before = model->rowCount();
        zeroReads(model);
        writeFile(folio.home() + QStringLiteral("/project/zz-added.txt"), "added\n");
        waitFor([&] { return model->rowCount() == before + 1; }, QStringLiteral("the added file to be listed"));
        settle(500);
        if (model->gets != 0 || model->outlines != 0)
            fail(QStringLiteral("a row change (a file added) with no folder open called %1, expected 0 of either").arg(readsText(model)));
    } else if (part == QLatin1String("rtl")) {
        QGuiApplication::setLayoutDirection(Qt::RightToLeft);
        QQuickWindow *window = openFixtureWindow(folio, "lineStyle=quiet\n");
        Lines lines(window);
        CountingModel *model = countingFor(lines);
        lines.openFixture();
        lines.waitForGuides(fixtureGuides, QStringLiteral("right to left before the width change"));
        settle(500);
        step(QStringLiteral("narrowing the right-to-left window"));
        const double width = lines.width();
        zeroReads(model);
        window->resize(1100, windowHeight);
        waitFor([&] { return std::abs(lines.width() - width) > 1; }, QStringLiteral("the list to narrow"));
        settle(500);
        if (model->gets != 0 || model->outlines != 0)
            fail(QStringLiteral("a width change right to left called %1, expected 0 of either").arg(readsText(model)));
        expectGeometry(lines, QStringLiteral("quiet"), QStringLiteral("right to left after the width change"), true);
    } else if (part == QLatin1String("big")) {
        writeSettings(folio.home(), {});
        makeFixture(QStringLiteral("big"), folio.home());
        Lines lines(folio.open(QStringLiteral("big")));
        waitFor([&] { return lines.model()->rowCount() == 10000; },
                QStringLiteral("10,000 rows (%1 listed)").arg(lines.model()->rowCount()), 30000);
        CountingModel *model = countingFor(lines);
        settle(500);
        // The GUI thread's CPU time over the 400 ms after a reset, with the
        // lines off and with them on; the difference is the rebuild's cost.
        const auto cost = [&](const QString &style) {
            lines.setStyle(style);
            settle(500);
            double least = 1e9;
            for (int i = 0; i < 3; ++i) {
                const double start = threadCpuMs();
                model->simulateReset();
                QTest::qWait(400);
                least = std::min(least, threadCpuMs() - start);
            }
            return least;
        };
        const double off = cost(QStringLiteral("off"));
        const double quiet = cost(QStringLiteral("quiet"));
        if (quiet - off >= 50)
            fail(QStringLiteral("a rebuild with 10,000 rows and nothing open took %1 ms (%2 ms with lines against %3 ms off), expected under 50")
                     .arg(num(quiet - off), num(quiet), num(off)));
    } else if (part == QLatin1String("insert")) {
        // Revision 5: 10,000 rows with one folder (50 items) open; one row
        // inserted in it is read with at most one outline() and no get().
        writeSettings(folio.home(), "lineStyle=quiet\n");
        makeFixture(QStringLiteral("bigtree"), folio.home());
        Lines lines(folio.open(QStringLiteral("bigtree")));
        CountingModel *model = countingFor(lines);
        lines.expand(QStringLiteral("open"));
        waitFor([&] { return model->rowCount() == 10000; },
                QStringLiteral("10,000 rows (%1 listed)").arg(model->rowCount()), 30000);
        lines.waitForGuides(1, QStringLiteral("with open/ open"));
        settle(800);
        if (model->total == 0)
            fail(QStringLiteral("TreeLines read no rows through its view's model while open/ opened, "
                                "so the counts here would prove nothing"));
        step(QStringLiteral("adding a file inside open/"));
        zeroReads(model);
        writeFile(folio.home() + QStringLiteral("/bigtree/open/new.txt"), "added\n");
        waitFor([&] { return model->rowCount() == 10001; }, QStringLiteral("the added file to be listed"));
        settle(800);
        if (model->gets != 0 || model->outlines > 1)
            fail(QStringLiteral("one row inserted in 10,000 rows called %1, expected 0 get() and at most 1 outline()")
                     .arg(readsText(model)));
        expectGeometry(lines, QStringLiteral("quiet"), QStringLiteral("after a row was inserted in 10,000 rows"));
    } else {
        fail(QStringLiteral("gate 21 takes idle, rtl, big or insert"));
    }
}

// Every QQuickItem under an item, the item itself not counted.
int descendantCount(QQuickItem *root)
{
    int count = -1;
    collect(root, [&](QQuickItem *) { ++count; });
    return count;
}

// 22 (revision 4): graph's dots are drawn as paths, not one item each. In
// fixture.sh's wide tree, few/ (2 files) open gives 1 dot and many/ (200
// folders) open gives 201; the items under treeLines must be as many in
// both.
void dotItemsGate(Folio &folio)
{
    writeSettings(folio.home(), "lineStyle=graph\n");
    makeFixture(QStringLiteral("wide"), folio.home());
    Lines lines(folio.open(QStringLiteral("wide")));
    if (lines.style() != QLatin1String("graph"))
        fail(QStringLiteral("treeLines.style is %1 with lineStyle=graph stored").arg(lines.style()));

    lines.expand(QStringLiteral("few"));
    lines.waitForGuides(1, QStringLiteral("with few open"));
    settle(500);
    const int fewDots = int(lines.dots().size());
    const int fewItems = descendantCount(lines.treeLines());
    if (fewDots != 1)
        fail(QStringLiteral("%1 dots with few (2 files) open in graph, expected 1").arg(fewDots));

    lines.collapse(QStringLiteral("few"));
    lines.expand(QStringLiteral("many"));
    waitFor([&] { return lines.model()->rowCount() == 202; },
            QStringLiteral("202 rows with many open (%1 listed)").arg(lines.model()->rowCount()));
    lines.waitForGuides(1, QStringLiteral("with many open"));
    settle(500);
    const int manyDots = int(lines.dots().size());
    const int manyItems = descendantCount(lines.treeLines());
    if (manyDots < 200)
        fail(QStringLiteral("%1 dots with many (200 folders) open in graph, expected 201").arg(manyDots));
    if (manyItems != fewItems)
        fail(QStringLiteral("treeLines has %1 items under it with %2 dots and %3 with 1 dot, expected the same number "
                            "(dots drawn as paths, not items)")
                 .arg(manyItems).arg(manyDots).arg(fewItems));
}

// The median of some numbers.
double median(QList<double> values)
{
    std::sort(values.begin(), values.end());
    const qsizetype n = values.size();
    return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2;
}

// The measurement of measure.sh and gate 25: in fixture.sh's bigtree,
// 10,000 rows with one folder (open/, 50 files) open, the GUI thread's CPU
// time from adding one file inside open/ until 400 ms after its row is
// listed, and the calls of get() and outline() the list's model had in that
// time. Five inserts per style (off, quiet, graph, trace); prints one line
// per style with the medians and returns the median CPU times by style.
// Run on the counting copy of FolderList.qml.
QHash<QString, double> measureInsert(Folio &folio)
{
    writeSettings(folio.home(), {});
    makeFixture(QStringLiteral("bigtree"), folio.home());
    Lines lines(folio.open(QStringLiteral("bigtree")));
    CountingModel *model = countingFor(lines);
    lines.expand(QStringLiteral("open"));
    waitFor([&] { return lines.model()->rowCount() == 10000; },
            QStringLiteral("10,000 rows (%1 listed)").arg(lines.model()->rowCount()), 30000);
    lines.setCursor(lines.rowOf(QStringLiteral("open/f00.txt")));
    settle(500);
    QHash<QString, double> medians;
    int added = 0;
    for (const QString &style : {QStringLiteral("off"), QStringLiteral("quiet"), QStringLiteral("graph"), QStringLiteral("trace")}) {
        lines.setStyle(style);
        settle(800);
        QList<double> cpu, gets, outlines;
        for (int i = 0; i < 5; ++i) {
            step(QStringLiteral("adding a file in style %1").arg(style));
            const int before = model->rowCount();
            zeroReads(model);
            const double start = threadCpuMs();
            writeFile(folio.home() + QStringLiteral("/bigtree/open/new%1.txt").arg(++added, 2, 10, QLatin1Char('0')), "added\n");
            waitFor([&] { return model->rowCount() == before + 1; }, QStringLiteral("the added file to be listed"));
            QTest::qWait(400);
            cpu << threadCpuMs() - start;
            gets << model->gets;
            outlines << model->outlines;
        }
        QStringList runs;
        for (double ms : cpu)
            runs << QString::number(ms, 'f', 1);
        medians.insert(style, median(cpu));
        std::printf("%-5s  rows %d  cpu median %.1f ms (runs %s)  get() median %d  outline() median %d\n", qPrintable(style),
                    model->rowCount(), median(cpu), qPrintable(runs.join(QStringLiteral(" "))), int(median(gets)),
                    int(median(outlines)));
        std::fflush(stdout);
    }
    return medians;
}

// 25 (revision 5): with each style on, the median CPU time of an insert
// in measureInsert is at most 20 ms above the median with the style off.
constexpr double insertLimitMs = 20;
void insertTimeGate(Folio &folio)
{
    const QHash<QString, double> medians = measureInsert(folio);
    QStringList over;
    for (const QString &style : styles) {
        const double extra = medians.value(style) - medians.value(QStringLiteral("off"));
        if (extra > insertLimitMs)
            over << QStringLiteral("%1 %2 ms").arg(style, QString::number(extra, 'f', 1));
    }
    if (!over.isEmpty())
        fail(QStringLiteral("one row inserted in 10,000 rows costs more with the lines than off: %1 (off %2 ms), expected at most %3 ms each")
                 .arg(over.join(QStringLiteral(", ")), QString::number(medians.value(QStringLiteral("off")), 'f', 1))
                 .arg(insertLimitMs));
}

// The screenshot probes, in screenshot pixels, all on lines no other line or
// the trace route crosses:
//   plain  the guide of api (depth 1) in the row of auth.md;
//   tint   the guide of docs (depth 0) in the selected row (migrating.md);
//   join   the guide of api one pixel below where the curve to v2 leaves it.
QHash<QString, QPoint> probePoints(Lines &lines)
{
    const Metrics m(lines.zoom());
    QQuickItem *list = lines.list();
    const QPointF origin = list->mapToScene(QPointF(0, 0)) - QPointF(0, list->property("contentY").toDouble());
    const QList<Row> rows = lines.rows();
    // Revision 6: in the screenshot's pixels, which are device pixels: at a
    // display scale s the logical point (x, y) is pixel (s x, s y).
    const double scale = lines.quick()->devicePixelRatio();
    const auto at = [&](const char *guide, const char *row, double dy) {
        const double x = origin.x() + m.left(rows.at(lines.rowOf(QLatin1String(guide))).depth) + 9.5;
        const double y = origin.y() + m.centre(lines.rowOf(QLatin1String(row))) + dy;
        return QPoint(int(std::floor(x * scale)), int(std::floor(y * scale)));
    };
    return {{QStringLiteral("plain"), at("docs/api", "docs/api/v1/auth.md", 0)},
            {QStringLiteral("tint"), at("docs", "docs/guides/migrating.md", 0)},
            {QStringLiteral("join"), at("docs/api", "docs/api/v2", -7)}};
}

QHash<QString, QPoint> readProbes(const QString &dir)
{
    QFile file(dir + QStringLiteral("/probe.txt"));
    if (!file.open(QIODevice::ReadOnly))
        fail(QStringLiteral("probe.txt is missing (shots did not finish)"));
    QHash<QString, QPoint> probes;
    for (const QString &line : QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList parts = line.simplified().split(QLatin1Char(' '));
        if (parts.size() == 3)
            probes.insert(parts[0], QPoint(parts[1].toInt(), parts[2].toInt()));
    }
    for (const char *name : {"plain", "tint", "join"})
        if (!probes.contains(QLatin1String(name)))
            fail(QStringLiteral("probe.txt has no %1 probe").arg(QLatin1String(name)));
    return probes;
}

int farthest(const QColor &a, const QColor &b)
{
    return std::max({std::abs(a.red() - b.red()), std::abs(a.green() - b.green()), std::abs(a.blue() - b.blue())});
}

QColor blended(const QColor &ground, const QColor &line, double opacity)
{
    const auto mix = [&](int b, int c) { return int(std::lround(b + (c - b) * opacity)); };
    return QColor(mix(ground.red(), line.red()), mix(ground.green(), line.green()), mix(ground.blue(), line.blue()));
}

// The screenshots: list view, fixture open, cursor on migrating.md, in
// styles off, quiet, graph and trace, dark and light.
void shots(Folio &folio, const QString &dir)
{
    if (dir.isEmpty())
        fail(QStringLiteral("shots needs the folder to write"));
    // TREE_GATES_SHOT_SETTINGS adds settings lines (for instance zoom=2) for
    // screenshots to look at by hand; gate 17 runs without it.
    Lines lines(openFixtureWindow(folio, qgetenv("TREE_GATES_SHOT_SETTINGS")));
    lines.openFixture();
    lines.setCursor(lines.rowOf(QStringLiteral("docs/guides/migrating.md")));
    const QHash<QString, QPoint> probes = probePoints(lines);
    QByteArray probeText;
    for (const char *name : {"plain", "tint", "join"}) {
        const QPoint p = probes.value(QLatin1String(name));
        probeText += QStringLiteral("%1 %2 %3\n").arg(QLatin1String(name)).arg(p.x()).arg(p.y()).toUtf8();
    }
    // The screenshots' size in pixels, so the checks know the scale.
    const double scale = lines.quick()->devicePixelRatio();
    probeText += QStringLiteral("size %1 %2\n").arg(jsRound(windowWidth * scale)).arg(jsRound(windowHeight * scale)).toUtf8();
    writeFile(dir + QStringLiteral("/probe.txt"), probeText);
    for (const Palette *palette : {&darkPalette, &lightPalette}) {
        folio.setTheme(*palette);
        for (const QString &style : QStringList{QStringLiteral("off")} + styles) {
            lines.setStyle(style);
            save(grab(lines), QStringLiteral("%1/%2-%3.png").arg(dir, style, QLatin1String(palette->name)));
        }
    }
}

// How far a screenshot pixel may be from the blended line colour: gate
// 17's slack at a whole scale (1 or 2), where the 1 px line covers whole
// pixels; at a fractional scale (1.5) the line is 1.5 pixels wide and its
// edges fall inside pixels, so revision 6 allows fractionalSlack there.
constexpr int fractionalSlack = 16;
int slackFor(const QSize &size)
{
    const double scale = double(size.width()) / windowWidth;
    return std::abs(scale - std::round(scale)) < exact ? colourSlack : fractionalSlack;
}

QImage loadImage(const QString &path)
{
    QImage image(path);
    if (image.isNull())
        fail(QStringLiteral("%1 is missing or not an image").arg(QFileInfo(path).fileName()));
    return image.convertToFormat(QImage::Format_RGB32);
}

// 17: each screenshot is the window's size, not blank, and shows the line
// colour, blended at the style's opacity, at the probe; style off does not.
void verifyShots(const QString &dir)
{
    const QHash<QString, QPoint> probes = readProbes(dir);
    const QPoint probe = probes.value(QStringLiteral("plain"));
    // Revision 6: the size the shots were taken at (1280 x 800 at scale 1),
    // and the slack for that scale.
    const QSize size = probes.contains(QStringLiteral("size")) ? QSize(probes.value(QStringLiteral("size")).x(), probes.value(QStringLiteral("size")).y())
                                                               : QSize(windowWidth, windowHeight);
    const int slack = slackFor(size);
    QStringList problems;
    for (const Palette *palette : {&darkPalette, &lightPalette}) {
        const QString theme = QLatin1String(palette->name);
        const QImage off = loadImage(QStringLiteral("%1/off-%2.png").arg(dir, theme));
        for (const QString &style : styles) {
            const QString name = QStringLiteral("%1-%2.png").arg(style, theme);
            const QImage image = loadImage(dir + QLatin1Char('/') + name);
            if (image.size() != size) {
                problems << QStringLiteral("%1 is %2 x %3, expected %4 x %5").arg(name).arg(image.width()).arg(image.height()).arg(size.width()).arg(size.height());
                continue;
            }
            QSet<QRgb> colours;
            for (int y = 0; y < image.height(); y += 2)
                for (int x = 0; x < image.width(); x += 2)
                    colours.insert(image.pixel(x, y));
            if (colours.size() < 32) {
                problems << QStringLiteral("%1 is nearly blank (%2 colours)").arg(name).arg(colours.size());
                continue;
            }
            const Look look = lookOf(style, palette->light);
            const QColor line(style == QLatin1String("graph") ? palette->magenta : palette->foreground);
            const QColor ground(off.pixel(probe));
            const auto blend = [&](int b, int c) { return int(std::lround(b + (c - b) * look.opacity)); };
            const QColor want(blend(ground.red(), line.red()), blend(ground.green(), line.green()), blend(ground.blue(), line.blue()));
            const QColor seen(image.pixel(probe));
            const auto far = [](const QColor &a, const QColor &b) {
                return std::max({std::abs(a.red() - b.red()), std::abs(a.green() - b.green()), std::abs(a.blue() - b.blue())});
            };
            if (far(seen, want) > slack)
                problems << QStringLiteral("%1 at (%2, %3) is %4, expected %5 (%6 at %7 over %8)")
                                .arg(name).arg(probe.x()).arg(probe.y()).arg(rgb(seen), rgb(want), rgb(line), num(look.opacity), rgb(ground));
            else if (far(ground, want) <= slack)
                problems << QStringLiteral("off-%1.png at the probe is already %2, the line colour").arg(theme, rgb(ground));
        }
    }
    if (!problems.isEmpty())
        fail(problems.mid(0, 3).join(QStringLiteral("; ")) + (problems.size() > 3 ? QStringLiteral("; and %1 more").arg(problems.size() - 3) : QString()));
}

// 23 (revision 2): over the selected row a line blends with the row's tint
// at the style's opacity, and where a curve leaves its guide the pixel is
// the plain guide's (nothing darker at the join).
void verifyOverlays(const QString &dir)
{
    const QHash<QString, QPoint> probes = readProbes(dir);
    const QPoint tint = probes.value(QStringLiteral("tint"));
    const QPoint join = probes.value(QStringLiteral("join"));
    const QPoint plainProbe = probes.value(QStringLiteral("plain"));
    QStringList problems;
    for (const Palette *palette : {&darkPalette, &lightPalette}) {
        const QString theme = QLatin1String(palette->name);
        const QImage off = loadImage(QStringLiteral("%1/off-%2.png").arg(dir, theme));
        for (const QString &style : styles) {
            const QString name = QStringLiteral("%1-%2.png").arg(style, theme);
            const QImage image = loadImage(dir + QLatin1Char('/') + name);
            const Look look = lookOf(style, palette->light);
            // The guide of docs is at depth 0: accent in graph.
            const QColor line(style == QLatin1String("graph") ? palette->accent : palette->foreground);
            const QColor ground(off.pixel(tint));
            const QColor want = blended(ground, line, look.opacity);
            const QColor seen(image.pixel(tint));
            if (farthest(seen, want) > colourSlack)
                problems << QStringLiteral("%1: over the selected row at (%2, %3) %4, expected %5 (%6 at %7 over the tint %8)")
                                .arg(name).arg(tint.x()).arg(tint.y()).arg(rgb(seen), rgb(want), rgb(line), num(look.opacity), rgb(ground));
            const QColor atJoin(image.pixel(join));
            const QColor plain(image.pixel(plainProbe));
            if (farthest(atJoin, plain) > 4)
                problems << QStringLiteral("%1: the join at (%2, %3) is %4, the plain guide %5, expected within 4")
                                .arg(name).arg(join.x()).arg(join.y()).arg(rgb(atJoin), rgb(plain));
        }
    }
    if (!problems.isEmpty())
        fail(problems.mid(0, 3).join(QStringLiteral("; ")) + (problems.size() > 3 ? QStringLiteral("; and %1 more").arg(problems.size() - 3) : QString()));
}

// Whether two PNGs are pixel-identical.
void compare(const QString &a, const QString &b)
{
    const QImage one = loadImage(a);
    const QImage two = loadImage(b);
    if (one.size() != two.size())
        fail(QStringLiteral("the screenshots are %1 x %2 and %3 x %4").arg(one.width()).arg(one.height()).arg(two.width()).arg(two.height()));
    int differ = 0;
    QPoint first(-1, -1);
    for (int y = 0; y < one.height(); ++y)
        for (int x = 0; x < one.width(); ++x)
            if (one.pixel(x, y) != two.pixel(x, y)) {
                if (differ++ == 0)
                    first = QPoint(x, y);
            }
    if (differ > 0)
        fail(QStringLiteral("%1 pixels differ from main's list, expected 0 (first at %2, %3: %4 against %5)")
                 .arg(differ).arg(first.x()).arg(first.y()).arg(rgb(QColor(one.pixel(first))), rgb(QColor(two.pixel(first)))));
}

int run(const QString &gate, const QString &arg, const QString &root)
{
    try {
        if (gate == QLatin1String("compare")) {
            compare(arg, qEnvironmentVariable("FOLIO_GATES_OTHER"));
        } else if (gate == QLatin1String("verify-shots")) {
            verifyShots(arg);
        } else if (gate == QLatin1String("verify-overlays")) {
            verifyOverlays(arg);
        } else {
            Disks::replace(std::make_shared<NoDisks>());
            QDir().mkpath(root + QStringLiteral("/home"));
            Folio *folio = new Folio(root);
            const int n = gate.toInt();
            if (gate == QLatin1String("listshot")) listShot(*folio, arg);
            else if (gate == QLatin1String("shots")) shots(*folio, arg);
            else if (gate == QLatin1String("measure")) measureInsert(*folio);
            else if (n == 4) offGate(*folio, arg);
            else if (n == 5) geometryGate(*folio);
            else if (n == 6) lookGate(*folio);
            else if (n == 7) dotsGate(*folio);
            else if (n == 8) routeGate(*folio);
            else if (n == 9) collapseGate(*folio);
            else if (n == 10) menuGate(*folio);
            else if (n == 11) buttonGate(*folio);
            else if (n == 12) zoomGate(*folio);
            else if (n == 13) deepGate(*folio);
            else if (n == 14) placesGate(*folio);
            else if (n == 15) badValueGate(*folio, arg);
            else if (n == 16) mirroredGate(*folio);
            else if (n == 20) realZoomGate(*folio);
            else if (n == 21) costGate(*folio, arg);
            else if (n == 22) dotItemsGate(*folio);
            else if (n == 25) insertTimeGate(*folio);
            else if (n == 26) keysGate(*folio);
            else if (n == 28) reloadGate(*folio);
            else fail(QStringLiteral("no gate %1 here").arg(gate));
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
    if (argc < 2 || argc > 4) {
        std::fprintf(stderr, "usage: tree-gates <gate 4 to 16, 20 to 22, 25|listshot|shots|measure|verify-shots|verify-overlays|compare> [arg] [arg]\n");
        return 2;
    }
    const QString gate = QString::fromLocal8Bit(argv[1]);
    const QString arg = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString();
    if (argc > 3)
        qputenv("FOLIO_GATES_OTHER", argv[3]);

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

    defaultHandler = qInstallMessageHandler(keepMessage);
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("folio"));
    app.setOrganizationName(QStringLiteral("folio"));
    QQuickStyle::setStyle(QStringLiteral("Material"));
    qmlRegisterType<FolderModel>("Folio", 1, 0, "FolderModel");
    qmlRegisterType<DragPicture>("Folio", 1, 0, "DragPicture");

    // Nothing may hang: a run that takes too long fails and ends here.
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [] {
        std::printf("FAIL: the gate ran longer than %d s (at: %s)\n", gateLimitMs / 1000, qPrintable(currentStep));
        std::fflush(stdout);
        _exit(1);
    });
    watchdog.start(gateLimitMs);

    const int status = run(gate, arg, root.path());
    // Ends without destroying the open windows, after removing the private
    // folders, which _exit would otherwise leave behind.
    watchdog.stop();
    root.remove();
    std::fflush(stderr);
    _exit(status);
}

// The counting wrapper's meta-object code (its classes are in this file).
#include "tree_gates.moc"
