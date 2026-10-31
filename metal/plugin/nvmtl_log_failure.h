#ifndef NVMTL_LOG_FAILURE_H
#define NVMTL_LOG_FAILURE_H
#include <string.h>
#include <ctype.h>

static inline int nvmtl_log_fault_word(const char *text, const char *word)
{
    const char *p = text;
    while ((p = strstr(p, word))) {
        if ((p == text || !isalnum((unsigned char)p[-1])) &&
            !isalnum((unsigned char)p[5])) return 1;
        p += 5;
    }
    return 0;
}

/* Keep device/feature failures in release syslog. In particular, Vulkan errors
 * such as "vkCreateDevice -> -8" do not contain the word FAIL. */
static inline int nvmtl_log_failure(const char *text)
{
    return text && (strstr(text, "FAIL") || strstr(text, "REFUS") ||
                    nvmtl_log_fault_word(text, "fault") ||
                    nvmtl_log_fault_word(text, "FAULT") || strstr(text, "-> -") ||
                    strstr(text, "missing") || strstr(text, "cannot") ||
                    strstr(text, "unsupported") || strstr(text, "refused") ||
                    /* framebuffer fetch (N48): a read that is not served, a draw that is skipped, a pass that is restarted */
                    strstr(text, "UNSUPPORTED") || strstr(text, "SKIPPED") || strstr(text, "RESTARTED") || strstr(text, "are zeroed") ||
                    /* a process denied a device: for WindowServer that is the "Failed to create MetalDevice" abort,
                     * and /tmp/nvmtl.log is gone after the reboot that rescues the machine */
                    strstr(text, "no device") ||
                    /* DRIVER-PLAN N49 open item 1: whether this process has its IOAccelCommandQueue (Activity Monitor's % GPU), once per
                     * process, opened or not: triage needs the line either way */
                    strstr(text, "GPU time queue:") ||
                    /* DRIVER-PLAN N52: the totals of the multisample copy passes (Chess's stencil resolve chain), once per process and every 10 s while they move:
                     * triage proves the chain from them, in a release build too */
                    strstr(text, "msdl: SUMMARY") || strstr(text, "msupload: SUMMARY") ||
                    /* DRIVER-PLAN N67: what a pipeline link did with each visible function (linked / unresolved / table slots),
                     * once per link (nvmtl_link_stage): the Mac report must show it from a release plugin too (a failed one says FAILED) */
                    strstr(text, "\" visible functions: linked "));
}
#endif
