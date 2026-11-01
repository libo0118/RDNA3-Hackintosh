//! DRIVER-PLAN N75: Function/Private pointer phis whose incoming values are
//! not emitted yet when the phi is (loop back edges, and the construct
//! tree's dispatch loop, where `%metal2vulkan.ct.slot.N = phi [undef, pre],
//! [%metal2vulkan.ct.nextslot.N, continue]` and `nextslot.N` merges the
//! cases' Function and Private values with `slot.N` itself).
//!
//! N72 (phi_mixed_storage.rs) splits a mixed phi when it is emitted, from
//! the storage of each incoming value it has already seen. A value on a back
//! edge has not been seen, so N72 declined and the ordinary paths failed
//! ("Private pointer phi ... has differing structural sources"). Here the
//! storage is worked out from the function's TIR before anything is emitted:
//! every address-space-0 pointer value gets the set of storages it can come
//! from (an `alloca` is Function, an address-space-0 global Private; GEPs and
//! copies take their base's, phis the union of their incoming values; any
//! other pointer is unknown). A value whose set is exactly {Function,
//! Private} is a member of a mixed network. Every member is split the same
//! way, under names fixed in advance, so a phi can name the arms and the
//! selector of a member that is emitted after it:
//!
//! * arm `g` of member `X` is `X.metal2vulkan.mixarm<g>` (a copy shares its
//!   source's arms, as `selected_pointers` aliases it at emission),
//! * the selector of a member phi `P` is `P.metal2vulkan.mixarm.cond`; a GEP
//!   or copy of a member has its base's selector.
//!
//! The arm phis' incoming lists and the arm GEPs are registered as forward
//! TIR facts (`tir_phi_incomings`, `forward_geps`), so the index-domain phi
//! machinery resolves a forward arm like any other forward pointer.

use super::*;
use crate::native::tir::TirOpcode;
use std::collections::BTreeSet;

pub(super) const ARM_TAG: &str = ".metal2vulkan.mixarm";

const FUNCTION_BIT: u8 = 1;
const PRIVATE_BIT: u8 = 2;
const UNKNOWN_BIT: u8 = 4;
const MIXED_BITS: u8 = FUNCTION_BIT | PRIVATE_BIT;
/// Enough to tell one root from several and to name two of them.
const MAX_ROOTS: usize = 3;

#[derive(Clone, Debug)]
enum NetDef {
    Root(u8),
    Gep(LlGep),
    Copy(LlValue),
    Phi(Vec<(LlValue, String)>),
    Unknown,
}

/// The function's address-space-0 pointer networks (see the module comment).
#[derive(Clone, Debug, Default)]
pub(in crate::native::emitter) struct MixedStorageNetwork {
    defs: HashMap<String, NetDef>,
    bits: HashMap<String, u8>,
    /// The roots (alloca names, address-space-0 global names) every value
    /// can come from, at most `MAX_ROOTS` kept: an arm can only be built on
    /// one root, and a forward reservation must be on one of these.
    roots: HashMap<String, BTreeSet<String>>,
    private_globals: HashSet<String>,
    /// Members taken by a pointer phi or select that is not a member itself
    /// (a phi that also merges an unknown pointer): such a consumer cannot be
    /// built from a split pointer.
    outside_consumers: HashSet<String>,
}

impl MixedStorageNetwork {
    pub(super) fn is_member(&self, name: &str) -> bool {
        self.bits.get(name) == Some(&MIXED_BITS)
    }

    pub(super) fn has_outside_consumer(&self, name: &str) -> bool {
        self.outside_consumers.contains(name)
    }

    /// The roots of one storage a value can come from (sorted, at most
    /// `MAX_ROOTS`).
    pub(super) fn storage_roots(&self, name: &str, storage: StorageClass) -> Vec<&str> {
        self.roots
            .get(name)
            .into_iter()
            .flatten()
            .filter(|root| {
                (storage == StorageClass::Private) == self.private_globals.contains(*root)
            })
            .map(String::as_str)
            .collect()
    }

    /// Every root a value can come from, when the TIR knows them all (no
    /// pointer of unknown origin flows into it).
    fn known_roots(&self, name: &str) -> Option<&BTreeSet<String>> {
        let bits = *self.bits.get(name)?;
        (bits & UNKNOWN_BIT == 0 && bits != 0)
            .then(|| self.roots.get(name))
            .flatten()
    }

