#include "funasr_nlsml.h"

#include <apr_strings.h>

#include <stdint.h>

char *funasr_nlsml_result_create(
    apr_pool_t *pool,
    const char *session_id,
    const char *result,
    uint64_t *success_count)
{
    uint64_t next_count;
    char *result_with_suffix;
    char *nlsml;

    if (!pool || !session_id || !result || result[0] == '\0' || !success_count ||
        *success_count == UINT64_MAX) {
        return NULL;
    }

    next_count = *success_count + 1U;
    result_with_suffix = apr_psprintf(
        pool,
        "%s@%s_%" APR_UINT64_T_FMT ".wav",
        result,
        session_id,
        (apr_uint64_t)next_count);
    nlsml = apr_psprintf(
        pool,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<result>\n"
        "  <interpretation grammar=\"session:%s\" confidence=\"1\">\n"
        "    <instance><result>%s</result></instance>\n"
        "    <input mode=\"speech\">%s</input>\n"
        "  </interpretation>\n"
        "</result>",
        session_id,
        result_with_suffix,
        result);
    if (!result_with_suffix || !nlsml) {
        return NULL;
    }

    *success_count = next_count;
    return nlsml;
}
