cmake_minimum_required(VERSION 3.21)
project(Iris LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT WIN32)
    message(FATAL_ERROR "Iris usa le API di Windows: va compilata su Windows.")
endif()

# --- Un solo .exe, senza DLL (come Voice to Chat) -------------------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- Processori supportati: AVX2 + FMA + F16C, non "nativo" -------------------
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
set(GGML_AVX    ON  CACHE BOOL "" FORCE)
set(GGML_AVX2   ON  CACHE BOOL "" FORCE)
set(GGML_FMA    ON  CACHE BOOL "" FORCE)
set(GGML_F16C   ON  CACHE BOOL "" FORCE)
set(GGML_BMI2   ON  CACHE BOOL "" FORCE)
set(GGML_AVX512 OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)

# --- Scheda video (Vulkan): vulkan-1.dll si carica solo quando serve ----------
set(GGML_VULKAN ON CACHE BOOL "" FORCE)

# --- llama.cpp PRIMA di whisper.cpp (whisper riusa la stessa ggml) ------------
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON   OFF CACHE BOOL "" FORCE)
set(LLAMA_CURL           OFF CACHE BOOL "" FORCE)
# libmtmd da sola (senza gli strumenti): serve alla voce neurale Qwen3-TTS.
set(LLAMA_BUILD_MTMD     ON  CACHE BOOL "" FORCE)
set(MTMD_VIDEO           OFF CACHE BOOL "" FORCE)
add_subdirectory(external/llama.cpp)

set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
add_subdirectory(external/whisper.cpp)

# --- Versione: il file VERSION (exe e aggiornamento automatico) -----------------
# pubblica.bat la scrive prima di compilare e pubblicare la release su GitHub.
file(READ "${CMAKE_SOURCE_DIR}/VERSION" IRIS_VERSION)
string(STRIP "${IRIS_VERSION}" IRIS_VERSION)
if (NOT IRIS_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "Il file VERSION deve contenere una versione come 1.2.0 (trovato: '${IRIS_VERSION}')")
endif()
set(IRIS_VERSION_MAJOR ${CMAKE_MATCH_1})
set(IRIS_VERSION_MINOR ${CMAKE_MATCH_2})
set(IRIS_VERSION_PATCH ${CMAKE_MATCH_3})
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/VERSION")
configure_file(src/version.h.in "${CMAKE_BINARY_DIR}/generated/version.h" @ONLY)
message(STATUS "Iris ${IRIS_VERSION}")

# --- Il nostro codice -----------------------------------------------------------
# "iris_core": tutto tranne le finestre, così lo usano sia il programma sia le
# prove automatiche (iris_tests).
add_library(iris_core STATIC
    src/agent.cpp
    src/app_paths.cpp
    src/app_settings.cpp
    src/assistant.cpp
    src/audio_capture.cpp
    src/autostart.cpp
    src/axis_config.cpp
    src/axis_input.cpp
    src/axis_server.cpp
    src/chat_format.cpp
    src/chat_parser.cpp
    src/command_matcher.cpp
    src/game_chat.cpp
    src/game_controller.cpp
    src/game_input.cpp
    src/game_log.cpp
    src/game_profile.cpp
    src/gamer_slang.cpp
    src/glossary.cpp
    src/gpu.cpp
    src/hotkey_listener.cpp
    src/html_utils.cpp
    src/http_client.cpp
    src/input_sender.cpp
    src/joystick_input.cpp
    src/language_guess.cpp
    src/llm_engine.cpp
    src/log.cpp
    src/mining_decoder.cpp
    src/mining_image.cpp
    src/mining_log.cpp
    src/mining_scanner.cpp
    src/model_downloader.cpp
    src/ppocr.cpp
    src/refinery.cpp
    src/ocr_engine.cpp
    src/screen_capture.cpp
    src/screen_match.cpp
    src/screen_ocr.cpp
    src/translation_prompt.cpp
    src/neural_voice.cpp
    src/speech_text.cpp
    src/speech_utils.cpp
    src/text_utils.cpp
    src/tools.cpp
    src/tts_engine.cpp
    src/updater.cpp
    src/vad_segmenter.cpp
    src/voice_detector.cpp
    src/voice_effect.cpp
    src/wake_word.cpp
    src/whisper_engine.cpp
)
# nlohmann/json: la copia che llama.cpp porta con sé (un solo header).
# onnxruntime_c_api.h: solo l'header (MIT); la DLL si carica al volo (vedi ppocr.h). Lo scarica compila.bat.
if (NOT EXISTS "${CMAKE_SOURCE_DIR}/external/onnxruntime/include/onnxruntime_c_api.h")
    message(FATAL_ERROR "Manca external/onnxruntime/include/onnxruntime_c_api.h: lancia compila.bat (lo scarica).")
