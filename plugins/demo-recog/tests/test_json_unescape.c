#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

/* Mock APR pool using standard malloc for testing */
typedef struct {
    void *data;
} apr_pool_t;

static void* apr_palloc(apr_pool_t *pool, size_t size)
{
    (void)pool; /* unused */
    return malloc(size);
}

/* The function under test - copied from demo_recog_engine.c */
static char* funasr_json_unescape_string(apr_pool_t *pool, const char *str)
{
    if (!str) {
        return NULL;
    }

    /* UTF-8 output can be up to 3x longer for worst case (ASCII to Chinese) */
    size_t len = strlen(str);
    char *out = apr_palloc(pool, len * 3 + 1);
    size_t out_idx = 0;

    for (size_t i = 0; i < len; ) {
        if (str[i] == '\\' && i + 1 < len) {
            switch (str[i + 1]) {
                case '"':  out[out_idx++] = '"';  i += 2; break;
                case '\\': out[out_idx++] = '\\'; i += 2; break;
                case '/':  out[out_idx++] = '/';  i += 2; break;
                case 'b':  out[out_idx++] = '\b'; i += 2; break;
                case 'f':  out[out_idx++] = '\f'; i += 2; break;
                case 'n':  out[out_idx++] = '\n'; i += 2; break;
                case 'r':  out[out_idx++] = '\r'; i += 2; break;
                case 't':  out[out_idx++] = '\t'; i += 2; break;
                case 'u':
                    /* Unicode escape \uXXXX */
                    if (i + 5 < len) {
                        /* Parse 4 hex digits */
                        unsigned int codepoint = 0;
                        for (int j = 0; j < 4; j++) {
                            char c = str[i + 2 + j];
                            codepoint <<= 4;
                            if (c >= '0' && c <= '9') {
                                codepoint |= (c - '0');
                            } else if (c >= 'a' && c <= 'f') {
                                codepoint |= (c - 'a' + 10);
                            } else if (c >= 'A' && c <= 'F') {
                                codepoint |= (c - 'A' + 10);
                            }
                        }
                        i += 6;  /* Skip all 6 chars of \uXXXX */

                        /* Convert Unicode codepoint to UTF-8 */
                        if (codepoint <= 0x7F) {
                            out[out_idx++] = (char)codepoint;
                        } else if (codepoint <= 0x7FF) {
                            out[out_idx++] = (char)(0xC0 | (codepoint >> 6));
                            out[out_idx++] = (char)(0x80 | (codepoint & 0x3F));
                        } else if (codepoint <= 0xFFFF) {
                            out[out_idx++] = (char)(0xE0 | (codepoint >> 12));
                            out[out_idx++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                            out[out_idx++] = (char)(0x80 | (codepoint & 0x3F));
                        } else {
                            out[out_idx++] = '?';
                        }
                    } else {
                        out[out_idx++] = str[i++];
                    }
                    break;
                default:
                    out[out_idx++] = str[i++];
                    break;
            }
        } else {
            out[out_idx++] = str[i++];
        }
    }

    out[out_idx] = '\0';
    return out;
}

/* Helper function to create UTF-8 string from bytes */
static char* make_utf8(const unsigned char *bytes, size_t len)
{
    char *str = malloc(len + 1);
    if (str) {
        memcpy(str, bytes, len);
        str[len] = '\0';
    }
    return str;
}

/* Test structure */
typedef struct {
    const char *name;
    const char *input;
    const char *expected_bytes;
    size_t expected_len;
} test_case_t;

/* Helper function to print UTF-8 bytes */
static void print_utf8_bytes(const char *str)
{
    if (!str) {
        printf("(null)");
        return;
    }
    printf("\"");
    for (size_t i = 0; str[i]; i++) {
        unsigned char c = (unsigned char)str[i];
        if (c >= 32 && c < 127) {
            printf("%c", c);
        } else {
            printf("\\x%02x", c);
        }
    }
    printf("\"");
}

/* Compare UTF-8 strings byte by byte */
static int utf8_equals_bytes(const char *result, const char *expected_bytes, size_t expected_len)
{
    if (!result && !expected_bytes) return 1;
    if (!result || !expected_bytes) return 0;
    return memcmp(result, expected_bytes, expected_len) == 0 && result[expected_len] == '\0';
}

/* Run a single test case */
static int run_test(const test_case_t *test)
{
    apr_pool_t pool = {0};
    char *result = funasr_json_unescape_string(&pool, test->input);

    int passed = utf8_equals_bytes(result, test->expected_bytes, test->expected_len);

    printf("[%s] %s\n", passed ? "PASS" : "FAIL", test->name);

    if (!passed) {
        printf("  Input:    ");
        print_utf8_bytes(test->input);
        printf("\n");

        printf("  Expected: ");
        for (size_t i = 0; i < test->expected_len; i++) {
            printf("\\x%02x", (unsigned char)test->expected_bytes[i]);
        }
        printf("\n");

        printf("  Got:      ");
        if (result) {
            for (size_t i = 0; result[i]; i++) {
                printf("\\x%02x", (unsigned char)result[i]);
            }
        } else {
            printf("(null)");
        }
        printf("\n");
    }

    if (result) {
        free(result);
    }

    return passed ? 0 : 1;
}

int main(void)
{
    printf("=== JSON Unicode Escape Sequence Decoding Tests ===\n\n");

    /* Test cases covering various scenarios */
    /* Expected values as byte arrays */
    static const unsigned char expected_utf8_6536[] = {0xE6, 0x94, 0xB6};      /* 收 */
    static const unsigned char expected_utf8_5230[] = {0xE5, 0x88, 0xB0};      /* 到 */
    static const unsigned char expected_utf8_90a3[] = {0xE9, 0x82, 0xA3};      /* 那 */
    static const unsigned char expected_utf8_4e2a[] = {0xE4, 0xB8, 0xAA};      /* 个 */
    static const unsigned char expected_utf8_4f60[] = {0xE4, 0xBD, 0xA0};      /* 你 */
    static const unsigned char expected_utf8_597d[] = {0xE5, 0xA5, 0xBD};      /* 好 */
    static const unsigned char expected_utf8_8c22[] = {0xE8, 0xB0, 0xA2};      /* 谢 */
    static const unsigned char expected_utf8_00e9[] = {0xC3, 0xA9};            /* é */
    static const unsigned char expected_utf8_00bf[] = {0xC2, 0xBF};            /* ¿ */
    static const unsigned char expected_utf8_4e2d[] = {0xE4, 0xB8, 0xAD};      /* 中 */
    static const unsigned char expected_utf8_0041[] = {0x41};                 /* A */

    /* Chinese phrase "收到那个" */
    static const unsigned char expected_utf8_phrase1[] = {
        0xE6, 0x94, 0xB6,  /* 收 */
        0xE5, 0x88, 0xB0,  /* 到 */
        0xE9, 0x82, 0xA3,  /* 那 */
        0xE4, 0xB8, 0xAA   /* 个 */
    };

    /* Chinese phrase "收到" */
    static const unsigned char expected_utf8_phrase2[] = {
        0xE6, 0x94, 0xB6,  /* 收 */
        0xE5, 0x88, 0xB0   /* 到 */
    };

    /* Chinese phrase "你好" */
    static const unsigned char expected_utf8_phrase3[] = {
        0xE4, 0xBD, 0xA0,  /* 你 */
        0xE5, 0xA5, 0xBD   /* 好 */
    };

    /* Chinese phrase "谢谢" */
    static const unsigned char expected_utf8_phrase4[] = {
        0xE8, 0xB0, 0xA2,  /* 谢 */
        0xE8, 0xB0, 0xA2   /* 谢 */
    };

    /* "Text:收到" */
    static const unsigned char expected_utf8_mixed1[] = {
        0x54, 0x65, 0x78, 0x74, 0x3A,  /* Text: */
        0xE6, 0x94, 0xB6,              /* 收 */
        0xE5, 0x88, 0xB0               /* 到 */
    };

    /* "\"Hello\nWorld你\"" */
    static const unsigned char expected_utf8_mixed2[] = {
        0x22,                           /* " */
        0x48, 0x65, 0x6C, 0x6C, 0x6F,  /* Hello */
        0x0A,                           /* \n */
        0x57, 0x6F, 0x72, 0x6C, 0x64,  /* World */
        0xE4, 0xBD, 0xA0,               /* 你 */
        0x22                            /* " */
    };

    /* "Start个End" (U+4E2A) */
    static const unsigned char expected_utf8_mixed3[] = {
        0x53, 0x74, 0x61, 0x72, 0x74,  /* Start */
        0xE4, 0xB8, 0xAA,               /* 个 */
        0x45, 0x6E, 0x64                /* End */
    };

    /* "Start中End" (U+4E2D) */
    static const unsigned char expected_utf8_mixed4[] = {
        0x53, 0x74, 0x61, 0x72, 0x74,  /* Start */
        0xE4, 0xB8, 0xAD,               /* 中 */
        0x45, 0x6E, 0x64                /* End */
    };

    test_case_t tests[] = {
        {
            "Simple ASCII text (no escapes)",
            "Hello World",
            (const char*)"Hello World",
            11
        },
        {
            "Quotation mark escape",
            "\\\"Hello\\\"",
            (const char*)"\"Hello\"",
            7
        },
        {
            "Backslash escape",
            "path\\\\to\\\\file",
            (const char*)"path\\to\\file",
            12
        },
        {
            "Slash escape",
            "http:\\/\\/example.com",
            (const char*)"http://example.com",
            18
        },
        {
            "Newline escape",
            "Line1\\nLine2",
            (const char*)"Line1\nLine2",
            11
        },
        {
            "Tab escape",
            "Col1\\tCol2",
            (const char*)"Col1\tCol2",
            9
        },
        {
            "Carriage return escape",
            "Line1\\rLine2",
            (const char*)"Line1\rLine2",
            11
        },
        {
            "Backspace escape",
            "AB\\bC",
            (const char*)"AB\bC",
            4
        },
        {
            "Form feed escape",
            "Page1\\fPage2",
            (const char*)"Page1\fPage2",
            11
        },
        {
            "Single Unicode escape - Chinese character '收' (U+6536)",
            "\\u6536",
            (const char*)expected_utf8_6536,
            sizeof(expected_utf8_6536)
        },
        {
            "Single Unicode escape - Chinese character '到' (U+5230)",
            "\\u5230",
            (const char*)expected_utf8_5230,
            sizeof(expected_utf8_5230)
        },
        {
            "Single Unicode escape - Chinese character '那' (U+90A3)",
            "\\u90a3",
            (const char*)expected_utf8_90a3,
            sizeof(expected_utf8_90a3)
        },
        {
            "Single Unicode escape - Chinese character '个' (U+4E2A)",
            "\\u4e2a",
            (const char*)expected_utf8_4e2a,
            sizeof(expected_utf8_4e2a)
        },
        {
            "Multiple Unicode escapes - Chinese phrase '收到那个'",
            "\\u6536\\u5230\\u90a3\\u4e2a",
            (const char*)expected_utf8_phrase1,
            sizeof(expected_utf8_phrase1)
        },
        {
            "Mixed ASCII and Unicode escapes",
            "Text:\\u6536\\u5230",
            (const char*)expected_utf8_mixed1,
            sizeof(expected_utf8_mixed1)
        },
        {
            "Unicode escape with lowercase hex",
            "\\u6536",
            (const char*)expected_utf8_6536,
            sizeof(expected_utf8_6536)
        },
        {
            "Unicode escape with uppercase hex",
            "\\u6536",
            (const char*)expected_utf8_6536,
            sizeof(expected_utf8_6536)
        },
        {
            "Mixed case hex digits in Unicode escape",
            "\\u6536",
            (const char*)expected_utf8_6536,
            sizeof(expected_utf8_6536)
        },
        {
            "Unicode escape for Latin extended character 'é' (U+00E9)",
            "\\u00e9",
            (const char*)expected_utf8_00e9,
            sizeof(expected_utf8_00e9)
        },
        {
            "Unicode escape for ASCII character 'A' (U+0041) - should be single byte",
            "\\u0041",
            (const char*)expected_utf8_0041,
            sizeof(expected_utf8_0041)
        },
        {
            "Unicode escape for character in 2-byte range '¿' (U+00BF)",
            "\\u00bf",
            (const char*)expected_utf8_00bf,
            sizeof(expected_utf8_00bf)
        },
        {
            "Consecutive Unicode escapes without text",
            "\\u4f60\\u597d",
            (const char*)expected_utf8_phrase3,
            sizeof(expected_utf8_phrase3)
        },
        {
            "Real-world ASR response - '收到那个'",
            "\\u6536\\u5230\\u90a3\\u4e2a",
            (const char*)expected_utf8_phrase1,
            sizeof(expected_utf8_phrase1)
        },
        {
            "Real-world ASR response - '你好' (Hello)",
            "\\u4f60\\u597d",
            (const char*)expected_utf8_phrase3,
            sizeof(expected_utf8_phrase3)
        },
        {
            "Real-world ASR response - '谢谢' (Thank you)",
            "\\u8c22\\u8c22",
            (const char*)expected_utf8_phrase4,
            sizeof(expected_utf8_phrase4)
        },
        {
            "Empty string",
            "",
            "",
            0
        },
        {
            "NULL input",
            NULL,
            NULL,
            0
        },
        {
            "Multiple escapes of different types",
            "\\\"Hello\\nWorld\\u4f60\\\"",
            (const char*)expected_utf8_mixed2,
            sizeof(expected_utf8_mixed2)
        },
        {
            "Backslash followed by non-escape character",
            "Test\\xString",
            (const char*)"Test\\xString",
            12
        },
        {
            "Truncated Unicode escape at end of string",
            "\\u653",
            (const char*)"\\u653",
            5
        },
        {
            "Unicode escape in middle of text",
            "Start\\u4e2dEnd",
            (const char*)expected_utf8_mixed4,
            sizeof(expected_utf8_mixed4)
        }
    };

    int total = sizeof(tests) / sizeof(tests[0]);
    int failed = 0;

    for (int i = 0; i < total; i++) {
        failed += run_test(&tests[i]);
    }

    printf("\n=== Test Summary ===\n");
    printf("Total: %d, Passed: %d, Failed: %d\n", total, total - failed, failed);

    return failed > 0 ? 1 : 0;
}
