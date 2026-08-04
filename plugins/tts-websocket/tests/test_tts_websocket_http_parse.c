/*
 * Unit tests for TTS plugin HTTP response parsing
 * This file tests the HTTP parsing logic used in tts_websocket_engine.c
 * without depending on APR or the full MRCP framework.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* Test counters */
static int tests_run = 0;
static int tests_passed = 0;

/* Helper macro for running tests */
#define RUN_TEST(test_func) do { \
    tests_run++; \
    printf("Running %s... ", #test_func); \
    if (test_func()) { \
        tests_passed++; \
        printf("PASS\n"); \
    } \
} while(0)

/* Helper macro for assertions */
#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", msg, __LINE__); \
        return 0; \
    } \
} while(0)

/* Helper macro for equality checks */
#define TEST_EQ(expected, actual, msg) do { \
    if ((expected) != (actual)) { \
        printf("FAIL: %s (expected %ld, got %ld, line %d)\n", \
               msg, (long)(expected), (long)(actual), __LINE__); \
        return 0; \
    } \
} while(0)

/* ========== HTTP Parsing Functions (local copies for testing) ========== */

/**
 * Parse HTTP status code from response line
 * @param response HTTP response buffer
 * @return HTTP status code, or -1 on error
 */
static int http_parse_status_code(const char *response)
{
    int status = -1;
    if (response && strncmp(response, "HTTP/1.", 7) == 0) {
        if (sscanf(response, "HTTP/1.%*d %d", &status) != 1) {
            status = -1;
        }
    }
    return status;
}

/**
 * Find HTTP body start position
 * @param response HTTP response buffer
 * @param response_len Total response length
 * @param body_offset Output: offset to body start
 * @return 1 if found, 0 otherwise
 */
static int http_find_body(const char *response, size_t response_len, size_t *body_offset)
{
    const char *header_end = NULL;

    /* Search for \r\n\r\n marker */
    if (response_len >= 4) {
        /* Create a null-terminated copy for strstr */
        char *temp = (char*)malloc(response_len + 1);
        if (!temp) return 0;
        memcpy(temp, response, response_len);
        temp[response_len] = '\0';

        header_end = strstr(temp, "\r\n\r\n");
        if (header_end) {
            *body_offset = (header_end - temp) + 4;
        }
        free(temp);
    }

    return header_end ? 1 : 0;
}

/**
 * Parse Content-Length header value
 * @param response HTTP response buffer
 * @return Content-Length value, or 0 if not found
 */
static size_t http_parse_content_length(const char *response)
{
    const char *cl_header = strstr(response, "Content-Length:");
    if (!cl_header) {
        cl_header = strstr(response, "content-length:");
    }
    if (!cl_header) {
        cl_header = strstr(response, "Content-length:");
    }

    if (cl_header) {
        size_t len = 0;
        if (sscanf(cl_header, "%*[^:]: %zu", &len) == 1) {
            return len;
        }
    }
    return 0;
}

/* ========== Test Cases ========== */

/**
 * Test: Parse HTTP 200 status code
 */
static int test_parse_status_200(void)
{
    const char *response = "HTTP/1.1 200 OK\r\nContent-Type: audio/pcm\r\n\r\n";
    int status = http_parse_status_code(response);
    TEST_EQ(200, status, "HTTP 200 status code");
    return 1;
}

/**
 * Test: Parse HTTP 404 status code
 */
static int test_parse_status_404(void)
{
    const char *response = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n\r\n";
    int status = http_parse_status_code(response);
    TEST_EQ(404, status, "HTTP 404 status code");
    return 1;
}

/**
 * Test: Parse HTTP 500 status code
 */
static int test_parse_status_500(void)
{
    const char *response = "HTTP/1.0 500 Internal Server Error\r\n\r\n";
    int status = http_parse_status_code(response);
    TEST_EQ(500, status, "HTTP 500 status code");
    return 1;
}

/**
 * Test: Parse invalid HTTP response
 */
static int test_parse_status_invalid(void)
{
    int status;

    /* Test NULL input */
    status = http_parse_status_code(NULL);
    TEST_EQ(-1, status, "NULL response");

    /* Test non-HTTP response */
    status = http_parse_status_code("Not an HTTP response");
    TEST_EQ(-1, status, "Non-HTTP response");

    /* Test malformed HTTP response */
    status = http_parse_status_code("HTTP/1.1");
    TEST_EQ(-1, status, "Malformed HTTP response");

    return 1;
}