endif()
target_include_directories(iris_core PUBLIC src external/miniaudio external/llama.cpp/vendor "${CMAKE_BINARY_DIR}/generated" external/onnxruntime/include)
target_link_libraries(iris_core PUBLIC
    whisper llama mtmd
    user32 gdi32 winhttp shell32 winmm dinput8 dxguid dxgi ole32 oleaut32 propsys windowsapp bcrypt ws2_32
    delayimp
)
target_link_options(iris_core INTERFACE /DELAYLOAD:vulkan-1.dll)
target_compile_definitions(iris_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
# C++20 per il nostro codice (C++/WinRT della voce di Windows).
set_target_properties(iris_core PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(iris_core PUBLIC /utf-8 /W3 /permissive- /bigobj)

# --- L'interfaccia: HTML/CSS/JS dentro WebView2 (già in Windows 10/11) ----------
# Il kit (header + libreria statica, niente DLL in più) lo scarica compila.bat.
set(WEBVIEW2_DIR "${CMAKE_SOURCE_DIR}/external/webview2/build/native")
if (NOT EXISTS "${WEBVIEW2_DIR}/include/WebView2.h")
    message(FATAL_ERROR "Manca il kit di WebView2 in external/webview2: lancia compila.bat (lo scarica da NuGet).")
endif()
# Le pagine finiscono dentro l'exe (risorse di iris.rc): se cambiano, si ricompila la risorsa.
set(IRIS_WEB_FILES
    "${CMAKE_SOURCE_DIR}/src/ui/web/index.html"
    "${CMAKE_SOURCE_DIR}/src/ui/web/app.css"
    "${CMAKE_SOURCE_DIR}/src/ui/web/app.js"
    "${CMAKE_SOURCE_DIR}/src/ui/web/axis.js"
    "${CMAKE_SOURCE_DIR}/src/ui/web/log.js"
    "${CMAKE_SOURCE_DIR}/src/ui/web/commands.js"
    "${CMAKE_SOURCE_DIR}/src/ui/axis/overlay.html"
    "${CMAKE_SOURCE_DIR}/src/ui/axis/index.html"
)
set_source_files_properties(src/iris.rc PROPERTIES OBJECT_DEPENDS "${IRIS_WEB_FILES}")

add_executable(iris WIN32
    src/main.cpp
    src/ui/ui.cpp
    src/ui/main_window.cpp
    src/ui/web_bridge.cpp
    src/ui/axis_overlay.cpp
    src/ui/axis_panes.cpp
    src/ui/hud_host.cpp
    src/ui/quick_bar.cpp
    src/ui/orb.cpp
    src/overlay.cpp
    src/ocr_overlay.cpp
    src/mining_overlay.cpp
    src/region_selector.cpp
    src/iris.rc
    src/iris.manifest
)
target_include_directories(iris PRIVATE "${WEBVIEW2_DIR}/include")
target_link_libraries(iris PRIVATE iris_core comctl32 comdlg32 uxtheme dwmapi gdiplus shcore shlwapi version
    "${WEBVIEW2_DIR}/x64/WebView2LoaderStatic.lib")
set_target_properties(iris PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# La sfera (ui/orb) anche nelle prove: --sfera ne disegna un fotogramma su sfondo trasparente.
add_executable(iris_tests tests/tests.cpp src/ui/orb.cpp src/ui/ui.cpp src/ui/axis_panes.cpp src/ui/quick_bar.cpp)
target_link_libraries(iris_tests PRIVATE iris_core comctl32 uxtheme dwmapi gdiplus shcore)
set_target_properties(iris_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
