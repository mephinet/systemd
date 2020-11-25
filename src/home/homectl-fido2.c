/* SPDX-License-Identifier: LGPL-2.1-or-later */

#if HAVE_LIBFIDO2
#include <fido.h>
#endif

#include "ask-password-api.h"
#include "errno-util.h"
#include "format-table.h"
#include "hexdecoct.h"
#include "homectl-fido2.h"
#include "homectl-pkcs11.h"
#include "libcrypt-util.h"
#include "libfido2-util.h"
#include "locale-util.h"
#include "memory-util.h"
#include "random-util.h"
#include "strv.h"

#if HAVE_LIBFIDO2
static int add_fido2_credential_id(
                JsonVariant **v,
                const void *cid,
                size_t cid_size) {

        _cleanup_(json_variant_unrefp) JsonVariant *w = NULL;
        _cleanup_strv_free_ char **l = NULL;
        _cleanup_free_ char *escaped = NULL;
        int r;

        assert(v);
        assert(cid);

        r = base64mem(cid, cid_size, &escaped);
        if (r < 0)
                return log_error_errno(r, "Failed to base64 encode FIDO2 credential ID: %m");

        w = json_variant_ref(json_variant_by_key(*v, "fido2HmacCredential"));
        if (w) {
                r = json_variant_strv(w, &l);
                if (r < 0)
                        return log_error_errno(r, "Failed to parse FIDO2 credential ID list: %m");

                if (strv_contains(l, escaped))
                        return 0;
        }

        r = strv_extend(&l, escaped);
        if (r < 0)
                return log_oom();

        w = json_variant_unref(w);
        r = json_variant_new_array_strv(&w, l);
        if (r < 0)
                return log_error_errno(r, "Failed to create FIDO2 credential ID JSON: %m");

        r = json_variant_set_field(v, "fido2HmacCredential", w);
        if (r < 0)
                return log_error_errno(r, "Failed to update FIDO2 credential ID: %m");

        return 0;
}

static int add_fido2_salt(
                JsonVariant **v,
                const void *cid,
                size_t cid_size,
                const void *fido2_salt,
                size_t fido2_salt_size,
                const void *secret,
                size_t secret_size) {

        _cleanup_(json_variant_unrefp) JsonVariant *l = NULL, *w = NULL, *e = NULL;
        _cleanup_(erase_and_freep) char *base64_encoded = NULL, *hashed = NULL;
        int r;

        /* Before using UNIX hashing on the supplied key we base64 encode it, since crypt_r() and friends
         * expect a NUL terminated string, and we use a binary key */
        r = base64mem(secret, secret_size, &base64_encoded);
        if (r < 0)
                return log_error_errno(r, "Failed to base64 encode secret key: %m");

        r = hash_password(base64_encoded, &hashed);
        if (r < 0)
                return log_error_errno(errno_or_else(EINVAL), "Failed to UNIX hash secret key: %m");

        r = json_build(&e, JSON_BUILD_OBJECT(
                                       JSON_BUILD_PAIR("credential", JSON_BUILD_BASE64(cid, cid_size)),
                                       JSON_BUILD_PAIR("salt", JSON_BUILD_BASE64(fido2_salt, fido2_salt_size)),
                                       JSON_BUILD_PAIR("hashedPassword", JSON_BUILD_STRING(hashed))));
        if (r < 0)
                return log_error_errno(r, "Failed to build FIDO2 salt JSON key object: %m");

        w = json_variant_ref(json_variant_by_key(*v, "privileged"));
        l = json_variant_ref(json_variant_by_key(w, "fido2HmacSalt"));

        r = json_variant_append_array(&l, e);
        if (r < 0)
                return log_error_errno(r, "Failed append FIDO2 salt: %m");

        r = json_variant_set_field(&w, "fido2HmacSalt", l);
        if (r < 0)
                return log_error_errno(r, "Failed to set FDO2 salt: %m");

        r = json_variant_set_field(v, "privileged", w);
        if (r < 0)
                return log_error_errno(r, "Failed to update privileged field: %m");

        return 0;
}
#endif

#define FIDO2_SALT_SIZE 32

