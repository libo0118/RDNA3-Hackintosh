//! Pointer phis whose incoming values live in two thread-private storage
//! classes: a Function variable (an `alloca`) on some edges and a Private
//! variable (an address-space-0 global, e.g. a `@__const.*` table that LLVM
//! substituted for a never-written local copy) on the others.
//!
//! Logical SPIR-V cannot express such a phi: a Function and a Private
//! pointer have different types, and neither storage class allows pointer
//! phis at all. The emitter already represents same-root Function/Private
//! phis in the index domain (`emit_pointer_phi_provenance`: the root stays
//! fixed, the indices become phis) and pointers of two different storages as
//! a selected pair (`SelectedPointer`: loads read both arms and OpSelect the
//! value). A mixed phi is split into exactly those two pieces:
//!
//! * one "arm" phi per storage class, each a same-root index-domain phi; on
//!   edges that come from the other storage its incoming value is the arm's
//!   root at index 0 (always in bounds, so the unselected arm can be read),
//! * a bool phi that is true on the edges of the Function arm,
//!
//! and the phi itself becomes a `SelectedPointer` of the two arms. GEPs on it
//! are applied to each arm (`emit_mixed_storage_pointer_gep`), loads select on
//! the loaded values, stores are guarded per arm. The phi is "demoted" from a
//! pointer to a selection of values; this is logged once per kernel.

use super::*;
use std::sync::{Mutex, OnceLock};

const ARM_TAG: &str = ".metal2vulkan.mixarm";

enum MixedArm {
    Modeled,
    Root { id: Word, pointee: LlType },
}

fn arm_storages() -> [StorageClass; 2] {
    [StorageClass::Function, StorageClass::Private]
}

/// Once per kernel (function name) for the whole process: the primary emitter
/// and the raw-construction retry both see the same phi.
fn log_demotion_once(function: &str, phi: &str) {
    static LOGGED: OnceLock<Mutex<HashSet<String>>> = OnceLock::new();
    let logged = LOGGED.get_or_init(|| Mutex::new(HashSet::new()));
    let first = logged
        .lock()
        .map(|mut set| set.insert(function.to_string()))
        .unwrap_or(true);
    if first {
        crate::notes::push(format!(
            "metal2vulkan: kernel {function}: pointer phi {phi} merges Function and Private storage; demoted to a per-storage value select (DRIVER-PLAN N72)"
        ));
    }
}

