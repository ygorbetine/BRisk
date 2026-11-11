cmake_minimum_required(VERSION 3.20)
project(briks_desktop_cui C)
if(WIN32)
  enable_language(RC)   # icone do executavel (src/briks.rc)
endif()

include(FetchContent)
FetchContent_Declare(cui
  GIT_REPOSITORY https://github.com/yappologistic/CUI.git
  GIT_TAG        e8688b8fd85c7b07afbddbac5fc748a905c12be0
  # gancho de render (cui_app_set_render_override) usado por src/glass.c; idempotente
  PATCH_COMMAND  ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_SOURCE_DIR}/patches/apply.cmake")
FetchContent_MakeAvailable(cui)

add_executable(briks_cui WIN32 src/main.c src/theme.c src/theme_cfg.c src/perf.c src/ui_auth.c src/ui_social.c src/ui_call.c src/app.c src/chat.c src/ui_history.c src/layout.c src/drag_stack.c src/drag_stack_ui.c src/dock.c src/script.c src/glass.c src/json.c src/backend.c src/core.c src/rtc.c src/profile.c src/msgui.c src/migrate.c src/update.c src/ui_update.c src/settings.c src/themes.c src/account.c src/ui_account.c src/ui_share.c src/ui_friends.c src/ui_groups.c)
if(WIN32)
  target_sources(briks_cui PRIVATE src/briks.rc)
endif()
if(MSVC)
  target_compile_options(briks_cui PRIVATE /utf-8)
endif()
target_link_libraries(briks_cui PRIVATE cui comdlg32 winmm winhttp bcrypt crypt32 shell32 opengl32 ws2_32)

# Logos do login (Google e Discord) ao lado do executável.
add_custom_command(TARGET briks_cui POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets/login" $<TARGET_FILE_DIR:briks_cui>/assets/login)

# Copia o backend local (briks-desktop-backend.exe) para ao lado do executável.
set(BRIKS_BACKEND_EXE "" CACHE FILEPATH "Caminho do briks-desktop-backend.exe")
if(NOT BRIKS_BACKEND_EXE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../desktop-backend/target/release/briks-desktop-backend.exe")
  set(BRIKS_BACKEND_EXE "${CMAKE_CURRENT_SOURCE_DIR}/../../desktop-backend/target/release/briks-desktop-backend.exe")
endif()
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

set(BRIKS_VERSION "1.3.0" CACHE STRING "Versao do app (usada pelo atualizador; o instalador recebe a sua em scripts/build-installer.ps1)")
option(BRIKS_UPDATE_DEFAULT_ON "Atualizacao automatica ligada por padrao no app instalado (build do instalador)" OFF)
target_compile_definitions(briks_cui PRIVATE BRIKS_APP_VERSION="${BRIKS_VERSION}")
if(BRIKS_UPDATE_DEFAULT_ON)
  target_compile_definitions(briks_cui PRIVATE BRIKS_UPDATE_DEFAULT_ON=1)
endif()
add_executable(briks_smoke tests/smoke.c src/update.c src/backend.c src/json.c src/layout.c src/drag_stack.c src/core.c)
target_link_libraries(briks_smoke PRIVATE winhttp bcrypt crypt32 shell32 ws2_32)
add_executable(briks_groups_test tests/groups_test.c src/json.c)
target_link_libraries(briks_groups_test PRIVATE cui bcrypt shell32 user32 gdi32 opengl32)
# briks_smoke fica na mesma pasta do briks_cui e ja enxerga backend/ ao lado (sem copia)

# Overlay: janela sempre no topo, sem foco, cliques passando (segundo executável, sem dependências).
add_executable(briks_overlay WIN32 src/overlay.c)
target_compile_options(briks_overlay PRIVATE /utf-8)
target_link_libraries(briks_overlay PRIVATE user32 gdi32 shell32)
