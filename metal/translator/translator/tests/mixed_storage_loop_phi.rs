//! DRIVER-PLAN N75: Function/Private pointer phis whose incoming values are
//! emitted after the phi (a loop's back edge, the construct tree's dispatch
//! loop). Geekbench 7's MPS `depthwiseConv_3x3` failed in round 3 with
//! "Private pointer phi %metal2vulkan.ct.slot.48 has differing structural
//! sources; raw construction failed: ... pointer merge storage mismatch
//! Function vs Private ..." although N72 split such phis: N72 judged each
//! incoming value by what had been emitted, and a back-edge value had not.
//! The fixtures are hand-made with those shapes (no Apple IR); each kernel
//! has its own name, because the demotion note is logged once per kernel.
//! Before the fix: `function_entry_private_back_edge` and
//! `private_entry_function_back_edge` failed with "<storage> pointer phi %p
//! has differing structural sources" (round 3's first error), the merge and
//! construct-tree fixtures with "... feeds another pointer phi; nested mixed
//! phis are not supported (N72)", and `a_loop_carried_private_merge_*`
//! translated but never read the Private table (it addressed the alloca).

use metal2vulkan::passes::Stage;
use metal2vulkan::{disassemble, notes, tools, translate_sanitized_native};

const TABLE: &str = "@weights = internal constant [9 x float] [float 1.0, float 2.0, float 3.0, float 4.0, float 5.0, float 6.0, float 7.0, float 8.0, float 9.0], align 4";

fn tail(kernel: &str) -> String {
    format!(
        r#"
!air.kernel = !{{!0}}
!0 = !{{ptr @{kernel}, !1, !2}}
!1 = !{{}}
!2 = !{{!3, !4}}
!3 = !{{i32 0, !"air.buffer", !"air.buffer_size", i32 16, !"air.struct_type_info", !5, !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"Input", !"air.arg_name", !"in"}}
!4 = !{{i32 1, !"air.buffer", !"air.buffer_size", i32 16, !"air.struct_type_info", !5, !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.arg_type_name", !"Output", !"air.arg_name", !"out"}}
!5 = !{{i32 0, i32 16, i32 0, !"uint", !"v0", i32 4, i32 4, i32 0, !"uint", !"v1", i32 8, i32 4, i32 0, !"uint", !"v2", i32 12, i32 4, i32 0, !"uint", !"v3"}}
"#
    )
}

/// The common head: `%i` from the input, `%local` a Function array with
/// `%slot` = &local[i] written once, `%first` = &local[0], `%g0` =
/// &weights[i] (the Private table), `%o0` the output word.
fn head(kernel: &str) -> String {
    format!(
        r#"target triple = "air64_v28-apple-macosx26.5.0"
%Input = type {{ [4 x i32] }}
%Output = type {{ [4 x i32] }}
{TABLE}

define void @{kernel}(ptr addrspace(2) %in, ptr addrspace(1) %out) {{
entry:
  %ip = getelementptr inbounds %Input, ptr addrspace(2) %in, i64 0, i32 0, i64 0
  %i = load i32, ptr addrspace(2) %ip
  %c = icmp ult i32 %i, 4
  %local = alloca [9 x float], align 4
  %slot = getelementptr inbounds [9 x float], ptr %local, i32 0, i32 %i
  store float 1.0, ptr %slot, align 4
  %first = getelementptr inbounds [9 x float], ptr %local, i32 0, i32 0
  %g0 = getelementptr inbounds [9 x float], ptr @weights, i32 0, i32 %i
  %o0 = getelementptr inbounds %Output, ptr addrspace(1) %out, i64 0, i32 0, i64 0
"#
    )
}

/// A loop whose pointer `%p` starts at `init` and takes `back` (defined by
/// `back_body` in the loop, after `%n1`) on the back edge.
fn single_loop(kernel: &str, init: &str, back_body: &str, back: &str) -> String {
    format!(
        r#"{head}  br label %loop
loop:
  %p = phi ptr [ {init}, %entry ], [ {back}, %latch ]
  %n = phi i32 [ 0, %entry ], [ %n1, %latch ]
  %acc = phi float [ 0.0, %entry ], [ %acc1, %latch ]
  %v = load float, ptr %p, align 4
  %acc1 = fadd float %acc, %v
  %n1 = add i32 %n, 1
{back_body}
  br label %latch
latch:
  %more = icmp ult i32 %n1, 3
  br i1 %more, label %loop, label %done
done:
  %vi = bitcast float %acc1 to i32
  store i32 %vi, ptr addrspace(1) %o0
  ret void
}}
{tail}"#,
        head = head(kernel),
        tail = tail(kernel)
    )
}

/// A loop whose back edge carries `%q = phi [%l, %useLocal], [%t, %useTable]`
/// with `%l` = &<left>[n1] and `%t` = &<right>[n1].
fn carried_merge(kernel: &str, init: &str, left: &str, right: &str) -> String {
    format!(
        r#"{head}  br label %loop
loop:
  %p = phi ptr [ {init}, %entry ], [ %q, %latch ]
  %n = phi i32 [ 0, %entry ], [ %n1, %latch ]
  %acc = phi float [ 0.0, %entry ], [ %acc1, %latch ]
  %v = load float, ptr %p, align 4
  %acc1 = fadd float %acc, %v
  %n1 = add i32 %n, 1
  %odd = icmp eq i32 %n1, 1
  br i1 %odd, label %useLocal, label %useTable
useLocal:
  %l = getelementptr inbounds [9 x float], ptr {left}, i32 0, i32 %n1
  br label %latch
useTable:
  %t = getelementptr inbounds [9 x float], ptr {right}, i32 0, i32 %n1
  br label %latch
latch:
  %q = phi ptr [ %l, %useLocal ], [ %t, %useTable ]
  %more = icmp ult i32 %n1, 3
  br i1 %more, label %loop, label %done
done:
  %vi = bitcast float %acc1 to i32
  store i32 %vi, ptr addrspace(1) %o0
  ret void
}}
{tail}"#,
        head = head(kernel),
        tail = tail(kernel)
    )
}

