# Developer Guide

```sh
cmake -S . -B build-core -DCMAKE_BUILD_TYPE=Release -DSS_BUILD_TESTS=ON -DSS_BUILD_APP=OFF
cmake --build build-core -j 8
ctest --test-dir build-core --output-on-failure    # 19 of 19 pass on this snapshot
```

- Core modules are discovered automatically from `core/<module>/CMakeLists.txt` (`CMakeLists.txt`). The `base` module is built first.
- The JUCE app needs `-DSS_BUILD_APP=ON` (not built in this pass).
- Use the specification's section 18 (testing) and 19 (build and release) as the source of truth.
