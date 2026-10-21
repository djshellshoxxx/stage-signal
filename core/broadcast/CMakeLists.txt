# broadcast shares the ffmpeg argument pieces and the bounded fan-out of the record module (see docs/specs/broadcast.md:
# the edge broadcast -> record needs an ARCHITECTURE.md note from the integrator). It does NOT include security headers.
ss_add_module(broadcast DEPS base model record)
target_include_directories(ss_broadcast PRIVATE ${SS_ROOT}/third_party)