/// The construct tree's dispatch loop as renest.rs writes it (header slot
/// phis fed from the merge block, one case per original block): case 0
/// branches to case 1 (the Function value `%first`) or case 2 (the Private
/// value `%g`), case 3 reads the slot and either stops or, with
/// `case3_tail`, loops on itself with `case3_slot` as the slot's next value.
fn construct_tree(kernel: &str, case3_tail: &str, case3_slot: &str, case3_pc: &str) -> String {
    format!(
        r#"{head}  br label %metal2vulkan.ct.header
metal2vulkan.ct.header:
  %metal2vulkan.ct.pc = phi i32 [ 0, %entry ], [ %metal2vulkan.ct.nextpc, %metal2vulkan.ct.continue ]
  %metal2vulkan.ct.slot.0 = phi ptr [ undef, %entry ], [ %metal2vulkan.ct.nextslot.0, %metal2vulkan.ct.continue ]
  %metal2vulkan.ct.slot.1 = phi i32 [ undef, %entry ], [ %metal2vulkan.ct.nextslot.1, %metal2vulkan.ct.continue ]
  br label %metal2vulkan.ct.test
metal2vulkan.ct.test:
  %metal2vulkan.ct.done = icmp eq i32 %metal2vulkan.ct.pc, 4
  br i1 %metal2vulkan.ct.done, label %metal2vulkan.ct.exit, label %metal2vulkan.ct.dispatch
metal2vulkan.ct.dispatch:
  switch i32 %metal2vulkan.ct.pc, label %metal2vulkan.ct.invalid [ i32 0, label %case0 i32 1, label %case1 i32 2, label %case2 i32 3, label %case3 ]
case0:
  br i1 %c, label %metal2vulkan.ct.route.0.0.select, label %metal2vulkan.ct.route.0.1.select
metal2vulkan.ct.route.0.0.select:
  br label %metal2vulkan.ct.casejoin.0
metal2vulkan.ct.route.0.1.select:
  br label %metal2vulkan.ct.casejoin.0
metal2vulkan.ct.casejoin.0:
  %metal2vulkan.ct.casepc.0 = phi i32 [ 1, %metal2vulkan.ct.route.0.0.select ], [ 2, %metal2vulkan.ct.route.0.1.select ]
  br label %metal2vulkan.ct.merge
case1:
  br label %metal2vulkan.ct.casejoin.1
metal2vulkan.ct.casejoin.1:
  br label %metal2vulkan.ct.merge
case2:
  %g = getelementptr inbounds [9 x float], ptr @weights, i32 0, i32 %i
  br label %metal2vulkan.ct.casejoin.2
metal2vulkan.ct.casejoin.2:
  br label %metal2vulkan.ct.merge
case3:
  %v = load float, ptr %metal2vulkan.ct.slot.0, align 4
  %vi = bitcast float %v to i32
  %acc = add i32 %metal2vulkan.ct.slot.1, %vi
  store i32 %acc, ptr addrspace(1) %o0
{case3_tail}
  br label %metal2vulkan.ct.casejoin.3
metal2vulkan.ct.casejoin.3:
  br label %metal2vulkan.ct.merge
metal2vulkan.ct.invalid:
  br label %metal2vulkan.ct.merge
metal2vulkan.ct.merge:
  %metal2vulkan.ct.nextpc = phi i32 [ %metal2vulkan.ct.casepc.0, %metal2vulkan.ct.casejoin.0 ], [ 3, %metal2vulkan.ct.casejoin.1 ], [ 3, %metal2vulkan.ct.casejoin.2 ], [ {case3_pc}, %metal2vulkan.ct.casejoin.3 ], [ 4, %metal2vulkan.ct.invalid ]
  %metal2vulkan.ct.nextslot.0 = phi ptr [ %metal2vulkan.ct.slot.0, %metal2vulkan.ct.casejoin.0 ], [ %first, %metal2vulkan.ct.casejoin.1 ], [ %g, %metal2vulkan.ct.casejoin.2 ], [ {case3_slot}, %metal2vulkan.ct.casejoin.3 ], [ %metal2vulkan.ct.slot.0, %metal2vulkan.ct.invalid ]
  %metal2vulkan.ct.nextslot.1 = phi i32 [ 0, %metal2vulkan.ct.casejoin.0 ], [ 0, %metal2vulkan.ct.casejoin.1 ], [ 0, %metal2vulkan.ct.casejoin.2 ], [ %acc, %metal2vulkan.ct.casejoin.3 ], [ 0, %metal2vulkan.ct.invalid ]
  br label %metal2vulkan.ct.continue
metal2vulkan.ct.continue:
  br label %metal2vulkan.ct.header
metal2vulkan.ct.exit:
  ret void
}}
{tail}"#,
        head = head(kernel),
        tail = tail(kernel)
    )
}

