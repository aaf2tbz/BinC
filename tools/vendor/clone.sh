#!/bin/bash
# tools/vendor/clone.sh — (re)create the vendored corpus with sparse checkouts.
# Idempotent: skips directories that already exist. Run from anywhere.
set -u
cd "$(dirname "$0")/../.." || exit 1
mkdir -p third_party && cd third_party || exit 1

clone_sparse() {
  local dir="$1" url="$2"
  if [ -d "$dir" ]; then echo "skip $dir (exists)"; return 0; fi
  git clone --depth 1 --filter=blob:none --sparse "$url" "$dir" || return 1
  ( cd "$dir" && git sparse-checkout set --no-cone \
      '**/*.hlsl' '**/*.fx' '**/*.fxh' '**/*.dds' '**/LICENSE*' 'README.md' ) || return 1
  echo "ok $dir"
}

[ -d DirectXShaderCompiler ] || git clone --depth 1 https://github.com/microsoft/DirectXShaderCompiler.git
[ -d ShaderConductor ]      || git clone --depth 1 https://github.com/microsoft/ShaderConductor.git
clone_sparse DirectX-Graphics-Samples https://github.com/microsoft/DirectX-Graphics-Samples.git
clone_sparse DirectX-SDK-Samples      https://github.com/microsoft/DirectX-SDK-Samples.git
# UnrealEngine is a PRIVATE Epic repo — needs an org-authorized SSH key. The
# sparse set is Engine/Shaders (the .usf/.ush corpus) + the shader config.
if [ -d UnrealEngine ]; then
  echo "skip UnrealEngine (exists)"
else
  git clone --depth 1 --filter=tree:0 --sparse --single-branch \
    git@github.com:EpicGames/UnrealEngine.git UnrealEngine && \
  ( cd UnrealEngine && git sparse-checkout set --no-cone \
      'Engine/Shaders/**' 'Engine/Config/Shader**' ) && echo "ok UnrealEngine"
fi

echo "vendored corpus ready:"
du -sh DirectXShaderCompiler ShaderConductor DirectX-Graphics-Samples DirectX-SDK-Samples UnrealEngine 2>/dev/null
