/************************************************************************************************************
* @internal
* @remark     Winbond - Confidential
* @copyright  Copyright (c) 2026 by Winbond. All rights reserved
* @endinternal
*
* @file       pkcs11-demo.c
* @brief      OP-TEE PKCS#11 user application demo for Sparrow Hawk
*
* ### project meta-w77q-pkcs11
*
************************************************************************************************************/

/*-----------------------------------------------------------------------------------------------------------
                                                   INCLUDES
-----------------------------------------------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <pkcs11.h>

/*-----------------------------------------------------------------------------------------------------------
                                                 DEFINITIONS
-----------------------------------------------------------------------------------------------------------*/

#define HEX_PRINT_MAX_BYTES  16u
#define RSA_KEY_BITS         2048u
#define AES_KEY_BYTES        32u

/* ── helpers ─────────────────────────────────────────────────────────────── */

#define CKR_CHECK(fn, rv)                                                       \
    do                                                                          \
    {                                                                           \
        if ((rv) != CKR_OK)                                                    \
        {                                                                       \
            fprintf(stderr, "FAIL  %-40s  rv=0x%08" PRIX32 "\n",               \
                    #fn, (uint32_t)(rv));                                       \
            goto cleanup;                                                       \
        }                                                                       \
        else                                                                    \
        {                                                                       \
            printf("OK    %s\n", #fn);                                         \
        }                                                                       \
    } while (0)

static void print_hex_L(const char *label, const CK_BYTE *buf, CK_ULONG len)
{
    printf("      %-16s [%lu B]  ", label, len);
    for (CK_ULONG i = 0; i < len && i < HEX_PRINT_MAX_BYTES; i++)
        printf("%02x", buf[i]);
    if (len > HEX_PRINT_MAX_BYTES)
        printf("…");
    printf("\n");
}

/* ── RSA-2048  keygen → sign → verify ───────────────────────────────────── */

static int32_t demo_rsa_L(CK_SESSION_HANDLE sess)
{
    printf("\n─── RSA-2048 keygen + sign + verify ───────────────────────\n");
    int32_t ret_i32 = -1;
    CK_RV rv;

    CK_OBJECT_HANDLE hPub = CK_INVALID_HANDLE, hPriv = CK_INVALID_HANDLE;

    CK_MECHANISM     mech_gen  = { CKM_RSA_PKCS_KEY_PAIR_GEN, NULL, 0 };
    CK_ULONG         bits      = RSA_KEY_BITS;
    CK_BYTE          pub_exp[] = { 0x01, 0x00, 0x01 };
    CK_BBOOL         t         = CK_TRUE;
    CK_BBOOL         f         = CK_FALSE;
    CK_OBJECT_CLASS  cls_pub   = CKO_PUBLIC_KEY;
    CK_OBJECT_CLASS  cls_priv  = CKO_PRIVATE_KEY;
    CK_KEY_TYPE      kt        = CKK_RSA;
    CK_UTF8CHAR      label[]   = "demo-rsa";

    CK_ATTRIBUTE pub_tmpl[] = {
        { CKA_CLASS,           &cls_pub,   sizeof(cls_pub)   },
        { CKA_KEY_TYPE,        &kt,        sizeof(kt)        },
        { CKA_TOKEN,           &t,         sizeof(t)         },
        { CKA_VERIFY,          &t,         sizeof(t)         },
        { CKA_MODULUS_BITS,    &bits,      sizeof(bits)      },
        { CKA_PUBLIC_EXPONENT, pub_exp,    sizeof(pub_exp)   },
        { CKA_LABEL,           label,      sizeof(label)-1   },
    };
    CK_ATTRIBUTE priv_tmpl[] = {
        { CKA_CLASS,       &cls_priv,  sizeof(cls_priv)  },
        { CKA_KEY_TYPE,    &kt,        sizeof(kt)        },
        { CKA_TOKEN,       &t,         sizeof(t)         },
        { CKA_SIGN,        &t,         sizeof(t)         },
        { CKA_SENSITIVE,   &t,         sizeof(t)         },
        { CKA_EXTRACTABLE, &f,         sizeof(f)         },
        { CKA_LABEL,       label,      sizeof(label)-1   },
    };

    rv = C_GenerateKeyPair(sess, &mech_gen,
                           pub_tmpl,  7u,
                           priv_tmpl, 7u,
                           &hPub, &hPriv);
    CKR_CHECK(C_GenerateKeyPair(RSA-2048u), rv);

    /* Sign */
    CK_MECHANISM mech_sign = { CKM_SHA256_RSA_PKCS, NULL, 0 };
    const CK_BYTE data[]   = "Hello from PKCS#11 RSA sign!";
    CK_BYTE  sig[512];
    CK_ULONG sig_len = sizeof(sig);

    rv = C_SignInit(sess, &mech_sign, hPriv);
    CKR_CHECK(C_SignInit, rv);
    rv = C_Sign(sess, (CK_BYTE_PTR)data, sizeof(data)-1u, sig, &sig_len);
    CKR_CHECK(C_Sign, rv);
    print_hex_L("RSA signature", sig, sig_len);

    /* Verify */
    rv = C_VerifyInit(sess, &mech_sign, hPub);
    CKR_CHECK(C_VerifyInit, rv);
    rv = C_Verify(sess, (CK_BYTE_PTR)data, sizeof(data)-1u, sig, sig_len);
    CKR_CHECK(C_Verify(RSA), rv);

    ret_i32 = 0;
cleanup:
    if (hPriv != CK_INVALID_HANDLE) C_DestroyObject(sess, hPriv);
    if (hPub  != CK_INVALID_HANDLE) C_DestroyObject(sess, hPub);
    return ret_i32;
}

/* ── EC P-256  keygen → ECDSA sign → verify ─────────────────────────────── */

static int32_t demo_ec_L(CK_SESSION_HANDLE sess)
{
    printf("\n─── EC P-256 keygen + ECDSA sign + verify ──────────────────\n");
    int32_t ret_i32 = -1;
    CK_RV rv;

    CK_OBJECT_HANDLE hPub = CK_INVALID_HANDLE, hPriv = CK_INVALID_HANDLE;

    /* DER encoding of OID 1.2.840.10045.3.1.7 (prime256v1 / P-256) */
    static const CK_BYTE oid_p256[] = {
        0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07
    };

    CK_MECHANISM    mech_gen  = { CKM_EC_KEY_PAIR_GEN, NULL, 0 };
    CK_BBOOL        t         = CK_TRUE;
    CK_BBOOL        f         = CK_FALSE;
    CK_OBJECT_CLASS cls_pub   = CKO_PUBLIC_KEY;
    CK_OBJECT_CLASS cls_priv  = CKO_PRIVATE_KEY;
    CK_KEY_TYPE     kt        = CKK_EC;
    CK_UTF8CHAR     label[]   = "demo-ec";

    CK_ATTRIBUTE pub_tmpl[] = {
        { CKA_CLASS,     &cls_pub,          sizeof(cls_pub)       },
        { CKA_KEY_TYPE,  &kt,               sizeof(kt)            },
        { CKA_TOKEN,     &t,                sizeof(t)             },
        { CKA_VERIFY,    &t,                sizeof(t)             },
        { CKA_EC_PARAMS, (void *)oid_p256,  sizeof(oid_p256)      },
        { CKA_LABEL,     label,             sizeof(label)-1       },
    };
    CK_ATTRIBUTE priv_tmpl[] = {
        { CKA_CLASS,       &cls_priv, sizeof(cls_priv) },
        { CKA_KEY_TYPE,    &kt,       sizeof(kt)       },
        { CKA_TOKEN,       &t,        sizeof(t)        },
        { CKA_SIGN,        &t,        sizeof(t)        },
        { CKA_SENSITIVE,   &t,        sizeof(t)        },
        { CKA_EXTRACTABLE, &f,        sizeof(f)        },
        { CKA_LABEL,       label,     sizeof(label)-1  },
    };

    rv = C_GenerateKeyPair(sess, &mech_gen,
                           pub_tmpl,  6u,
                           priv_tmpl, 7u,
                           &hPub, &hPriv);
    CKR_CHECK(C_GenerateKeyPair(EC P-256u), rv);

    /* ECDSA-SHA256 sign */
    CK_MECHANISM  mech_sign = { CKM_ECDSA_SHA256, NULL, 0 };
    const CK_BYTE data[]    = "Hello from PKCS#11 ECDSA sign!";
    CK_BYTE       sig[128];
    CK_ULONG      sig_len   = sizeof(sig);

    rv = C_SignInit(sess, &mech_sign, hPriv);
    CKR_CHECK(C_SignInit, rv);
    rv = C_Sign(sess, (CK_BYTE_PTR)data, sizeof(data)-1u, sig, &sig_len);
    CKR_CHECK(C_Sign(ECDSA), rv);
    print_hex_L("EC  signature", sig, sig_len);

    rv = C_VerifyInit(sess, &mech_sign, hPub);
    CKR_CHECK(C_VerifyInit, rv);
    rv = C_Verify(sess, (CK_BYTE_PTR)data, sizeof(data)-1u, sig, sig_len);
    CKR_CHECK(C_Verify(ECDSA), rv);

    ret_i32 = 0;
cleanup:
    if (hPriv != CK_INVALID_HANDLE) C_DestroyObject(sess, hPriv);
    if (hPub  != CK_INVALID_HANDLE) C_DestroyObject(sess, hPub);
    return ret_i32;
}

/* ── AES-256  keygen → CBC encrypt → decrypt ────────────────────────────── */

static int32_t demo_aes_L(CK_SESSION_HANDLE sess)
{
    printf("\n─── AES-256 keygen + AES-CBC encrypt + decrypt ──────────────\n");
    int32_t ret_i32 = -1;
    CK_RV rv;

    CK_OBJECT_HANDLE hKey = CK_INVALID_HANDLE;

    CK_MECHANISM    mech_gen  = { CKM_AES_KEY_GEN, NULL, 0 };
    CK_BBOOL        t         = CK_TRUE;
    CK_BBOOL        f         = CK_FALSE;
    CK_OBJECT_CLASS cls       = CKO_SECRET_KEY;
    CK_KEY_TYPE     kt        = CKK_AES;
    CK_ULONG        key_len   = AES_KEY_BYTES;
    CK_UTF8CHAR     label[]   = "demo-aes";

    CK_ATTRIBUTE key_tmpl[] = {
        { CKA_CLASS,       &cls,      sizeof(cls)     },
        { CKA_KEY_TYPE,    &kt,       sizeof(kt)      },
        { CKA_TOKEN,       &t,        sizeof(t)       },
        { CKA_ENCRYPT,     &t,        sizeof(t)       },
        { CKA_DECRYPT,     &t,        sizeof(t)       },
        { CKA_VALUE_LEN,   &key_len,  sizeof(key_len) },
        { CKA_SENSITIVE,   &t,        sizeof(t)       },
        { CKA_EXTRACTABLE, &f,        sizeof(f)       },
        { CKA_LABEL,       label,     sizeof(label)-1 },
    };

    rv = C_GenerateKey(sess, &mech_gen, key_tmpl, 9u, &hKey);
    CKR_CHECK(C_GenerateKey(AES-256u), rv);

    /* AES-CBC: IV = 16 zero bytes; plaintext must be block-aligned (32 B ok) */
    CK_BYTE iv[16] = {0};
    CK_MECHANISM mech_enc = { CKM_AES_CBC, iv, sizeof(iv) };

    const char  plain[]   = "PKCS11 AES-256-CBC  demo message"; /* 32 B */
    CK_BYTE     cipher[64];
    CK_ULONG    cipher_len = sizeof(cipher);

    rv = C_EncryptInit(sess, &mech_enc, hKey);
    CKR_CHECK(C_EncryptInit, rv);
    rv = C_Encrypt(sess, (CK_BYTE_PTR)plain, (CK_ULONG)strlen(plain),
                   cipher, &cipher_len);
    CKR_CHECK(C_Encrypt, rv);
    print_hex_L("ciphertext", cipher, cipher_len);

    /* Decrypt */
    CK_BYTE  recovered[64];
    CK_ULONG rec_len = sizeof(recovered);

    rv = C_DecryptInit(sess, &mech_enc, hKey);
    CKR_CHECK(C_DecryptInit, rv);
    rv = C_Decrypt(sess, cipher, cipher_len, recovered, &rec_len);
    CKR_CHECK(C_Decrypt, rv);

    if (rec_len == strlen(plain) && memcmp(recovered, plain, rec_len) == 0)
        printf("OK    plaintext matches: \"%.*s\"\n", (int)rec_len, recovered);
    else
    {
        fprintf(stderr, "FAIL  AES round-trip mismatch\n");
        goto cleanup;
    }

    ret_i32 = 0;
cleanup:
    if (hKey != CK_INVALID_HANDLE) C_DestroyObject(sess, hKey);
    return ret_i32;
}

/* ── Random number generation ───────────────────────────────────────────── */

static int32_t demo_random_L(CK_SESSION_HANDLE sess)
{
    printf("\n─── Random number generation ───────────────────────────────\n");
    CK_BYTE  rnd[32];
    CK_RV    rv = C_GenerateRandom(sess, rnd, sizeof(rnd));
    if (rv != CKR_OK)
    {
        fprintf(stderr, "FAIL  C_GenerateRandom  rv=0x%08" PRIX32 "\n", (uint32_t)rv);
        return -1u;
    }
    printf("OK    C_GenerateRandom\n");
    print_hex_L("random[32]", rnd, sizeof(rnd));
    return 0u;
}

/* ── main ────────────────────────────────────────────────────────────────── */

static void usage_L(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "  --slot   <n>    PKCS#11 slot index     (default: 0)\n"
        "  --label  <s>    Token label             (default: pkcs11-demo)\n"
        "  --so-pin <s>    Security Officer PIN    (default: 12345678)\n"
        "  --pin    <s>    User PIN                (default: 87654321)\n"
        "  --no-init       Skip token initialisation (token already set up)\n"
        "  --help          Show this help\n", prog);
}

int main(int argc, char **argv)
{
    CK_ULONG slot_index = 0;
    const char *label  = "pkcs11-demo";
    const char *so_pin = "12345678";
    const char *pin    = "87654321";
    int32_t do_init_i32 = 1;

    for (int32_t i = 1; i < argc; i++)
    {
        if      (!strcmp(argv[i], "--slot")    && i+1 < argc) slot_index = atol(argv[++i]);
        else if (!strcmp(argv[i], "--label")   && i+1 < argc) label      = argv[++i];
        else if (!strcmp(argv[i], "--so-pin")  && i+1 < argc) so_pin     = argv[++i];
        else if (!strcmp(argv[i], "--pin")     && i+1 < argc) pin        = argv[++i];
        else if (!strcmp(argv[i], "--no-init"))                do_init_i32    = 0;
        else if (!strcmp(argv[i], "--help"))
        {
            usage_L(argv[0]);
            return 0u;
        }
        else
        {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage_L(argv[0]);
            return 1u;
        }
    }

    printf("════════════════════════════════════════\n");
    printf(" OP-TEE PKCS#11 Demo  —  Sparrow Hawk\n");
    printf("════════════════════════════════════════\n\n");

    CK_RV              rv;
    CK_SESSION_HANDLE  sess = CK_INVALID_HANDLE;
    CK_SLOT_ID        *slots = NULL;
    CK_ULONG           slot_count = 0;
    int32_t            ret_i32 = 1;

    /* 1. Initialise library */
    rv = C_Initialize(NULL);
    CKR_CHECK(C_Initialize, rv);

    /* 2. Print library info */
    CK_INFO info;
    rv = C_GetInfo(&info);
    CKR_CHECK(C_GetInfo, rv);
    printf("      Library: %.32s  v%u.%u\n",
           info.libraryDescription,
           info.libraryVersion.major,
           info.libraryVersion.minor);

    /* 3. Enumerate slots */
    rv = C_GetSlotList(CK_FALSE, NULL, &slot_count);
    CKR_CHECK(C_GetSlotList(count), rv);
    printf("      Found %lu slot(s)\n", slot_count);

    if (slot_index >= slot_count)
    {
        fprintf(stderr, "FAIL  slot index %lu out of range (0..%lu)\n",
                slot_index, slot_count - 1u);
        goto cleanup;
    }

    slots = calloc(slot_count, sizeof(CK_SLOT_ID));
    if (slots == NULL)
    {
        fprintf(stderr, "FAIL  out of memory\n");
        goto cleanup;
    }

    rv = C_GetSlotList(CK_FALSE, slots, &slot_count);
    CKR_CHECK(C_GetSlotList(fill), rv);
    CK_SLOT_ID slot = slots[slot_index];
    printf("      Using slot[%lu] = slot_id %lu\n", slot_index, slot);

    /* 4. Optionally initialise token */
    if (do_init_i32)
    {
        printf("\n─── Token initialisation ───────────────────────────────────\n");
        rv = C_InitToken(slot,
                         (CK_UTF8CHAR_PTR)so_pin, (CK_ULONG)strlen(so_pin),
                         (CK_UTF8CHAR_PTR)label);
        CKR_CHECK(C_InitToken, rv);

        /* Open SO session to set user PIN */
        rv = C_OpenSession(slot, CKF_SERIAL_SESSION | CKF_RW_SESSION,
                           NULL, NULL, &sess);
        CKR_CHECK(C_OpenSession(SO), rv);

        rv = C_Login(sess, CKU_SO, (CK_UTF8CHAR_PTR)so_pin, (CK_ULONG)strlen(so_pin));
        CKR_CHECK(C_Login(SO), rv);

        rv = C_InitPIN(sess, (CK_UTF8CHAR_PTR)pin, (CK_ULONG)strlen(pin));
        CKR_CHECK(C_InitPIN, rv);

        C_Logout(sess);
        C_CloseSession(sess);
        sess = CK_INVALID_HANDLE;
    }

    /* 5. Open RW user session */
    rv = C_OpenSession(slot, CKF_SERIAL_SESSION | CKF_RW_SESSION,
                       NULL, NULL, &sess);
    CKR_CHECK(C_OpenSession(user), rv);

    rv = C_Login(sess, CKU_USER, (CK_UTF8CHAR_PTR)pin, (CK_ULONG)strlen(pin));
    CKR_CHECK(C_Login(USER), rv);

    /* 6. Run demonstrations */
    int32_t rsa_ok_i32  = demo_rsa_L(sess);
    int32_t ec_ok_i32   = demo_ec_L(sess);
    int32_t aes_ok_i32  = demo_aes_L(sess);
    int32_t rnd_ok_i32  = demo_random_L(sess);

    printf("\n════════════════════════════════════════\n");
    printf(" Summary\n");
    printf("════════════════════════════════════════\n");
    printf("  RSA-2048 sign/verify   : %s\n", rsa_ok_i32 == 0 ? "PASS" : "FAIL");
    printf("  EC P-256  sign/verify  : %s\n", ec_ok_i32  == 0 ? "PASS" : "FAIL");
    printf("  AES-256  enc/dec       : %s\n", aes_ok_i32 == 0 ? "PASS" : "FAIL");
    printf("  Random generation      : %s\n", rnd_ok_i32 == 0 ? "PASS" : "FAIL");

    ret_i32 = (rsa_ok_i32 || ec_ok_i32 || aes_ok_i32 || rnd_ok_i32) ? 1u : 0u;

cleanup:
    if (sess != CK_INVALID_HANDLE)
    {
        C_Logout(sess);
        C_CloseSession(sess);
    }
    free(slots);
    C_Finalize(NULL);
    printf("\nExiting with code %" PRId32 "\n", ret_i32);
    return ret_i32;
}