/// A loop exiting into its guard's join (`%sibling`), where the mixed phi
/// lives: the planner hands this CFG to the construct tree's straddle
/// dispatch (`%metal2vulkan.ct.sl.slot.N`), end to end.
fn loop_exit_into_sibling(kernel: &str) -> String {
    format!(
        r#"{head}  br label %owner
owner:
  %g = getelementptr inbounds [9 x float], ptr @weights, i32 0, i32 %i
  br i1 %c, label %sibling, label %loop
loop:
  %n = phi i32 [ 0, %owner ], [ %n1, %loop_tail ]
  br label %body
body:
  %n1 = add i32 %n, 1
  %leave = icmp eq i32 %n1, %i
  br i1 %leave, label %sibling, label %loop_tail
loop_tail:
  %again = icmp ult i32 %n1, 3
  br i1 %again, label %loop, label %tail
sibling:
  %q = phi ptr [ %g, %owner ], [ %slot, %body ]
  %v = load float, ptr %q, align 4
  br label %post
tail:
  br label %post
post:
  %r = phi float [ %v, %sibling ], [ 0.0, %tail ]
  %vi = bitcast float %r to i32
  store i32 %vi, ptr addrspace(1) %o0
  ret void
}}
{tail}"#,
        head = head(kernel),
        tail = tail(kernel)
    )
}

fn tmp() -> std::path::PathBuf {
    let dir = std::env::temp_dir().join(format!("m2v_mixed_loop_phi_{}", std::process::id()));
    let _ = std::fs::create_dir_all(&dir);
    dir
}

