#!/bin/sh
# Regression gate for hlsl_build's dynamically sized D3D9 uniform packer.
# These exact UE rows previously reached hlsl_lower.c:1019 and crashed after
# the global-name token-paste fix exposed their full global declarations.
set -eu

BINC=${BINC:-./binc}
UE_ROOT=${UE_ROOT:-../third_party/UnrealEngine}
STUB_ROOT=${STUB_ROOT:-../tools/hlsl-diff/ue-stubs}
COMMON="$UE_ROOT/Engine/Shaders/Private/Common.ush"

run_row() {
    shader=$1
    name=$2
    out="/tmp/binc-ue-uniform-capacity-${name}.metallib"
    log="/tmp/binc-ue-uniform-capacity-${name}.log"
    rm -f "$out" "$log"
    set +e
    "$BINC" --stage-all -T ps_5_0 \
        -I "$STUB_ROOT" \
        -I "$UE_ROOT" \
        -include "$COMMON" \
        -D COMPILER_DXC -D PLATFORM_WINDOWS -D SM6_PROFILE \
        -D COMPILER_SUPPORTS_ATTRIBUTES -D A8_SAMPLE_MASK=.r \
        -D WSVECTOR_IS_TILEOFFSET=1 \
        -D UE_LWC_RENDER_TILE_SIZE=2097152.0 \
        -D UE_LWC_RENDER_TILE_SIZE_RCP=4.76837158203125e-7 \
        -D UE_LWC_RENDER_TILE_SIZE_SQRT=1448.1546878700494 \
        -D UE_LWC_RENDER_TILE_SIZE_RSQRT=0.00069053396600248776 \
        -D 'WORKING_COLOR_SPACE_RGB_TO_XYZ_MAT=float3x3(1.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,1.0)' \
        -D 'XYZ_TO_RGB_WORKING_COLOR_SPACE_MAT=float3x3(1.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,1.0)' \
        -D 'SRGB_TO_WORKING_COLOR_SPACE_MAT=float3x3(1.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,1.0)' \
        -D WORKING_COLOR_SPACE_IS_SRGB=0 \
        "$UE_ROOT/Engine/Shaders/Private/$shader" -o "$out" >"$log" 2>&1
    rc=$?
    set -e
    if [ "$rc" -ge 128 ] || grep -Eqi 'segmentation fault|bus error|abort trap|stack-buffer-overflow' "$log"; then
        cat "$log"
        printf '%s: compiler crash (rc=%s)\n' "$shader" "$rc" >&2
        exit 1
    fi
    printf '%s: non-crash diagnostic/compile (rc=%s)\n' "$shader" "$rc"
}

run_row PostProcessCompositePrimitives.usf composite
run_row PostProcessMaterialShaders.usf material
printf '%s\n' 'UE UNIFORM CAPACITY NON-CRASH PASS'
