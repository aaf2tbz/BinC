; ModuleID = 'render.metal'
source_filename = "render.metal"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v29-apple-macosx27.0.0"

%struct.VIn = type { <4 x float> }
%"struct.metal::matrix" = type { [4 x <4 x float>] }

; Function Attrs: argmemonly mustprogress nofree norecurse nosync nounwind readonly willreturn
define <{ <4 x float>, <4 x float> }> @vs(i32 noundef %0, %struct.VIn addrspace(1)* nocapture noundef readonly "air-buffer-no-alias" %1, %"struct.metal::matrix" addrspace(2)* nocapture noundef readonly align 16 dereferenceable(64) "air-buffer-no-alias" %2) local_unnamed_addr #0 {
  %4 = zext i32 %0 to i64
  %5 = getelementptr inbounds %struct.VIn, %struct.VIn addrspace(1)* %1, i64 %4, i32 0
  %6 = load <4 x float>, <4 x float> addrspace(1)* %5, align 16, !tbaa !31, !alias.scope !34, !noalias !37
  %7 = getelementptr inbounds %"struct.metal::matrix", %"struct.metal::matrix" addrspace(2)* %2, i64 0, i32 0, i64 0
  %8 = load <4 x float>, <4 x float> addrspace(2)* %7, align 16, !tbaa !39, !alias.scope !37, !noalias !34
  %9 = shufflevector <4 x float> %6, <4 x float> poison, <4 x i32> zeroinitializer
  %10 = fmul fast <4 x float> %8, %9
  %11 = getelementptr inbounds %"struct.metal::matrix", %"struct.metal::matrix" addrspace(2)* %2, i64 0, i32 0, i64 1
  %12 = load <4 x float>, <4 x float> addrspace(2)* %11, align 16, !tbaa !39, !alias.scope !37, !noalias !34
  %13 = shufflevector <4 x float> %6, <4 x float> undef, <4 x i32> <i32 1, i32 1, i32 1, i32 1>
  %14 = fmul fast <4 x float> %12, %13
  %15 = fadd fast <4 x float> %14, %10
  %16 = getelementptr inbounds %"struct.metal::matrix", %"struct.metal::matrix" addrspace(2)* %2, i64 0, i32 0, i64 2
  %17 = load <4 x float>, <4 x float> addrspace(2)* %16, align 16, !tbaa !39, !alias.scope !37, !noalias !34
  %18 = shufflevector <4 x float> %6, <4 x float> undef, <4 x i32> <i32 2, i32 2, i32 2, i32 2>
  %19 = fmul fast <4 x float> %17, %18
  %20 = fadd fast <4 x float> %15, %19
  %21 = getelementptr inbounds %"struct.metal::matrix", %"struct.metal::matrix" addrspace(2)* %2, i64 0, i32 0, i64 3
  %22 = load <4 x float>, <4 x float> addrspace(2)* %21, align 16, !tbaa !39, !alias.scope !37, !noalias !34
  %23 = shufflevector <4 x float> %6, <4 x float> undef, <4 x i32> <i32 3, i32 3, i32 3, i32 3>
  %24 = fmul fast <4 x float> %22, %23
  %25 = fadd fast <4 x float> %20, %24
  %26 = insertvalue <{ <4 x float>, <4 x float> }> undef, <4 x float> %25, 0
  %27 = insertvalue <{ <4 x float>, <4 x float> }> %26, <4 x float> <float 1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, 1
  ret <{ <4 x float>, <4 x float> }> %27
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind readnone willreturn
define <4 x float> @fs(<4 x float> %0, <4 x float> returned %1) local_unnamed_addr #1 {
  ret <4 x float> %1
}

attributes #0 = { argmemonly mustprogress nofree norecurse nosync nounwind readonly willreturn "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="128" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #1 = { mustprogress nofree norecurse nosync nounwind readnone willreturn "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="128" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6, !7, !8}
!air.vertex = !{!9}
!air.fragment = !{!18}
!air.compile_options = !{!24, !25, !26}
!llvm.ident = !{!27}
!air.version = !{!28}
!air.language_version = !{!29}
!air.source_file_name = !{!30}

!0 = !{i32 2, !"SDK Version", [2 x i32] [i32 27, i32 0]}
!1 = !{i32 1, !"wchar_size", i32 4}
!2 = !{i32 7, !"frame-pointer", i32 2}
!3 = !{i32 7, !"air.max_device_buffers", i32 31}
!4 = !{i32 7, !"air.max_constant_buffers", i32 31}
!5 = !{i32 7, !"air.max_threadgroup_buffers", i32 31}
!6 = !{i32 7, !"air.max_textures", i32 128}
!7 = !{i32 7, !"air.max_read_write_textures", i32 8}
!8 = !{i32 7, !"air.max_samplers", i32 16}
!9 = !{<{ <4 x float>, <4 x float> }> (i32, %struct.VIn addrspace(1)*, %"struct.metal::matrix" addrspace(2)*)* @vs, !10, !13}
!10 = !{!11, !12}
!11 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!12 = !{!"air.vertex_output", !"generated(5colorDv4_f)", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!13 = !{!14, !15, !17}
!14 = !{i32 0, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"vid"}
!15 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.struct_type_info", !16, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"VIn", !"air.arg_name", !"v"}
!16 = !{i32 0, i32 16, i32 0, !"float4", !"pos"}
!17 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 64, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 64, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4x4", !"air.arg_name", !"mvp"}
!18 = !{<4 x float> (<4 x float>, <4 x float>)* @fs, !19, !21}
!19 = !{!20}
!20 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!21 = !{!22, !23}
!22 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos", !"air.arg_unused"}
!23 = !{i32 1, !"air.fragment_input", !"generated(5colorDv4_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!24 = !{!"air.compile.denorms_disable"}
!25 = !{!"air.compile.fast_math_enable"}
!26 = !{!"air.compile.framebuffer_fetch_enable"}
!27 = !{!"Apple metal version 32023.921 (metalfe-32023.921)"}
!28 = !{i32 2, i32 9, i32 0}
!29 = !{!"Metal", i32 4, i32 1, i32 0}
!30 = !{!"/private/tmp/binc_probe/render.metal"}
!31 = !{!32, !32, i64 0}
!32 = !{!"omnipotent char", !33, i64 0}
!33 = !{!"Simple C++ TBAA"}
!34 = !{!35}
!35 = distinct !{!35, !36, !"air-alias-scope-arg(1)"}
!36 = distinct !{!36, !"air-alias-scopes(vs)"}
!37 = !{!38}
!38 = distinct !{!38, !36, !"air-alias-scope-arg(2)"}
!39 = !{!40, !40, i64 0}
!40 = !{!"float", !32, i64 0}
