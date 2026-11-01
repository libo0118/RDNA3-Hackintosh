//! DRIVER-PLAN N76: a Function/Private pointer phi whose every value is one
//! pointer (or `undef`, or the phi network itself) is that pointer.
//!
//! The construct tree's dispatch slot of Geekbench 7's MPS
//! `depthwiseConv_3x3` is `%metal2vulkan.ct.slot.48 = phi [undef, pre],
//! [%nextslot.48, continue]`, and every case feeds `nextslot.48` a load of
//! one capture field, which the emitter forwards to the Function variable
//! stored there (tir/field_forward.rs). The index-domain paths need a GEP
//! to template the phi on and find none (the variable is a scalar `alloca
//! float`), so the slot failed with "Function pointer phi ... has differing
//! structural sources". Such a network carries one pointer only; each of its
//! phis is emitted as an alias of it, as a phi whose incoming values are all
//! equal already is (phi_compare.rs).
//!
//! The one pointer must dominate every phi of the network: it is a global,
//! a parameter or a value of the entry block (an `undef` edge may come from
//! a block nothing else dominates, such as the dispatch's preheader).

use super::*;

/// Enough for a dispatch slot fed by every case of a large construct tree;
/// a bigger network keeps the ordinary paths.
const MAX_UNIFORM_WALK: usize = 1 << 14;

impl Emitter {
    /// The values of the function's entry block and its parameters: the
    /// pointers a uniform phi may alias (see the module comment).
    pub(in crate::native::emitter) fn record_entry_block_values(
        &mut self,
        tir: &crate::native::tir::TirFunction,
        params: &[(String, LlType)],
    ) {
        self.entry_block_values = tir
            .blocks
            .first()
            .into_iter()
            .flat_map(|block| &block.insts)
            .filter_map(|inst| inst.result.clone())
            .chain(params.iter().map(|(name, _)| name.clone()))
            .collect();
    }

    /// The one pointer a phi network carries, if it carries one only. Forward
    /// phis are walked through their TIR incoming values; an emitted value is
    /// a leaf (a phi already collapsed by this stands for its pointer).
    fn uniform_pointer_phi_source(
        &self,
        name: &str,
        incoming: &[(LlValue, String)],
    ) -> Option<LlValue> {
        let mut queue = incoming
            .iter()
            .map(|(value, _)| value.clone())
            .collect::<std::collections::VecDeque<_>>();
        let mut visited = HashSet::from([name.to_string()]);
        let mut leaf: Option<LlValue> = None;
        let mut steps = 0;
        while let Some(value) = queue.pop_front() {
            steps += 1;
            if steps > MAX_UNIFORM_WALK {
                return None;
            }
            let value = match self.forwarded_pointer_value(&value) {
                LlValue::Local(local) => self
                    .uniform_pointer_phis
                    .get(&local)
                    .cloned()
                    .unwrap_or(LlValue::Local(local)),
                other => other,
            };
            let candidate = match &value {
                LlValue::Undef => continue,
                LlValue::Global(global) => {
                    self.global_values.get(global)?;
                    value.clone()
                }
                LlValue::Local(local) if self.values.contains_key(local) => {
                    if !self.entry_block_values.contains(local) {
                        return None;
                    }
                    value.clone()
                }
                LlValue::Local(local) => {
                    if !visited.insert(local.clone()) {
                        continue;
                    }
                    if self.tir_result_types.get(local) != Some(&LlType::Ptr(0)) {
                        return None;
                    }
                    let forward = self.tir_phi_incomings.get(local)?;
                    queue.extend(forward.iter().map(|(value, _)| value.clone()));
                    continue;
                }
                _ => return None,
            };
            match &leaf {
                None => leaf = Some(candidate),
                Some(existing) if *existing == candidate => {}
                Some(_) => return None,
            }
        }
        leaf
    }

    /// Emits a uniform pointer phi as an alias of its one pointer (see the
    /// module comment). False, with nothing changed, for any other phi.
    pub(in crate::native::emitter) fn emit_uniform_pointer_phi(
        &mut self,
        name: &str,
        incoming: &[(LlValue, String)],
        result_ty: &LlType,
    ) -> Result<bool, String> {
        if *result_ty != LlType::Ptr(0)
            || self.null_rooted_pointer_values.contains(name)
            // An earlier forward use reserved an index domain for it: that
            // index must be defined by the ordinary paths.
            || self.gep_provenance.contains_key(name)
            || self.values.contains_key(&pointer_index_name(name))
        {
            return Ok(false);
        }
        let Some(source) = self.uniform_pointer_phi_source(name, incoming) else {
            return Ok(false);
        };
        let Some(meta) = self.pointer_meta_for_value(&source, 0)? else {
            return Ok(false);
        };
        if !matches!(meta.storage, StorageClass::Function | StorageClass::Private) {
            return Ok(false);
        }
        let id = self.value_id(&source, result_ty)?;
        self.values
            .insert(name.to_string(), (id, result_ty.clone()));
        self.record_pointer_meta(name.to_string(), meta);
        if let LlValue::Local(local) = &source {
            if let Some(provenance) = self.gep_provenance.get(local).cloned() {
                self.gep_provenance.insert(name.to_string(), provenance);
            }
            if let Some(nullness) = self.pointer_nullness.get(local).copied() {
                self.record_pointer_nullness(name.to_string(), nullness);
            }
        }
        crate::notes::push_once(
            &format!("N76 uniform {}", self.mixed_storage_function),
            format!(
                "metal2vulkan: kernel {}: pointer phi {name} carries one pointer ({}); emitted as an alias of it (DRIVER-PLAN N76)",
                self.mixed_storage_function,
                match &source {
                    LlValue::Local(local) | LlValue::Global(local) => local.as_str(),
                    _ => "?",
                }
            ),
        );
        self.uniform_pointer_phis.insert(name.to_string(), source);
        Ok(true)
    }
}
