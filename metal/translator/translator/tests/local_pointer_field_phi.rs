//! DRIVER-PLAN N76: pointer phis fed by loads of a local pointer field (a
//! lambda's capture struct holding `&local`). Geekbench 7's MPS
//! `depthwiseConv_3x3` failed in rounds 2-4 with "Private pointer phi
//! %metal2vulkan.ct.slot.48 has differing structural sources; raw
//! construction failed: ... pointer merge storage mismatch Function vs
//! Private at %1425 across [%72, %1425]": `%1425` was such a load emitted
//! after the phi, and every analysis that runs before emission took it as a
//! pointer of unknown origin, Private by default, while the emitter forwards
//! it to the Function variable stored in the field.
//!
//! Hand-made fixtures with that shape (no Apple IR), one kernel name each
//! (the demotion note is logged once per kernel). At d7080b0 the seven
//! translating fixtures fail: the field loops with the raw-construction
//! error ("pointer merge storage mismatch Function vs Private at
//! Local(\"%b\") across [Local(\"%acc0\"), Local(\"%b\")]"), the construct
//! tree slots of field loads with the primary error ("Private pointer phi
//! %metal2vulkan.ct.slot.0 has differing structural sources"), the mixed
//! slots with "... nextslot.0 ... feeds another pointer phi" and no
//! translator note. The two refusals (a field stored twice, a helper that
//! writes the field) pass there too and must stay refusals.

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

/// `%acc0` a Function float, `%slot` = &local[i] in a Function array,
/// `%g0` = &weights[i] (the Private table), `%cap` a capture struct whose
/// field 0 (`%field`) holds `stored` and whose field 2 (`%field2`) holds
/// `stored2` (both stored in the entry block).
fn head(kernel: &str, stored: &str, stored2: &str) -> String {
    format!(
        r#"target triple = "air64_v28-apple-macosx26.5.0"
%Input = type {{ [4 x i32] }}
%Output = type {{ [4 x i32] }}
%Captures = type {{ ptr, i32, ptr }}
{TABLE}

define void @{kernel}(ptr addrspace(2) %in, ptr addrspace(1) %out) {{
entry:
  %ip = getelementptr inbounds %Input, ptr addrspace(2) %in, i64 0, i32 0, i64 0
  %i = load i32, ptr addrspace(2) %ip
  %c = icmp ult i32 %i, 4
  %acc0 = alloca float, align 4
  store float 0.0, ptr %acc0, align 4
  %local = alloca [9 x float], align 4
  %slot = getelementptr inbounds [9 x float], ptr %local, i32 0, i32 %i
  store float 1.0, ptr %slot, align 4
  %g0 = getelementptr inbounds [9 x float], ptr @weights, i32 0, i32 %i
  %cap = alloca %Captures, align 8
  %field = getelementptr inbounds %Captures, ptr %cap, i64 0, i32 0
  %field2 = getelementptr inbounds %Captures, ptr %cap, i64 0, i32 2
  store ptr {stored}, ptr %field, align 8
  store ptr {stored2}, ptr %field2, align 8
  %o0 = getelementptr inbounds %Output, ptr addrspace(1) %out, i64 0, i32 0, i64 0
"#
    )
}

/// A loop whose pointer `%p` starts at a load of `%field` and takes, on the
/// back edge, a load of `back_field` made in the latch (after `%p`).
fn field_loop(kernel: &str, stored: &str, stored2: &str, back_field: &str) -> String {
    format!(
        r#"{head}  %a = load ptr, ptr %field, align 8
  br label %loop
loop:
  %p = phi ptr [ %a, %entry ], [ %b, %latch ]
  %n = phi i32 [ 0, %entry ], [ %n1, %latch ]
  %acc = phi float [ 0.0, %entry ], [ %acc1, %latch ]
  %v = load float, ptr %p, align 4
  %acc1 = fadd float %acc, %v
  %n1 = add i32 %n, 1
  br label %latch
latch:
  %b = load ptr, ptr {back_field}, align 8
  %more = icmp ult i32 %n1, 3
  br i1 %more, label %loop, label %done
done:
  %vi = bitcast float %acc1 to i32
  store i32 %vi, ptr addrspace(1) %o0
  ret void
}}
{tail}"#,
        head = head(kernel, stored, stored2),
        tail = tail(kernel)
    )
}

