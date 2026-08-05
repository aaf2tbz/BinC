; ModuleID = 's1.metal'
source_filename = "s1.metal"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v29-apple-macosx27.0.0"

%struct.Particle = type { float, float, float, float, float, float }

; Function Attrs: argmemonly mustprogress nofree norecurse nosync nounwind willreturn
define void @step(%struct.Particle addrspace(1)* nocapture noundef "air-buffer-no-alias" %0, float addrspace(2)* nocapture noundef readonly align 4 dereferenceable(4) "air-buffer-no-alias" %1) local_unnamed_addr #0 {
  %3 = getelementptr inbounds %struct.Particle, %struct.Particle addrspace(1)* %0, i64 0, i32 3
  %4 = load float, float addrspace(1)* %3, align 4, !tbaa !22, !alias.scope !27, !noalias !30
  %5 = load float, float addrspace(2)* %1, align 4, !tbaa !32, !alias.scope !30, !noalias !27
  %6 = fmul fast float %5, %4
  %7 = getelementptr inbounds %struct.Particle, %struct.Particle addrspace(1)* %0, i64 0, i32 0
  %8 = load float, float addrspace(1)* %7, align 4, !tbaa !33, !alias.scope !27, !noalias !30
  %9 = fadd fast float %8, %6
  store float %9, float addrspace(1)* %7, align 4, !tbaa !33, !alias.scope !27, !noalias !30
  ret void
}

attributes #0 = { argmemonly mustprogress nofree norecurse nosync nounwind willreturn "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="0" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6, !7, !8}
!air.kernel = !{!9}
!air.compile_options = !{!15, !16, !17}
!llvm.ident = !{!18}
!air.version = !{!19}
!air.language_version = !{!20}
!air.source_file_name = !{!21}

!0 = !{i32 2, !"SDK Version", [2 x i32] [i32 27, i32 0]}
!1 = !{i32 1, !"wchar_size", i32 4}
!2 = !{i32 7, !"frame-pointer", i32 2}
!3 = !{i32 7, !"air.max_device_buffers", i32 31}
!4 = !{i32 7, !"air.max_constant_buffers", i32 31}
!5 = !{i32 7, !"air.max_threadgroup_buffers", i32 31}
!6 = !{i32 7, !"air.max_textures", i32 128}
!7 = !{i32 7, !"air.max_read_write_textures", i32 8}
!8 = !{i32 7, !"air.max_samplers", i32 16}
!9 = !{void (%struct.Particle addrspace(1)*, float addrspace(2)*)* @step, !10, !11}
!10 = !{}
!11 = !{!12, !14}
!12 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.struct_type_info", !13, !"air.arg_type_size", i32 24, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"Particle", !"air.arg_name", !"p"}
!13 = !{i32 0, i32 4, i32 0, !"float", !"x", i32 4, i32 4, i32 0, !"float", !"y", i32 8, i32 4, i32 0, !"float", !"z", i32 12, i32 4, i32 0, !"float", !"vx", i32 16, i32 4, i32 0, !"float", !"vy", i32 20, i32 4, i32 0, !"float", !"vz"}
!14 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"dt"}
!15 = !{!"air.compile.denorms_disable"}
!16 = !{!"air.compile.fast_math_enable"}
!17 = !{!"air.compile.framebuffer_fetch_enable"}
!18 = !{!"Apple metal version 32023.921 (metalfe-32023.921)"}
!19 = !{i32 2, i32 9, i32 0}
!20 = !{!"Metal", i32 4, i32 1, i32 0}
!21 = !{!"/private/tmp/binc_probe/s1.metal"}
!22 = !{!23, !24, i64 12}
!23 = !{!"_ZTS8Particle", !24, i64 0, !24, i64 4, !24, i64 8, !24, i64 12, !24, i64 16, !24, i64 20}
!24 = !{!"float", !25, i64 0}
!25 = !{!"omnipotent char", !26, i64 0}
!26 = !{!"Simple C++ TBAA"}
!27 = !{!28}
!28 = distinct !{!28, !29, !"air-alias-scope-arg(0)"}
!29 = distinct !{!29, !"air-alias-scopes(step)"}
!30 = !{!31}
!31 = distinct !{!31, !29, !"air-alias-scope-arg(1)"}
!32 = !{!24, !24, i64 0}
!33 = !{!23, !24, i64 0}
