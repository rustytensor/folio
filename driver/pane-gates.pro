# pane-gates: a program that opens Folio's real main window offscreen and
# drags the lines between its panes with synthetic mouse events.
#
# It is compiled from Folio's own sources (everything in src/ but main.cpp,
# plus the QML resources), so it always drives the interface as the checkout
# has it. Build it out of source, inside the checkout's build folder:
#
#   cd <checkout>/build/gates && qmake6 FOLIO_REPO=<checkout> <this file> && make
#
# Release only, without debug information.

isEmpty(FOLIO_REPO): error("Pass FOLIO_REPO=<path to the folio checkout> to qmake.")
!exists($$FOLIO_REPO/src/Main.qml): error("$$FOLIO_REPO is not a folio checkout.")

TEMPLATE = app
TARGET = pane-gates

QT += core gui network qml quick quickcontrols2 dbus concurrent testlib
CONFIG += c++17 release link_pkgconfig cmdline warn_on
CONFIG -= debug debug_and_release force_debug_info separate_debug_info ltcg app_bundle
PKGCONFIG += gio-2.0 gio-unix-2.0 fontconfig zlib

INCLUDEPATH += $$FOLIO_REPO/src

FOLIO_SOURCES = $$files($$FOLIO_REPO/src/*.cpp)
FOLIO_SOURCES -= $$FOLIO_REPO/src/main.cpp
SOURCES += $$PWD/pane_gates.cpp $$FOLIO_SOURCES
HEADERS += $$files($$FOLIO_REPO/src/*.h)
RESOURCES += $$FOLIO_REPO/src/resources.qrc

# Folio's CMake build puts src/*.qml and src/*.js into the QML module FolioUI
# (qrc:/FolioUI/Main.qml), precompiled into a resource Windows loads with
# Q_INIT_RESOURCE(qmlcache_folio_core). The driver packs the same files
# under the same paths into a resource of that name, uncompiled: rcc names
# the resource after the .qrc file, so Folio's own code finds it.
FOLIO_QML = $$files($$FOLIO_REPO/src/*.qml) $$files($$FOLIO_REPO/src/*.js)
FOLIO_QML_QRC = $$OUT_PWD/qmlcache_folio_core.qrc
FOLIO_QML_LINES = "<RCC>" "    <qresource prefix=\"/FolioUI\">"
for(file, FOLIO_QML): FOLIO_QML_LINES += "        <file alias=\"$$basename(file)\">$$file</file>"
FOLIO_QML_LINES += "    </qresource>" "</RCC>"
write_file($$FOLIO_QML_QRC, FOLIO_QML_LINES)|error("Could not write $$FOLIO_QML_QRC.")
RESOURCES += $$FOLIO_QML_QRC

# Folio's CMake build defines where an installed Folio finds its plugins.
# The driver names its own (empty) plugin folder, so this is never read.
DEFINES += FOLIO_PACKAGED_PLUGINS=\\\"/usr/share/folio/plugins\\\"

QMAKE_CXXFLAGS += -Wextra
# No symbols are kept in the binary.
QMAKE_LFLAGS += -s

OBJECTS_DIR = obj
MOC_DIR = moc
RCC_DIR = rcc
