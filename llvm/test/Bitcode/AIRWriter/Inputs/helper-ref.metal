// A typed ref for typed-refs*.ll: Apple's AIR declares
// air.atomic.local.add.s.i32 with a typed i32 addrspace(3)* parameter.
// Compiled from this folder with Task 0 Step 6's form (xcrun-metal.sh's, with
// src/airconv/meson.build's arguments):
//   /usr/bin/xcrun -sdk macosx metal -x metal -I . -Xclang -main-file-name \
//     -Xclang helper-ref.metal -o helper-ref.air -c - -std=metal3.1 \
//     --target=air64-apple-macos14.0 < helper-ref.metal
#include <metal_stdlib>

using namespace metal;

// As air_tessellation.metal's get_next_index.
int
get_next_index(threadgroup int *out_count) {
  return atomic_fetch_add_explicit(reinterpret_cast<threadgroup atomic_int *>(out_count), 1, memory_order_relaxed);
}