    /// The single storage of a value that is not a member (Function or
    /// Private only), from the TIR: the storage a forward value will have.
    pub(super) fn pure_storage(&self, name: &str) -> Option<StorageClass> {
        match self.bits.get(name)? {
            &FUNCTION_BIT => Some(StorageClass::Function),
            &PRIVATE_BIT => Some(StorageClass::Private),
            _ => None,
        }
    }

    /// The member a value's arms belong to: copies share their source's.
    fn arm_owner<'a>(&'a self, mut name: &'a str) -> &'a str {
        let mut steps = 0;
        while let Some(NetDef::Copy(LlValue::Local(source))) = self.defs.get(name) {
            if !self.is_member(source) || steps > self.defs.len() {
                break;
            }
            name = source;
            steps += 1;
        }
        name
    }

    pub(super) fn arm_name(&self, name: &str, group: usize) -> String {
        format!("{}{ARM_TAG}{group}", self.arm_owner(name))
    }

    /// The member phi whose selector a member uses: GEPs and copies walk to
    /// their base. None for a member that reaches no phi (cannot happen for a
    /// member, whose two storages meet only at a phi).
    pub(super) fn cond_name<'a>(&'a self, mut name: &'a str) -> Option<String> {
        for _ in 0..=self.defs.len() {
            match self.defs.get(name)? {
                NetDef::Phi(_) => return Some(format!("{name}{ARM_TAG}.cond")),
                NetDef::Gep(LlGep {
                    base:
                        TypedValue {
                            value: LlValue::Local(base),
                            ..
                        },
                    ..
                })
                | NetDef::Copy(LlValue::Local(base)) => name = base,
                _ => return None,
            }
        }
        None
    }

    /// A forward GEP straight off a root (an alloca or a global), the
    /// template of an arm whose own values are all still to come.
    pub(super) fn root_gep(&self, name: &str) -> Option<&LlGep> {
        match self.defs.get(name)? {
            NetDef::Gep(gep) => match &gep.base.value {
                LlValue::Global(_) => Some(gep),
                LlValue::Local(base) => {
                    matches!(self.defs.get(base), Some(NetDef::Root(_))).then_some(gep)
                }
                _ => None,
            },
            _ => None,
        }
    }
}

/// For every network value, the values whose definition reads it.
fn network_users(defs: &HashMap<String, NetDef>) -> HashMap<String, Vec<String>> {
    let mut users: HashMap<String, Vec<String>> = HashMap::new();
    for (name, def) in defs {
        let mut add = |value: &LlValue| {
            if let LlValue::Local(local) = value {
                users.entry(local.clone()).or_default().push(name.clone());
            }
        };
        match def {
            NetDef::Gep(gep) => add(&gep.base.value),
            NetDef::Copy(source) => add(source),
            NetDef::Phi(incoming) => incoming.iter().for_each(|(value, _)| add(value)),
            NetDef::Root(_) | NetDef::Unknown => {}
        }
    }
    users
}

/// The roots of every value (see `MixedStorageNetwork::roots`), over the
/// same worklist as the storages: sets only grow and are capped.
fn network_roots(
    defs: &HashMap<String, NetDef>,
    users: &HashMap<String, Vec<String>>,
    private_globals: &HashSet<&str>,
) -> HashMap<String, BTreeSet<String>> {
    let mut roots: HashMap<String, BTreeSet<String>> = defs
        .iter()
        .map(|(name, def)| {
            let initial = match def {
                NetDef::Root(_) => BTreeSet::from([name.clone()]),
                _ => BTreeSet::new(),
            };
            (name.clone(), initial)
        })
        .collect();
    let mut keys = defs.keys().cloned().collect::<Vec<_>>();
    keys.sort();
    let mut queue = std::collections::VecDeque::from(keys);
    while let Some(name) = queue.pop_front() {
        let sources: Vec<&LlValue> = match &defs[&name] {
            NetDef::Root(_) | NetDef::Unknown => continue,
            NetDef::Gep(gep) => vec![&gep.base.value],
            NetDef::Copy(source) => vec![source],
            NetDef::Phi(incoming) => incoming.iter().map(|(value, _)| value).collect(),
        };
        let mut grown = Vec::new();
        for source in sources {
            match source {
                LlValue::Local(local) => {
                    grown.extend(roots.get(local).into_iter().flatten().cloned());
                }
                LlValue::Global(global) if private_globals.contains(global.as_str()) => {
                    grown.push(global.clone());
                }
                _ => {}
            }
        }
        let current = roots.get_mut(&name).expect("every def has roots");
        let mut changed = false;
        for root in grown {
            if current.len() >= MAX_ROOTS {
                break;
            }
            changed |= current.insert(root);
        }
        if changed {
            queue.extend(users.get(&name).into_iter().flatten().cloned());
        }
    }
    roots
}

