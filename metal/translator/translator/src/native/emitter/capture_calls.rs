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
//! functions it hands the struct (or a field address of it) on to, and
//! every access is kept as a field path of the caller's variable. Only a
//! struct some callee loads a pointer out of needs the splice; for it, a
//! use the splice cannot make right is refused by name instead of reading
//! the placeholder: a callee that stores a pointer into the struct (the
//! caller's forwarding binds loads to one store, flow-insensitively), an
//! integer, vector or aggregate store over one of its pointer fields, a
//! pointer loaded through a struct nested inside it (two GEP levels), a
//! struct chosen by a select or phi of different variables, a callee that
//! passes it outside the module, merges, returns or stores it, or recurses
//! with it.

use super::*;
use crate::native::ir::LlFunction;
use crate::native::tir::{TirInst, TirOpcode, TirOperand, TirTerminator};

/// A pointer into one of the caller's variables: the variable, the GEP
/// type the field path is in (None for the variable itself), the constant
/// field path (None when dynamic or below a nested struct) and how many GEP
/// levels it is below the variable.
#[derive(Clone, Debug, PartialEq, Eq)]
struct Address {
    root: String,
    gep_type: Option<String>,
    key: Option<Vec<u64>>,
    depth: usize,
}

impl Address {
    fn root(root: &str) -> Self {
        Address {
            root: root.to_string(),
            gep_type: None,
            key: Some(Vec::new()),
            depth: 0,
        }
    }

    fn field(&self, gep: &crate::native::ir::LlGep) -> Self {
        let indices = gep
            .indices
            .iter()
            .map(|index| match index.value {
                LlValue::Int(v) | LlValue::Hex(v) => Some(v),
                LlValue::SignedInt(v) if v >= 0 => Some(v as u64),
                _ => None,
            })
            .collect::<Option<Vec<_>>>();
        let local = indices.map(|mut key| {
            if key.len() > 1 && key.first() == Some(&0) {
                key.remove(0);
            }
            key
        });
        if self.depth == 0 {
            Address {
                root: self.root.clone(),
                gep_type: Some(format!("{:?}", gep.source_ty)),
                key: local,
                depth: 1,
            }
        } else {
            Address {
                root: self.root.clone(),
                gep_type: self.gep_type.clone(),
                key: None,
                depth: self.depth + 1,
            }
        }
    }

