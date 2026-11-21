# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Tramas compartidas de la mesh (mismo aura_trama.h que gateway y hojas)
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../componentes/aura_mesh)

# Versión del firmware: subirla en cada release
set(PROJECT_VER "0.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(aura-concentrador-pantalla)
