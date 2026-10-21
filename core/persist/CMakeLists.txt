ss_add_module(persist DEPS base model security)
target_include_directories(ss_persist PRIVATE ${SS_ROOT}/third_party)
