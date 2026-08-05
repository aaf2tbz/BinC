; ModuleID = 'BinC.emit(blend)'
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v29-apple-macosx27.0.0"

define void @binc_blend(float addrspace(1)* nocapture noundef readonly %a,
                        float addrspace(1)* nocapture noundef readonly %b,
                        float addrspace(1)* nocapture noundef writeonly %out,
                        i32 noundef %id) local_unnamed_addr #0 {
  %i = zext i32 %id to i64
  %pa = getelementptr inbounds float, float addrspace(1)* %a, i64 %i
  %pb = getelementptr inbounds float, float addrspace(1)* %b, i64 %i
  %va = load float, float addrspace(1)* %pa, align 4
  %vb = load float, float addrspace(1)* %pb, align 4
  %ha = fmul fast float %va, 5.000000e-01
  %hb = fmul fast float %vb, 5.000000e-01
  %r  = fadd fast float %ha, %hb
  %po = getelementptr inbounds float, float addrspace(1)* %out, i64 %i
  store float %r, float addrspace(1)* %po, align 4
  ret void
}

attributes #0 = { argmemonly mustprogress nofree norecurse nosync nounwind willreturn "no-trapping-math"="true" }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6, !7}
!air.kernel = !{!8}
!air.compile_options = !{!9, !10}
!llvm.ident = !{!11}
!air.version = !{!12}
!air.language_version = !{!13}
!air.source_file_name = !{!14}

!0 = !{i32 2, !"SDK Version", [2 x i32] [i32 27, i32 0]}
!1 = !{i32 1, !"wchar_size", i32 4}
!2 = !{i32 7, !"air.max_device_buffers", i32 31}
!3 = !{i32 7, !"air.max_constant_buffers", i32 31}
!4 = !{i32 7, !"air.max_threadgroup_buffers", i32 31}
!5 = !{i32 7, !"air.max_textures", i32 128}
!6 = !{i32 7, !"air.max_read_write_textures", i32 8}
!7 = !{i32 7, !"air.max_samplers", i32 16}
!8 = !{void (float addrspace(1)*, float addrspace(1)*, float addrspace(1)*, i32)* @binc_blend, !15, !16}
!9 = !{!"air.compile.denorms_disable"}
!10 = !{!"air.compile.fast_math_enable"}
!11 = !{!"BinC compiler v0.0.1"}
!12 = !{i32 2, i32 9, i32 0}
!13 = !{!"Metal", i32 4, i32 1, i32 0}
!14 = !{!"binc:blend"}
!15 = !{}
!16 = !{!17, !18, !19, !20}
!17 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"a"}
!18 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"b"}
!19 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out"}
!20 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"id"}
