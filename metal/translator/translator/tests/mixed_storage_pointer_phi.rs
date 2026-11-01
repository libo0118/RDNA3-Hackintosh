//! DRIVER-PLAN N72: a pointer phi whose incoming values live in a Function
//! variable (an `alloca`) on some edges and in a Private variable (an
//! address-space-0 global, e.g. a `@__const` table LLVM substituted for a
//! never-written local copy) on others. Geekbench 7's MPS `depthwiseConv_3x3`
//! failed with "pointer merge storage mismatch Function vs Private". The
//! fixtures are hand-made with that shape (no Apple IR). Each must translate
//! to valid Logical SPIR-V (translate_sanitized_native runs spirv-val) with
//! no pointer-typed OpPhi (the arms are chosen by a bool phi).

use metal2vulkan::passes::Stage;
use metal2vulkan::{disassemble, translate_sanitized_native};

const TAIL: &str = r#"
!air.kernel = !{!0}
!0 = !{ptr @k, !1, !2}
!1 = !{}
!2 = !{!3, !4}
!3 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 16, !"air.struct_type_info", !5, !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"Input", !"air.arg_name", !"in"}
!4 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 16, !"air.struct_type_info", !5, !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.arg_type_name", !"Output", !"air.arg_name", !"out"}
!5 = !{i32 0, i32 16, i32 0, !"uint", !"v0", i32 4, i32 4, i32 0, !"uint", !"v1", i32 8, i32 4, i32 0, !"uint", !"v2", i32 12, i32 4, i32 0, !"uint", !"v3"}
"#;

/// A kernel whose `merge` block holds `%p = phi ptr [<left>, %left],
/// [<right>, %right]`; `left_body` / `right_body` define the incoming
/// values, `use_p` reads `%p` into `%v` (a float stored to the output).
fn kernel(left_body: &str, left: &str, right_body: &str, right: &str, use_p: &str) -> String {
    format!(
        r#"target triple = "air64_v28-apple-macosx26.5.0"
%Input = type {{ [4 x i32] }}
%Output = type {{ [4 x i32] }}
@weights = internal global [9 x float] [float 1.0, float 2.0, float 3.0, float 4.0, float 5.0, float 6.0, float 7.0, float 8.0, float 9.0], align 4

define void @k(ptr addrspace(2) %in, ptr addrspace(1) %out) {{
entry:
  %ip = getelementptr inbounds %Input, ptr addrspace(2) %in, i64 0, i32 0, i64 0
  %i = load i32, ptr addrspace(2) %ip
  %c = icmp ult i32 %i, 4
  %local = alloca [9 x float], align 4
  %slot = getelementptr inbounds [9 x float], ptr %local, i32 0, i32 %i
  store float 1.0, ptr %slot, align 4
  %first = getelementptr inbounds [9 x float], ptr %local, i32 0, i32 0
  br i1 %c, label %left, label %right
left:
{left_body}
  br label %merge
right:
{right_body}
  br label %merge
merge:
  %p = phi ptr [ {left}, %left ], [ {right}, %right ]
{use_p}
  %o0 = getelementptr inbounds %Output, ptr addrspace(1) %out, i64 0, i32 0, i64 0
  %vi = bitcast float %v to i32
  store i32 %vi, ptr addrspace(1) %o0
  ret void
}}
{TAIL}"#
    )
}

fn tmp() -> std::path::PathBuf {
    let dir = std::env::temp_dir().join(format!("m2v_mixed_phi_{}", std::process::id()));
    let _ = std::fs::create_dir_all(&dir);
    dir
}

const GLOBAL_GEP: &str = "  %g = getelementptr inbounds [9 x float], ptr @weights, i32 0, i32 %i";
const LOAD: &str = "  %v = load float, ptr %p, align 4";

/// Translates, then checks the split: no OpPhi of a Function or Private
/// pointer, and both storages addressed.
fn assert_split(ll: &str) -> String {
    let spv = translate_sanitized_native(ll, Stage::Kernel, &tmp())
        .unwrap_or_else(|error| panic!("the mixed-storage phi must translate: {error}"));
    let asm = disassemble(&spv).expect("disassemble");
    let pointer_types = asm
        .lines()
        .filter(|line| {
            line.contains("OpTypePointer Function") || line.contains("OpTypePointer Private")
        })
        .filter_map(|line| line.split('=').next().map(|id| id.trim().to_string()))
        .collect::<Vec<_>>();
    for line in asm.lines().filter(|line| line.contains("OpPhi")) {
        let ty = line.split_whitespace().nth(3).unwrap_or_default();
        assert!(
            !pointer_types.iter().any(|pointer| pointer == ty),
            "a Function/Private pointer phi survived: {line}\n{asm}"
        );
    }
    assert!(
        asm.contains("Private") && asm.contains("Function"),
        "both arms must be addressed\n{asm}"
    );
    asm
}

