# Generated Faust C++

`loop.cpp` in this directory is the fallback pre-generated output of:

```
faust -lang cpp -cn AloopLoopDsp -I ../.. -I ../../effects/home/faust ../aloop.dsp -o loop.cpp
```

CMakeLists.txt uses this automatically when `faust` isn't found on the build
machine (see the root `CMakeLists.txt`'s `FAUST_BIN` check). Regenerate it
after any change to `dsp/loop.dsp`, `dsp/aloop.dsp`, `dsp/effects_runtime.dsp`,
or any `effects/home/faust/*.dsp` file, on a machine that has `faust`
installed, and commit the regenerated file — CI's `generate-faust-cpp` job
also does this automatically on Ubuntu and hands the result to the other OS
build jobs, so a stale committed copy here only matters for a fully offline
local build.
