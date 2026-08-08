#!/bin/bash
# Install the SCW-generated-file stubs into the vendored UnrealEngine tree.
# The sparse UE clone lacks Engine/Generated/ (the ShaderCompilerWorker emits
# these per-engine-version); the BasePassPixelShader.usf compile needs the
# uniform-buffer structs, the FPixelMaterialInputs placeholder substitution,
# and a few helpers that live behind dead #ifs or outside the corpus.
# Idempotent: skips files already present, appends the MaterialTemplate tail
# only once.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
GEN="$ROOT/third_party/UnrealEngine/Engine/Generated"
PRIV="$ROOT/third_party/UnrealEngine/Engine/Shaders/Private"

install -d "$GEN/UniformBuffers"
for f in GeneratedUniformBuffers.ush Material.ush UniformBuffers/Primitive.ush UniformBuffers/View.ush; do
    if [ ! -f "$GEN/$f" ]; then
        cp "$HERE/ue-stubs/$f" "$GEN/$f"
        echo "installed $GEN/$f"
    fi
done

# MaterialTemplate.ush is a real UE file: append the GetPrimitiveData overload
# stubs (the real ones sit behind #if GET_PRIMITIVE_DATA_OVERRIDE) once.
if ! grep -q "binc ue-stubs" "$PRIV/MaterialTemplate.ush" 2>/dev/null; then
    echo "" >> "$PRIV/MaterialTemplate.ush"
    cat "$HERE/ue-stubs/MaterialTemplate.ush.tail" >> "$PRIV/MaterialTemplate.ush"
    echo "appended GetPrimitiveData stubs to MaterialTemplate.ush"
fi
echo "ue stubs: ok"