fn value_bits(value: &LlValue, bits: &HashMap<String, u8>, private_globals: &HashSet<&str>) -> u8 {
    match value {
        LlValue::Undef => 0,
        LlValue::Local(name) => bits.get(name).copied().unwrap_or(UNKNOWN_BIT),
        LlValue::Global(name) if private_globals.contains(name.as_str()) => PRIVATE_BIT,
        _ => UNKNOWN_BIT,
    }
}

impl Emitter {
    /// Builds the function's network (see the module comment) and registers
    /// the members' arms as forward TIR facts. A function without a member
    /// is left exactly as before.
    pub(in crate::native::emitter) fn collect_mixed_storage_network(
        &mut self,
        tir: &crate::native::tir::TirFunction,
    ) {
        let private_globals = self
            .ir
            .globals
            .iter()
            .filter(|global| global.addrspace == 0)
            .map(|global| global.name.as_str())
            .collect::<HashSet<_>>();
        let mut defs = HashMap::new();
        let mut select_operands = Vec::new();
        for inst in tir.blocks.iter().flat_map(|block| &block.insts) {
            let (Some(result), Some(LlType::Ptr(0))) = (&inst.result, &inst.result_ty) else {
                continue;
            };
            let def = match inst.opcode {
                TirOpcode::Alloca => NetDef::Root(FUNCTION_BIT),
                TirOpcode::GetElementPtr => match inst.gep() {
                    Some(gep) if gep.base.ty == LlType::Ptr(0) => NetDef::Gep((**gep).clone()),
                    _ => NetDef::Unknown,
                },
                TirOpcode::Bitcast | TirOpcode::Freeze | TirOpcode::Metal2VulkanInlineParameter => {
                    match inst
                        .operands
                        .first()
                        .and_then(|operand| operand.as_typed_value())
                    {
                        Some(source) if source.ty == LlType::Ptr(0) => NetDef::Copy(source.value),
                        _ => NetDef::Unknown,
                    }
                }
                TirOpcode::Phi => match inst.phi_incoming() {
                    Some((_, incoming)) => NetDef::Phi(incoming.clone()),
                    None => NetDef::Unknown,
                },
                TirOpcode::Load => match self.field_forwards.get(result) {
                    Some(stored) if stored.ty == LlType::Ptr(0) => {
                        NetDef::Copy(stored.value.clone())
                    }
                    _ => NetDef::Unknown,
                },
                TirOpcode::Select => {
                    if let Some(arms) = inst.select_arms() {
                        select_operands.push([arms.0.value.clone(), arms.1.value.clone()]);
                    }
                    NetDef::Unknown
                }
                _ => NetDef::Unknown,
            };
            defs.insert(result.clone(), def);
        }
        if !defs.values().any(|def| matches!(def, NetDef::Phi(_))) {
            self.mixed_storage_network = MixedStorageNetwork::default();
            return;
        }

        // Union of storages to a fixed point over a worklist: a value is
        // recomputed only when an operand grew, and bits only grow (three of
        // them), so the work is linear in the network's edges.
        let mut bits = defs
            .iter()
            .map(|(name, def)| {
                let initial = match def {
                    NetDef::Root(bits) => *bits,
                    NetDef::Unknown => UNKNOWN_BIT,
                    _ => 0,
                };
                (name.clone(), initial)
            })
            .collect::<HashMap<_, _>>();
        let users = network_users(&defs);
        let mut queue = defs
            .keys()
            .cloned()
            .collect::<std::collections::VecDeque<_>>();
        while let Some(name) = queue.pop_front() {
            let next = match &defs[&name] {
                NetDef::Root(_) | NetDef::Unknown => continue,
                NetDef::Gep(gep) => value_bits(&gep.base.value, &bits, &private_globals),
                NetDef::Copy(source) => value_bits(source, &bits, &private_globals),
                NetDef::Phi(incoming) => incoming.iter().fold(0, |acc, (value, _)| {
                    acc | value_bits(value, &bits, &private_globals)
                }),
            };
            let current = bits.get_mut(&name).expect("every def has bits");
            if *current | next != *current {
                *current |= next;
                queue.extend(users.get(&name).into_iter().flatten().cloned());
            }
        }
        let roots = network_roots(&defs, &users, &private_globals);
        let private_globals = private_globals
            .into_iter()
            .map(str::to_string)
            .collect::<HashSet<_>>();
        let network = MixedStorageNetwork {
            defs,
            bits,
            roots,
            private_globals,
            outside_consumers: HashSet::new(),
        };
        if !network.bits.values().any(|bits| *bits == MIXED_BITS) {
            // No member: kept only for the forward-reservation root check.
            self.mixed_storage_network = network;
            return;
        }
        let mut outside_consumers = HashSet::new();
        for (name, def) in &network.defs {
            let NetDef::Phi(incoming) = def else {
                continue;
            };
            if network.is_member(name) {
                continue;
            }
            for (value, _) in incoming {
                if let LlValue::Local(local) = value {
                    if network.is_member(local) {
                        outside_consumers.insert(network.arm_owner(local).to_string());
                        outside_consumers.insert(local.clone());
                    }
                }
            }
        }
        for operands in select_operands {
            for value in operands {
                if let LlValue::Local(local) = value {
                    if network.is_member(&local) {
                        outside_consumers.insert(network.arm_owner(&local).to_string());
                        outside_consumers.insert(local);
                    }
                }
            }
        }
        let network = MixedStorageNetwork {
            outside_consumers,
            ..network
        };
        self.register_mixed_network_arms(&network);
        self.mixed_storage_network = network;
    }

