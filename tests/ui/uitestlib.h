// Helpers shared by the interface tests: made-up media, a demo project, waiting, screenshots, pretend mouse and keys.
#pragma once
#include "../../src/app/appstate.h"
#include <QString>
#include <QPoint>
#include <QImage>

class QWidget;

namespace uitest {

// Made-up media: "synth:<tag>" is a 10-second 320x180 clip (30 fps) whose picture shows moving colour bars with the
// frame number in them, and whose sound is a tone of 220 Hz x tag. "synthpic:<tag>" is a still picture.
// "synthaudio:<tag>" has sound only.
// Sets AppState's media opener to understand these, and fills the plug-in catalog from the given files (in this process).
void setup(const std::vector<std::string>& ofxFiles, const std::vector<std::string>& vstFiles);

// Two video tracks and two audio tracks with a handful of events (one with effects if test effects are loaded,
// an overlap, a fade, a still, a clip with picture and sound grouped).
void buildDemoProject();

// Lets queued work finish: runs the event loop for 'ms' milliseconds.
void settle(int ms = 100);
// Saves a picture of a widget (and returns it).
QImage shot(QWidget* w, const QString& path);

// Pretend input, sent straight to the widget (positions in the widget's own coordinates).
void mousePress(QWidget* w, QPoint pos, Qt::MouseButton button = Qt::LeftButton, Qt::KeyboardModifiers mods = Qt::NoModifier);
void mouseMove(QWidget* w, QPoint pos, Qt::MouseButtons buttons = Qt::LeftButton, Qt::KeyboardModifiers mods = Qt::NoModifier);
void mouseRelease(QWidget* w, QPoint pos, Qt::MouseButton button = Qt::LeftButton, Qt::KeyboardModifiers mods = Qt::NoModifier);
void mouseClick(QWidget* w, QPoint pos, Qt::MouseButton button = Qt::LeftButton, Qt::KeyboardModifiers mods = Qt::NoModifier);
void mouseDoubleClick(QWidget* w, QPoint pos);
void mouseDrag(QWidget* w, QPoint from, QPoint to, int steps = 8, Qt::KeyboardModifiers mods = Qt::NoModifier);
void wheel(QWidget* w, QPoint pos, int delta, Qt::KeyboardModifiers mods = Qt::NoModifier);
void key(QWidget* w, int key, Qt::KeyboardModifiers mods = Qt::NoModifier, const QString& text = QString());

// One line per check, like the other tests: "PASS name" / "FAIL name: detail". summary() prints the totals and
// returns the process exit code.
void check(bool ok, const std::string& name, const std::string& detail = std::string());
int summary();

}  // namespace uitest
