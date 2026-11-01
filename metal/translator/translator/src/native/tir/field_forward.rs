//! DRIVER-PLAN N76: the pointer a load of a local pointer field will be at
//! emission, known before anything is emitted.
//!
//! The emitter does not keep pointers in memory: a store of a pointer into a
//! field of a Function variable (`store ptr %x, ptr %field`) records `%x`
//! for the field, and a later load of the field is `%x` itself
//! (`emit_pointer_from_local_field_load`, `pointer_forward_values`). The
//! analyses that run before emission (the storage each pointer will have,
//! `derive_pointer_storage_from`; the Function/Private network, N75) saw
//! such a load as a pointer of unknown origin and gave it the address
//! space's default storage, Private. A loop or construct-tree phi that
//! merges loads of a field holding a Function pointer (Geekbench 7's MPS
//! `depthwiseConv_3x3`: a lambda's capture struct holding `&local`) was then
//! taken as Private, and failed once the first load was forwarded to the
//! Function variable ("Private pointer phi %metal2vulkan.ct.slot.48 has
//! differing structural sources"; "pointer merge storage mismatch Function
//! vs Private").
//!
//! A load is forwarded here only when the answer cannot depend on emission
//! order: the field has exactly one pointer store in the function, its value
//! is a global or a value of the entry block (it dominates every load), and
//! the variable is used only through constant field addresses of one type
//! that do not overlap (nor does any integer, vector or aggregate store into
//! it overlap a pointer field), read and written in place (never stored, merged or
//! indexed dynamically), or passed whole to a function of the module that
//! only reads through that parameter (the real kernel hands its capture to
//! its lambdas, which load fields and never store into them). A load that
//! runs before the store reads an uninitialised field, which the stored
//! value refines.

use super::*;
use crate::native::ir::LlFunction;
use std::collections::BTreeMap;

#[derive(Clone, Debug)]
struct FieldAddress {
    root: String,
    key: Vec<u64>,
}

fn const_u64(value: &TypedValue) -> Option<u64> {
    match value.value {
        LlValue::Int(v) | LlValue::Hex(v) => Some(v),
        LlValue::SignedInt(v) if v >= 0 => Some(v as u64),
        _ => None,
    }
}

/// `key` mirrors the emitter's `local_pointer_field_key`: the constant
/// index path with a leading zero dropped when more indices follow.
fn field_key(indices: &[TypedValue]) -> Option<Vec<u64>> {
    let mut key = indices.iter().map(const_u64).collect::<Option<Vec<_>>>()?;
    if key.len() > 1 && key.first() == Some(&0) {
        key.remove(0);
    }
    Some(key)
}

/// Two field paths of one variable can address the same bytes when one is
/// a prefix of the other (a whole sub-object and one of its fields).
fn overlapping(a: &[u64], b: &[u64]) -> bool {
    a != b && (a.starts_with(b) || b.starts_with(a))
}

fn is_lifetime_marker(inst: &TirInst) -> bool {
    inst.opcode == TirOpcode::Call
        && (inst.call().as_ref().is_some_and(|call| {
            call.callee
                .trim_start_matches('@')
                .starts_with("llvm.lifetime.")
        }) || inst
            .void_call_line()
            .as_deref()
            .is_some_and(|line| line.contains("@llvm.lifetime.")))
}

/// Whether `callee`'s parameter `index` (a pointer) is only read through:
/// used as a load address or as the base of GEPs that are only load
/// addresses, nothing else (no store into it, no call, no copy).
fn reads_only_through_parameter(callee: &LlFunction, index: usize) -> bool {
    let Some((param, LlType::Ptr(_))) = callee.params.get(index) else {
        return false;
    };
    let mut addresses = HashSet::from([param.clone()]);
    for inst in callee.carrier_insts() {
        let (Some(result), Some(gep)) = (&inst.result, inst.gep()) else {
            continue;
        };
        if matches!(&gep.base.value, LlValue::Local(base) if base == param) {
            addresses.insert(result.clone());
        }
    }
    for inst in callee.carrier_insts() {
        if is_lifetime_marker(inst) {
            continue;
        }
        if inst.opcode == TirOpcode::GetElementPtr
            && inst.result.as_ref().is_some_and(|r| addresses.contains(r))
        {
            // Its base is the parameter; an address used as an index is not.
            let indexed_by_address = inst.gep().as_ref().is_some_and(|gep| {
                gep.indices.iter().any(
                    |index| matches!(&index.value, LlValue::Local(name) if addresses.contains(name)),
                )
            });
            if indexed_by_address {
                return false;
            }
            continue;
        }
        if inst.opcode == TirOpcode::Load {
            continue;
        }
        let mut uses_address = false;
        inst.visit_uses(|name| uses_address |= addresses.contains(name));
        if uses_address {
            return false;
        }
    }
    !callee.blocks.iter().any(|block| {
        block.typed.as_ref().is_some_and(|typed| {
            matches!(&typed.terminator, TirTerminator::Ret(Some(name)) if addresses.contains(name))
                || matches!(&typed.ret, RetEmit::Value(TypedValue { value: LlValue::Local(name), .. }) if addresses.contains(name))
        })
    })
}

