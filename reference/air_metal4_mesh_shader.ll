; ModuleID = 'mesh.metal'
source_filename = "mesh.metal"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v29-apple-macosx27.0.0"

%struct._mesh_t = type opaque

; Function Attrs: mustprogress nounwind willreturn
define void @mesh_main(%struct._mesh_t addrspace(7)* %0, <3 x i32> noundef %1) local_unnamed_addr #0 {
  tail call void @air.set_primitive_count_mesh(%struct._mesh_t addrspace(7)* nocapture %0, i32 1) #2
  tail call void @air.set_position_mesh(%struct._mesh_t addrspace(7)* nocapture %0, i32 0, <4 x float> <float -1.000000e+00, float -1.000000e+00, float 0.000000e+00, float 1.000000e+00>) #2
  tail call void @air.set_position_mesh(%struct._mesh_t addrspace(7)* nocapture %0, i32 1, <4 x float> <float 1.000000e+00, float -1.000000e+00, float 0.000000e+00, float 1.000000e+00>) #2
  tail call void @air.set_position_mesh(%struct._mesh_t addrspace(7)* nocapture %0, i32 2, <4 x float> <float 0.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00>) #2
  tail call void @air.set_index_mesh(%struct._mesh_t addrspace(7)* nocapture %0, i32 0, i8 0) #2
  tail call void @air.set_index_mesh(%struct._mesh_t addrspace(7)* nocapture %0, i32 1, i8 1) #2
  tail call void @air.set_index_mesh(%struct._mesh_t addrspace(7)* nocapture %0, i32 2, i8 2) #2
  tail call void @air.set_primitive_data_mesh.v4f16(%struct._mesh_t addrspace(7)* nocapture %0, i32 0, i32 0, <4 x half> <half 0xH3C00, half 0xH0000, half 0xH0000, half 0xH3C00>) #2
  ret void
}

; Function Attrs: argmemonly mustprogress nounwind willreturn
declare void @air.set_primitive_count_mesh(%struct._mesh_t addrspace(7)* nocapture, i32) local_unnamed_addr #1

; Function Attrs: argmemonly mustprogress nounwind willreturn
declare void @air.set_position_mesh(%struct._mesh_t addrspace(7)* nocapture, i32, <4 x float>) local_unnamed_addr #1

; Function Attrs: argmemonly mustprogress nounwind willreturn
declare void @air.set_index_mesh(%struct._mesh_t addrspace(7)* nocapture, i32, i8) local_unnamed_addr #1

; Function Attrs: argmemonly mustprogress nounwind willreturn
declare void @air.set_primitive_data_mesh.v4f16(%struct._mesh_t addrspace(7)* nocapture, i32, i32, <4 x half>) local_unnamed_addr #1

attributes #0 = { mustprogress nounwind willreturn "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="128" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #1 = { argmemonly mustprogress nounwind willreturn }
attributes #2 = { argmemonly nounwind willreturn }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6, !7, !8}
!air.mesh = !{!9}
!air.compile_options = !{!19, !20, !21}
!llvm.ident = !{!22}
!air.version = !{!23}
!air.language_version = !{!24}
!air.source_file_name = !{!25}

!0 = !{i32 2, !"SDK Version", [2 x i32] [i32 27, i32 0]}
!1 = !{i32 1, !"wchar_size", i32 4}
!2 = !{i32 7, !"frame-pointer", i32 2}
!3 = !{i32 7, !"air.max_device_buffers", i32 31}
!4 = !{i32 7, !"air.max_constant_buffers", i32 31}
!5 = !{i32 7, !"air.max_threadgroup_buffers", i32 31}
!6 = !{i32 7, !"air.max_textures", i32 128}
!7 = !{i32 7, !"air.max_read_write_textures", i32 8}
!8 = !{i32 7, !"air.max_samplers", i32 16}
!9 = !{void (%struct._mesh_t addrspace(7)*, <3 x i32>)* @mesh_main, !10, !11}
!10 = !{}
!11 = !{!12, !18}
!12 = !{i32 0, !"air.mesh", !13, !"air.arg_type_name", !"mesh<VertexOut, PrimOut, 3, 1, triangle>", !"air.arg_name", !"m"}
!13 = !{!"air.mesh_type_info", !14, !16, i32 3, i32 1, !"air.triangle"}
!14 = !{!15}
!15 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!16 = !{!17}
!17 = !{!"air.mesh_primitive_data", i32 0, !"generated(5colorDv4_Dh)", !"air.arg_type_name", !"half4", !"air.arg_name", !"color"}
!18 = !{i32 1, !"air.threadgroup_position_in_grid", !"air.arg_type_name", !"uint3", !"air.arg_name", !"gid", !"air.arg_unused"}
!19 = !{!"air.compile.denorms_disable"}
!20 = !{!"air.compile.fast_math_enable"}
!21 = !{!"air.compile.framebuffer_fetch_enable"}
!22 = !{!"Apple metal version 32023.921 (metalfe-32023.921)"}
!23 = !{i32 2, i32 9, i32 0}
!24 = !{!"Metal", i32 4, i32 1, i32 0}
!25 = !{!"/private/tmp/binc_probe/mesh.metal"}