#[test]
fn alloca_gep_and_private_global_gep_merge_by_value() {
    assert_split(&kernel("", "%slot", GLOBAL_GEP, "%g", LOAD));
}

#[test]
fn the_private_arm_may_come_first() {
    assert_split(&kernel(GLOBAL_GEP, "%g", "", "%slot", LOAD));
}

#[test]
fn whole_roots_merge_like_their_first_element() {
    // The exact pair of the Geekbench error: a whole alloca against a GEP
    // into the Private table.
    assert_split(&kernel("", "%local", GLOBAL_GEP, "%g", LOAD));
    assert_split(&kernel("", "%slot", "", "@weights", LOAD));
}

#[test]
fn pointer_arithmetic_on_the_merged_pointer_applies_to_each_arm() {
    let asm = assert_split(&kernel(
        "",
        "%first",
        "  %g = getelementptr inbounds [9 x float], ptr @weights, i32 0, i32 0",
        "%g",
        "  %q = getelementptr inbounds float, ptr %p, i32 %i\n  %v = load float, ptr %q, align 4",
    ));
    assert!(
        !asm.contains("OpPtrAccessChain"),
        "Function/Private pointer arithmetic must stay an index (no OpPtrAccessChain)\n{asm}"
    );
}

#[test]
fn a_store_through_the_merged_pointer_is_guarded_per_arm() {
    let asm = assert_split(&kernel(
        "",
        "%slot",
        GLOBAL_GEP,
        "%g",
        "  store float 5.0, ptr %p, align 4\n  %v = load float, ptr %p, align 4",
    ));
    assert!(
        asm.matches("OpStore").count() >= 3,
        "the setup store, one store per arm and the output store\n{asm}"
    );
}

#[test]
fn a_copy_of_the_merged_pointer_stays_split() {
    // ECC review of 4ff5e85: a bitcast copy lost the split and loaded both
    // arms with an OpSelect (an out-of-bounds read of the unselected root).
    let asm = assert_split(&kernel(
        "",
        "%first",
        "  %g = getelementptr inbounds [9 x float], ptr @weights, i32 0, i32 0",
        "%g",
        "  %s = bitcast ptr %p to ptr\n  %q = getelementptr inbounds float, ptr %s, i32 %i\n  %v = load float, ptr %q, align 4",
    ));
    assert!(
        !asm.contains("OpSelect "),
        "the load must branch to the selected arm, not read both\n{asm}"
    );
    assert!(asm.contains("OpSelectionMerge"), "no branch per arm\n{asm}");
}

fn assert_refused_clearly(ll: &str, consumer: &str) {
    match translate_sanitized_native(ll, Stage::Kernel, &tmp()) {
        Ok(_) => panic!("a consumer of a split pointer must be refused"),
        Err(error) => assert!(
            error.contains("nested mixed phis are not supported (N72)") && error.contains(consumer),
            "the refusal must name {consumer} and N72; got: {error}"
        ),
    }
}

#[test]
fn a_pointer_walk_over_the_merged_pointer_stays_split() {
    // N72 refused this (a split pointer feeding another phi); with N75 the
    // walk is a member of the same Function/Private network and is split
    // the same way, its selector carried around the loop.
    let asm = assert_split(&kernel(
        "",
        "%slot",
        GLOBAL_GEP,
        "%g",
        "  br label %walk
walk:
  %w = phi ptr [ %p, %merge ], [ %wn, %walk ]
  %n = phi i32 [ 0, %merge ], [ %n1, %walk ]
  %wn = getelementptr inbounds float, ptr %w, i32 1
  %n1 = add i32 %n, 1
  %more = icmp ult i32 %n1, 2
  br i1 %more, label %walk, label %done
done:
  %v = load float, ptr %w, align 4",
    ));
    assert!(
        !asm.contains("OpPtrAccessChain"),
        "the walk must stay an index on each arm\n{asm}"
    );
    assert!(
        asm.contains("OpSelectionMerge"),
        "the load must branch per arm\n{asm}"
    );
}

#[test]
fn a_select_of_the_merged_pointer_is_refused_by_name() {
    assert_refused_clearly(
        &kernel(
            "",
            "%slot",
            GLOBAL_GEP,
            "%g",
            "  %s = select i1 %c, ptr %p, ptr %first\n  %v = load float, ptr %s, align 4",
        ),
        "%p",
    );
}
