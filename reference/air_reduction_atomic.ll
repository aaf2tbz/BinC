; ModuleID = 'reduce.metal'
source_filename = "reduce.metal"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v29-apple-macosx27.0.0"

%"struct.metal::_atomic" = type { float }

; Function Attrs: mustprogress nounwind willreturn
define void @sum(float addrspace(1)* nocapture noundef readonly "air-buffer-no-alias" %0, %"struct.metal::_atomic" addrspace(1)* nocapture noundef "air-buffer-no-alias" %1, i32 noundef %2, i32 noundef %3) local_unnamed_addr #0 {
  %5 = icmp ult i32 %2, %3
  br i1 %5, label %6, label %12

6:                                                ; preds = %4
  %7 = getelementptr inbounds %"struct.metal::_atomic", %"struct.metal::_atomic" addrspace(1)* %1, i64 0, i32 0
  %8 = zext i32 %2 to i64
  %9 = getelementptr inbounds float, float addrspace(1)* %0, i64 %8
  %10 = load float, float addrspace(1)* %9, align 4, !tbaa !24, !alias.scope !28, !noalias !31
  %11 = tail call fast float @air.atomic.global.add.f32(float addrspace(1)* nocapture %7, float %10, i32 0, i32 2, i32 0, i1 false) #2
  br label %12

12:                                               ; preds = %6, %4
  ret void
}

; Function Attrs: mustprogress nounwind willreturn
declare float @air.atomic.global.add.f32(float addrspace(1)* nocapture, float, i32, i32, i32, i1) local_unnamed_addr #1

attributes #0 = { mustprogress nounwind willreturn "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="0" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #1 = { mustprogress nounwind willreturn }
attributes #2 = { nounwind willreturn }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6, !7, !8}
!air.kernel = !{!9}
!air.compile_options = !{!17, !18, !19}
!llvm.ident = !{!20}
!air.version = !{!21}
!air.language_version = !{!22}
!air.source_file_name = !{!23}

!0 = !{i32 2, !"SDK Version", [2 x i32] [i32 27, i32 0]}
!1 = !{i32 1, !"wchar_size", i32 4}
!2 = !{i32 7, !"frame-pointer", i32 2}
!3 = !{i32 7, !"air.max_device_buffers", i32 31}
!4 = !{i32 7, !"air.max_constant_buffers", i32 31}
!5 = !{i32 7, !"air.max_threadgroup_buffers", i32 31}
!6 = !{i32 7, !"air.max_textures", i32 128}
!7 = !{i32 7, !"air.max_read_write_textures", i32 8}
!8 = !{i32 7, !"air.max_samplers", i32 16}
!9 = !{void (float addrspace(1)*, %"struct.metal::_atomic" addrspace(1)*, i32, i32)* @sum, !10, !11}
!10 = !{}
!11 = !{!12, !13, !15, !16}
!12 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"in"}
!13 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.struct_type_info", !14, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"metal::_atomic", !"air.arg_name", !"acc"}
!14 = !{i32 0, i32 4, i32 0, !"float", !"__s"}
!15 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
!16 = !{i32 3, !"air.threads_per_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"n"}
!17 = !{!"air.compile.denorms_disable"}
!18 = !{!"air.compile.fast_math_enable"}
!19 = !{!"air.compile.framebuffer_fetch_enable"}
!20 = !{!"Apple metal version 32023.921 (metalfe-32023.921)"}
!21 = !{i32 2, i32 9, i32 0}
!22 = !{!"Metal", i32 4, i32 1, i32 0}
!23 = !{!"/private/tmp/binc_probe/reduce.metal"}
!24 = !{!25, !25, i64 0}
!25 = !{!"float", !26, i64 0}
!26 = !{!"omnipotent char", !27, i64 0}
!27 = !{!"Simple C++ TBAA"}
!28 = !{!29}
!29 = distinct !{!29, !30, !"air-alias-scope-arg(0)"}
!30 = distinct !{!30, !"air-alias-scopes(sum)"}
!31 = !{!32}
!32 = distinct !{!32, !30, !"air-alias-scope-arg(1)"}
