; ModuleID = 'tile2d.metal'
source_filename = "tile2d.metal"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v29-apple-macosx27.0.0"

%struct._texture_2d_t = type opaque
%struct._sampler_t = type opaque

@_ZZ8tileblurN5metal9texture2dIfLNS_6accessE1ELNS_16memory_coherenceE0EvEENS0_IfLS1_2ELS2_0EvEEDv2_tS5_S5_E4smem = internal unnamed_addr addrspace(3) global [16 x [16 x <4 x float>]] undef, align 16

; Function Attrs: convergent mustprogress nounwind willreturn
define void @tileblur(%struct._texture_2d_t addrspace(1)* nocapture readonly %0, %struct._texture_2d_t addrspace(1)* %1, <2 x i16> noundef %2, <2 x i16> noundef %3, <2 x i16> noundef %4) local_unnamed_addr #0 {
  %6 = tail call %struct._sampler_t addrspace(2)* @air.get_read_sampler() #5
  %7 = tail call { <4 x float>, i8 } @air.read_texture_2d.i16.v4f32(%struct._texture_2d_t addrspace(1)* nocapture readonly %0, %struct._sampler_t addrspace(2)* %6, <2 x i16> %2, <2 x i16> zeroinitializer, i16 0, i32 1) #6
  %8 = extractvalue { <4 x float>, i8 } %7, 0
  %9 = extractelement <2 x i16> %3, i64 1
  %10 = zext i16 %9 to i64
  %11 = extractelement <2 x i16> %3, i64 0
  %12 = zext i16 %11 to i64
  %13 = getelementptr inbounds [16 x [16 x <4 x float>]], [16 x [16 x <4 x float>]] addrspace(3)* @_ZZ8tileblurN5metal9texture2dIfLNS_6accessE1ELNS_16memory_coherenceE0EvEENS0_IfLS1_2ELS2_0EvEEDv2_tS5_S5_E4smem, i64 0, i64 %10, i64 %12
  store <4 x float> %8, <4 x float> addrspace(3)* %13, align 16, !tbaa !24
  tail call void @air.wg.barrier(i32 2, i32 5, i32 1) #7
  %14 = load <4 x float>, <4 x float> addrspace(3)* %13, align 16, !tbaa !24
  tail call void @air.write_texture_2d.i16.v4f32(%struct._texture_2d_t addrspace(1)* nocapture %1, <2 x i16> %2, <4 x float> %14, i16 0, i32 2) #8, !alias.scope !28
  ret void
}

; Function Attrs: convergent mustprogress nounwind willreturn
declare void @air.wg.barrier(i32, i32, i32) local_unnamed_addr #1

; Function Attrs: inaccessiblememonly mustprogress nofree nounwind readonly willreturn
declare %struct._sampler_t addrspace(2)* @air.get_read_sampler() local_unnamed_addr #2

; Function Attrs: argmemonly mustprogress nofree nounwind readonly willreturn
declare { <4 x float>, i8 } @air.read_texture_2d.i16.v4f32(%struct._texture_2d_t addrspace(1)* nocapture readonly, %struct._sampler_t addrspace(2)*, <2 x i16>, <2 x i16>, i16, i32) local_unnamed_addr #3

; Function Attrs: argmemonly mustprogress nounwind willreturn
declare void @air.write_texture_2d.i16.v4f32(%struct._texture_2d_t addrspace(1)* nocapture, <2 x i16>, <4 x float>, i16, i32) local_unnamed_addr #4

attributes #0 = { convergent mustprogress nounwind willreturn "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="128" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #1 = { convergent mustprogress nounwind willreturn }
attributes #2 = { inaccessiblememonly mustprogress nofree nounwind readonly willreturn }
attributes #3 = { argmemonly mustprogress nofree nounwind readonly willreturn }
attributes #4 = { argmemonly mustprogress nounwind willreturn }
attributes #5 = { inaccessiblememonly nounwind readonly willreturn }
attributes #6 = { argmemonly nounwind readonly willreturn }
attributes #7 = { convergent nounwind willreturn }
attributes #8 = { argmemonly nounwind willreturn }

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
!9 = !{void (%struct._texture_2d_t addrspace(1)*, %struct._texture_2d_t addrspace(1)*, <2 x i16>, <2 x i16>, <2 x i16>)* @tileblur, !10, !11}
!10 = !{}
!11 = !{!12, !13, !14, !15, !16}
!12 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"texture2d<float, read>", !"air.arg_name", !"src"}
!13 = !{i32 1, !"air.texture", !"air.location_index", i32 1, i32 1, !"air.write", !"air.arg_type_name", !"texture2d<float, write>", !"air.arg_name", !"dst"}
!14 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"ushort2", !"air.arg_name", !"gid"}
!15 = !{i32 3, !"air.thread_position_in_threadgroup", !"air.arg_type_name", !"ushort2", !"air.arg_name", !"lid"}
!16 = !{i32 4, !"air.threadgroup_position_in_grid", !"air.arg_type_name", !"ushort2", !"air.arg_name", !"grp", !"air.arg_unused"}
!17 = !{!"air.compile.denorms_disable"}
!18 = !{!"air.compile.fast_math_enable"}
!19 = !{!"air.compile.framebuffer_fetch_enable"}
!20 = !{!"Apple metal version 32023.921 (metalfe-32023.921)"}
!21 = !{i32 2, i32 9, i32 0}
!22 = !{!"Metal", i32 4, i32 1, i32 0}
!23 = !{!"/private/tmp/binc_probe/tile2d.metal"}
!24 = !{!25, !25, i64 0}
!25 = !{!"float", !26, i64 0}
!26 = !{!"omnipotent char", !27, i64 0}
!27 = !{!"Simple C++ TBAA"}
!28 = !{!29}
!29 = distinct !{!29, !30, !"air-alias-scope-textures"}
!30 = distinct !{!30, !"air-alias-scopes(tileblur)"}
