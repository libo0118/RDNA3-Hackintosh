//! DRIVER-PLAN N76: calls that hand a function a local struct of pointers.
//!
//! The emitter keeps no pointer in memory: a store of a pointer into a field
//! of a Function variable only records it for the caller's later loads
//! (tir/field_forward.rs), and memory gets a placeholder. A callee that
//! loads a pointer out of such a struct reads the placeholder, and its first
//! store through it is refused ("store through %98, an unmodeled pointer
//! placeholder that addresses nothing"). Geekbench 7's MPS
//! `depthwiseConv_3x3` hands its lambdas their capture struct this way (the
//! lambdas have several blocks, so the ordinary leaf inliner keeps them).
//!
//! Such call sites are found before anything is emitted and refused all at
//! once as cursor call sites, so the emission retries once with them spliced
//! into their callers (emit_tiers.rs, inline.rs), where the loads are
//! forwarded like any other.

use super::*;
use crate::native::ir::LlFunction;
use crate::native::tir::{TirInst, TirOpcode, TirOperand};

/// The pointers derived from `seeds` in `function` (GEPs, identity copies,
/// inlined-parameter copies, freezes), seeds included, each with its seed.
fn derived_addresses(function: &LlFunction, seeds: &[String]) -> HashMap<String, String> {
    let mut addresses = seeds
        .iter()
        .map(|seed| (seed.clone(), seed.clone()))
        .collect::<HashMap<_, _>>();
    let base_of = |inst: &TirInst| -> Option<String> {
        match inst.opcode {
            TirOpcode::GetElementPtr => match &inst.gep().as_ref()?.base.value {
                LlValue::Local(base) => Some(base.clone()),
                _ => None,
            },
            TirOpcode::Bitcast | TirOpcode::Freeze | TirOpcode::Metal2VulkanInlineParameter => {
                match inst.operands.first()? {
                    TirOperand::Value { name, .. } => Some(name.clone()),
                    _ => None,
                }
            }
            _ => None,
        }
    };
    let mut changed = true;
    while changed {
        changed = false;
        for inst in function.carrier_insts() {
            let Some(result) = &inst.result else {
                continue;
            };
            if addresses.contains_key(result) || !matches!(inst.result_ty, Some(LlType::Ptr(_))) {
                continue;
            }
            let Some(seed) = base_of(inst).and_then(|base| addresses.get(&base).cloned()) else {
                continue;
            };
            addresses.insert(result.clone(), seed);
            changed = true;
        }
    }
    addresses
}

/// Whether `callee` loads an address-space-0 pointer through its parameter
/// `index`.
fn loads_pointer_through(callee: &LlFunction, index: usize) -> bool {
    let Some((param, LlType::Ptr(_))) = callee.params.get(index) else {
        return false;
    };
    let addresses = derived_addresses(callee, std::slice::from_ref(param));
    callee.carrier_insts().any(|inst| {
        inst.opcode == TirOpcode::Load
            && inst.result_ty == Some(LlType::Ptr(0))
            && matches!(
                inst.operands.first(),
                Some(TirOperand::Value { name, .. }) if addresses.contains_key(name)
            )
    })
}

/// Every `(callee, argument)` where a function passes a pointer into one of
/// its variables that holds a stored address-space-0 pointer (a Function or
/// Private pointer, which only the caller's forwarding knows) to a function
/// of the module that loads such a pointer through that parameter. Device
/// pointers in a struct have their own paths (raw buffers, BDA).
pub(super) fn pointer_struct_call_sites(functions: &[LlFunction]) -> Vec<(String, String)> {
    let mut sites = Vec::new();
    for function in functions {
        let roots = function
            .carrier_insts()
            .filter(|inst| inst.opcode == TirOpcode::Alloca)
            .filter_map(|inst| inst.result.clone())
            .collect::<Vec<_>>();
        if roots.is_empty() {
            continue;
        }
        let addresses = derived_addresses(function, &roots);
        let tables = function
            .carrier_insts()
            .filter_map(|inst| {
                let store = inst.store().as_ref()?;
                let (value, ptr) = store.as_ref();
                let LlValue::Local(ptr) = &ptr.value else {
                    return None;
                };
                (value.ty == LlType::Ptr(0))
                    .then(|| addresses.get(ptr).cloned())
                    .flatten()
            })
            .collect::<HashSet<_>>();
        if tables.is_empty() {
            continue;
        }
        for inst in function.carrier_insts() {
            let Some(call) = inst.call().as_ref() else {
                continue;
            };
            let Some(callee) = functions.iter().find(|f| f.name == call.callee) else {
                continue;
            };
            for (index, argument) in call.args.iter().enumerate() {
                let LlValue::Local(name) = &argument.value else {
                    continue;
                };
                let passes_table = addresses
                    .get(name)
                    .is_some_and(|root| tables.contains(root));
                let site = (callee.name.clone(), name.clone());
                if passes_table && loads_pointer_through(callee, index) && !sites.contains(&site) {
                    sites.push(site);
                }
            }
        }
    }
    sites
}
