
Xclipse 940 Superwrapper - Full package (drop-in source + build + workflow)

Contents:
 - etc/exynostools/profiles/vendor/xilinx_xc/manifest.json  (authoritative user manifest)
 - usr/lib/libxeno_wrapper.c  (full wrapper source)
 - usr/lib/bc_emulate.c      (BC fallback helpers)
 - usr/share/vulkan/icd.d/xeno_wrapper.json
 - usr/share/vulkan/implicit_layer.d/xclipse_autotune.json
 - usr/share/vulkan/explicit_layer.d/xclipse_debughud.json
 - CMakeLists.txt
 - build/build_android_aarch64.sh
 - .github/workflows/build.yml

Usage:
 - On GitHub: push this repo, run the workflow (Actions -> Build Xclipse Wrapper & Collect Feature Dump)
 - On-device: build libxeno_wrapper.so with NDK or copy compiled .so and install jsons to expected paths
 - Logs and feature dump will be written to /data/local/tmp or /var/log paths as configured. Upload them back to me for tuning.
