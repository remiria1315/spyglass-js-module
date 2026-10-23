set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_LIBC OFF CACHE BOOL "" FORCE)

FetchContent_Declare(quickjs
        GIT_REPOSITORY https://github.com/quickjs-ng/quickjs.git
        GIT_TAG v0.17.0
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL)

FetchContent_MakeAvailable(quickjs)