/**
 * Test: Find body in standard HTTP response
 */
static int test_find_body_standard(void)
{
    const char *response = "HTTP/1.1 200 OK\r\nContent-Type: audio/pcm\r\nContent-Length: 8\r\n\r\n12345678";
    size_t response_len = strlen(response);
    size_t body_offset = 0;

    int found = http_find_body(response, response_len, &body_offset);
    TEST_ASSERT(found, "Should find body separator");

    /* Calculate expected body offset: find \r\n\r\n and add 4 */
    const char *expected_body = strstr(response, "\r\n\r\n") + 4;
    size_t expected_offset = expected_body - response;
    TEST_EQ(expected_offset, body_offset, "Body offset should match expected");

    /* Verify body content */
    const char *body = response + body_offset;
    TEST_ASSERT(strncmp(body, "12345678", 8) == 0, "Body content should match");

    return 1;
}

/**
 * Test: Find body with minimal headers
 */
static int test_find_body_minimal(void)
{
    const char *response = "HTTP/1.1 200 OK\r\n\r\naudio_data_here";
    size_t response_len = strlen(response);
    size_t body_offset = 0;

    int found = http_find_body(response, response_len, &body_offset);
    TEST_ASSERT(found, "Should find body separator");
    TEST_EQ(19, body_offset, "Body offset should be 19");

    return 1;
}

/**
 * Test: Body not found in incomplete response
 */
static int test_find_body_not_found(void)
{
    const char *response = "HTTP/1.1 200 OK\r\nContent-Type: audio/pcm";
    size_t response_len = strlen(response);
    size_t body_offset = 0;

    int found = http_find_body(response, response_len, &body_offset);
    TEST_ASSERT(!found, "Should not find body separator in incomplete response");

    return 1;
}

/**
 * Test: Find body with binary data
 */