/// For every address-space-0 pointer load of a local pointer field whose
/// value is fixed (see the module comment), the stored pointer. Loads in
/// `skip` (the pre-inlined helpers' loads, which the emitter forwards only
/// for device addresses) are left out; `functions` are the module's, for
/// the calls a variable is passed to.
pub(in crate::native) fn local_pointer_field_forwards(
    tir: &TirFunction,
    skip: &HashSet<String>,
    functions: &[LlFunction],
) -> HashMap<String, TypedValue> {
    let entry_defs = tir
        .blocks
        .first()
        .map(|block| {
            block
                .insts
                .iter()
                .filter_map(|inst| inst.result.clone())
                .collect::<HashSet<_>>()
        })
        .unwrap_or_default();
    let insts = || tir.blocks.iter().flat_map(|block| &block.insts);

    // Variables and their identity copies, then their constant field
    // addresses (and the GEP type each root is indexed with).
    let mut fields: HashMap<String, FieldAddress> = HashMap::new();
    for inst in insts() {
        if inst.opcode == TirOpcode::Alloca {
            if let Some(result) = &inst.result {
                fields.insert(
                    result.clone(),
                    FieldAddress {
                        root: result.clone(),
                        key: Vec::new(),
                    },
                );
            }
        }
    }
    if fields.is_empty() {
        return HashMap::new();
    }
    let mut poisoned: HashSet<String> = HashSet::new();
    let mut root_gep_types: HashMap<String, HashSet<String>> = HashMap::new();
    let mut copies: HashSet<String> = HashSet::new();
    // Identity copies: a pointer bitcast, a freeze, and the parameter copy
    // an inlined helper takes the variable through.
    for inst in insts() {
        let copy = inst.identity_ptr_bitcast().or_else(|| {
            matches!(
                inst.opcode,
                TirOpcode::Freeze | TirOpcode::Metal2VulkanInlineParameter
            )
            .then(|| Some((inst.result.as_deref()?, operand_name(inst.operands.first()?)?)))
            .flatten()
        });
        if let Some((result, base)) = copy {
            if let Some(address) = fields.get(base).cloned() {
                if address.key.is_empty() {
                    fields.insert(result.to_string(), address);
                    copies.insert(result.to_string());
                }
            }
        }
    }
    for inst in insts() {
        if inst.opcode != TirOpcode::GetElementPtr {
            continue;
        }
        let (Some(result), Some(gep)) = (&inst.result, inst.gep()) else {
            continue;
        };
        let LlValue::Local(base) = &gep.base.value else {
            continue;
        };
        let Some(address) = fields.get(base).cloned() else {
            continue;
        };
        if !address.key.is_empty() {
            // A field of a field: not a path the emitter keys the same way.
            // (A copy of a copy is found above only in definition order.)
            poisoned.insert(address.root);
            continue;
        }
        let Some(key) = field_key(&gep.indices) else {
            poisoned.insert(address.root);
            continue;
        };
        root_gep_types
            .entry(address.root.clone())
            .or_default()
            .insert(format!("{:?}", gep.source_ty));
        fields.insert(
            result.clone(),
            FieldAddress {
                root: address.root,
                key,
            },
        );
    }

    let mut loads: Vec<(String, String, Vec<u64>)> = Vec::new();
    let mut stores: BTreeMap<(String, Vec<u64>), Vec<TypedValue>> = BTreeMap::new();
    let mut keys: HashMap<String, HashSet<Vec<u64>>> = HashMap::new();
    let mut scalar_stores: HashMap<String, HashSet<Vec<u64>>> = HashMap::new();
    for inst in insts() {
        let defines_address = inst.result.as_ref().is_some_and(|result| {
            fields.contains_key(result)
                && (inst.opcode == TirOpcode::Alloca
                    || inst.opcode == TirOpcode::GetElementPtr
                    || copies.contains(result))
        });
        if defines_address || is_lifetime_marker(inst) {
            continue;
        }
        match inst.opcode {
            TirOpcode::Load => {
                let address = inst
                    .operands
                    .first()
                    .and_then(operand_name)
                    .and_then(|name| fields.get(name));
                if let (Some(address), Some(result)) = (address, &inst.result) {
                    if matches!(inst.result_ty, Some(LlType::Ptr(_))) {
                        keys.entry(address.root.clone())
                            .or_default()
                            .insert(address.key.clone());
                        if inst.result_ty == Some(LlType::Ptr(0)) {
                            loads.push((result.clone(), address.root.clone(), address.key.clone()));
                        }
                    }
                    continue;
                }
            }
            TirOpcode::Store => {
                if let Some(store) = inst.store() {
                    let (value, ptr) = store.as_ref();
                    if let LlValue::Local(stored) = &value.value {
                        if let Some(address) = fields.get(stored) {
                            // The variable's address escapes into memory.
                            poisoned.insert(address.root.clone());
                        }
                    }
                    let address = match &ptr.value {
                        LlValue::Local(name) => fields.get(name),
                        _ => None,
                    };
                    if let Some(address) = address {
                        if matches!(value.ty, LlType::Ptr(_)) {
                            keys.entry(address.root.clone())
                                .or_default()
                                .insert(address.key.clone());
                            stores
                                .entry((address.root.clone(), address.key.clone()))
                                .or_default()
                                .push(value.clone());
                        } else {
                            // An integer, vector or aggregate store: it
                            // overwrites a pointer field it overlaps.
                            scalar_stores
                                .entry(address.root.clone())
                                .or_default()
                                .insert(address.key.clone());
                        }
                        continue;
                    }
                }
            }
            TirOpcode::Call => {
                if let Some(call) = inst.call() {
                    // A whole variable passed to a read-only parameter of a
                    // function of the module; anything else below poisons.
                    let callee = functions.iter().find(|f| f.name == call.callee);
                    let passed = call
                        .args
                        .iter()
                        .enumerate()
                        .filter_map(|(index, argument)| match &argument.value {
                            LlValue::Local(name)
                                if fields.get(name).is_some_and(|a| a.key.is_empty())
                                    && callee
                                        .is_some_and(|f| reads_only_through_parameter(f, index)) =>
                            {
                                Some(name.as_str())
                            }
                            _ => None,
                        })
                        .collect::<HashSet<_>>();
                    inst.visit_uses(|name| {
                        if let Some(address) = fields.get(name) {
                            if !passed.contains(name) {
                                poisoned.insert(address.root.clone());
                            }
                        }
                    });
                    continue;
                }
            }
            _ => {}
        }
        // Any other use of a variable or of one of its field addresses.
        inst.visit_uses(|name| {
            if let Some(address) = fields.get(name) {
                poisoned.insert(address.root.clone());
            }
        });
    }
    for block in &tir.blocks {
        let returned = match (&block.terminator, &block.ret) {
            (_, RetEmit::Value(value)) => match &value.value {
                LlValue::Local(name) => Some(name.as_str()),
                _ => None,
            },
            (TirTerminator::Ret(Some(name)), _) => Some(name.as_str()),
            _ => None,
        };
        if let Some(address) = returned.and_then(|name| fields.get(name)) {
            poisoned.insert(address.root.clone());
        }
    }
    for (root, types) in &root_gep_types {
        if types.len() > 1 {
            poisoned.insert(root.clone());
        }
    }
    for (root, used) in &keys {
        let used = used.iter().collect::<Vec<_>>();
        let overlap = used
            .iter()
            .enumerate()
            .any(|(at, a)| used[at + 1..].iter().any(|b| overlapping(a, b)));
        let overwritten = scalar_stores.get(root).is_some_and(|scalars| {
            scalars
                .iter()
                .any(|scalar| used.iter().any(|key| *key == scalar || overlapping(key, scalar)))
        });
        if overlap || overwritten {
            poisoned.insert(root.clone());
        }
    }

    let mut forwards = HashMap::new();
    for (load, root, key) in loads {
        if poisoned.contains(&root) || skip.contains(&load) {
            continue;
        }
        let Some([value]) = stores.get(&(root, key)).map(Vec::as_slice) else {
            continue;
        };
        let dominates = match &value.value {
            LlValue::Global(_) => true,
            LlValue::Local(name) => entry_defs.contains(name),
            _ => false,
        };
        if value.ty == LlType::Ptr(0) && dominates {
            forwards.insert(load, value.clone());
        }
    }
    forwards
}

