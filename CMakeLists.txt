cmake_minimum_required(VERSION 3.21)
project(clientosh VERSION 1.0.8 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

include(GNUInstallDirs)
include(CTest)

# ---- Version header -------------------------------------------------------
# Keep the numeric product version separate from the build channel. Official
# release builds pass "stable", rolling beta builds pass "beta" and a numeric
# CI build number, while an ordinary local build is clearly marked as "dev".
set(CLIENTOSH_BUILD_CHANNEL "dev" CACHE STRING
    "Build channel used in the displayed and package version (dev, beta, stable)")
set_property(CACHE CLIENTOSH_BUILD_CHANNEL PROPERTY STRINGS dev beta stable)
set(CLIENTOSH_BUILD_NUMBER "" CACHE STRING
    "Positive numeric build identifier required for beta builds")

if(NOT CLIENTOSH_BUILD_CHANNEL MATCHES "^(dev|beta|stable)$")
    message(FATAL_ERROR
        "CLIENTOSH_BUILD_CHANNEL must be one of: dev, beta, stable")
endif()

set(CLIENTOSH_VERSION_BUILD 0)
if(CLIENTOSH_BUILD_CHANNEL STREQUAL "beta")
    if(NOT CLIENTOSH_BUILD_NUMBER MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
            "Beta builds require a positive numeric CLIENTOSH_BUILD_NUMBER")
    endif()
    set(CLIENTOSH_VERSION_DISPLAY
        "${PROJECT_VERSION}-beta.${CLIENTOSH_BUILD_NUMBER}")
    set(CLIENTOSH_DEBIAN_VERSION
        "${PROJECT_VERSION}~beta.${CLIENTOSH_BUILD_NUMBER}")
    set(CLIENTOSH_RPM_RELEASE "0.beta.${CLIENTOSH_BUILD_NUMBER}")
    # PE file versions use four unsigned 16-bit numeric components.
    math(EXPR CLIENTOSH_VERSION_BUILD "${CLIENTOSH_BUILD_NUMBER} % 65535")
    if(CLIENTOSH_VERSION_BUILD EQUAL 0)
        set(CLIENTOSH_VERSION_BUILD 1)
    endif()
elseif(CLIENTOSH_BUILD_CHANNEL STREQUAL "stable")
    set(CLIENTOSH_VERSION_DISPLAY "${PROJECT_VERSION}")
    set(CLIENTOSH_DEBIAN_VERSION "${PROJECT_VERSION}")
    set(CLIENTOSH_RPM_RELEASE "1")
else()
    set(CLIENTOSH_VERSION_DISPLAY "${PROJECT_VERSION}-dev")
    set(CLIENTOSH_DEBIAN_VERSION "${PROJECT_VERSION}~dev")
    set(CLIENTOSH_RPM_RELEASE "0.dev")
endif()

set(CLIENTOSH_FILE_VERSION
    "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.${CLIENTOSH_VERSION_BUILD}")
message(STATUS "clientosh display version: ${CLIENTOSH_VERSION_DISPLAY}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/clientosh-version.txt"
    "${CLIENTOSH_VERSION_DISPLAY}\n")

# Generates src/version.h (from version.h.in) so About, --version and Qt all
# report the channel-aware version computed above.
configure_file(src/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/version.h" @ONLY)

# Windows resources and installer scripts receive both the channel-aware
# display version and a strictly numeric four-part file version.
if(WIN32)
    set(CLIENTOSH_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/installer")

    # Absolute (backslash) paths so the generated NSIS/Inno scripts work from any
    # working directory and on CI build machines.
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" CLIENTOSH_SOURCE_CMAKE)
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_BINARY_DIR}" CLIENTOSH_BINARY_CMAKE)
    string(REPLACE "/" "\\\\" CLIENTOSH_SOURCE_WIN "${CLIENTOSH_SOURCE_CMAKE}")
    string(REPLACE "/" "\\\\" CLIENTOSH_BINARY_WIN "${CLIENTOSH_BINARY_CMAKE}")

    # Substitution values consumed by the *.in templates (@ONLY).
    set(APP_VERSION   "${CLIENTOSH_VERSION_DISPLAY}")
    set(APP_FILE_VERSION "${CLIENTOSH_FILE_VERSION}")
    set(BUILD_DIR     "${CLIENTOSH_BINARY_WIN}")
    set(OUT_DIR       "${CLIENTOSH_SOURCE_WIN}")
    set(LICENSE_DIR   "${CLIENTOSH_SOURCE_WIN}\\LICENSE")
    set(ICO_DIR       "${CLIENTOSH_SOURCE_WIN}\\packaging\\clientosh.ico")
    # windres/rc.exe resolve resource paths against the working directory, so
    # pass the app icon as an absolute path using plain forward slashes.
    set(CLIENTOSH_ICO_ABS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/clientosh.ico")

    configure_file(clientosh.rc.in "${CLIENTOSH_GENERATED_DIR}/clientosh.rc" @ONLY)
    configure_file(packaging/clientosh_installer.nsi.in
                   "${CLIENTOSH_GENERATED_DIR}/clientosh_installer.nsi" @ONLY)
    configure_file(packaging/clientosh_installer.iss.in
                   "${CLIENTOSH_GENERATED_DIR}/clientosh_installer.iss" @ONLY)
else()
    set(CLIENTOSH_GENERATED_DIR "")
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Svg)

# Must match clientoshAddonAbi() in AddonTypes.cpp
if(WIN32)
    set(CLIENTOSH_ADDON_ABI "qt${Qt6_VERSION_MAJOR}.${Qt6_VERSION_MINOR}-win64-mingw")
elseif(APPLE)
    set(CLIENTOSH_ADDON_ABI "qt${Qt6_VERSION_MAJOR}.${Qt6_VERSION_MINOR}-macos")
else()
    set(CLIENTOSH_ADDON_ABI "qt${Qt6_VERSION_MAJOR}.${Qt6_VERSION_MINOR}-linux-x64")
endif()
message(STATUS "clientosh addon ABI: ${CLIENTOSH_ADDON_ABI}")

# ---- libssh (pkg-config, LIBSSH_ROOT, or system paths) ----
set(LIBSSH_ROOT "" CACHE PATH "Optional libssh install prefix (include/ + lib/)")

set(CLIENTOSH_LIBSSH_TARGET "")
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT LIBSSH_ROOT)
    pkg_check_modules(PC_LIBSSH QUIET IMPORTED_TARGET libssh)
    if(TARGET PkgConfig::PC_LIBSSH)
        set(CLIENTOSH_LIBSSH_TARGET PkgConfig::PC_LIBSSH)
    endif()
endif()

# ---- OpenSSL (crypto) for the vault (AES-256-GCM) ----
# Prefer the OpenSSL that lives beside libssh (e.g. under LIBSSH_ROOT) so the
# bundled runtime DLLs already copied next to the exe are the ones we use.
set(OPENSSL_ROOT_DIR "" CACHE PATH "Optional OpenSSL install prefix")
if(NOT OPENSSL_ROOT_DIR AND LIBSSH_ROOT)
    set(OPENSSL_ROOT_DIR "${LIBSSH_ROOT}")
endif()
# Homebrew OpenSSL is keg-only (not on the default search path). If we are on
# Apple and no OpenSSL root was provided, point find_package at the keg.
if(NOT OPENSSL_ROOT_DIR AND APPLE)
    foreach(_keg
            "/opt/homebrew/opt/openssl@3"
            "/opt/homebrew/opt/openssl"
            "/usr/local/opt/openssl@3"
            "/usr/local/opt/openssl")
        if(EXISTS "${_keg}/include/openssl/evp.h")
            set(OPENSSL_ROOT_DIR "${_keg}")
            break()
        endif()
    endforeach()
endif()
if(WIN32 AND OPENSSL_ROOT_DIR)
    set(OPENSSL_USE_STATIC_LIBS FALSE)
endif()
find_package(OpenSSL REQUIRED)
set(CLIENTOSH_OPENSSL_TARGET OpenSSL::Crypto)

if(NOT CLIENTOSH_LIBSSH_TARGET)
    set(_libssh_hints)
    if(LIBSSH_ROOT)
        list(APPEND _libssh_hints "${LIBSSH_ROOT}")
    endif()
    if(WIN32)
        list(APPEND _libssh_hints "C:/msys64/mingw64" "C:/msys64/ucrt64")
    elseif(APPLE)
        list(APPEND _libssh_hints "/opt/homebrew" "/usr/local")
    endif()

    find_path(LIBSSH_INCLUDE_DIR
        NAMES libssh/libssh.h
        HINTS ${_libssh_hints}
        PATH_SUFFIXES include
        REQUIRED
    )
    find_library(LIBSSH_LIBRARY
        NAMES ssh libssh
        HINTS ${_libssh_hints}
        PATH_SUFFIXES lib lib64
        REQUIRED
    )

    add_library(clientosh_libssh INTERFACE)
    target_include_directories(clientosh_libssh INTERFACE "${LIBSSH_INCLUDE_DIR}")
    target_link_libraries(clientosh_libssh INTERFACE "${LIBSSH_LIBRARY}")
    set(CLIENTOSH_LIBSSH_TARGET clientosh_libssh)
endif()

add_executable(clientosh
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/TerminalWidget.h
    src/TerminalWidget.cpp
    src/core/SshSession.h
    src/core/SshSession.cpp
    src/core/TelnetSession.h
    src/core/TelnetSession.cpp
    src/core/SerialSession.h
    src/core/SerialSession.cpp
    src/core/XmodemSender.h
    src/core/XmodemSender.cpp
    src/core/SessionProfile.h
    src/core/SessionManager.h
    src/core/SessionManager.cpp
    src/core/SftpClient.h
    src/core/SftpClient.cpp
    src/core/SftpCrossTransfer.h
    src/core/SftpCrossTransfer.cpp
    src/core/ServerStatsClient.h
    src/core/ServerStatsClient.cpp
    src/core/FontManager.h
    src/core/FontManager.cpp
    src/core/CryptoEngine.h
    src/core/CryptoEngine.cpp
    src/core/KeyringAdapter.h
    src/core/KeyringAdapter.cpp
    src/core/VaultManager.h
    src/core/VaultManager.cpp
    src/core/CliLaunch.h
    src/core/CliLaunch.cpp
    src/core/PrivateKeyLoader.h
    src/core/SshPasswordAuth.h
    src/core/sync/SyncKey.h
    src/core/sync/SyncKey.cpp
    src/core/sync/SyncCrypto.h
    src/core/sync/SyncCrypto.cpp
    src/core/sync/SyncPayload.h
    src/core/sync/SyncPayload.cpp
    src/core/sync/SyncGitHub.h
    src/core/sync/SyncGitHub.cpp
    src/core/sync/SyncWorker.h
    src/core/sync/SyncWorker.cpp
    src/core/sync/SyncController.h
    src/core/sync/SyncController.cpp
    src/core/sync/SyncConfig.h
    src/core/sync/SyncConfig.cpp
    src/core/addons/AddonTypes.h
    src/core/addons/AddonTypes.cpp
    src/core/addons/AddonConfig.h
    src/core/addons/AddonConfig.cpp
    src/core/addons/AddonStore.h
    src/core/addons/AddonStore.cpp
    src/core/addons/AddonHost.h
    src/core/addons/AddonHost.cpp
    src/PanelTypes.h
    src/DropOverlay.h
    src/DropOverlay.cpp
    src/PaneFrame.h
    src/PaneFrame.cpp
    src/DashboardPage.h
    src/DashboardPage.cpp
    src/SessionWorkspace.h
    src/SessionWorkspace.cpp
    src/SessionChip.h
    src/SessionChip.cpp
    src/TopNavBar.h
    src/TopNavBar.cpp
    src/SftpWindow.h
    src/SftpWindow.cpp
    src/platform/WindowStayOnTop.h
    src/platform/WindowStayOnTop.cpp
    src/platform/ConsoleAttach.h
    src/platform/ConsoleAttach.cpp
    src/ui/Motion.h
    src/ui/Motion.cpp
    src/ui/PasswordReveal.h
    src/ui/PasswordReveal.cpp
    resources.qrc
    $<$<BOOL:${WIN32}>:${CLIENTOSH_GENERATED_DIR}/clientosh.rc>
)

if(APPLE)
    target_sources(clientosh PRIVATE src/platform/WindowStayOnTop_mac.mm)
endif()

target_include_directories(clientosh PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(clientosh PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Svg
    ${CLIENTOSH_LIBSSH_TARGET}
    ${CLIENTOSH_OPENSSL_TARGET}
)

# ---- AI agent addon (lazy-loaded MODULE; not linked into clientosh) --------
add_library(clientosh_addon_ai_agent MODULE
    src/addons/ai_agent/AiAgentPlugin.cpp
    src/addons/ai_agent/AiAgentBridgeImpl.cpp
    src/addons/ai_agent/AiAgentClient.cpp
    src/addons/ai_agent/AiAgentPanel.cpp
    src/addons/ai_agent/AiAgentSettingsPage.cpp
    src/addons/ai_agent/ai_agent.qrc
)
target_include_directories(clientosh_addon_ai_agent PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/addons/ai_agent
)
target_link_libraries(clientosh_addon_ai_agent PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Svg
)
set_target_properties(clientosh_addon_ai_agent PROPERTIES
    PREFIX ""
    OUTPUT_NAME "ai_agent"
    AUTOMOC ON
    AUTORCC ON
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_dependencies(clientosh clientosh_addon_ai_agent)

add_custom_command(TARGET clientosh_addon_ai_agent POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        "-DPLUGIN_FILE=$<TARGET_FILE:clientosh_addon_ai_agent>"
        "-DBUNDLE_DIR=$<TARGET_FILE_DIR:clientosh>/addons-bundle"
        "-DABI=${CLIENTOSH_ADDON_ABI}"
        "-DVERSION=${PROJECT_VERSION}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/bundle_ai_agent_addon.cmake"
    VERBATIM
)

if(BUILD_TESTING)
    add_executable(clientosh_xmodem_sender_test
        tests/XmodemSenderTest.cpp
        src/core/XmodemSender.cpp
    )
    target_include_directories(clientosh_xmodem_sender_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(clientosh_xmodem_sender_test PRIVATE Qt6::Core)
    add_test(NAME xmodem_sender COMMAND clientosh_xmodem_sender_test)

    add_executable(clientosh_profile_auth_test
        tests/ProfileAuthTest.cpp
        src/core/sync/SyncPayload.cpp
    )
    target_include_directories(clientosh_profile_auth_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    )
    target_link_libraries(clientosh_profile_auth_test PRIVATE Qt6::Core)
    add_test(NAME profile_auth_migration COMMAND clientosh_profile_auth_test)

    add_executable(clientosh_vault_fallback_test
        tests/VaultFallbackTest.cpp
        src/core/CryptoEngine.cpp
        src/core/KeyringAdapter.cpp
    )
    target_include_directories(clientosh_vault_fallback_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    )
    target_link_libraries(clientosh_vault_fallback_test PRIVATE
        Qt6::Core
        ${CLIENTOSH_OPENSSL_TARGET}
    )
    if(APPLE)
        target_link_libraries(clientosh_vault_fallback_test PRIVATE
            "-framework Security" "-framework IOKit" "-framework CoreFoundation")
    elseif(WIN32)
        target_link_libraries(clientosh_vault_fallback_test PRIVATE advapi32)
    endif()
    add_test(NAME vault_file_fallback COMMAND clientosh_vault_fallback_test)
endif()

if(APPLE)
    target_link_libraries(clientosh PRIVATE
        "-framework Security"
        "-framework IOKit"
        "-framework CoreFoundation"
        "-framework AppKit"
    )
endif()

if(WIN32)
    target_link_libraries(clientosh PRIVATE ws2_32 advapi32)
    # WINDOWS subsystem: no console flash when opening the GUI with no args.
    # CLI output uses AttachConsole / WriteConsoleW (see ConsoleAttach.cpp).
    set_target_properties(clientosh PROPERTIES WIN32_EXECUTABLE TRUE)

    # Bundle runtime DLLs next to the exe when building against MSYS2/custom prefix
    if(LIBSSH_ROOT)
        set(_dll_candidates
            libssh.dll
            libcrypto-3-x64.dll
            libssl-3-x64.dll
            libcrypto-3.dll
            libssl-3.dll
            zlib1.dll
            libwinpthread-1.dll
            libgcc_s_seh-1.dll
            libstdc++-6.dll
        )
        foreach(_dll IN LISTS _dll_candidates)
            set(_dll_path "${LIBSSH_ROOT}/bin/${_dll}")
            if(EXISTS "${_dll_path}")
                add_custom_command(TARGET clientosh POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_dll_path}"
                        "$<TARGET_FILE_DIR:clientosh>"
                    VERBATIM
                )
            endif()
        endforeach()
    endif()

    # Bundle the Qt6 runtime DLLs and the MinGW C++ runtime so the exe launches
    # from a plain shell without the Qt toolchain on PATH.
    get_target_property(_qt_bin_dir Qt6::Core IMPORTED_LOCATION)
    if(_qt_bin_dir)
        get_filename_component(_qt_bin_dir "${_qt_bin_dir}" DIRECTORY)
    endif()
    set(_qt_dlls
        Qt6Core.dll
        Qt6Gui.dll
        Qt6Network.dll
        Qt6Svg.dll
        Qt6Widgets.dll
    )
    foreach(_dll IN LISTS _qt_dlls)
        if(EXISTS "${_qt_bin_dir}/${_dll}")
            add_custom_command(TARGET clientosh POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_qt_bin_dir}/${_dll}"
                    "$<TARGET_FILE_DIR:clientosh>"
                VERBATIM
            )
        endif()
    endforeach()

    # MinGW C++ runtime (from LIBSSH_ROOT when it is an MSYS2 prefix, else from
    # the Qt toolchain) plus the Qt platform plugin.
    set(_gcc_dlls libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
    foreach(_dll IN LISTS _gcc_dlls)
        set(_gcc_path "")
        if(LIBSSH_ROOT AND EXISTS "${LIBSSH_ROOT}/bin/${_dll}")
            set(_gcc_path "${LIBSSH_ROOT}/bin/${_dll}")
        elseif(CMAKE_CXX_COMPILER)
            get_filename_component(_gcc_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
            if(EXISTS "${_gcc_dir}/${_dll}")
                set(_gcc_path "${_gcc_dir}/${_dll}")
            endif()
        endif()
        if(_gcc_path)
            add_custom_command(TARGET clientosh POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_gcc_path}" "$<TARGET_FILE_DIR:clientosh>"
                VERBATIM
            )
        endif()
    endforeach()

    if(_qt_bin_dir AND EXISTS "${_qt_bin_dir}/../plugins/platforms/qwindows.dll")
        add_custom_command(TARGET clientosh POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:clientosh>/platforms"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_qt_bin_dir}/../plugins/platforms/qwindows.dll"
                "$<TARGET_FILE_DIR:clientosh>/platforms"
            VERBATIM
        )
        # Image format plugins (PNG/ICO) used by the icon and app.
        if(EXISTS "${_qt_bin_dir}/../plugins/imageformats")
            add_custom_command(TARGET clientosh POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${_qt_bin_dir}/../plugins/imageformats"
                    "$<TARGET_FILE_DIR:clientosh>/imageformats"
                VERBATIM
            )
        endif()
        if(EXISTS "${_qt_bin_dir}/../plugins/iconengines")
            add_custom_command(TARGET clientosh POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${_qt_bin_dir}/../plugins/iconengines"
                    "$<TARGET_FILE_DIR:clientosh>/iconengines"
                VERBATIM
            )
        endif()
        if(EXISTS "${_qt_bin_dir}/../plugins/tls")
            add_custom_command(TARGET clientosh POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${_qt_bin_dir}/../plugins/tls"
                    "$<TARGET_FILE_DIR:clientosh>/tls"
                VERBATIM
            )
        endif()
    endif()

    # Deploy the complete Qt runtime (platforms/styles/iconengines/tls/imageformats
    # plugins plus any extra core DLLs such as opengl32sw) via windeployqt when it
    # ships with the Qt used for this build. This mirrors the release CI and makes
    # the built exe a self-contained, correctly-rendering bundle regardless of which
    # Qt toolchain is on PATH during the build.
    if(_qt_bin_dir AND EXISTS "${_qt_bin_dir}/windeployqt.exe")
        add_custom_command(TARGET clientosh POST_BUILD
            COMMAND "${_qt_bin_dir}/windeployqt.exe"
                    --no-translations --release
                    "$<TARGET_FILE_DIR:clientosh>/clientosh.exe"
            COMMAND ${CMAKE_COMMAND} -E echo "windeployqt: bundled Qt runtime from ${_qt_bin_dir}"
            VERBATIM
        )
    endif()
elseif(APPLE)
    set_target_properties(clientosh PROPERTIES
        MACOSX_BUNDLE FALSE
        INSTALL_RPATH "@executable_path/../lib"
    )
else()
    set_target_properties(clientosh PROPERTIES
        INSTALL_RPATH "$ORIGIN"
    )
endif()

# ---- Install (used by .deb / make install) ----
install(TARGETS clientosh
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE DESTINATION .
)

if(UNIX AND NOT APPLE)
    install(FILES packaging/clientosh.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
    )
    install(FILES icons/terminal.svg
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
        RENAME clientosh.svg
    )
    install(FILES packaging/clientosh.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo
        RENAME io.github.clientosh.metainfo.xml
    )
endif()

# ---- CPack (.deb on Linux) ----
set(CPACK_PACKAGE_NAME "clientosh")
set(CPACK_PACKAGE_VENDOR "clientosh")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Raw dark-gray SSH client (Qt 6 + libssh)")
set(CPACK_PACKAGE_DESCRIPTION
    "clientosh is a lightweight SSH client with a dark monospace UI, session management, detachable terminals, and scrollback.")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_CONTACT "clientosh <clientosh@localhost>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/hdmain/clientosh")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
    set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
endif()

if(UNIX AND NOT APPLE)
    # Produce both a .deb (Debian/Ubuntu) and a .rpm (Fedora/RHEL/openSUSE) from
    # the same installation tree via CPack.
    set(CPACK_GENERATOR "DEB;RPM")

    # ---- Debian / Ubuntu (.deb) ----
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "clientosh <clientosh@localhost>")
    set(CPACK_DEBIAN_PACKAGE_VERSION "${CLIENTOSH_DEBIAN_VERSION}")
    set(CPACK_DEBIAN_PACKAGE_SECTION "net")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
    # Prefer auto shlibdeps when dpkg-dev is installed; keep explicit fallback deps.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS
        "libqt6widgets6 | libqt6widgets6t64, libqt6network6 | libqt6network6t64, libqt6svg6 | libqt6svg6t64, libssh-4, libssl3"
    )

    # ---- RPM (Fedora / RHEL / openSUSE) ----
    set(CPACK_RPM_PACKAGE_SUMMARY "Raw dark-gray SSH client (Qt 6 + libssh)")
    set(CPACK_RPM_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_RPM_PACKAGE_RELEASE "${CLIENTOSH_RPM_RELEASE}")
    set(CPACK_RPM_PACKAGE_LICENSE "MIT")
    set(CPACK_RPM_PACKAGE_GROUP "Applications/Internet")
    set(CPACK_RPM_PACKAGE_URL "https://github.com/hdmain/clientosh")
    set(CPACK_RPM_PACKAGE_RELOCATABLE OFF)
    set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
    set(CPACK_RPM_PACKAGE_AUTOREQ ON)
    # libssh is dlopen'd / linked; listed deps mirror the Debian ones.
    set(CPACK_RPM_PACKAGE_REQUIRES
        "qt6-qtbase, qt6-qtsvg, libssh, openssl-libs"
    )
endif()

include(CPack)