    /// The pointee of every network value, from its definition: an alloca's
    /// declared type, a global's, a GEP's selected element, a copy's
    /// source's, a phi's first incoming value that has one. A forward arm
    /// needs it before anything of its member is emitted.
    fn mixed_network_pointees(&self, network: &MixedStorageNetwork) -> HashMap<String, LlType> {
        let mut pointees: HashMap<String, LlType> = HashMap::new();
        let users = network_users(&network.defs);
        // Sorted, so the pointee a phi of mixed pointees takes does not
        // depend on hash order.
        let mut keys = network.defs.keys().cloned().collect::<Vec<_>>();
        keys.sort();
        let mut queue = std::collections::VecDeque::from(keys);
        while let Some(name) = queue.pop_front() {
            if pointees.contains_key(&name) {
                continue;
            }
            let known = |value: &LlValue, pointees: &HashMap<String, LlType>| match value {
                LlValue::Local(local) => pointees.get(local).cloned(),
                LlValue::Global(global) => self.pointer_pointees.get(global).cloned(),
                _ => None,
            };
            let pointee = match &network.defs[&name] {
                NetDef::Root(_) => self.local_alloca_pointees.get(&name).cloned(),
                NetDef::Gep(gep) => self
                    .resolve_type(&gep.source_ty)
                    .and_then(|source| gep_pointee(&source, &gep.indices))
                    .ok(),
                NetDef::Copy(source) => known(source, &pointees),
                NetDef::Phi(incoming) => incoming
                    .iter()
                    .find_map(|(value, _)| known(value, &pointees)),
                NetDef::Unknown => None,
            };
            if let Some(pointee) = pointee {
                pointees.insert(name.clone(), pointee);
                queue.extend(users.get(&name).into_iter().flatten().cloned());
            }
        }
        pointees
    }