impl Emitter {
    /// Splits a Function-vs-Private pointer phi (see the module comment).
    /// Returns Ok(false) when the phi is not of that shape, so the ordinary
    /// paths (and their diagnostics) still run.
    pub(in crate::native::emitter) fn emit_mixed_storage_pointer_phi(
        &mut self,
        name: &str,
        incoming: &[(LlValue, String)],
        result_ty: &LlType,
        instructions: &mut Vec<Instruction>,
    ) -> Result<bool, String> {
        let LlType::Ptr(addrspace) = result_ty else {
            return Ok(false);
        };
        if *addrspace != 0 || incoming.len() < 2 {
            return Ok(false);
        }
        // Which arm each edge feeds; None for undef (it may take either arm).
        // Classified first, without side effects: a module without a mixed
        // phi must come out exactly as before.
        let mut groups = Vec::with_capacity(incoming.len());
        let mut classes = Vec::with_capacity(incoming.len());
        for (value, _) in incoming {
            if matches!(value, LlValue::Undef) {
                groups.push(None);
                classes.push(None);
                continue;
            }
            let Some((class, storage)) = self.classify_mixed_arm(value)? else {
                return Ok(false);
            };
            let Some(group) = arm_storages().iter().position(|s| *s == storage) else {
                return Ok(false);
            };
            groups.push(Some(group));
            classes.push(Some((class, storage)));
        }
        if !(0..2).all(|group| groups.contains(&Some(group))) {
            return Ok(false);
        }
        self.refuse_split_pointer_consumers(name)?;
        let mut arm_values = Vec::with_capacity(incoming.len());
        for (position, ((value, _), class)) in incoming.iter().zip(classes).enumerate() {
            arm_values.push(match class {
                Some((MixedArm::Root { id, pointee }, storage)) => {
                    self.install_mixed_arm_root(name, position, id, storage, pointee)
                }
                _ => value.clone(),
            });
        }
        let incoming = incoming
            .iter()
            .zip(arm_values)
            .map(|((_, label), value)| (value, label.clone()))
            .collect::<Vec<_>>();
        let incoming = incoming.as_slice();

        let mut arms = Vec::with_capacity(2);
        let mut pointees = Vec::with_capacity(2);
        for (group, storage) in arm_storages().into_iter().enumerate() {
            let (template_name, template) = incoming
                .iter()
                .zip(&groups)
                .find_map(|((value, _), g)| match (value, g) {
                    (LlValue::Local(local), Some(g)) if *g == group => self
                        .gep_provenance
                        .get(local)
                        .cloned()
                        .map(|p| (local.clone(), p)),
                    _ => None,
                })
                .expect("each arm has a modeled incoming value (checked above)");
            let pointee = self.pointer_pointees.get(&template_name).cloned();
            let zero = self.zero_index_provenance(&template).ok_or_else(|| {
                format!(
                    "native emitter: mixed-storage pointer phi {name}: {storage:?} arm template {template_name} has no structural zero path"
                )
            })?;
            let zero_name = format!("{name}{ARM_TAG}{group}.zero");
            self.gep_provenance.insert(zero_name.clone(), zero);
            self.pointer_storage.insert(zero_name.clone(), storage);
            if let Some(pointee) = &pointee {
                self.pointer_pointees
                    .insert(zero_name.clone(), pointee.clone());
            }
            let arm_incoming = incoming
                .iter()
                .zip(&groups)
                .map(|((value, label), g)| match g {
                    Some(g) if *g == group => (value.clone(), label.clone()),
                    None if group == 0 => (value.clone(), label.clone()),
                    _ => (LlValue::Local(zero_name.clone()), label.clone()),
                })
                .collect::<Vec<_>>();
            let arm_name = format!("{name}{ARM_TAG}{group}");
            self.emit_phi_resolved(arm_name.clone(), result_ty, arm_incoming, instructions)
                .map_err(|error| {
                    format!(
                        "native emitter: mixed-storage pointer phi {name}: {storage:?} arm failed: {error}"
                    )
                })?;
            if !self.values.contains_key(&arm_name)
                || self.pointer_storage.get(&arm_name) != Some(&storage)
            {
                return Err(format!(
                    "native emitter: mixed-storage pointer phi {name}: {storage:?} arm has no index-domain pointer"
                ));
            }
            arms.push(arm_name);
            pointees.push(pointee);
        }

        let bool_ty = self.type_id(&LlType::Bool)?;
        let cond = self.fresh();
        let mut operands = Vec::with_capacity(incoming.len() * 2);
        let mut seen: HashMap<Word, bool> = HashMap::new();
        for ((_, label), group) in incoming.iter().zip(&groups) {
            let takes_function_arm = group.unwrap_or(0) == 0;
            let label_id = self.label_id(label)?;
            if let Some(existing) = seen.insert(label_id, takes_function_arm) {
                if existing != takes_function_arm {
                    return Err(format!(
                        "native emitter: mixed-storage pointer phi {name} has both storages from predecessor {label}"
                    ));
                }
                continue;
            }
            operands.push(Operand::IdRef(self.const_bool(takes_function_arm)?));
            operands.push(Operand::IdRef(label_id));
        }
        instructions.push(Self::inst(Op::Phi, Some(bool_ty), Some(cond), operands));

        if let [Some(first), Some(second)] = pointees.as_slice() {
            if first == second {
                self.pointer_pointees
                    .insert(name.to_string(), first.clone());
            }
        }
        self.selected_pointers.insert(
            name.to_string(),
            SelectedPointer {
                cond,
                true_value: LlValue::Local(arms[0].clone()),
                false_value: LlValue::Local(arms[1].clone()),
                ty: result_ty.clone(),
            },
        );
        self.mixed_storage_pointers.insert(name.to_string());
        log_demotion_once(&self.mixed_storage_function, name);
        Ok(true)
    }

