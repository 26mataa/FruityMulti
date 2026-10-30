cmake_minimum_required(VERSION 3.25)

project(FruityMultiBridge VERSION 1.0.0 LANGUAGES CXX)

set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../vst3sdk ${CMAKE_CURRENT_BINARY_DIR}/sdk)

set(SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vst3sdk)
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/imgui)

smtg_add_vst3plugin(FruityMultiBridge
    source/fmplugin.cpp
    source/fmgui.cpp
    source/fmstation.cpp
    ${SDK_DIR}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
    # Dear ImGui, utilise seulement pour le rendu (voir fmgui.cpp)
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

target_include_directories(FruityMultiBridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../vst3sdk
    ${CMAKE_CURRENT_SOURCE_DIR}/source
    ${IMGUI_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/polices
)


target_compile_features(FruityMultiBridge PRIVATE cxx_std_17)

target_link_libraries(FruityMultiBridge PRIVATE sdk base pluginterfaces ws2_32 d3d11 dxgi d3dcompiler)

# les libelles de la fenetre sont accentues : sources en UTF-8
if (MSVC)
    target_compile_options(FruityMultiBridge PRIVATE /utf-8)
endif ()
