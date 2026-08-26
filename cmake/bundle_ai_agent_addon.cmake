# Generates addons-bundle/index.json for the AI agent plugin next to the exe.
cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED PLUGIN_FILE OR NOT DEFINED BUNDLE_DIR OR NOT DEFINED ABI OR NOT DEFINED VERSION)
  message(FATAL_ERROR "bundle_ai_agent_addon.cmake requires PLUGIN_FILE, BUNDLE_DIR, ABI, VERSION")
endif()

if(NOT EXISTS "${PLUGIN_FILE}")
  message(FATAL_ERROR "Plugin not found: ${PLUGIN_FILE}")
endif()

get_filename_component(PLUGIN_NAME "${PLUGIN_FILE}" NAME)
file(MAKE_DIRECTORY "${BUNDLE_DIR}/ai-agent")
file(COPY "${PLUGIN_FILE}" DESTINATION "${BUNDLE_DIR}/ai-agent")

file(SHA256 "${BUNDLE_DIR}/ai-agent/${PLUGIN_NAME}" PLUGIN_SHA256)
file(SIZE "${BUNDLE_DIR}/ai-agent/${PLUGIN_NAME}" PLUGIN_SIZE)

# file:// URL with forward slashes (Qt QUrl accepts this on Windows).
file(TO_CMAKE_PATH "${BUNDLE_DIR}/ai-agent/${PLUGIN_NAME}" PLUGIN_PATH_CMAKE)
string(REPLACE "\\" "/" PLUGIN_PATH_UNIX "${PLUGIN_PATH_CMAKE}")
set(PLUGIN_URL "file:///${PLUGIN_PATH_UNIX}")

string(TIMESTAMP TODAY "%Y-%m-%d")
configure_file(
  "${CMAKE_CURRENT_LIST_DIR}/ai_agent_index.json.in"
  "${BUNDLE_DIR}/index.json"
  @ONLY
)

message(STATUS "Bundled AI agent addon -> ${BUNDLE_DIR} (${PLUGIN_SHA256})")
