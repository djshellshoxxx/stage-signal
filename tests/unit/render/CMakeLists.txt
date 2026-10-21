ss_add_test(render DEPS model render)
target_compile_definitions(test_render PRIVATE SS_RENDER_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