    /// Whether two accesses can touch the same bytes (conservative).
    fn overlaps(&self, other: &Address) -> bool {
        if self.root != other.root {
            return false;
        }
        match (&self.key, &other.key) {
            (Some(a), Some(b)) if a.is_empty() || b.is_empty() => true,
            (Some(a), Some(b)) => {
                self.gep_type != other.gep_type || a.starts_with(b) || b.starts_with(a)
            }
            _ => true,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Access {
    PointerLoad,
    PointerStore,
    OtherStore,
}

/// Everything a function (and what it calls) does with the caller's
/// variables, as field paths of them.
#[derive(Default, Debug)]
struct Uses {
    accesses: Vec<(Address, Access)>,
    /// Why some use cannot be followed (the first reason).
    untracked: Option<String>,
}

impl Uses {
    fn untrack(&mut self, why: impl Into<String>) {
        if self.untracked.is_none() {
            self.untracked = Some(why.into());
        }
    }
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

/// The addresses derived in `function` from `seeds` (GEPs, identity
/// copies, inlined-parameter copies, freezes, and selects/phis of one
/// address), and the selects/phis that merge different ones (by variable).
fn derived_addresses(
    function: &LlFunction,
    seeds: Vec<(String, Address)>,
) -> (HashMap<String, Address>, HashMap<String, String>) {
    let mut addresses = seeds.into_iter().collect::<HashMap<_, _>>();
    let mut merged: HashMap<String, String> = HashMap::new();
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
            let derived = match inst.opcode {
                TirOpcode::GetElementPtr => inst.gep().as_ref().and_then(|gep| match &gep.base.value {
                    LlValue::Local(base) => addresses.get(base).map(|a| a.field(gep)),
                    _ => None,
                }),
                TirOpcode::Bitcast
                | TirOpcode::Freeze
                | TirOpcode::Metal2VulkanInlineParameter => {
                    operand_name(inst.operands.first()).and_then(|n| addresses.get(n).cloned())
                }
                TirOpcode::Select | TirOpcode::Phi => {
                    let mut operands = Vec::new();
                    let mut other = false;
                    let values = inst
                        .phi_values()
                        .map(|values| values.cloned().collect::<Vec<_>>())
                        .or_else(|| {
                            inst.select_arms()
                                .as_deref()
                                .map(|(t, f)| vec![t.value.clone(), f.value.clone()])
                        })
                        .unwrap_or_default();
                    for value in &values {
                        match value {
                            LlValue::Local(name) if name == result => {}
                            LlValue::Local(name) => match addresses.get(name) {
                                Some(address) => operands.push(address.clone()),
                                None => other |= true,
                            },
                            _ => other = true,
                        }
                    }
                    let merges = values.iter().any(|value| {
                        matches!(value, LlValue::Local(name) if merged.contains_key(name))
                    });
                    match operands.first() {
                        Some(first)
                            if !other && !merges && operands.iter().all(|a| a == first) =>
                        {
                            Some(first.clone())
                        }
                        Some(first) => {
                            merged.insert(result.clone(), first.root.clone());
                            changed = true;
                            None
                        }
                        None => {
                            if merges {
                                let root = values.iter().find_map(|value| match value {
                                    LlValue::Local(name) => merged.get(name).cloned(),
                                    _ => None,
                                });
                                if let Some(root) = root {
                                    merged.insert(result.clone(), root);
                                    changed = true;
                                }
                            }
                            None
                        }
                    }
                }
                _ => None,
            };
            if let Some(address) = derived {
                addresses.insert(result.clone(), address);
                changed = true;
                continue;
            }
            // A field of a merged struct is merged too.
            let merged_base = match inst.opcode {
                TirOpcode::GetElementPtr => inst.gep().as_ref().and_then(|gep| match &gep.base.value {
                    LlValue::Local(base) => merged.get(base).cloned(),
                    _ => None,
                }),
                TirOpcode::Bitcast
                | TirOpcode::Freeze
                | TirOpcode::Metal2VulkanInlineParameter => {
                    operand_name(inst.operands.first()).and_then(|n| merged.get(n).cloned())
                }
                _ => None,
            };
            if let Some(root) = merged_base {
                merged.insert(result.clone(), root);
                changed = true;
            }
        }
    }
    (addresses, merged)
}

/// Collects into `uses` what `function` does with `addresses` (its seeds
/// are pointers into the caller's variables), following module callees.
fn collect_uses(
    functions: &[LlFunction],
    function: &LlFunction,
    seeds: Vec<(String, Address)>,
    visiting: &mut Vec<String>,
    uses: &mut Uses,
) {
    if visiting.contains(&function.name) {
        uses.untrack(format!("@{} recurses with it", function.name));
        return;
    }
    visiting.push(function.name.clone());
    let (addresses, merged) = derived_addresses(function, seeds);
    for name in merged.keys() {
        uses.untrack(format!("@{} merges it with another struct ({name})", function.name));
    }
    for inst in function.carrier_insts() {
        let defines = inst
            .result
            .as_ref()
            .is_some_and(|r| addresses.contains_key(r) || merged.contains_key(r));
        if defines || is_lifetime_marker(inst) {
            continue;
        }
        match inst.opcode {
            TirOpcode::Load => {
                if let Some(address) = operand_name(inst.operands.first()).and_then(|n| addresses.get(n)) {
                    if inst.result_ty == Some(LlType::Ptr(0)) {
                        uses.accesses.push((address.clone(), Access::PointerLoad));
                    }
                    continue;
                }
            }
            TirOpcode::Store => {
                if let Some((value, ptr)) = inst.store().as_deref() {
                    if matches!(&value.value, LlValue::Local(n) if addresses.contains_key(n) || merged.contains_key(n)) {
                        uses.untrack(format!("@{} stores its address", function.name));
                    }
                    let into = match &ptr.value {
                        LlValue::Local(n) => addresses.get(n),
                        _ => None,
                    };
                    if let Some(address) = into {
                        let access = if matches!(value.ty, LlType::Ptr(_)) {
                            Access::PointerStore
                        } else {
                            Access::OtherStore
                        };
                        uses.accesses.push((address.clone(), access));
                        continue;
                    }
                }
            }
            TirOpcode::Call => {
                if let Some(call) = inst.call() {
                    let mut handed = Vec::new();
                    for (index, argument) in call.args.iter().enumerate() {
                        if let LlValue::Local(name) = &argument.value {
                            if let Some(address) = addresses.get(name) {
                                handed.push((index, name.clone(), address.clone()));
                            } else if merged.contains_key(name) {
                                uses.untrack(format!("@{} passes a merged struct on", function.name));
                            }
                        }
                    }
                    let mut nested = false;
                    inst.visit_uses(|name| {
                        nested |= (addresses.contains_key(name) || merged.contains_key(name))
                            && !call.args.iter().any(|a| matches!(&a.value, LlValue::Local(n) if n == name));
                    });
                    if nested {
                        uses.untrack(format!("@{} uses it inside a call operand", function.name));
                    }
                    if handed.is_empty() {
                        continue;
                    }
                    let Some(next) = functions.iter().find(|f| f.name == call.callee) else {
                        uses.untrack(format!("@{} passes it to @{}, outside the module", function.name, call.callee));
                        continue;
                    };
                    let seeds = handed
                        .into_iter()
                        .filter_map(|(index, _, address)| {
                            next.params.get(index).map(|(param, _)| (param.clone(), address))
                        })
                        .collect::<Vec<_>>();
                    collect_uses(functions, next, seeds, visiting, uses);
                    continue;
                }
            }
            _ => {}
        }
        let mut touches = false;
        inst.visit_uses(|name| touches |= addresses.contains_key(name) || merged.contains_key(name));
        if touches {
            uses.untrack(format!("@{} uses it in a {:?}", function.name, inst.opcode));
        }
    }
    let returned = function.blocks.iter().any(|block| {
        block.typed.as_ref().is_some_and(|typed| {
            matches!(&typed.terminator, TirTerminator::Ret(Some(name)) if addresses.contains_key(name) || merged.contains_key(name))
        })
    });
    if returned {
        uses.untrack(format!("@{} returns its address", function.name));
    }
    visiting.pop();
}

/// Every `(callee, argument)` to splice (see the module comment): a call
/// that hands a function a pointer into one of the caller's variables when
/// that function, or one it hands it on to, loads an address-space-0
/// pointer out of the variable. Err names a use of such a variable the
/// splice cannot make right. Variables no callee loads a pointer out of are
/// left as they are; device pointers in a struct have their own paths.
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
        let (addresses, merged) = derived_addresses(
            function,
            roots.iter().map(|root| (root.clone(), Address::root(root))).collect(),
        );
        // The caller's own accesses, then each call's.
        let mut own = Uses::default();
        for inst in function.carrier_insts() {
            match inst.opcode {
                TirOpcode::Load if inst.result_ty == Some(LlType::Ptr(0)) => {
                    match operand_name(inst.operands.first()) {
                        Some(name) if merged.contains_key(name) => {
                            own.accesses.push((
                                Address {
                                    root: merged[name].clone(),
                                    gep_type: None,
                                    key: None,
                                    depth: 1,
                                },
                                Access::PointerLoad,
                            ));
                            own.untrack(format!(
                                "{} in @{} loads a pointer out of a local struct of pointers chosen by a select or phi ({name})",
                                inst.result.as_deref().unwrap_or("a load"),
                                function.name
                            ));
                        }
                        _ => {}
                    }
                }
                TirOpcode::Store => {
                    if let Some((value, ptr)) = inst.store().as_deref() {
                        if let LlValue::Local(name) = &ptr.value {
                            if let Some(address) = addresses.get(name) {
                                let access = if value.ty == LlType::Ptr(0) {
                                    Access::PointerStore
                                } else {
                                    Access::OtherStore
                                };
                                own.accesses.push((address.clone(), access));
                            }
                        }
                    }
                }
                _ => {}
            }
        }
        let mut calls: Vec<(String, String, String, Uses)> = Vec::new();
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
                let mut uses = Uses::default();
                let root = if let Some(address) = addresses.get(name) {
                    let Some((param, _)) = callee.params.get(index) else {
                        continue;
                    };
                    collect_uses(
                        functions,
                        callee,
                        vec![(param.clone(), address.clone())],
                        &mut vec![function.name.clone()],
                        &mut uses,
                    );
                    address.root.clone()
                } else if let Some(root) = merged.get(name) {
                    uses.accesses.push((
                        Address { root: root.clone(), gep_type: None, key: None, depth: 1 },
                        Access::PointerLoad,
                    ));
                    uses.untrack(format!("the struct is chosen by a select or phi ({name})"));
                    root.clone()
                } else {
                    continue;
                };
                calls.push((callee.name.clone(), name.clone(), root, uses));
            }
        }
        for root in &roots {
            let loads_pointer = |uses: &Uses| {
                uses.accesses
                    .iter()
                    .any(|(a, access)| a.root == *root && *access == Access::PointerLoad)
            };
            let needed = calls
                .iter()
                .filter(|(_, _, r, uses)| r == root && loads_pointer(uses))
                .collect::<Vec<_>>();
            let merged_load = own.untracked.is_some() && loads_pointer(&own);
            if needed.is_empty() && !merged_load {
                continue;
            }
            let (callee, argument) = needed
                .first()
                .map(|(c, a, _, _)| (c.clone(), a.clone()))
                .unwrap_or_default();
            let refuse = |why: &str| {
                Err(format!(
                    "native emitter: @{callee} is handed {argument}, a local struct holding pointers (in @{}), and {why}; not supported (N76)",
                    function.name
                ))
            };
            if merged_load {
                return Err(format!(
                    "native emitter: {}; not supported (N76)",
                    own.untracked.clone().unwrap_or_default()
                ));
            }
            let all = calls
                .iter()
                .filter(|(_, _, r, _)| r == root)
                .map(|(_, _, _, uses)| uses)
                .chain(std::iter::once(&own));
            let mut pointer_fields = Vec::new();
            let mut other_stores = Vec::new();
            let mut pointer_stores_by_callees = false;
            for uses in all.clone() {
                if let Some(why) = &uses.untracked {
                    if !std::ptr::eq(uses, &own) {
                        return refuse(&format!("cannot be followed: {why}"));
                    }
                }
                for (address, access) in &uses.accesses {
                    if address.root != *root {
                        continue;
                    }
                    match access {
                        Access::PointerLoad if address.depth > 1 => {
                            return refuse("loads a pointer through a struct nested inside it");
                        }
                        Access::PointerLoad => pointer_fields.push(address.clone()),
                        Access::PointerStore => {
                            if !std::ptr::eq(uses, &own) {
                                pointer_stores_by_callees = true;
                            }
                            pointer_fields.push(address.clone());
                        }
                        Access::OtherStore => other_stores.push(address.clone()),
                    }
                }
            }
            if pointer_stores_by_callees {
                return refuse("a callee stores a pointer into it");
            }
            if other_stores
                .iter()
                .any(|store| pointer_fields.iter().any(|field| field.overlaps(store)))
            {
                return refuse("an integer, vector or aggregate store overlaps one of its pointer fields");
            }
            for (callee, argument, _, _) in needed {
                let site = (callee.clone(), argument.clone());
                if !sites.contains(&site) {
                    sites.push(site);
                }
            }
        }
    }
    Ok(sites)
}