/// Translates (translate_sanitized_native runs spirv-val; it is run again
/// here so a change there cannot hide a bad module), then checks the split:
/// no OpPhi of a Function or Private pointer, both storages addressed.
/// Returns the disassembly and this translation's notes.
fn assert_split(ll: &str) -> (String, Vec<String>) {
    let _ = notes::take();
    let spv = translate_sanitized_native(ll, Stage::Kernel, &tmp())
        .unwrap_or_else(|error| panic!("the loop-carried mixed phi must translate: {error}"));
    let taken = notes::take();
    tools::spirv_val_bytes(&spv, &tmp()).expect("spirv-val");
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
        asm.contains("OpTypePointer Private") && asm.contains("OpTypePointer Function"),
        "both arms must be addressed\n{asm}"
    );
    (asm, taken)
}

/// The note the host logs as `translator note: ...` (DRIVER-PLAN N72, N75).
fn assert_noted(notes: &[String], kernel: &str, phi: &str) {
    let expected = format!(
        "metal2vulkan: kernel {kernel}: pointer phi {phi} merges Function and Private storage; demoted to a per-storage value select; its network is loop-carried (DRIVER-PLAN N72, N75)"
    );
    assert!(
        notes.iter().any(|note| note == &expected),
        "missing note {expected:?}; got {notes:?}"
    );
}

/// `%id = Op... operands` lines by result id.
fn definitions(asm: &str) -> std::collections::HashMap<&str, Vec<&str>> {
    asm.lines()
        .filter_map(|line| {
            let tokens = line.split_whitespace().collect::<Vec<_>>();
            (tokens.get(1) == Some(&"=")).then(|| (tokens[0], tokens))
        })
        .collect()
}

/// The variable a pointer is based on, through access chains.
fn base_variable<'a>(
    defs: &std::collections::HashMap<&'a str, Vec<&'a str>>,
    mut id: &'a str,
) -> Option<&'a str> {
    for _ in 0..16 {
        let tokens = defs.get(id)?;
        match tokens[2] {
            "OpVariable" => return Some(id),
            op if op.contains("AccessChain") => id = tokens[4],
            "OpCopyObject" => id = tokens[4],
            _ => return None,
        }
    }
    None
}

/// The split pointer's per-arm loads (`OpBranchConditional %sel %t %f`
/// whose two blocks each start with an OpLoad): for each, the true arm
/// reads a Function variable and the false arm the Private variable named
/// `table` (not merely "some" Private access). Returns how many there are.
fn assert_arm_roots(asm: &str, table: &str) -> usize {
    let defs = definitions(asm);
    let named = |id: &str, name: &str| asm.contains(&format!("OpName {id} \"{name}\""));
    let lines = asm.lines().collect::<Vec<_>>();
    let first_load_after = |label: &str| {
        let start = lines
            .iter()
            .position(|line| line.trim() == format!("{label} = OpLabel"))?;
        lines[start + 1..]
            .iter()
            .take_while(|line| !line.contains("OpLabel"))
            .find(|line| line.contains("= OpLoad"))
            .and_then(|line| line.split_whitespace().nth(4))
    };
    let mut pairs = 0;
    for (index, line) in lines.iter().enumerate() {
        let tokens = line.split_whitespace().collect::<Vec<_>>();
        if tokens.first() != Some(&"OpBranchConditional")
            || index == 0
            || !lines[index - 1].contains("OpSelectionMerge")
        {
            continue;
        }
        let (Some(true_ptr), Some(false_ptr)) =
            (first_load_after(tokens[2]), first_load_after(tokens[3]))
        else {
            continue;
        };
        let (Some(true_var), Some(false_var)) = (
            base_variable(&defs, true_ptr),
            base_variable(&defs, false_ptr),
        ) else {
            continue;
        };
        let storage = |var: &str| defs[var].last().copied().unwrap_or_default();
        let is_function = |var: &str| defs[var].contains(&"Function");
        if !is_function(true_var) && !named(false_var, table) {
            continue;
        }
        assert!(
            is_function(true_var),
            "the selector's true arm must read the Function variable, reads {true_var} ({})\n{asm}",
            storage(true_var)
        );
        assert!(
            defs[false_var].contains(&"Private") && named(false_var, table),
            "the selector's false arm must read the Private table {table}, reads {false_var}\n{asm}"
        );
        pairs += 1;
    }
    assert!(pairs > 0, "no per-arm load of the split pointer\n{asm}");
    pairs
}