    /// Forward facts for every member's arms, so a phi that names an arm
    /// before it is emitted resolves it like any forward pointer.
    fn register_mixed_network_arms(&mut self, network: &MixedStorageNetwork) {
        let storages = [StorageClass::Function, StorageClass::Private];
        let pointees = self.mixed_network_pointees(network);
        for (name, def) in &network.defs {
            if !network.is_member(name) {
                continue;
            }
            for (group, storage) in storages.into_iter().enumerate() {
                let arm = format!("{name}{ARM_TAG}{group}");
                match def {
                    NetDef::Phi(incoming) => {
                        let arm_incoming = incoming
                            .iter()
                            .map(|(value, label)| {
                                let value = match value {
                                    LlValue::Local(local) if network.is_member(local) => {
                                        LlValue::Local(network.arm_name(local, group))
                                    }
                                    LlValue::Local(local)
                                        if network.pure_storage(local) == Some(storage) =>
                                    {
                                        value.clone()
                                    }
                                    LlValue::Global(_) if storage == StorageClass::Private => {
                                        value.clone()
                                    }
                                    // The other storage's edge: its zero, made
                                    // at emission; an analysis placeholder here.
                                    _ => LlValue::Undef,
                                };
                                (value, label.clone())
                            })
                            .collect::<Vec<_>>();
                        for (value, _) in &arm_incoming {
                            if let LlValue::Local(local) = value {
                                self.pointer_phi_incoming_values.insert(local.clone());
                            }
                        }
                        self.tir_phi_incomings.insert(arm.clone(), arm_incoming);
                        self.pointer_phi_values.insert(arm.clone());
                    }
                    NetDef::Gep(gep) => {
                        let LlValue::Local(base) = &gep.base.value else {
                            continue;
                        };
                        let mut arm_gep = gep.clone();
                        arm_gep.base.value = LlValue::Local(network.arm_name(base, group));
                        self.forward_geps.insert(arm.clone(), arm_gep);
                    }
                    _ => continue,
                }
                self.tir_result_types.insert(arm.clone(), LlType::Ptr(0));
                if let Some(pointee) = pointees.get(name) {
                    self.pointer_pointees.insert(arm.clone(), pointee.clone());
                }
                self.pointer_storage.insert(arm, storage);
            }
        }
    }

    /// A member phi whose arm would merge two roots of one storage: one
    /// index-domain arm has one root, and a forward value of the other root
    /// would be read through the first one. Refused by name.
    pub(super) fn refuse_multi_root_network(&self, name: &str) -> Result<(), String> {
        for storage in [StorageClass::Function, StorageClass::Private] {
            let roots = self.mixed_storage_network.storage_roots(name, storage);
            if roots.len() > 1 {
                return Err(format!(
                    "native emitter: mixed-storage pointer phi {name}: its network has more than one {storage:?} root ({}) (N75)",
                    roots.join(", ")
                ));
            }
        }
        Ok(())
    }

    /// A pointer phi whose provenance an earlier forward use reserved on a
    /// root (`prior_root`) that none of its values comes from (open (b) of
    /// N75: the use would read the wrong variable). Refused by name; only
    /// when the TIR knows every root of the phi and `prior_root` is itself
    /// an alloca or a Private global, so an alias root is never refused.
    pub(in crate::native::emitter) fn refuse_reserved_on_foreign_root(
        &self,
        name: &str,
        prior_root: Word,
    ) -> Result<(), String> {
        let network = &self.mixed_storage_network;
        let Some(roots) = network.known_roots(name) else {
            return Ok(());
        };
        let root_id = |root: &str| {
            if network.private_globals.contains(root) {
                self.global_values.get(root).map(|(id, _)| *id)
            } else {
                self.values.get(root).map(|(id, _)| *id)
            }
        };
        if roots.iter().any(|root| root_id(root) == Some(prior_root)) {
            return Ok(());
        }
        let mut candidates = network
            .defs
            .iter()
            .filter(|(_, def)| matches!(def, NetDef::Root(_)))
            .map(|(root, _)| root.as_str())
            .chain(network.private_globals.iter().map(String::as_str))
            .collect::<Vec<_>>();
        candidates.sort();
        let Some(foreign) = candidates
            .into_iter()
            .find(|root| root_id(root) == Some(prior_root))
        else {
            return Ok(());
        };
        Err(format!(
            "native emitter: pointer phi {name} was taken on root {foreign} by an earlier use, but its values come from {} (N75)",
            roots.iter().cloned().collect::<Vec<_>>().join(", ")
        ))
    }

