add_executable(mosaicraft_desktop_worker EXCLUDE_FROM_ALL
    main.cpp
    EditorSession.cpp
)
target_link_libraries(mosaicraft_desktop_worker PRIVATE mosaicraft_core mosaicraft_cuda)