/// The construct tree's dispatch loop as renest.rs writes it: case 0 picks
/// case 1 (a load of `%field`) or case 2 (`case2` defines `%l2`), case 3
/// reads the slot and accumulates into the Function float through a load of
/// `%field` (the real kernel's `store float ..., ptr %1425`).
fn construct_tree(kernel: &str, stored: &str, stored2: &str, case2: &str) -> String {
    format!(
        r#"{head}  br label %metal2vulkan.ct.header
metal2vulkan.ct.header:
  %metal2vulkan.ct.pc = phi i32 [ 0, %entry ], [ %metal2vulkan.ct.nextpc, %metal2vulkan.ct.continue ]
  %metal2vulkan.ct.slot.0 = phi ptr [ undef, %entry ], [ %metal2vulkan.ct.nextslot.0, %metal2vulkan.ct.continue ]
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
  %l1 = load ptr, ptr %field, align 8
  br label %metal2vulkan.ct.casejoin.1
metal2vulkan.ct.casejoin.1:
  br label %metal2vulkan.ct.merge
case2:
{case2}
  br label %metal2vulkan.ct.casejoin.2
metal2vulkan.ct.casejoin.2:
  br label %metal2vulkan.ct.merge
case3:
  %v = load float, ptr %metal2vulkan.ct.slot.0, align 4
  %sum = load ptr, ptr %field, align 8
  %old = load float, ptr %sum, align 4
  %new = fadd float %old, %v
  store float %new, ptr %sum, align 4
  %vi = bitcast float %new to i32
  store i32 %vi, ptr addrspace(1) %o0
  br label %metal2vulkan.ct.casejoin.3
metal2vulkan.ct.casejoin.3:
  br label %metal2vulkan.ct.merge
metal2vulkan.ct.invalid:
  br label %metal2vulkan.ct.merge
metal2vulkan.ct.merge:
  %metal2vulkan.ct.nextpc = phi i32 [ %metal2vulkan.ct.casepc.0, %metal2vulkan.ct.casejoin.0 ], [ 3, %metal2vulkan.ct.casejoin.1 ], [ 3, %metal2vulkan.ct.casejoin.2 ], [ 4, %metal2vulkan.ct.casejoin.3 ], [ 4, %metal2vulkan.ct.invalid ]
  %metal2vulkan.ct.nextslot.0 = phi ptr [ %metal2vulkan.ct.slot.0, %metal2vulkan.ct.casejoin.0 ], [ %l1, %metal2vulkan.ct.casejoin.1 ], [ %l2, %metal2vulkan.ct.casejoin.2 ], [ %metal2vulkan.ct.slot.0, %metal2vulkan.ct.casejoin.3 ], [ %metal2vulkan.ct.slot.0, %metal2vulkan.ct.invalid ]
  br label %metal2vulkan.ct.continue
metal2vulkan.ct.continue:
  br label %metal2vulkan.ct.header
metal2vulkan.ct.exit:
  ret void
}}
{tail}"#,
        head = head(kernel, stored, stored2),
        tail = tail(kernel)
    )
}

fn tmp() -> std::path::PathBuf {
    let dir = std::env::temp_dir().join(format!("m2v_field_phi_{}", std::process::id()));
    let _ = std::fs::create_dir_all(&dir);
    dir
}