/// The table must be read, through the split's per-arm loads.
fn assert_reads_the_table(asm: &str) {
    assert_arm_roots(asm, "weights");
}

/// The translation must be refused with this text (never a module, never
/// a bare undefined id).
fn assert_refused(ll: &str, expected: &str) {
    match translate_sanitized_native(ll, Stage::Kernel, &tmp()) {
        Ok(_) => panic!("must be refused with {expected:?}"),
        Err(error) => assert!(
            error.contains(expected) && !error.contains("undefined id"),
            "expected {expected:?}; got: {error}"
        ),
    }
}

const TABLE2: &str = "@weights2 = internal constant [9 x float] [float 11.0, float 12.0, float 13.0, float 14.0, float 15.0, float 16.0, float 17.0, float 18.0, float 19.0], align 4\n";
const LOCAL2: &str =
    "  %local2 = alloca [9 x float], align 4\n  store float 5.0, ptr %local2, align 4\n";

fn with_table2(ll: String) -> String {
    ll.replacen("define void", &format!("{TABLE2}define void"), 1)
}

fn with_local2(ll: String) -> String {
    ll.replacen(
        "  br label %loop\nloop:",
        &format!("{LOCAL2}  br label %loop\nloop:"),
        1,
    )
}

#[test]
fn function_entry_private_back_edge() {
    let (asm, notes) = assert_split(&single_loop(
        "fe_pb",
        "%slot",
        "  %pn = getelementptr inbounds [9 x float], ptr @weights, i32 0, i32 %n1",
        "%pn",
    ));
    assert_reads_the_table(&asm);
    assert!(
        asm.contains("OpSelectionMerge"),
        "the load must branch per arm\n{asm}"
    );
    assert_noted(&notes, "fe_pb", "%p");
}

#[test]
fn private_entry_function_back_edge() {
    // Round 3's first error, "Private pointer phi ... has differing
    // structural sources", in its smallest form.
    let (asm, notes) = assert_split(&single_loop(
        "pe_fb",
        "%g0",
        "  %pn = getelementptr inbounds [9 x float], ptr %local, i32 0, i32 %n1",
        "%pn",
    ));
    assert_reads_the_table(&asm);
    assert_noted(&notes, "pe_fb", "%p");
}

#[test]
fn a_loop_carried_mixed_merge_is_split() {
    // The raw-construction error's shape: the loop phi's back-edge value is
    // itself a two-edge Function/Private phi emitted after it.
    let (asm, notes) = assert_split(&carried_merge("cm", "%slot", "%local", "@weights"));
    assert_reads_the_table(&asm);
    assert_noted(&notes, "cm", "%p");
}

#[test]
fn a_loop_carried_private_merge_reads_the_table() {
    // Before N75 this translated and validated but addressed `%local` on
    // every iteration: the forward Private phi was given the Function
    // entry value's root.
    let (asm, _) = assert_split(&carried_merge("cp", "%slot", "@weights", "@weights"));
    assert_reads_the_table(&asm);
}

#[test]
fn the_construct_tree_slot_is_split() {
    let (asm, notes) = assert_split(&construct_tree(
        "ct_slot",
        "",
        "%metal2vulkan.ct.slot.0",
        "4",
    ));
    assert_reads_the_table(&asm);
    assert_noted(&notes, "ct_slot", "%metal2vulkan.ct.slot.0");
}

#[test]
fn a_construct_tree_slot_stepping_its_own_pointer_is_split() {
    // The slot's back edge is derived from the slot itself (a GEP of it),
    // through the dispatch loop.
    let (asm, notes) = assert_split(&construct_tree(
        "ct_step",
        "  %qn = getelementptr inbounds float, ptr %metal2vulkan.ct.slot.0, i32 1\n  %again = icmp ult i32 %acc, 100\n  %pc3 = select i1 %again, i32 3, i32 4",
        "%qn",
        "%pc3",
    ));
    assert_reads_the_table(&asm);
    assert_noted(&notes, "ct_step", "%metal2vulkan.ct.slot.0");
}

#[test]
fn the_straddle_dispatch_of_a_real_cfg_is_split() {
    let (asm, notes) = assert_split(&loop_exit_into_sibling("straddle"));
    assert_reads_the_table(&asm);
    assert!(
        notes.iter().any(|note| note
            .starts_with("metal2vulkan: kernel straddle: pointer phi %metal2vulkan.ct.sl.slot.")),
        "the dispatch slot must be the demoted phi; got {notes:?}"
    );
}

