ss_add_test(media DEPS media model)
target_include_directories(test_media PRIVATE ${SS_ROOT}/third_party)
# End-to-end test of the packaging gate executable (REQ-S15).
add_dependencies(test_media ss_asset_check)
target_compile_definitions(test_media PRIVATE SS_ASSET_CHECK_EXE="$<TARGET_FILE:ss_asset_check>")