    /// A network member emitted without its split, after a phi already
    /// named its arms or its selector (they would never be defined): refused
    /// by name instead of leaving ids that no instruction defines.
    pub(in crate::native::emitter) fn refuse_unsplit_network_member(
        &self,
        name: &str,
        what: &str,
    ) -> Result<(), String> {
        let network = &self.mixed_storage_network;
        if !network.is_member(name) {
            return Ok(());
        }
        let cond = network
            .defs
            .get(name)
            .filter(|def| matches!(def, NetDef::Phi(_)))
            .map(|_| format!("{name}{ARM_TAG}.cond"));
        let named = [0, 1]
            .map(|group| format!("{name}{ARM_TAG}{group}"))
            .into_iter()
            .chain(cond)
            .find(|reserved| self.values.contains_key(reserved));
        match named {
            Some(reserved) => Err(format!(
                "native emitter: mixed-storage {what} {name} (Function vs Private) cannot be split, but a phi already took {reserved}; this network is not supported (N75)"
            )),
            None => Ok(()),
        }
    }

    /// The template of an arm whose phi has no value of the arm's storage
    /// already seen: the nearest value of that storage in the network (a
    /// seen value with its provenance, or a forward GEP straight off a root),
    /// searched from the phi through the members' incoming values and bases.
    pub(super) fn mixed_network_arm_template(
        &mut self,
        phi: &str,
        storage: StorageClass,
    ) -> Result<Option<(GepProvenance, LlType)>, String> {
        let mut queue = std::collections::VecDeque::from([phi.to_string()]);
        let mut visited = HashSet::new();
        while let Some(name) = queue.pop_front() {
            if !visited.insert(name.clone()) {
                continue;
            }
            if self.pointer_storage.get(&name) == Some(&storage)
                && !self.mixed_storage_network.is_member(&name)
            {
                if let (Some(provenance), Some(pointee)) = (
                    self.gep_provenance.get(&name).cloned(),
                    self.pointer_pointees.get(&name).cloned(),
                ) {
                    return Ok(Some((provenance, pointee)));
                }
            }
            if self.mixed_storage_network.pure_storage(&name) == Some(storage) {
                if let Some(template) = self.mixed_forward_root_template(&name, storage)? {
                    return Ok(Some(template));
                }
            }
            // Walk members and values of the wanted storage only.
            if !self.mixed_storage_network.is_member(&name)
                && self.mixed_storage_network.pure_storage(&name) != Some(storage)
            {
                continue;
            }
            match self.mixed_storage_network.defs.get(&name) {
                Some(NetDef::Phi(incoming)) => {
                    for (value, _) in incoming {
                        if let LlValue::Local(local) = value {
                            queue.push_back(local.clone());
                        }
                    }
                }
                Some(NetDef::Gep(LlGep {
                    base:
                        TypedValue {
                            value: LlValue::Local(base),
                            ..
                        },
                    ..
                }))
                | Some(NetDef::Copy(LlValue::Local(base))) => queue.push_back(base.clone()),
                _ => {}
            }
        }
        Ok(None)
    }

    /// The template of an arm whose own incoming values are all forward: a
    /// forward GEP straight off a root that is already emitted, as the
    /// provenance it will have (its root, its path).
    pub(super) fn mixed_forward_root_template(
        &mut self,
        value: &str,
        storage: StorageClass,
    ) -> Result<Option<(GepProvenance, LlType)>, String> {
        let Some(gep) = self.mixed_storage_network.root_gep(value).cloned() else {
            return Ok(None);
        };
        let root = match &gep.base.value {
            LlValue::Global(global) => self.global_values.get(global).map(|(id, _)| *id),
            LlValue::Local(local) => self.values.get(local).map(|(id, _)| *id),
            _ => None,
        };
        let Some(root) = root else {
            return Ok(None);
        };
        let source_ty = self.resolve_type(&gep.source_ty)?;
        let pointee = gep_pointee(&source_ty, &gep.indices)?;
        let root_is_indexed_container = self.is_indexed_container_root(root, Some(storage));
        Ok(Some((
            GepProvenance {
                root,
                addrspace: 0,
                source_ty,
                indices: gep.indices.clone(),
                root_indices: None,
                root_is_indexed_container,
            },
            pointee,
        )))
    }
}
