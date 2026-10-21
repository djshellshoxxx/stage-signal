ss_add_module(media DEPS base model)
target_include_directories(ss_media PRIVATE ${SS_ROOT}/third_party)

# REQ-S15: packaging gate. Exit 0 = every asset verified, 1 = absent/mismatched entry, 2 = usage/parse error.
add_executable(ss_asset_check tools/asset_check_main.cpp)
target_include_directories(ss_asset_check PRIVATE ${SS_ROOT}/third_party)
target_link_libraries(ss_asset_check PRIVATE ss_media)
target_compile_features(ss_asset_check PRIVATE cxx_std_17)
ss_apply_warnings(ss_asset_check)
