option(UV_LIBRARY "use installed libuv instead of building from source")
option(UVW_LIBRARY "use installed uvw instead of building from source")
option(UV_TERMUX_PATCH "apply libuv_termux.diff" ${OS_ANDROID})

function(lanplay_ensure_dependency dep_name dep_dir dep_repo)
    if (EXISTS "${dep_dir}/CMakeLists.txt")
        return()
    endif()

    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
        message(STATUS "Installing ${dep_name} via submodule")
        execute_process(
            COMMAND git submodule update --init -- ${dep_dir}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
    endif()

    if (EXISTS "${dep_dir}/CMakeLists.txt")
        return()
    endif()

    message(STATUS "${dep_name} source missing. Cloning ${dep_repo}")
    execute_process(
        COMMAND git clone --depth 1 ${dep_repo} ${dep_dir}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RESULT_VARIABLE clone_result
    )

    if (NOT clone_result EQUAL 0)
        message(FATAL_ERROR "Failed to fetch ${dep_name}. Install it manually or clone ${dep_repo} into ${dep_dir}")
    endif()
endfunction()

if (UV_LIBRARY)
    find_package(Libuv REQUIRED)
    add_library(uv_a STATIC IMPORTED)
    set_target_properties(uv_a PROPERTIES
        IMPORTED_LOCATION ${LIBUV_LIBRARIES}
        INTERFACE_INCLUDE_DIRECTORIES ${LIBUV_INCLUDE_DIR}
    )
else()
    lanplay_ensure_dependency("libuv" "external/libuv" "https://github.com/libuv/libuv.git")
    add_subdirectory(external/libuv EXCLUDE_FROM_ALL)
    target_include_directories(uv_a INTERFACE external/libuv/include)
    if (UV_TERMUX_PATCH)
        message(STATUS "Apply libuv_termux.diff")
        execute_process(COMMAND git apply ../patch/libuv_termux.diff
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/external/libuv)
    else()
        execute_process(COMMAND git apply -R ../patch/libuv_termux.diff
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/external/libuv)
    endif()
endif()

if (UVW_LIBRARY)
    find_package(UVW REQUIRED)
    add_library(uvw STATIC IMPORTED)
    set_target_properties(uvw PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES ${LIBUVW_INCLUDE_DIR}
    )
else()
    lanplay_ensure_dependency("uvw" "external/uvw" "https://github.com/skypjack/uvw.git")
    add_subdirectory(external/uvw EXCLUDE_FROM_ALL)
    include_directories(external/uvw/src)
endif()
