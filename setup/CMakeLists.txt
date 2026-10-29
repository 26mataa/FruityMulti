cmake_minimum_required(VERSION 3.25)

project(FruityMultiSetup LANGUAGES CXX)

add_executable(FruityMultiSetup WIN32 setup.cpp)

# Le plugin et l'agent sont embarques en ressources : l'installateur contient
# tout et fonctionne sans reseau. Le fichier payload.rc est genere par
# re/tools/faire_setup.py (il pointe sur setup/payload/).
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/payload.rc)
    target_sources(FruityMultiSetup PRIVATE payload.rc)
else ()
    message(WARNING "payload.rc absent : l'installateur sera vide. Lance re/tools/faire_setup.py")
endif ()

target_compile_features(FruityMultiSetup PRIVATE cxx_std_17)

target_link_libraries(FruityMultiSetup PRIVATE shell32)

if (MSVC)
    target_compile_options(FruityMultiSetup PRIVATE /utf-8)
endif ()

# binaire autonome : pas de runtime redistribuable a installer
set_target_properties(FruityMultiSetup PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
