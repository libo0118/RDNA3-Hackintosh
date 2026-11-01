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
//! forwarded like any other. The callee is followed into the module
//! functions it hands the struct on to. A call the splice cannot make right
//! is refused by name instead of reading the placeholder: a callee that
//! stores a pointer into the struct (the caller's forwarding binds loads to
//! one store, flow-insensitively), a struct merged by a select or phi
//! before the call, a callee that passes it to a function outside the
//! module, merges, returns or stores it, or recurses with it.

use super::*;
use crate::native::ir::LlFunction;
use crate::native::tir::{TirInst, TirOpcode, TirOperand, TirTerminator};

/// What a function does with a pointer parameter into a struct of
/// pointers, worst first in `Ord` (a call's uses combine by `max`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
enum StructUse {
    /// Reads or writes integers, floats and vectors through it only.
    Scalars,
    /// Loads an address-space-0 pointer out of it: splice the call.
    LoadsPointer,
    /// Stores a pointer into it.
    StoresPointer,
    /// Hands it somewhere the analysis does not follow.
    Untracked,
}

fn operand_name(operand: Option<&TirOperand>) -> Option<&str> {
    match operand? {
        TirOperand::Value { name, .. } => Some(name.as_str()),
        _ => None,
    }
}

fn is_lifetime_marker(inst: &TirInst) -> bool {
    inst.call().as_ref().is_some_and(|call| {
        call.callee
            .trim_start_matches('@')
            .starts_with("llvm.lifetime.")
    }) || inst
        .void_call_line()
        .as_deref()
        .is_some_and(|line| line.contains("@llvm.lifetime."))
}

