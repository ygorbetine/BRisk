cmake_minimum_required(VERSION 3.20)
project(briks_desktop_cui C)

include(FetchContent)
FetchContent_Declare(cui
  GIT_REPOSITORY https://github.com/yappologistic/CUI.git
  GIT_TAG        e8688b8fd85c7b07afbddbac5fc748a905c12be0
  # gancho de render (cui_app_set_render_override) usado por src/glass.c; idempotente
  PATCH_COMMAND  ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_SOURCE_DIR}/patches/apply.cmake")
FetchContent_MakeAvailable(cui)

add_executable(briks_cui WIN32 src/main.c src/theme.c src/theme_cfg.c src/perf.c src/ui_auth.c src/ui_social.c src/ui_call.c src/app.c src/chat.c src/ui_history.c src/layout.c src/drag_stack.c src/drag_stack_ui.c src/dock.c src/script.c src/glass.c src/json.c src/backend.c src/core.c src/rtc.c src/profile.c)
if(MSVC)
  target_compile_options(briks_cui PRIVATE /utf-8)
endif()
target_link_libraries(briks_cui PRIVATE cui winhttp bcrypt shell32 opengl32 ws2_32)

# Copia o backend local (briks-desktop-backend.exe) para ao lado do executável.
set(BRIKS_BACKEND_EXE "" CACHE FILEPATH "Caminho do briks-desktop-backend.exe")
if(BRIKS_BACKEND_EXE AND EXISTS "${BRIKS_BACKEND_EXE}")
  add_custom_command(TARGET briks_cui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:briks_cui>/backend
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BRIKS_BACKEND_EXE}" $<TARGET_FILE_DIR:briks_cui>/backend/briks-desktop-backend.exe)
endif()

# Copia o briks-core.exe (cargo build --release em briks-core) para ao lado do executável.
set(BRIKS_CORE_EXE "${CMAKE_CURRENT_SOURCE_DIR}/briks-core/target/release/briks-core.exe" CACHE FILEPATH "Caminho do briks-core.exe")
if(EXISTS "${BRIKS_CORE_EXE}")
  add_custom_command(TARGET briks_cui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BRIKS_CORE_EXE}" $<TARGET_FILE_DIR:briks_cui>/briks-core.exe)
endif()

add_executable(briks_smoke tests/smoke.c src/backend.c src/json.c src/layout.c src/drag_stack.c src/core.c)
target_link_libraries(briks_smoke PRIVATE winhttp bcrypt shell32 ws2_32)
add_custom_command(TARGET briks_smoke POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_FILE_DIR:briks_cui>/backend $<TARGET_FILE_DIR:briks_smoke>/backend)

# Overlay: janela sempre no topo, sem foco, cliques passando (segundo executável, sem dependências).
add_executable(briks_overlay WIN32 src/overlay.c)
target_compile_options(briks_overlay PRIVATE /utf-8)
target_link_libraries(briks_overlay PRIVATE user32 gdi32 shell32)