int identity_add_fido2_parameters(
                JsonVariant **v,
                const char *device) {

#if HAVE_LIBFIDO2
        _cleanup_(fido_cbor_info_free_wrapper) fido_cbor_info_t *di = NULL;
        _cleanup_(fido_assert_free_wrapper) fido_assert_t *a = NULL;
        _cleanup_(fido_cred_free_wrapper) fido_cred_t *c = NULL;
        _cleanup_(fido_dev_free_wrapper) fido_dev_t *d = NULL;
        _cleanup_(erase_and_freep) char *used_pin = NULL;
        _cleanup_(erase_and_freep) void *salt = NULL;
        JsonVariant *un, *realm, *rn;
        bool found_extension = false;
        const void *cid, *secret;
        const char *fido_un;
        size_t n, cid_size, secret_size;
        char **e;
        int r;

        /* Construction is like this: we generate a salt of 32 bytes. We then ask the FIDO2 device to
         * HMAC-SHA256 it for us with its internal key. The result is the key used by LUKS and account
         * authentication. LUKS and UNIX password auth all do their own salting before hashing, so that FIDO2
         * device never sees the volume key.
         *
         * S = HMAC-SHA256(I, D)
         *
         * with: S → LUKS/account authentication key                                         (never stored)
         *       I → internal key on FIDO2 device                              (stored in the FIDO2 device)
         *       D → salt we generate here               (stored in the privileged part of the JSON record)
         *
         */

        assert(v);
        assert(device);

        r = dlopen_libfido2();
        if (r < 0)
                return log_error_errno(r, "FIDO2 token support is not installed.");

        salt = malloc(FIDO2_SALT_SIZE);
        if (!salt)
                return log_oom();

        r = genuine_random_bytes(salt, FIDO2_SALT_SIZE, RANDOM_BLOCK);
        if (r < 0)
                return log_error_errno(r, "Failed to generate salt: %m");

        d = sym_fido_dev_new();
        if (!d)
                return log_oom();

        r = sym_fido_dev_open(d, device);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to open FIDO2 device %s: %s", device, sym_fido_strerr(r));

        if (!sym_fido_dev_is_fido2(d))
                return log_error_errno(SYNTHETIC_ERRNO(ENODEV),
                                       "Specified device %s is not a FIDO2 device.", device);

        di = sym_fido_cbor_info_new();
        if (!di)
                return log_oom();

        r = sym_fido_dev_get_cbor_info(d, di);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to get CBOR device info for %s: %s", device, sym_fido_strerr(r));

        e = sym_fido_cbor_info_extensions_ptr(di);
        n = sym_fido_cbor_info_extensions_len(di);

        for (size_t i = 0; i < n; i++)
                if (streq(e[i], "hmac-secret")) {
                        found_extension = true;
                        break;
                }

        if (!found_extension)
                return log_error_errno(SYNTHETIC_ERRNO(ENODEV),
                                       "Specified device %s is a FIDO2 device, but does not support the required HMAC-SECRET extension.", device);

        c = sym_fido_cred_new();
        if (!c)
                return log_oom();

        r = sym_fido_cred_set_extensions(c, FIDO_EXT_HMAC_SECRET);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to enable HMAC-SECRET extension on FIDO2 credential: %s", sym_fido_strerr(r));

        r = sym_fido_cred_set_rp(c, "io.systemd.home", "Home Directory");
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to set FIDO2 credential relying party ID/name: %s", sym_fido_strerr(r));

        r = sym_fido_cred_set_type(c, COSE_ES256);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to set FIDO2 credential type to ES256: %s", sym_fido_strerr(r));

        un = json_variant_by_key(*v, "userName");
        if (!un)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "userName field of user record is missing");
        if (!json_variant_is_string(un))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "userName field of user record is not a string");

        realm = json_variant_by_key(*v, "realm");
        if (realm) {
                if (!json_variant_is_string(realm))
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "realm field of user record is not a string");

                fido_un = strjoina(json_variant_string(un), json_variant_string(realm));
        } else
                fido_un = json_variant_string(un);

        rn = json_variant_by_key(*v, "realName");
        if (rn && !json_variant_is_string(rn))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "realName field of user record is not a string");

        r = sym_fido_cred_set_user(c,
                               (const unsigned char*) fido_un, strlen(fido_un), /* We pass the user ID and name as the same */
                               fido_un,
                               rn ? json_variant_string(rn) : NULL,
                               NULL /* icon URL */);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to set FIDO2 credential user data: %s", sym_fido_strerr(r));

        r = sym_fido_cred_set_clientdata_hash(c, (const unsigned char[32]) {}, 32);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to set FIDO2 client data hash: %s", sym_fido_strerr(r));

        r = sym_fido_cred_set_rk(c, FIDO_OPT_FALSE);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to turn off FIDO2 resident key option of credential: %s", sym_fido_strerr(r));

        r = sym_fido_cred_set_uv(c, FIDO_OPT_FALSE);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to turn off FIDO2 user verification option of credential: %s", sym_fido_strerr(r));

        log_info("Initializing FIDO2 credential on security token.");

        log_notice("%s%s(Hint: This might require verification of user presence on security token.)",
                   emoji_enabled() ? special_glyph(SPECIAL_GLYPH_TOUCH) : "",
                   emoji_enabled() ? " " : "");

        r = sym_fido_dev_make_cred(d, c, NULL);
        if (r == FIDO_ERR_PIN_REQUIRED) {
                _cleanup_free_ char *text = NULL;

                if (asprintf(&text, "Please enter security token PIN:") < 0)
                        return log_oom();

                for (;;) {
                        _cleanup_(strv_free_erasep) char **pin = NULL;
                        char **i;

                        r = ask_password_auto(text, "user-home", NULL, "fido2-pin", USEC_INFINITY, 0, &pin);
                        if (r < 0)
                                return log_error_errno(r, "Failed to acquire user PIN: %m");

                        r = FIDO_ERR_PIN_INVALID;
                        STRV_FOREACH(i, pin) {
                                if (isempty(*i)) {
                                        log_info("PIN may not be empty.");
                                        continue;
                                }

                                r = sym_fido_dev_make_cred(d, c, *i);
                                if (r == FIDO_OK) {
                                        used_pin = strdup(*i);
                                        if (!used_pin)
                                                return log_oom();
                                        break;
                                }
                                if (r != FIDO_ERR_PIN_INVALID)
                                        break;
                        }

                        if (r != FIDO_ERR_PIN_INVALID)
                                break;

                        log_notice("PIN incorrect, please try again.");
                }
        }
        if (r == FIDO_ERR_PIN_AUTH_BLOCKED)
                return log_notice_errno(SYNTHETIC_ERRNO(EPERM),
                                        "Token PIN is currently blocked, please remove and reinsert token.");
        if (r == FIDO_ERR_ACTION_TIMEOUT)
                return log_error_errno(SYNTHETIC_ERRNO(ENOSTR),
                                       "Token action timeout. (User didn't interact with token quickly enough.)");
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to generate FIDO2 credential: %s", sym_fido_strerr(r));

        cid = sym_fido_cred_id_ptr(c);
        if (!cid)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to get FIDO2 credential ID.");

        cid_size = sym_fido_cred_id_len(c);

        a = sym_fido_assert_new();
        if (!a)
                return log_oom();

        r = sym_fido_assert_set_extensions(a, FIDO_EXT_HMAC_SECRET);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to enable HMAC-SECRET extension on FIDO2 assertion: %s", sym_fido_strerr(r));

        r = sym_fido_assert_set_hmac_salt(a, salt, FIDO2_SALT_SIZE);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to set salt on FIDO2 assertion: %s", sym_fido_strerr(r));

        r = sym_fido_assert_set_rp(a, "io.systemd.home");
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to set FIDO2 assertion ID: %s", sym_fido_strerr(r));

        r = sym_fido_assert_set_clientdata_hash(a, (const unsigned char[32]) {}, 32);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to set FIDO2 assertion client data hash: %s", sym_fido_strerr(r));

        r = sym_fido_assert_allow_cred(a, cid, cid_size);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to add FIDO2 assertion credential ID: %s", sym_fido_strerr(r));

        r = sym_fido_assert_set_up(a, FIDO_OPT_FALSE);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to turn off FIDO2 assertion user presence: %s", sym_fido_strerr(r));

        log_info("Generating secret key on FIDO2 security token.");

        r = sym_fido_dev_get_assert(d, a, used_pin);
        if (r == FIDO_ERR_UP_REQUIRED) {
                r = sym_fido_assert_set_up(a, FIDO_OPT_TRUE);
                if (r != FIDO_OK)
                        return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                               "Failed to turn on FIDO2 assertion user presence: %s", sym_fido_strerr(r));

                log_notice("%s%sIn order to allow secret key generation, please verify presence on security token.",
                           emoji_enabled() ? special_glyph(SPECIAL_GLYPH_TOUCH) : "",
                           emoji_enabled() ? " " : "");

                r = sym_fido_dev_get_assert(d, a, used_pin);
        }
        if (r == FIDO_ERR_ACTION_TIMEOUT)
                return log_error_errno(SYNTHETIC_ERRNO(ENOSTR),
                                       "Token action timeout. (User didn't interact with token quickly enough.)");
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to ask token for assertion: %s", sym_fido_strerr(r));

        secret = sym_fido_assert_hmac_secret_ptr(a, 0);
        if (!secret)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to retrieve HMAC secret.");

        secret_size = sym_fido_assert_hmac_secret_len(a, 0);

        r = add_fido2_credential_id(v, cid, cid_size);
        if (r < 0)
                return r;

        r = add_fido2_salt(v,
                           cid,
                           cid_size,
                           salt,
                           FIDO2_SALT_SIZE,
                           secret,
                           secret_size);
        if (r < 0)
                return r;

        /* If we acquired the PIN also include it in the secret section of the record, so that systemd-homed
         * can use it if it needs to, given that it likely needs to decrypt the key again to pass to LUKS or
         * fscrypt. */
        r = identity_add_token_pin(v, used_pin);
        if (r < 0)
                return r;

        return 0;
#else
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                               "FIDO2 tokens not supported on this build.");
#endif
}