#[test]
fn an_arm_without_a_zero_path_is_refused_with_its_reason() {
    // Three edges (two entry values and a step of the phi itself): each arm
    // is an index-domain phi off one of its values, which cannot take the
    // other storage's edges (the Function-only form of this loop is refused
    // too). It must be a named refusal, never a module with a dangling id.
    let ll = single_loop("zero", "%first", "", "%pn")
        .replace(
            "  br label %loop\nloop:",
            "  br i1 %c, label %left, label %right\nleft:\n  br label %loop\nright:\n  %g = getelementptr inbounds [9 x float], ptr @weights, i32 0, i32 0\n  br label %loop\nloop:",
        )
        .replace("[ %first, %entry ]", "[ %first, %left ], [ %g, %right ]")
        .replace(
            "  br label %latch\nlatch:",
            "  %pn = getelementptr inbounds float, ptr %p, i32 1\n  br label %latch\nlatch:",
        )
        .replace("%n = phi i32 [ 0, %entry ]", "%n = phi i32 [ 0, %left ], [ 0, %right ]")
        .replace(
            "%acc = phi float [ 0.0, %entry ]",
            "%acc = phi float [ 0.0, %left ], [ 0.0, %right ]",
        );
    assert_refused(
        &ll,
        "mixed-storage pointer phi %p: Function arm's index domain cannot take the arm's structural zero path (N75)",
    );
}

// HIGH-1 of the ECC review of 14081f6: an arm is one index-domain phi on one
// root; a network with two roots of one storage was translated reading the
// first root only. Each must be refused by name.

#[test]
fn a_second_private_table_on_the_back_edge_is_refused() {
    assert_refused(
        &with_table2(carried_merge("mr_p", "%g0", "%local", "@weights2")),
        "mixed-storage pointer phi %p: its network has more than one Private root (@weights, @weights2) (N75)",
    );
}

#[test]
fn a_second_alloca_on_the_back_edge_is_refused() {
    assert_refused(
        &with_local2(carried_merge("mr_f", "%slot", "%local2", "@weights")),
        "mixed-storage pointer phi %p: its network has more than one Function root (%local, %local2) (N75)",
    );
}

#[test]
fn a_second_private_table_in_a_construct_tree_case_is_refused() {
    assert_refused(
        &with_table2(construct_tree(
            "mr_ctp",
            "  %g2 = getelementptr inbounds [9 x float], ptr @weights2, i32 0, i32 %i",
            "%g2",
            "4",
        )),
        "mixed-storage pointer phi %metal2vulkan.ct.slot.0: its network has more than one Private root (@weights, @weights2) (N75)",
    );
}

#[test]
fn a_second_alloca_in_a_construct_tree_case_is_refused() {
    assert_refused(
        &construct_tree(
            "mr_ctf",
            "  %l2a = alloca [9 x float], align 4\n  %l2 = getelementptr inbounds [9 x float], ptr %l2a, i32 0, i32 %i",
            "%l2",
            "4",
        ),
        "mixed-storage pointer phi %metal2vulkan.ct.slot.0: its network has more than one Function root (%l2a, %local) (N75)",
    );
}

// N75 OPEN (b), outside mixed networks: a loop phi whose back edge is a
// forward phi of another root of the same storage was reserved on the entry
// value's root and read it (spirv-val green). Now refused when that phi is
// emitted.

#[test]
fn a_function_loop_with_a_second_alloca_on_the_back_edge_is_refused() {
    assert_refused(
        &with_local2(carried_merge("fr_f", "%slot", "%local2", "%local2")),
        "pointer phi %q was taken on root %local by an earlier use, but its values come from %local2 (N75)",
    );
}

#[test]
fn a_private_loop_with_a_second_table_on_the_back_edge_is_refused() {
    assert_refused(
        &with_table2(carried_merge("fr_p", "%g0", "@weights2", "@weights2")),
        "pointer phi %q was taken on root @weights by an earlier use, but its values come from @weights2 (N75)",
    );
}

