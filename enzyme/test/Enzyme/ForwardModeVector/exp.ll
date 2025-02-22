; RUN: %opt < %s %loadEnzyme -enzyme -enzyme-preopt=false -O3 -S | FileCheck %s

%struct.Gradients = type { double, double, double }

; Function Attrs: nounwind
declare %struct.Gradients @__enzyme_fwddiff(double (double)*, ...)

; Function Attrs: nounwind readnone uwtable
define double @tester(double %x) {
entry:
  %0 = tail call fast double @llvm.exp.f64(double %x)
  ret double %0
}

define %struct.Gradients @test_derivative(double %x) {
entry:
  %0 = tail call %struct.Gradients (double (double)*, ...) @__enzyme_fwddiff(double (double)* nonnull @tester, metadata !"enzyme_width", i64 3, double %x, double 1.0, double 2.0, double 3.0)
  ret %struct.Gradients %0
}

; Function Attrs: nounwind readnone speculatable
declare double @llvm.exp.f64(double)


; CHECK: define %struct.Gradients @test_derivative(double %x)
; CHECK-NEXT: entry:
; CHECK-NEXT:   %0 = tail call fast double @llvm.exp.f64(double %x)
; CHECK-NEXT:   %1 = fmul fast double %0, 2.000000e+00
; CHECK-NEXT:   %2 = fmul fast double %0, 3.000000e+00
; CHECK-NEXT:   %3 = insertvalue %struct.Gradients zeroinitializer, double %0, 0
; CHECK-NEXT:   %4 = insertvalue %struct.Gradients %3, double %1, 1
; CHECK-NEXT:   %5 = insertvalue %struct.Gradients %4, double %2, 2
; CHECK-NEXT:   ret %struct.Gradients %5
; CHECK-NEXT: }