######################################################################
# GP32emu - Qt6/Linux frontend
#
# Build with:
#   qmake6 GP32emu.pro && make
# or:
#   make -f Makefile.linux qt
#
# The legacy/headless/SDL makefile is intentionally named Makefile.sdl so qmake
# can generate a normal root Makefile without colliding with project sources.
######################################################################

QT += core gui widgets
CONFIG += c++17
TARGET = GP32emu
TEMPLATE = app

QMAKE_CFLAGS += -std=c11 -DGP32EMU_ENABLE_THREADS=1 -D_XOPEN_SOURCE=700 -D_FILE_OFFSET_BITS=64 -DZIP_ENABLE_DEFLATE=0 -DZIP_HAVE_SYMLINK=0
QMAKE_CFLAGS_WARN_ON += -Wall -Wextra
QMAKE_CXXFLAGS_WARN_ON += -Wall -Wextra
LIBS += -pthread

INCLUDEPATH += include src src/qt

SOURCES += \
    src/gp32.c \
    src/arm920t.c \
    src/s3c2400.c \
    src/smartmedia.c \
    src/smc_direct.c \
    src/fxe.c \
    src/fpk.c \
    src/zip.c \
    src/third_party/zip/zip.c \
    src/input_script.c \
    src/platform/platform.c \
    src/audio/gp32_audio_resampler.c \
    src/media/gp32_media.c \
    src/media/gp32_video_effects.c \
    src/qt/main.cpp \
    src/qt/GP32Engine.cpp \
    src/qt/GP32MainWindow.cpp \
    src/qt/GP32VideoWidget.cpp

HEADERS += \
    include/gp32emu/gp32.h \
    include/gp32emu/platform.h \
    include/gp32emu/video_effects.h \
    src/arm920t.h \
    src/s3c2400.h \
    src/smartmedia.h \
    src/smc_direct.h \
    src/fxe.h \
    src/fpk.h \
    src/input_script.h \
    src/platform/platform_internal.h \
    src/audio/gp32_audio_resampler.h \
    src/media/gp32_media.h \
    src/zip.h \
    src/qt/GP32Engine.h \
    src/qt/GP32MainWindow.h \
    src/qt/GP32VideoWidget.h

RESOURCES += resources/gp32emu.qrc

# Qt6 audio is optional. If SDL3 development files are present, use the same
# corrected SDL3 stream backend as the standalone SDL3 frontend. Otherwise the
# Qt6 UI still builds and runs silently.
!no_sdl3_audio {
    CONFIG += link_pkgconfig
    packagesExist(sdl3) {
        PKGCONFIG += sdl3
        DEFINES += GP32EMU_QT_SDL3_AUDIO SDL_MAIN_HANDLED
        SOURCES += \
            src/platform/sdl3/common.c \
            src/platform/sdl3/audio.c
        HEADERS += src/platform/sdl3/common.h
    } else {
        warning("SDL3 development package not found; Qt6 frontend will build without live audio. Install SDL3 development files or pass CONFIG+=no_sdl3_audio intentionally.")
    }
}

appimage.target = appimage
appimage.commands = QMAKE=$$QMAKE_QMAKE $$PWD/packaging/make_appimage.sh
appmake.target = appmake
appmake.depends = appimage
QMAKE_EXTRA_TARGETS += appimage appmake
QMAKE_DISTCLEAN += -r $$PWD/build-appimage
