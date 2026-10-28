# M0 gate lives outside ctest: the deliverable is
# docs/devlogs/2026-10-27-03-M0探针结果.md, filled with real probe output.
#
# This file exists so the integration tests of M1+ (01_tls_read.cpp,
# 02_pure_native.cpp, ...) have a place to land. Until then it registers the
# M0 spike as a single test, so `ctest` re-runs every probe.

add_test(NAME m0_probes COMMAND unicorn_spike all)
