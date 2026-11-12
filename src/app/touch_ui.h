// Whether this build has the touch interface (touch_shell.h): Android and iOS only. Defining RENO_TOUCH_UI to 1 on the
// compiler's command line builds it elsewhere too, for its tests.
#pragma once
#include <QtGlobal>

#ifndef RENO_TOUCH_UI
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
#define RENO_TOUCH_UI 1
#else
#define RENO_TOUCH_UI 0
#endif
#endif