#[test]
fn single_storage_loops_still_translate() {
    // No regression from the network analysis or the root check: one root
    // per loop, Function-only and Private-only.
    for ll in [
        single_loop(
            "one_f",
            "%slot",
            "  %pn = getelementptr inbounds [9 x float], ptr %local, i32 0, i32 %n1",
            "%pn",
        ),
        carried_merge("one_fm", "%slot", "%local", "%local"),
    ] {
        let spv = translate_sanitized_native(&ll, Stage::Kernel, &tmp())
            .unwrap_or_else(|error| panic!("a Function-only loop must translate: {error}"));
        tools::spirv_val_bytes(&spv, &tmp()).expect("spirv-val");
    }
    for ll in [
        single_loop(
            "one_p",
            "%g0",
            "  %pn = getelementptr inbounds [9 x float], ptr @weights, i32 0, i32 %n1",
            "%pn",
        ),
        carried_merge("one_pm", "%g0", "@weights", "@weights"),
    ] {
        let spv = translate_sanitized_native(&ll, Stage::Kernel, &tmp())
            .unwrap_or_else(|error| panic!("a Private-only loop must translate: {error}"));
        tools::spirv_val_bytes(&spv, &tmp()).expect("spirv-val");
        let asm = disassemble(&spv).expect("disassemble");
        let defs = definitions(&asm);
        let reads_table = asm
            .lines()
            .filter(|line| line.contains("= OpLoad"))
            .filter_map(|line| line.split_whitespace().nth(4))
            .filter_map(|ptr| base_variable(&defs, ptr))
            .any(|var| asm.contains(&format!("OpName {var} \"weights\"")));
        assert!(reads_table, "the loop must load from the table\n{asm}");
    }
}

#[test]
fn a_store_through_a_loop_carried_split_pointer_is_guarded_per_arm() {
    let ll = single_loop(
        "store",
        "%slot",
        "  store float %acc1, ptr %p, align 4\n  %pn = getelementptr inbounds [9 x float], ptr @weights, i32 0, i32 %n1",
        "%pn",
    )
    .replace("internal constant", "internal global");
    let (asm, _) = assert_split(&ll);
    assert_reads_the_table(&asm);
    let defs = definitions(&asm);
    let stored_to = asm
        .lines()
        .filter(|line| line.trim_start().starts_with("OpStore"))
        .filter_map(|line| line.split_whitespace().nth(1))
        .filter_map(|ptr| base_variable(&defs, ptr))
        .collect::<Vec<_>>();
    assert!(
        stored_to.iter().any(|var| defs[var].contains(&"Function"))
            && stored_to.iter().any(|var| defs[var].contains(&"Private")),
        "one guarded store per arm (Function and Private) {stored_to:?}\n{asm}"
    );
}

#[test]
fn a_phi_that_also_takes_a_loaded_pointer_is_refused() {
    // `%w` merges a member with a pointer of unknown storage (loaded from a
    // field with two stores, which N76 does not forward): it is outside the
    // network and cannot take the split. (A field with one store is
    // forwarded and joins the network: tests/local_pointer_field_phi.rs.)
    let ll = carried_merge("outside", "%slot", "%local", "@weights")
        .replacen(
            "  br label %loop\nloop:",
            "  %pp = alloca ptr, align 8\n  store ptr %slot, ptr %pp, align 8\n  br i1 %c, label %restore, label %reload\nrestore:\n  store ptr %first, ptr %pp, align 8\n  br label %reload\nreload:\n  %u = load ptr, ptr %pp, align 8\n  br label %loop\nloop:",
            1,
        )
        .replace("[ %slot, %entry ]", "[ %slot, %reload ]")
        .replace("[ 0, %entry ]", "[ 0, %reload ]")
        .replace("[ 0.0, %entry ]", "[ 0.0, %reload ]")
        .replace(
            "done:\n",
            "done:\n  br i1 %c, label %pick, label %tail\npick:\n  br label %tail\ntail:\n  %w = phi ptr [ %u, %done ], [ %q, %pick ]\n  %wv = load float, ptr %w, align 4\n  %wi = bitcast float %wv to i32\n  store i32 %wi, ptr addrspace(1) %o0\n",
        );
    assert_refused(
        &ll,
        "mixed-storage pointer phi %q (Function vs Private) feeds another pointer phi; nested mixed phis are not supported (N72)",
    );
}