    /// How an incoming edge can feed its arm, and the arm's storage: a
    /// modeled GEP as it is, a whole root (an alloca, an address-space-0
    /// global) as a GEP of the root at index 0 (`install_mixed_arm_root`).
    /// None declines the split (the old paths and their diagnostics run).
    fn classify_mixed_arm(
        &self,
        value: &LlValue,
    ) -> Result<Option<(MixedArm, StorageClass)>, String> {
        let (id, storage, pointee) = match value {
            LlValue::Local(local) => {
                let Some(storage) = self.pointer_storage.get(local).copied() else {
                    return Ok(None);
                };
                if self.selected_pointers.contains_key(local)
                    || self.selected_access_trees.contains_key(local)
                    || self.raw_offsets.contains_key(local)
                    || self.unmodeled_pointers.contains(local)
                {
                    return Ok(None);
                }
                if self.gep_provenance.contains_key(local) {
                    return Ok(Some((MixedArm::Modeled, storage)));
                }
                if !self.mixed_alloca_roots.contains(local) {
                    return Ok(None);
                }
                let (Some((id, _)), Some(pointee)) = (
                    self.values.get(local).cloned(),
                    self.pointer_pointees.get(local).cloned(),
                ) else {
                    return Ok(None);
                };
                (id, storage, pointee)
            }
            LlValue::Global(global) => {
                let Some(decl) = self
                    .ir
                    .globals
                    .iter()
                    .find(|g| g.name == *global && g.addrspace == 0)
                else {
                    return Ok(None);
                };
                let pointee = self.resolve_type(&decl.ty)?;
                let Some((id, _)) = self.global_values.get(global).cloned() else {
                    return Ok(None);
                };
                (id, StorageClass::Private, pointee)
            }
            _ => return Ok(None),
        };
        if super::super::body::type_contains_pointer(&pointee) {
            return Ok(None);
        }
        Ok(Some((MixedArm::Root { id, pointee }, storage)))
    }

    /// A whole root as a GEP of it at index 0, under a synthetic name.
    fn install_mixed_arm_root(
        &mut self,
        phi: &str,
        position: usize,
        id: Word,
        storage: StorageClass,
        pointee: LlType,
    ) -> LlValue {
        let alias = format!("{phi}{ARM_TAG}.root{position}");
        self.values.insert(alias.clone(), (id, LlType::Ptr(0)));
        self.pointer_storage.insert(alias.clone(), storage);
        self.pointer_pointees.insert(alias.clone(), pointee.clone());
        let root_is_indexed_container = self.is_indexed_container_root(id, Some(storage));
        self.gep_provenance.insert(
            alias.clone(),
            GepProvenance {
                root: id,
                addrspace: 0,
                source_ty: pointee,
                indices: vec![TypedValue {
                    ty: LlType::Int(32),
                    value: LlValue::Int(0),
                }],
                root_indices: None,
                root_is_indexed_container,
            },
        );
        LlValue::Local(alias)
    }

    /// A split pointer is a pair of pointers, not a value: a phi or select
    /// that takes it (a walk over the merged pointer, a nested mixed phi, a
    /// mixed phi on a back edge) cannot be built from it. Refused here with
    /// the names, before such a consumer fails later on an undefined id.
    fn refuse_split_pointer_consumers(&self, name: &str) -> Result<(), String> {
        if self.pointer_phi_incoming_values.contains(name) {
            return Err(format!(
                "native emitter: mixed-storage pointer phi {name} (Function vs Private) feeds another pointer phi; nested mixed phis are not supported (N72)"
            ));
        }
        let selected_by = self
            .forward_pointer_selects
            .iter()
            .filter(|(_, (true_value, false_value))| {
                [&true_value.value, &false_value.value]
                    .into_iter()
                    .any(|value| matches!(value, LlValue::Local(local) if local == name))
            })
            .map(|(select, _)| select.as_str())
            .min();
        if let Some(select) = selected_by {
            return Err(format!(
                "native emitter: mixed-storage pointer phi {name} (Function vs Private) feeds pointer select {select}; nested mixed phis are not supported (N72)"
            ));
        }
        Ok(())
    }