/// Translates (spirv-val run again here), returns the disassembly and this
/// translation's notes; no Function or Private pointer may be an OpPhi.
fn translate(ll: &str) -> (String, Vec<String>) {
    let _ = notes::take();
    let spv = translate_sanitized_native(ll, Stage::Kernel, &tmp())
        .unwrap_or_else(|error| panic!("the field-load phi must translate: {error}"));
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
    (asm, taken)
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

/// The variable a pointer is based on, through access chains and copies.
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

/// The storage classes of the variables the loads read.
fn loaded_storages(asm: &str) -> Vec<String> {
    let defs = definitions(asm);
    asm.lines()
        .filter(|line| line.contains("= OpLoad"))
        .filter_map(|line| line.split_whitespace().nth(4))
        .filter_map(|ptr| base_variable(&defs, ptr))
        .filter_map(|var| defs[var].get(4).map(|storage| storage.to_string()))
        .collect()
}

/// The note the host logs as `translator note: ...`: the N75 form when the
/// phi was split from the network, the N72 form when every value was seen
/// (a forwarded field load is its stored pointer, which may be).
fn assert_noted(notes: &[String], kernel: &str, phi: &str, loop_carried: bool) {
    let expected = format!(
        "metal2vulkan: kernel {kernel}: pointer phi {phi} merges Function and Private storage; demoted to a per-storage value select{}",
        if loop_carried {
            "; its network is loop-carried (DRIVER-PLAN N72, N75)"
        } else {
            " (DRIVER-PLAN N72)"
        }
    );
    assert!(
        notes.iter().any(|note| note == &expected),
        "missing note {expected:?}; got {notes:?}"
    );
}

fn assert_reads_both(asm: &str) {
    let storages = loaded_storages(asm);
    assert!(
        storages.iter().any(|storage| storage == "Private")
            && storages.iter().any(|storage| storage == "Function"),
        "both arms must be read {storages:?}\n{asm}"
    );
}

#[test]
fn a_loop_over_a_field_load_translates() {
    // The raw-construction error in its smallest form: `%p` merges two
    // loads of one field holding a scalar Function variable, the second
    // emitted after the phi.
    let (asm, notes) = translate(&field_loop("fl_one", "%acc0", "%g0", "%field"));
    let storages = loaded_storages(&asm);
    assert!(
        storages.iter().any(|storage| storage == "Function")
            && !storages.iter().any(|storage| storage == "Private"),
        "the loop must read the Function float only {storages:?}\n{asm}"
    );
    assert!(notes.is_empty(), "nothing is demoted: {notes:?}");
}

#[test]
fn a_construct_tree_slot_of_field_loads_translates() {
    // The primary error's shape (the real kernel's slot.48): `slot.0`
    // merges `undef` and the forward `nextslot.0`, whose cases are loads of
    // one field; case 3 also accumulates through a load of it.
    let (asm, notes) = translate(&construct_tree(
        "ct_fields",
        "%acc0",
        "%g0",
        "  %l2 = load ptr, ptr %field, align 8",
    ));
    let storages = loaded_storages(&asm);
    assert!(
        storages.iter().any(|storage| storage == "Function")
            && !storages.iter().any(|storage| storage == "Private"),
        "nothing Private is read {storages:?}\n{asm}"
    );
    assert!(notes.is_empty(), "nothing is demoted: {notes:?}");
}

#[test]
fn a_construct_tree_slot_of_a_field_load_and_the_table_is_split() {
    // The mixed form: a load of a field holding &local[i] (Function) in one
    // case, the Private table in the other. The slot is an N75 network
    // member now (the load was "unknown" before), and its demotion is noted.
    let (asm, notes) = translate(&construct_tree(
        "ct_mixed",
        "%slot",
        "%g0",
        "  %l2 = getelementptr inbounds [9 x float], ptr @weights, i32 0, i32 %i",
    ));
    assert_reads_both(&asm);
    assert_noted(&notes, "ct_mixed", "%metal2vulkan.ct.slot.0", true);
}

#[test]
fn a_loop_over_fields_holding_both_storages_is_split() {
    // `%p` = [&local[i] from %field, the table from %field2 on the back
    // edge]: mixed, through loads only. The back-edge load is the table,
    // already emitted, so N72 splits it as it is emitted.
    let (asm, notes) = translate(&field_loop("fl_mixed", "%slot", "%g0", "%field2"));
    assert_reads_both(&asm);
    assert_noted(&notes, "fl_mixed", "%p", false);
}

#[test]
fn a_mixed_merge_of_a_scalar_variable_is_split() {
    // The scalar `%acc0` (no GEP) and the table, both through field loads:
    // the Function arm is the variable itself.
    let (asm, notes) = translate(&field_loop("fl_scalar", "%acc0", "%g0", "%field2"));
    assert_reads_both(&asm);
    assert_noted(&notes, "fl_scalar", "%p", false);
}

#[test]
fn a_construct_tree_slot_of_a_scalar_and_the_table_is_split() {
    // The real kernel's capture holds a scalar `alloca float`; with the
    // table in the other case the slot is a loop-carried member.
    let (asm, notes) = translate(&construct_tree(
        "ct_scalar",
        "%acc0",
        "%g0",
        "  %l2 = getelementptr inbounds [9 x float], ptr @weights, i32 0, i32 %i",
    ));
    assert_reads_both(&asm);
    assert_noted(&notes, "ct_scalar", "%metal2vulkan.ct.slot.0", true);
}

#[test]
fn a_field_stored_twice_is_not_forwarded() {
    // Two stores (the second on one path only): the loaded pointer depends
    // on the path, so N76 does not forward it. (The emitter itself forwards
    // the store emitted last, `%g0`, on both paths: N76 OPEN.) The loop phi
    // must stay a named refusal, never a module reading one store.
    let ll = field_loop("fl_twice", "%slot", "%g0", "%field")
        .replace(
            "  %a = load ptr, ptr %field, align 8\n  br label %loop\n",
            "  br i1 %c, label %restore, label %reload\nrestore:\n  store ptr %g0, ptr %field, align 8\n  br label %reload\nreload:\n  %a = load ptr, ptr %field, align 8\n  br label %loop\n",
        )
        .replace("[ %a, %entry ]", "[ %a, %reload ]")
        .replace("[ 0, %entry ]", "[ 0, %reload ]")
        .replace("[ 0.0, %entry ]", "[ 0.0, %reload ]");
    match translate_sanitized_native(&ll, Stage::Kernel, &tmp()) {
        Ok(_) => panic!("a phi over a field with two stores must be refused"),
        Err(error) => assert!(
            error.contains("pointer phi %p has differing structural sources")
                && !error.contains("undefined id"),
            "must be a named refusal: {error}"
        ),
    }
}

/// A helper the capture is passed to, as the real kernel's lambdas take
/// theirs: it reads field 0 and the float behind it; with `store_field` it
/// also writes field 0 (back the pointer it read: a second store).
fn helper(store_field: bool) -> String {
    format!(
        r#"
define internal float @read_capture(ptr %c) {{
entry:
  %cf = getelementptr inbounds %Captures, ptr %c, i64 0, i32 0
  %cp = load ptr, ptr %cf, align 8
  %cv = load float, ptr %cp, align 4
{}  ret float %cv
}}
"#,
        if store_field {
            "  store ptr %cp, ptr %cf, align 8\n"
        } else {
            ""
        }
    )
}

fn with_helper_call(ll: String, store_field: bool) -> String {
    ll.replacen("define void", &format!("{}\ndefine void", helper(store_field)), 1)
        .replacen(
            "  %vi = bitcast float %new to i32\n",
            "  %h = call float @read_capture(ptr %cap)\n  %hs = fadd float %new, %h\n  %vi = bitcast float %hs to i32\n",
            1,
        )
}

#[test]
fn a_capture_passed_to_a_reading_helper_is_still_forwarded() {
    // The real kernel passes its capture struct to lambdas that only read
    // it: the field loads stay forwarded.
    let ll = with_helper_call(
        construct_tree(
            "ct_helper",
            "%acc0",
            "%g0",
            "  %l2 = load ptr, ptr %field, align 8",
        ),
        false,
    );
    assert!(ll.contains("call float @read_capture"), "{ll}");
    let (asm, notes) = translate(&ll);
    assert!(
        !loaded_storages(&asm).iter().any(|storage| storage == "Private"),
        "nothing Private is read\n{asm}"
    );
    assert!(notes.is_empty(), "nothing is demoted: {notes:?}");
}

#[test]
fn a_capture_passed_to_a_writing_helper_is_not_forwarded() {
    // A helper that stores into the capture may change a field: no load of
    // it is forwarded, and the slot keeps the old named refusal.
    let ll = with_helper_call(
        construct_tree(
            "ct_writer",
            "%acc0",
            "%g0",
            "  %l2 = load ptr, ptr %field, align 8",
        ),
        true,
    );
    match translate_sanitized_native(&ll, Stage::Kernel, &tmp()) {
        Ok(_) => panic!("a slot over a capture a helper writes must stay refused"),
        Err(error) => assert!(
            error.contains("pointer phi %metal2vulkan.ct.slot.0 has differing structural sources")
                && !error.contains("undefined id"),
            "must be the named refusal: {error}"
        ),
    }
}

/// A two-block helper (the ordinary leaf inliner keeps it a call) that
/// loads the Function pointer out of the capture it is handed and
/// accumulates through it, as the real kernel's lambdas do.
const LAMBDA: &str = r#"
define internal fastcc zeroext i1 @lambda(ptr %c, float %x) {
entry:
  %cf = getelementptr inbounds %Captures, ptr %c, i64 0, i32 0
  %cp = load ptr, ptr %cf, align 8
  %pos = fcmp ogt float %x, 0.0
  br i1 %pos, label %add, label %skip
add:
  %old = load float, ptr %cp, align 4
  %new = fadd float %old, %x
  store float %new, ptr %cp, align 4
  br label %skip
skip:
  ret i1 %pos
}
"#;

#[test]
fn a_capture_handed_to_a_multi_block_lambda_is_spliced() {
    // Before the splice: "store through %cp, an unmodeled pointer
    // placeholder that addresses nothing" (the callee read the placeholder
    // the caller's store left in memory).
    let ll = construct_tree("ct_lambda", "%acc0", "%g0", "  %l2 = load ptr, ptr %field, align 8")
        .replacen("define void", &format!("{LAMBDA}\ndefine void"), 1)
        .replacen(
            "  %vi = bitcast float %new to i32\n",
            "  %took = call fastcc zeroext i1 @lambda(ptr %cap, float %v)\n  %vi = bitcast float %new to i32\n",
            1,
        );
    let (asm, notes) = translate(&ll);
    assert!(
        !asm.contains("OpFunctionCall"),
        "the lambda must be spliced into the kernel\n{asm}"
    );
    let expected = "metal2vulkan: kernel ct_lambda: 1 call(s) hand a local struct of pointers to a function that loads them (@lambda(%cap)); splicing them into their callers (DRIVER-PLAN N76)";
    assert!(
        notes.iter().any(|note| note == expected),
        "missing note {expected:?}; got {notes:?}"
    );
}
