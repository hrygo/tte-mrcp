#include "funasr_nlsml.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK_TRUE(label, expression) \
    do { \
        if (!(expression)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, label); \
            failures++; \
        } \
    } while (0)

static void test_successful_results_are_numbered_per_channel(void)
{
    apr_pool_t *pool = NULL;
    uint64_t success_count = 0;
    uint64_t new_channel_success_count = 0;
    char *first;
    char *second;
    char *new_channel_first;

    CHECK_TRUE("pool is created", apr_pool_create(&pool, NULL) == APR_SUCCESS);
    if (!pool) {
        return;
    }

    first = funasr_nlsml_result_create(
        pool,
        "session-a",
        "hello",
        &success_count);
    second = funasr_nlsml_result_create(
        pool,
        "session-a",
        "goodbye",
        &success_count);
    new_channel_first = funasr_nlsml_result_create(
        pool,
        "session-b",
        "welcome",
        &new_channel_success_count);

    CHECK_TRUE("first result gets _1.wav",
               first && strcmp(
                   first,
                   "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                   "<result>\n"
                   "  <interpretation grammar=\"session:session-a\" confidence=\"1\">\n"
                   "    <instance><result>hello@session-a_1.wav</result></instance>\n"
                   "    <input mode=\"speech\">hello</input>\n"
                   "  </interpretation>\n"
                   "</result>") == 0);
    CHECK_TRUE("second result gets _2.wav",
               second && strcmp(
                   second,
                   "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                   "<result>\n"
                   "  <interpretation grammar=\"session:session-a\" confidence=\"1\">\n"
                   "    <instance><result>goodbye@session-a_2.wav</result></instance>\n"
                   "    <input mode=\"speech\">goodbye</input>\n"
                   "  </interpretation>\n"
                   "</result>") == 0);
    CHECK_TRUE("successful result count advances twice", success_count == 2);
    CHECK_TRUE("new channel starts with _1.wav",
               new_channel_first &&
                   strstr(new_channel_first, "welcome@session-b_1.wav") != NULL &&
                   new_channel_success_count == 1);

    apr_pool_destroy(pool);
}

static void test_invalid_result_does_not_consume_sequence(void)
{
    apr_pool_t *pool = NULL;
    uint64_t success_count = 0;
    char *first;

    CHECK_TRUE("pool is created", apr_pool_create(&pool, NULL) == APR_SUCCESS);
    if (!pool) {
        return;
    }

    CHECK_TRUE("missing result is rejected",
               funasr_nlsml_result_create(
                   pool,
                   "session-b",
                   NULL,
                   &success_count) == NULL);
    CHECK_TRUE("missing result does not consume a sequence", success_count == 0);
    CHECK_TRUE("empty result is rejected",
               funasr_nlsml_result_create(
                   pool,
                   "session-b",
                   "",
                   &success_count) == NULL);
    CHECK_TRUE("empty result does not consume a sequence", success_count == 0);
    first = funasr_nlsml_result_create(
        pool,
        "session-b",
        "recovered",
        &success_count);
    CHECK_TRUE("first valid result remains _1.wav",
               first &&
                   strstr(first, "recovered@session-b_1.wav") != NULL &&
                   success_count == 1);

    apr_pool_destroy(pool);
}

int main(void)
{
    if (apr_initialize() != APR_SUCCESS) {
        fprintf(stderr, "FAIL APR initialization\n");
        return 1;
    }

    test_successful_results_are_numbered_per_channel();
    test_invalid_result_does_not_consume_sequence();

    apr_terminate();

    if (failures != 0) {
        fprintf(stderr, "%d NLSML assertion(s) failed\n", failures);
        return 1;
    }

    printf("PASS test_funasr_nlsml\n");
    return 0;
}