    /// Refuses a phi or select (`consumer`) with a split pointer among its
    /// operands, naming both.
    pub(in crate::native::emitter) fn refuse_split_pointer_operand<'a>(
        &self,
        consumer: &str,
        operands: impl IntoIterator<Item = &'a LlValue>,
    ) -> Result<(), String> {
        for value in operands {
            if let LlValue::Local(local) = value {
                if self.mixed_storage_pointers.contains(local) {
                    return Err(format!(
                        "native emitter: {consumer} takes the split mixed-storage pointer {local} (Function vs Private); nested mixed phis are not supported (N72)"
                    ));
                }
            }
        }
        Ok(())
    }

    /// A copy of a split pointer (bitcast, freeze, an inlined parameter) is
    /// split too: its loads must branch and its GEPs apply per arm.
    pub(in crate::native::emitter) fn copy_mixed_storage_marker(&mut self, from: &str, to: &str) {
        if self.mixed_storage_pointers.contains(from) {
            self.mixed_storage_pointers.insert(to.to_string());
        }
    }

    /// A load through a split pointer: each arm loads in its own branch, so
    /// the arm that is not selected is never read (its indices belong to
    /// the other root and may be out of its bounds).
    pub(in crate::native::emitter) fn emit_mixed_storage_pointer_load(
        &mut self,
        ptr_name: &str,
        result: Word,
        result_ty: &LlType,
        access_align: Option<u64>,
        instructions: &mut Vec<Instruction>,
    ) -> Result<bool, String> {
        if !self.mixed_storage_pointers.contains(ptr_name) {
            return Ok(false);
        }
        let selected = self
            .selected_pointers
            .get(ptr_name)
            .cloned()
            .ok_or_else(|| format!("native emitter: split pointer {ptr_name} lost its arms"))?;
        let result_ty = self.resolve_type(result_ty)?;
        let result_type = self.type_id(&result_ty)?;
        let true_label = self.fresh();
        let false_label = self.fresh();
        let merge_label = self.fresh();
        instructions.push(Self::inst(
            Op::SelectionMerge,
            None,
            None,
            vec![
                Operand::IdRef(merge_label),
                Operand::SelectionControl(SelectionControl::NONE),
            ],
        ));
        instructions.push(Self::inst(
            Op::BranchConditional,
            None,
            None,
            vec![
                Operand::IdRef(selected.cond),
                Operand::IdRef(true_label),
                Operand::IdRef(false_label),
            ],
        ));
        let mut phi_operands = Vec::with_capacity(4);
        for ((label, arm), storage) in [
            (true_label, &selected.true_value),
            (false_label, &selected.false_value),
        ]
        .into_iter()
        .zip(arm_storages())
        {
            instructions.push(Self::inst(Op::Label, None, Some(label), vec![]));
            let value = self.emit_selected_direct_load_arm_value(
                arm,
                &selected.ty,
                storage,
                &result_ty,
                access_align,
                instructions,
            )?;
            let end = instructions
                .iter()
                .rev()
                .find(|i| i.class.opcode == Op::Label)
                .and_then(|i| i.result_id)
                .unwrap_or(label);
            instructions.push(Self::inst(
                Op::Branch,
                None,
                None,
                vec![Operand::IdRef(merge_label)],
            ));
            phi_operands.push(Operand::IdRef(value));
            phi_operands.push(Operand::IdRef(end));
        }
        instructions.push(Self::inst(Op::Label, None, Some(merge_label), vec![]));
        instructions.push(Self::inst(
            Op::Phi,
            Some(result_type),
            Some(result),
            phi_operands,
        ));
        Ok(true)
    }

    /// The template's path with every dynamic index replaced by 0: the
    /// arm's value on the other storage's edges (never selected, but read).
    fn zero_index_provenance(&self, template: &GepProvenance) -> Option<GepProvenance> {
        let mut zero = template.clone();
        let mut selected_ty = zero.source_ty.clone();
        for (position, index) in zero.indices.iter_mut().enumerate() {
            if !structural_pointer_index(position, &mut selected_ty, index, false)? {
                index.value = LlValue::Int(0);
            }
        }
        Some(zero)
    }

    /// A GEP off a split phi (or off a GEP of one): applied to each arm as an
    /// ordinary index-domain GEP, the result is again a selected pair.
    pub(in crate::native::emitter) fn emit_mixed_storage_pointer_gep(
        &mut self,
        name: &str,
        gep: &LlGep,
        instructions: &mut Vec<Instruction>,
    ) -> Result<bool, String> {
        let LlValue::Local(base) = &gep.base.value else {
            return Ok(false);
        };
        if !self.mixed_storage_pointers.contains(base) {
            return Ok(false);
        }
        let selected = self
            .selected_pointers
            .get(base)
            .cloned()
            .ok_or_else(|| format!("native emitter: split pointer {base} lost its arms"))?;
        let mut arm_results = Vec::with_capacity(2);
        for (group, arm) in [&selected.true_value, &selected.false_value]
            .into_iter()
            .enumerate()
        {
            let arm_name = format!("{name}{ARM_TAG}{group}");
            let arm_gep = LlGep {
                inbounds: gep.inbounds,
                source_ty: gep.source_ty.clone(),
                base: TypedValue {
                    ty: gep.base.ty.clone(),
                    value: arm.clone(),
                },
                indices: gep.indices.clone(),
            };
            self.emit_gep_result(&arm_name, &arm_gep, instructions)
                .map_err(|error| {
                    format!(
                        "native emitter: GEP {name} of split pointer {base}, arm {group}: {error}"
                    )
                })?;
            if !self.values.contains_key(&arm_name) {
                return Err(format!(
                    "native emitter: GEP {name} of split pointer {base}, arm {group} has no pointer"
                ));
            }
            arm_results.push(LlValue::Local(arm_name));
        }
        let pointee = gep_pointee(&self.resolve_type(&gep.source_ty)?, &gep.indices)?;
        self.pointer_pointees.insert(name.to_string(), pointee);
        let false_value = arm_results.pop().expect("two arms");
        let true_value = arm_results.pop().expect("two arms");
        self.selected_pointers.insert(
            name.to_string(),
            SelectedPointer {
                cond: selected.cond,
                true_value,
                false_value,
                ty: selected.ty,
            },
        );
        self.mixed_storage_pointers.insert(name.to_string());
        Ok(true)
    }

    /// A store through a split pointer: each arm stores under its own
    /// condition (only the selected one runs).
    pub(in crate::native::emitter) fn emit_mixed_storage_pointer_store(
        &mut self,
        ptr_name: &str,
        object_ty: &LlType,
        object: Word,
        instructions: &mut Vec<Instruction>,
    ) -> Result<bool, String> {
        if !self.mixed_storage_pointers.contains(ptr_name) {
            return Ok(false);
        }
        let selected = self
            .selected_pointers
            .get(ptr_name)
            .cloned()
            .ok_or_else(|| format!("native emitter: split pointer {ptr_name} lost its arms"))?;
        let object_ty = self.resolve_type(object_ty)?;
        for (when_true, arm) in [(true, &selected.true_value), (false, &selected.false_value)] {
            let LlValue::Local(arm_name) = arm else {
                return Err(format!(
                    "native emitter: split pointer {ptr_name} has a non-local arm {arm:?}"
                ));
            };
            let pointee = self
                .pointer_pointee_for_value(arm)?
                .map(|p| self.resolve_type(&p))
                .transpose()?;
            if !pointee
                .as_ref()
                .is_some_and(|p| types_compatible(p, &object_ty))
            {
                return Err(format!(
                    "native emitter: store of {object_ty:?} through split pointer {ptr_name}: arm {arm_name} points at {pointee:?}"
                ));
            }
            let ptr = self.value_id(arm, &selected.ty)?;
            let condition = self.store_arm_condition(selected.cond, when_true, instructions)?;
            self.emit_guarded(condition, instructions, |_, instructions| {
                instructions.push(Self::inst(
                    Op::Store,
                    None,
                    None,
                    vec![Operand::IdRef(ptr), Operand::IdRef(object)],
                ));
                Ok(())
            })?;
        }
        Ok(true)
    }
}
