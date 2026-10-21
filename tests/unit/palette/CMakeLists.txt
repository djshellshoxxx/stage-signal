ss_add_test(palette DEPS palette)
# The completeness test parses the real action enum so a new ActionId without metadata fails the build's tests.
target_compile_definitions(test_palette PRIVATE "SS_ACTIONS_HPP=\"${SS_ROOT}/core/base/include/ss/base/actions.hpp\"")