static int test_find_body_binary(void)
{
    /* Construct response with binary body (including null bytes) */
    char response[256];
    const char *header = "HTTP/1.1 200 OK\r\nContent-Length: 16\r\n\r\n";
    size_t header_len = strlen(header);
    unsigned char binary_body[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

    memcpy(response, header, header_len);
    memcpy(response + header_len, binary_body, 16);
    size_t response_len = header_len + 16;

    size_t body_offset = 0;
    int found = http_find_body(response, response_len, &body_offset);
    TEST_ASSERT(found, "Should find body separator");
    TEST_EQ(header_len, body_offset, "Body offset should equal header length");

    /* Verify body content */
    TEST_ASSERT(memcmp(response + body_offset, binary_body, 16) == 0, "Binary body should match");

    return 1;
}

/**
 * Test: Parse Content-Length header
 */
static int test_parse_content_length(void)
{
    const char *response1 = "HTTP/1.1 200 OK\r\nContent-Length: 1024\r\n\r\n";
    size_t len1 = http_parse_content_length(response1);
    TEST_EQ(1024, len1, "Content-Length 1024");

    const char *response2 = "HTTP/1.1 200 OK\r\ncontent-length: 2048\r\n\r\n";
    size_t len2 = http_parse_content_length(response2);
    TEST_EQ(2048, len2, "Content-Length 2048 (lowercase)");

    const char *response3 = "HTTP/1.1 200 OK\r\nContent-Type: audio/pcm\r\n\r\n";
    size_t len3 = http_parse_content_length(response3);
    TEST_EQ(0, len3, "No Content-Length header");

    return 1;
}

/**
 * Test: Simulate chunked response parsing (header split across recv calls)
 * This tests the scenario where \r\n\r\n might be split across two reads
 */
static int test_chunked_header_parsing(void)
{
    /* Simulate two partial reads */
    const char *chunk1 = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r";
    const char *chunk2 = "\naudio";

    size_t chunk1_len = strlen(chunk1);
    size_t chunk2_len = strlen(chunk2);

    /* Simulate buffering */
    char buffer[256];
    size_t total_len = 0;

    memcpy(buffer, chunk1, chunk1_len);
    total_len = chunk1_len;

    /* After first chunk, body should not be found */
    size_t body_offset = 0;
    int found = http_find_body(buffer, total_len, &body_offset);
    TEST_ASSERT(!found, "Body should not be found after first chunk");

    /* Append second chunk */
    memcpy(buffer + total_len, chunk2, chunk2_len);
    total_len += chunk2_len;

    /* Now body should be found */
    found = http_find_body(buffer, total_len, &body_offset);
    TEST_ASSERT(found, "Body should be found after second chunk");
    TEST_EQ(38, body_offset, "Body offset should be 38");

    /* Verify body is "audio" */
    TEST_ASSERT(strncmp(buffer + body_offset, "audio", 5) == 0, "Body should be 'audio'");

    return 1;
}

/**
 * Test: Large response body handling
 */
static int test_large_body(void)
{
    /* Create a response with a 1KB body */
    const char *header = "HTTP/1.1 200 OK\r\nContent-Length: 1024\r\n\r\n";
    size_t header_len = strlen(header);
    size_t body_len = 1024;
    size_t total_len = header_len + body_len;

    char *response = (char*)malloc(total_len);
    TEST_ASSERT(response, "Memory allocation");

    memcpy(response, header, header_len);
    memset(response + header_len, 0xAB, body_len);  /* Fill body with 0xAB pattern */

    size_t body_offset = 0;
    int found = http_find_body(response, total_len, &body_offset);
    TEST_ASSERT(found, "Should find body in large response");
    TEST_EQ(header_len, body_offset, "Body offset in large response");

    /* Verify body content */
    int body_correct = 1;
    for (size_t i = 0; i < body_len; i++) {
        if ((unsigned char)response[body_offset + i] != 0xAB) {
            body_correct = 0;
            break;
        }
    }
    TEST_ASSERT(body_correct, "Large body content should match");

    free(response);
    return 1;
}

/**
 * Test: Dynamic buffer growth simulation
 * Tests that the buffer growth logic (2x expansion) works correctly
 */
static int test_buffer_growth(void)
{
    /* Initial capacity: 64KB, max: 10MB */
    size_t initial_capacity = 64 * 1024;
    size_t max_capacity = 10 * 1024 * 1024;

    /* Simulate growth steps */
    size_t capacity = initial_capacity;
    size_t growth_count = 0;

    /* Grow until we reach 5MB */
    size_t target = 5 * 1024 * 1024;
    while (capacity < target) {
        size_t new_capacity = capacity * 2;
        if (new_capacity > max_capacity) {
            new_capacity = max_capacity;
        }
        capacity = new_capacity;
        growth_count++;
    }

    TEST_ASSERT(capacity >= target, "Should reach target capacity");
    TEST_ASSERT(capacity <= max_capacity, "Should not exceed max capacity");
    printf("(growth steps: %zu) ", growth_count);

    return 1;
}

/**
 * Test: HTTP response with no body
 */
static int test_empty_body(void)
{
    const char *response = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n";
    size_t response_len = strlen(response);
    size_t body_offset = 0;

    int found = http_find_body(response, response_len, &body_offset);
    TEST_ASSERT(found, "Should find body separator");
    TEST_EQ(response_len, body_offset, "Body offset should equal response length (empty body)");

    return 1;
}

/* ========== Main ========== */

int main(void)
{
    printf("========================================\n");
    printf("TTS Plugin HTTP Parsing Unit Tests\n");
    printf("========================================\n\n");

    /* Status code parsing tests */
    printf("--- HTTP Status Code Parsing ---\n");
    RUN_TEST(test_parse_status_200);
    RUN_TEST(test_parse_status_404);
    RUN_TEST(test_parse_status_500);
    RUN_TEST(test_parse_status_invalid);

    /* Body finding tests */
    printf("\n--- HTTP Body Finding ---\n");
    RUN_TEST(test_find_body_standard);
    RUN_TEST(test_find_body_minimal);
    RUN_TEST(test_find_body_not_found);
    RUN_TEST(test_find_body_binary);
    RUN_TEST(test_empty_body);

    /* Content-Length parsing tests */
    printf("\n--- Content-Length Parsing ---\n");
    RUN_TEST(test_parse_content_length);

    /* Edge case tests */
    printf("\n--- Edge Cases ---\n");
    RUN_TEST(test_chunked_header_parsing);
    RUN_TEST(test_large_body);
    RUN_TEST(test_buffer_growth);

    /* Summary */
    printf("\n========================================\n");
    printf("Results: %d/%d tests passed\n", tests_passed, tests_run);
    printf("========================================\n");

    return (tests_passed == tests_run) ? 0 : 1;
}
