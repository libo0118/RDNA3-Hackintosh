/* metal/plugin/nvmtl_ci_refl.h (DRIVER-PLAN N60): the function-node match CoreImage's argument reflection of a visible function starts
 * from (bare and quoted names), the CoreImage struct names reflect() takes as one argument, and the choice between the argument
 * sources, whose NONE answer is what stands between Preview and CIKernelReflection::consolidate's NULL write.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include "nvmtl_ci_refl.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
    /* bare name: the offset is just past "ptr @main1, " */
    const char *n1 = "ptr @main1, !12, !13";
    CHECK(nvmtl_cirefl_head_skip(n1, "main1") == (long)strlen("ptr @main1, "));
    CHECK(strcmp(n1 + nvmtl_cirefl_head_skip(n1, "main1"), "!12, !13") == 0);
    /* quoted name (a namespace or a clone): the old bare match missed it */
    const char *n2 = "ptr @\"ns::main1\", !4, !5, !6";
    CHECK(nvmtl_cirefl_head_skip(n2, "ns::main1") == (long)strlen("ptr @\"ns::main1\", "));
    CHECK(strcmp(n2 + nvmtl_cirefl_head_skip(n2, "ns::main1"), "!4, !5, !6") == 0);
    /* other functions, prefixes of names, wrong quoting, no comma, not a function node, nulls */
    CHECK(nvmtl_cirefl_head_skip(n1, "main") == -1);
    CHECK(nvmtl_cirefl_head_skip(n1, "main12") == -1);
    CHECK(nvmtl_cirefl_head_skip("ptr @main10, !1", "main1") == -1);
    CHECK(nvmtl_cirefl_head_skip(n2, "ns::main") == -1);
    CHECK(nvmtl_cirefl_head_skip("ptr @\"ns::main1, !1", "ns::main1") == -1);
    CHECK(nvmtl_cirefl_head_skip("ptr @main1 !1", "main1") == -1);
    CHECK(nvmtl_cirefl_head_skip("!\"air.arg_type_name\", !\"float4\"", "main1") == -1);
    CHECK(nvmtl_cirefl_head_skip(NULL, "main1") == -1);
    CHECK(nvmtl_cirefl_head_skip(n1, NULL) == -1);
    CHECK(nvmtl_cirefl_head_skip(n1, "") == -1);
    CHECK(nvmtl_cirefl_head_skip("ptr @f,", "f") == (long)strlen("ptr @f,"));

    /* the 14 CoreImage struct names */
    static const char *const ci[] = { "coreimage::destination", "coreimage::Destination", "coreimage::priv::destination",
        "coreimage::priv::destination_h", "coreimage::priv::Destination", "coreimage::priv::Destination_h",
        "coreimage::group::destination", "coreimage::group::destination_h", "coreimage::group::Destination",
        "coreimage::group::Destination_h", "coreimage::sampler", "coreimage::Sampler", "coreimage::sampler_h", "coreimage::Sampler_h" };
    for (unsigned i = 0; i < sizeof ci / sizeof ci[0]; i++) CHECK(nvmtl_cirefl_is_ci_struct(ci[i]));
    CHECK(!nvmtl_cirefl_is_ci_struct("sampler"));
    CHECK(!nvmtl_cirefl_is_ci_struct("float4"));
    CHECK(!nvmtl_cirefl_is_ci_struct("coreimage::sampler_x"));
    CHECK(!nvmtl_cirefl_is_ci_struct(""));
    CHECK(!nvmtl_cirefl_is_ci_struct(NULL));

    /* the source order: ci, stitching, plain; an empty source never wins; nothing usable = poison */
    CHECK(nvmtl_cirefl_pick(3, 2, 1) == NVMTL_CIREFL_CI);
    CHECK(nvmtl_cirefl_pick(0, 2, 1) == NVMTL_CIREFL_STITCH);
    CHECK(nvmtl_cirefl_pick(0, 0, 4) == NVMTL_CIREFL_PLAIN);
    CHECK(nvmtl_cirefl_pick(0, 0, 0) == NVMTL_CIREFL_NONE);
    CHECK(nvmtl_cirefl_pick(-1, -1, -1) == NVMTL_CIREFL_NONE);

    if (failures) { fprintf(stderr, "test_ci_refl: %d failure(s)\n", failures); return 1; }
    printf("test_ci_refl: ok\n");
    return 0;
}
