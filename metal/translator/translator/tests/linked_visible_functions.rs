//! Visible functions linked at pipeline creation (MTLLinkedFunctions), the path
//! `nvmtl_link_visible` takes: the kernel's stubs become direct calls to the linked
//! definitions before SPIR-V emission, because Logical SPIR-V has no function pointers.
//!
//! The shape of MPS's `AMDConvolution::ndArrayConvolution2DAMD_NHWC` on the bare-metal Mac
//! (2026-10-31): C++-qualified names, and stubs `!air.visible_function_references` does not
//! list. Those stubs used to survive the link and the emitter refused the kernel with
//! "unsupported Metal visible function reference".

use metal2vulkan::linked_functions::{
    visible_function_reference_symbols, LinkedFunctionLinkage, LinkedFunctionReference,
};
use metal2vulkan::passes::{Stage, TransformOptions};

const KERNEL: &str = r#"target triple = "air64_v27-apple-macosx15.7.0"

declare i32 @"AMDConvolution::act.MTL_VISIBLE_FN_REF"(i32) local_unnamed_addr section "air.externally_defined"
declare i32 @scale.MTL_VISIBLE_FN_REF(i32) local_unnamed_addr section "air.externally_defined"

define void @"AMDConvolution::ndArrayConvolution2DAMD_NHWC"(ptr addrspace(1) %out) {
entry:
  %a = call i32 @"AMDConvolution::act.MTL_VISIBLE_FN_REF"(i32 41)
  %b = call i32 @scale.MTL_VISIBLE_FN_REF(i32 %a)
  store i32 %b, ptr addrspace(1) %out, align 4
  ret void
}

!air.kernel = !{!0}
!air.visible_function_references = !{!5}
!0 = !{ptr @"AMDConvolution::ndArrayConvolution2DAMD_NHWC", !1, !2}
!1 = !{}
!2 = !{!3}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"int", !"air.arg_name", !"out"}
!5 = !{!"air.visible_function_reference", ptr @scale.MTL_VISIBLE_FN_REF, !"scale"}
"#;

const ACT: &str = r#"target triple = "air64_v27-apple-macosx15.7.0"

define i32 @"AMDConvolution::act"(i32 %x) {
entry:
  %y = add i32 %x, 1
  ret i32 %y
}
"#;

const SCALE: &str = r#"target triple = "air64_v27-apple-macosx15.7.0"

define i32 @scale(i32 %x) {
entry:
  %y = mul i32 %x, 3
  ret i32 %y
}
"#;

fn linkage(with_act: bool) -> LinkedFunctionLinkage {
    let mut visible_references = vec![LinkedFunctionReference {
        symbol: "scale".into(),
        module_ll: SCALE.into(),
    }];
    if with_act {
        visible_references.push(LinkedFunctionReference {
            symbol: "AMDConvolution::act".into(),
            module_ll: ACT.into(),
        });
    }
    LinkedFunctionLinkage {
        visible_references,
        visible_tables: vec![],
        intersection_tables: vec![],
    }
}

fn translate(ll: &str) -> Result<Vec<u8>, String> {
    metal2vulkan::translate_native_no_retry_constructed_with_options(
        ll,
        Stage::Kernel,
        TransformOptions::default(),
    )
}

#[test]
fn the_plugin_is_told_every_stub_listed_or_not() {
    let symbols = visible_function_reference_symbols(KERNEL).unwrap();
    assert_eq!(symbols, vec!["scale", "AMDConvolution::act"]);
}

#[test]
fn linked_visible_functions_become_direct_calls_and_translate() {
    metal2vulkan::visible_stubs::reset();
    let linked = metal2vulkan::specialize_linked_module(KERNEL, Stage::Kernel, &linkage(true))
        .expect("links");
    let report = metal2vulkan::visible_stubs::take();
    assert!(!linked.contains(".MTL_VISIBLE_FN_REF"), "{linked}");
    assert!(
        linked.contains("call i32 @\"AMDConvolution::act\"(i32 41)"),
        "{linked}"
    );
    assert!(linked.contains("call i32 @scale(i32 %a)"), "{linked}");
    assert_eq!(report.linked.len(), 2, "{report:?}");
    assert!(report.unresolved.is_empty(), "{report:?}");
    assert_eq!(
        report.unlisted.iter().map(String::as_str).collect::<Vec<_>>(),
        vec!["AMDConvolution::act"]
    );
    let summary = report.summary();
    assert!(
        summary.starts_with("linked 2 [AMDConvolution::act, scale]; unresolved 0 []"),
        "{summary}"
    );
    let spv = translate(&linked).expect("the linked kernel translates");
    assert!(spv.len() > 20 && spv[..4] == [0x03, 0x02, 0x23, 0x07]);
}

#[test]
fn an_unsupplied_visible_function_called_on_a_live_path_is_refused_by_name() {
    metal2vulkan::visible_stubs::reset();
    let linked = metal2vulkan::specialize_linked_module(KERNEL, Stage::Kernel, &linkage(false))
        .expect("an unsupplied reference is left unresolved, not refused at link time");
    let report = metal2vulkan::visible_stubs::take();
    assert_eq!(
        report.unresolved.iter().map(String::as_str).collect::<Vec<_>>(),
        vec!["AMDConvolution::act"]
    );
    let err = translate(&linked).expect_err("a live call to an unlinked function is refused");
    assert!(
        err.contains("\"AMDConvolution::act\" is called on a live path"),
        "{err}"
    );
}

#[test]
fn an_unlinked_kernel_is_refused_with_the_names_it_needs() {
    let err = translate(KERNEL).expect_err("visible stubs cannot be emitted");
    assert!(
        err.contains("unsupported Metal visible function reference"),
        "{err}"
    );
    assert!(err.contains("[AMDConvolution::act, scale]"), "{err}");
}
