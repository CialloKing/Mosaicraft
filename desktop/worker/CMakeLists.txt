add_executable(mosaicraft_desktop_worker EXCLUDE_FROM_ALL
    main.cpp
    EditorSession.cpp
    EditorCandidates.cpp
)
target_link_libraries(mosaicraft_desktop_worker PRIVATE mosaicraft_core mosaicraft_cuda)
target_compile_definitions(mosaicraft_desktop_worker PRIVATE
    MOSAICRAFT_TRIAL_CPU_TILES=$<BOOL:${MOSAICRAFT_TRIAL_CPU_TILES}>
    MOSAICRAFT_TRIAL_LBP_SIMD=$<BOOL:${MOSAICRAFT_TRIAL_LBP_SIMD}>
)