/// The pointers derived from `seeds` in `function` (GEPs, identity copies,
/// inlined-parameter copies, freezes), seeds included, each with its seed;
/// and the selects and phis that merge one of them.
fn derived_addresses(
    function: &LlFunction,
    seeds: &[String],
) -> (HashMap<String, String>, HashMap<String, String>) {
    let mut addresses = seeds
        .iter()
        .map(|seed| (seed.clone(), seed.clone()))
        .collect::<HashMap<_, _>>();
    let mut merged = HashMap::new();
    let base_of = |inst: &TirInst| -> Option<String> {
        match inst.opcode {
            TirOpcode::GetElementPtr => match &inst.gep().as_ref()?.base.value {
                LlValue::Local(base) => Some(base.clone()),
                _ => None,
            },
            TirOpcode::Bitcast | TirOpcode::Freeze | TirOpcode::Metal2VulkanInlineParameter => {
                operand_name(inst.operands.first()).map(str::to_string)
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
            if addresses.contains_key(result)
                || merged.contains_key(result)
                || !matches!(inst.result_ty, Some(LlType::Ptr(_)))
            {
                continue;
            }
            if matches!(inst.opcode, TirOpcode::Select | TirOpcode::Phi) {
                let mut seed = None;
                inst.visit_uses(|name| {
                    if seed.is_none() {
                        seed = addresses.get(name).or_else(|| merged.get(name)).cloned();
                    }
                });
                if let Some(seed) = seed {
                    merged.insert(result.clone(), seed);
                    changed = true;
                }
                continue;
            }
            let Some(base) = base_of(inst) else {
                continue;
            };
            if let Some(seed) = addresses.get(&base).cloned() {
                addresses.insert(result.clone(), seed);
                changed = true;
            } else if let Some(seed) = merged.get(&base).cloned() {
                merged.insert(result.clone(), seed);
                changed = true;
            }
        }
    }
    (addresses, merged)
}

/// What `callee` does with its parameter `index` (see `StructUse`),
/// following the module functions it passes the parameter on to.
fn struct_use(
    functions: &[LlFunction],
    callee: &LlFunction,
    index: usize,
    visiting: &mut HashSet<(String, usize)>,
) -> StructUse {
    let Some((param, LlType::Ptr(_))) = callee.params.get(index) else {
        return StructUse::Scalars;
    };
    if !visiting.insert((callee.name.clone(), index)) {
        // Recursion with the struct: the splice cannot end.
        return StructUse::Untracked;
    }
    let (addresses, merged) = derived_addresses(callee, std::slice::from_ref(param));
    let is_address = |name: &str| addresses.contains_key(name);
    let mut worst = if merged.is_empty() {
        StructUse::Scalars
    } else {
        StructUse::Untracked
    };
    for inst in callee.carrier_insts() {
        let defines_address = inst
            .result
            .as_deref()
            .is_some_and(|result| is_address(result) && result != param);
        if defines_address || is_lifetime_marker(inst) {
            continue;
        }
        let found = match inst.opcode {
            TirOpcode::Load if operand_name(inst.operands.first()).is_some_and(is_address) => {
                if inst.result_ty == Some(LlType::Ptr(0)) {
                    StructUse::LoadsPointer
                } else {
                    StructUse::Scalars
                }
            }
            TirOpcode::Store => match inst.store().as_deref() {
                Some((value, ptr)) => {
                    let into = matches!(&ptr.value, LlValue::Local(name) if is_address(name));
                    let stores_address =
                        matches!(&value.value, LlValue::Local(name) if is_address(name));
                    if stores_address {
                        StructUse::Untracked
                    } else if into && matches!(value.ty, LlType::Ptr(_)) {
                        StructUse::StoresPointer
                    } else {
                        StructUse::Scalars
                    }
                }
                None => StructUse::Untracked,
            },
            TirOpcode::Call => match inst.call().as_ref() {
                Some(call) => {
                    let next = functions.iter().find(|f| f.name == call.callee);
                    let mut used = StructUse::Scalars;
                    for (position, argument) in call.args.iter().enumerate() {
                        let LlValue::Local(name) = &argument.value else {
                            continue;
                        };
                        if !is_address(name) {
                            continue;
                        }
                        used = used.max(match next {
                            Some(next) => struct_use(functions, next, position, visiting),
                            None => StructUse::Untracked,
                        });
                    }
                    let mut nested = false;
                    inst.visit_uses(|name| {
                        nested |= is_address(name)
                            && !call
                                .args
                                .iter()
                                .any(|a| matches!(&a.value, LlValue::Local(n) if n == name));
                    });
                    if nested {
                        StructUse::Untracked
                    } else {
                        used
                    }
                }
                None => {
                    let mut uses = false;
                    inst.visit_uses(|name| uses |= is_address(name));
                    if uses {
                        StructUse::Untracked
                    } else {
                        StructUse::Scalars
                    }
                }
            },
            _ => {
                let mut uses = false;
                inst.visit_uses(|name| uses |= is_address(name));
                if uses {
                    StructUse::Untracked
                } else {
                    StructUse::Scalars
                }
            }
        };
        worst = worst.max(found);
    }
    let returned = callee.blocks.iter().any(|block| {
        block.typed.as_ref().is_some_and(|typed| {
            matches!(&typed.terminator, TirTerminator::Ret(Some(name)) if is_address(name))
        })
    });
    if returned {
        worst = StructUse::Untracked;
    }
    visiting.remove(&(callee.name.clone(), index));
    worst
}

/// Every `(callee, argument)` where a function passes a pointer into one of
/// its variables that holds a stored address-space-0 pointer (a Function or
/// Private pointer, which only the caller's forwarding knows) to a function
/// of the module that loads such a pointer through that parameter, directly
/// or in a function it hands it on to. Err names a call the splice cannot
/// make right (see the module comment). Device pointers in a struct have
/// their own paths (raw buffers, BDA); calls outside the module are left to
/// them too.
pub(super) fn pointer_struct_call_sites(
    functions: &[LlFunction],
) -> Result<Vec<(String, String)>, String> {
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
        let (addresses, merged) = derived_addresses(function, &roots);
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
        // A pointer loaded out of a struct chosen by a select or phi (a
        // select-consuming call spliced earlier, inline.rs, makes one): the
        // forwarding is per variable, so it would read the placeholder.
        for inst in function.carrier_insts() {
            if inst.opcode != TirOpcode::Load || inst.result_ty != Some(LlType::Ptr(0)) {
                continue;
            }
            let Some(address) = operand_name(inst.operands.first()) else {
                continue;
            };
            if merged.get(address).is_some_and(|root| tables.contains(root)) {
                return Err(format!(
                    "native emitter: {} in @{} loads a pointer out of a local struct of pointers chosen by a select or phi ({address}); not supported (N76)",
                    inst.result.as_deref().unwrap_or("a load"),
                    function.name
                ));
            }
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
                let (root, is_merged) = match (addresses.get(name), merged.get(name)) {
                    (Some(root), _) => (root, false),
                    (None, Some(root)) => (root, true),
                    (None, None) => continue,
                };
                if !tables.contains(root) {
                    continue;
                }
                let used = struct_use(functions, callee, index, &mut HashSet::new());
                let refuse = |why: &str| {
                    Err(format!(
                        "native emitter: @{} is handed {name}, a local struct holding pointers (in @{}), and {why}; not supported (N76)",
                        callee.name, function.name
                    ))
                };
                match used {
                    StructUse::Scalars => {}
                    StructUse::LoadsPointer if is_merged => {
                        return refuse(
                            "loads a pointer through it, but the struct is chosen by a select or phi",
                        );
                    }
                    StructUse::LoadsPointer => {
                        let site = (callee.name.clone(), name.clone());
                        if !sites.contains(&site) {
                            sites.push(site);
                        }
                    }
                    StructUse::StoresPointer => {
                        return refuse("stores a pointer into it");
                    }
                    StructUse::Untracked => {
                        return refuse(
                            "hands it on where the translator cannot follow it (recursion, a function outside the module, a merge, a return or a store of the address)",
                        );
                    }
                }
            }
        }
    }
    Ok(sites)
}
