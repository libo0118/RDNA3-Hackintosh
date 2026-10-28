/*
 * CoreImage's reading of a Metal function (DRIVER-PLAN N60), the pure part, so tests/test_ci_refl.c can run it on the host.
 *
 * Card run 2026-10-26 (app survey, macOS 26.4.1): Preview died in
 *   CIKernelReflection::consolidate(CIKernelReflection const&, CIKernelReflection const&) +178   (NULL write)
 *   <- -[CIKernel _initWithString:andCIKernelLibrary:usingCruftCompatibility:isInternal:] <- +[CIColorKernel kernelWithString:]
 * CoreImage compiles a CIKL kernel to Metal source ([[stitchable]] functions, `-[CIKernelLibrary initWithSource:error:]` ->
 * `newLibraryWithSource:options:error:`), then CIKernelReflection::reflect(CIKernelLibrary*, name) asks the library's MTLFunction for
 * functionType, returnType and `arguments` and turns them into a vector of CI::KernelArgumentType. consolidate() then copies the
 * argument types of the FOSL (source) reflection over that vector, element by element, without a size check: the library's vector was
 * empty (reflect accepts a function without arguments), the source's was not, so the first store went to NULL.
 * Our NVMTLFunction answered `arguments` from the AIR's `!air.ci` / stitching metadata only, and with nil for anything else.
 *
 * What the functions here are used for (NVMTLObjects.m, nvmtl_ci_*):
 *   - nvmtl_cirefl_head_skip: the node of a function in `!air.visible` / `!air.ci` starts `ptr @NAME,`; LLVM prints a name with
 *     ':' or '.' (a namespace, a clone) quoted, `ptr @"NAME",`; the old bare-only match skipped those functions.
 *   - nvmtl_cirefl_is_ci_struct: the 14 struct type names CIKernelReflection::reflect(CIKernelLibrary*, ...) recognises (the
 *     decompiled list in CoreImage 26.4.1, `coreimage::destination` ... `coreimage::Sampler_h`); such an argument is ONE entry
 *     whatever its members are.
 *   - nvmtl_cirefl_pick: which source of arguments to answer from, and when to answer a poison argument instead (see below).
 *
 * The poison argument. When none of the parsers finds a usable argument list, an empty answer is the worst one (the crash above).
 * An argument of binding type 18 whose data type is None goes through reflect's argument loop as a value of 0 and reaches the
 * default arm of its second switch: CIKernelError(3, "Unsupported type for parameter") and reflect returns false, which makes
 * -[CIKernel _initWithString:...] skip consolidate() and build the kernel from the FOSL reflection alone (the path taken with
 * CI_USE_MTL_DAG_FOR_CIKL_SRC=0). A kernel that fails to build is a smaller failure than a dead Preview.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NVMTL_CI_REFL_H
#define NVMTL_CI_REFL_H
#include <stddef.h>
#include <string.h>

/* `body` is the text of a metadata node after "= !{"; returns the offset just past "ptr @NAME," and the blanks after it, or -1 when
 * the node is not the one of function `fname`. NAME may be bare or in quotes. */
static inline long nvmtl_cirefl_head_skip(const char *body, const char *fname)
{
    if (!body || !fname || !*fname) return -1;
    const size_t n = strlen(fname);
    if (strncmp(body, "ptr @", 5) != 0) return -1;
    const char *p = body + 5;
    if (*p == '"') {
        p++;
        if (strncmp(p, fname, n) != 0 || p[n] != '"') return -1;
        p += n + 1;
    } else {
        if (strncmp(p, fname, n) != 0) return -1;
        p += n;
    }
    if (*p != ',') return -1;
    p++;
    while (*p == ' ') p++;
    return (long)(p - body);
}

/* The struct type names that reflect() takes as one argument each (CoreImage 26.4.1, CIKernelReflection::reflect(CIKernelLibrary*,
 * char const*, NSError**): `coreimage::destination` / `Destination` / `priv::` / `group::` variants and `sampler` / `Sampler` /
 * `sampler_h` / `Sampler_h`). */
static inline int nvmtl_cirefl_is_ci_struct(const char *t)
{
    static const char *const names[] = {
        "coreimage::destination", "coreimage::Destination",
        "coreimage::priv::destination", "coreimage::priv::destination_h",
        "coreimage::priv::Destination", "coreimage::priv::Destination_h",
        "coreimage::group::destination", "coreimage::group::destination_h",
        "coreimage::group::Destination", "coreimage::group::Destination_h",
        "coreimage::sampler", "coreimage::Sampler", "coreimage::sampler_h", "coreimage::Sampler_h" };
    if (!t) return 0;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(t, names[i]) == 0) return 1;
    return 0;
}

/* Where `-[NVMTLFunction arguments]` / `returnType` of a visible function come from. The first source (in this order) that found
 * at least one argument wins; a source that found none (an entry parsed with no usable argument, or no entry) does not count,
 * because an empty list is what crashes CoreImage. NONE means: answer the poison argument. */
enum { NVMTL_CIREFL_NONE = 0, NVMTL_CIREFL_CI = 1, NVMTL_CIREFL_STITCH = 2, NVMTL_CIREFL_PLAIN = 3 };
static inline int nvmtl_cirefl_pick(long ci_args, long stitch_args, long plain_args)
{
    if (ci_args > 0) return NVMTL_CIREFL_CI;
    if (stitch_args > 0) return NVMTL_CIREFL_STITCH;
    if (plain_args > 0) return NVMTL_CIREFL_PLAIN;
    return NVMTL_CIREFL_NONE;
}
#endif
