cmake_minimum_required(VERSION 3.20)
project(UsagiDesktopPet LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
  message(FATAL_ERROR "MSVC(Visual Studio 2022 Build Tools)로 빌드하세요.")
endif()

add_executable(usagi_pet WIN32
  src/main.cpp
  src/common.cpp
  src/sprites.cpp
  src/audio.cpp
  src/wintarget.cpp
  src/bubble.cpp
  src/voice.cpp
  src/gemini.cpp
  src/agent.cpp
  src/fire.cpp
  src/media.cpp
  src/lyrics.cpp
  src/app.manifest
  src/resources.rc
)
target_compile_definitions(usagi_pet PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(usagi_pet PRIVATE /utf-8 /W4 /permissive- /wd4100)
target_link_libraries(usagi_pet PRIVATE user32 gdi32 shell32 ole32 windowscodecs dwmapi shcore advapi32 gdiplus winmm winhttp crypt32 windowsapp)
# 정적 CRT: 배포할 때 VC++ 재배포 패키지가 필요 없음
set_property(TARGET usagi_pet PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 실행에 필요한 에셋을 EXE 옆으로 복사 (EXE 기준 상대 경로 assets\ 에서 읽음)
add_custom_command(TARGET usagi_pet POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets/sprites $<TARGET_FILE_DIR:usagi_pet>/assets/sprites
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets/characters $<TARGET_FILE_DIR:usagi_pet>/assets/characters
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/assets/usagi.ini $<TARGET_FILE_DIR:usagi_pet>/assets/usagi.ini
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/settings.ini $<TARGET_FILE_DIR:usagi_pet>/settings.ini
)
