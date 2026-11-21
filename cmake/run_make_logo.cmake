# Runs make_logo (tools/make_logo.cpp) during the build, with no window system and, on Windows, Qt's DLLs from Qt's own
# folder (they are only copied beside the programs after reno.exe is built).
#   cmake -DEXE=<make_logo> -DOUT=<folder> [-DQTBIN=<Qt's bin folder>] [-DQTPLUGINS=<Qt's plugins folder>] -P run_make_logo.cmake
if(QTBIN AND CMAKE_HOST_WIN32)
  set(ENV{PATH} "${QTBIN};$ENV{PATH}")
endif()
if(QTPLUGINS)
  set(ENV{QT_PLUGIN_PATH} "${QTPLUGINS}")
endif()
# (Windows keeps its own platform: there the offscreen one finds no fonts and draws the letters as boxes. It opens no
# window either way.)
if(CMAKE_HOST_WIN32)
  set(ENV{QT_QPA_PLATFORM} windows)
else()
  set(ENV{QT_QPA_PLATFORM} offscreen)
endif()
file(MAKE_DIRECTORY "${OUT}")
execute_process(COMMAND "${EXE}" "${OUT}" RESULT_VARIABLE _result)
if(NOT _result EQUAL 0)
  message(FATAL_ERROR "make_logo could not write the icons into ${OUT} (${_result})")
endif()
