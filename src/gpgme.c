/*
 * Copyright(c) 2017 Free Software Foundation, Inc.
 *
 * This file is part of Wget.
 *
 * Wget is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Wget is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Wget.  If not, see <https://www.gnu.org/licenses/>.
 *
 *
 * GPGME Helper Routines
 *
 * Changelog
 * 04.09.2017  Darshit Shah created
 *
 */


#include <config.h>
#include <locale.h>

#include "wget_gpgme.h"
#include "wget_log.h"
#include "wget_main.h"
#include "wget_options.h"
#include "canonicalize.h"
#include <string.h>

#ifdef WITH_GPGME

#include <gpgme.h>

static void _add_valid_sig(wget_gpg_info_t *info)
{
	if (info)
		info->valid_sigs++;

}

static void _add_invalid_sig(wget_gpg_info_t *info)
{
	if (info)
		info->invalid_sigs++;
}

static void _add_bad_sig(wget_gpg_info_t *info)
{
	if (info)
		info->bad_sigs++;
}

static void _add_missing_sig(wget_gpg_info_t *info)
{
	if (info)
		info->missing_sigs++;
}

static void _clear_info(wget_gpg_info_t *info)
{
	if (!info)
		return;

	info->invalid_sigs = 0;
	info->valid_sigs = 0;
	info->bad_sigs = 0;
	info->missing_sigs = 0;
}

static gpgme_protocol_t _proto_for_content_type(const char *type)
{
	if (!wget_strcasecmp_ascii(type, "application/pgp-signature")) {
		return GPGME_PROTOCOL_OpenPGP;
	}
	// ...
	// Possibly more later

	// GPGME does not accept this value in any operation
	return GPGME_PROTOCOL_UNKNOWN;
}

static int _validate_sigs(gpgme_signature_t sig, wget_gpg_info_t *info, const char *sig_filename)
{
	int ret = WGET_E_SUCCESS;

	for (gpgme_signature_t cur = sig; cur; cur = cur->next) {
		if (sig->summary & (GPGME_SIGSUM_VALID | GPGME_SIGSUM_GREEN)) {
			_add_valid_sig(info); // Good!
		} else if (sig->summary & GPGME_SIGSUM_SYS_ERROR) {
			// There was an internal GPGME error
			error_printf(_("GPGME Failure\n"));
			// Short circuit out
			return WGET_E_GPG_VER_ERR;
		} else {
			char *err_msg = NULL;
			if (sig->summary & GPGME_SIGSUM_RED) {
				wget_asprintf(&err_msg, _("Invalid Signature"));
				_add_bad_sig(info);
			} else if (sig->summary & GPGME_SIGSUM_KEY_EXPIRED) {
				wget_asprintf(&err_msg, _("Key %s expired"), cur->fpr);
				_add_invalid_sig(info);
			} else if (sig->summary & GPGME_SIGSUM_SIG_EXPIRED) {
				wget_asprintf(&err_msg, _("Signature expired"));
				_add_invalid_sig(info);
			} else if (sig->summary & GPGME_SIGSUM_KEY_MISSING) {
				wget_asprintf(&err_msg, _("Key %s missing"), cur->fpr);
				_add_missing_sig(info);
			}

			if (sig_filename) {
				error_printf("%s: %s\n", sig_filename, err_msg);
			} else {
				error_printf("%s\n", err_msg);
			}

			if (ret == WGET_E_SUCCESS)
				ret = WGET_E_GPG_VER_FAIL;

			xfree(err_msg);
		}
	}

	return ret;
}

/**
 * Splits the given string into two strings by replacing the last instance of '.' with '\0'.
 * Returns a pointer to the first character of the second string, or NULL if the given
 * string contains no '.'s
 */
static char *_remove_ext(char *str)
{
	char *ext = strrchr(str, '.');

	if (ext)
		*ext++ = '\0';

	return ext;
}


static char *_determine_base_file(const char *real_filename, const char *base_filename)
{
	size_t base_len = strlen(base_filename);
	size_t real_len = strlen(real_filename);

	if (!wget_strncmp(real_filename, base_filename, base_len < real_len ? real_len : base_len)) {
		char *f = wget_strdup(real_filename);
		_remove_ext(f);
		return f;
	}

	char *real_name_cpy = wget_strdup(real_filename);
	char *base_name_cpy = wget_strdup(base_filename);

	char *real_ext = _remove_ext(real_name_cpy);
	char *ans = NULL;

	if (!real_ext) {
		error_printf(_("Invalid signature, signature file must have a sig extension\n"));
		goto done;
	}

	// If the real name minus the extension is the same as the base name, there is a collision string added
	// to the back. Which needs to be added to the file that has been signed so that we compare like vs. like.
	if (!wget_strncmp(real_name_cpy, base_name_cpy, base_len)) {
		// Strip the extension from the base name (this will remove a '.sig' or similar)
		_remove_ext(base_name_cpy);

		// Create and store the corrected file name
		wget_asprintf(&ans, "%s.%s", base_name_cpy, real_ext);
	}

 done:
	xfree(real_name_cpy);
	xfree(base_name_cpy);

	return ans;
}

static int _verify_detached_sig(gpgme_data_t sig_buff, gpgme_data_t data_buf, wget_gpg_info_t *info,
		const char *sig_filename)
{
	gpgme_ctx_t ctx;
	gpgme_error_t e;
	gpgme_verify_result_t verify_result;
	int res;

	_clear_info(info);

	e = gpgme_new(&ctx);
	if (e != GPG_ERR_NO_ERROR) {
		error_printf(_("Failed to init gpgme context\n"));
		return WGET_E_GPG_VER_ERR;
	}


	if (config.gnupg_homedir) {
		char *canon_home = canonicalize_filename_mode(config.gnupg_homedir, CAN_EXISTING);

		if (canon_home) {
			debug_printf("Setting home dir: %s\n", canon_home);

			e = gpgme_ctx_set_engine_info(ctx, GPGME_PROTOCOL_OpenPGP, NULL, canon_home);
			xfree(canon_home);

			if (e != GPG_ERR_NO_ERROR) {
				error_printf(_("Couldn't specify gnupg homedir\n"));
				res = WGET_E_GPG_VER_ERR;
				goto done;
			}
		} else {
			error_printf(_("Couldn't canonicalize %s. (Does the path exist?)\n"), config.gnupg_homedir);
			res = WGET_E_GPG_VER_ERR;
			goto done;
		}
	}

	// For detatched signatures the last argument is supposed to be NULL
	e = gpgme_op_verify(ctx, sig_buff, data_buf, NULL);
	if (e != GPG_ERR_NO_ERROR) {
		error_printf(_("Error during verification\n"));
		res = WGET_E_GPG_VER_ERR;
		goto done;
	}

	verify_result = gpgme_op_verify_result(ctx);
	if (!verify_result) {
		error_printf(_("GPGME verify failed!\n"));
		res = WGET_E_GPG_VER_ERR;
		goto done;
	}

	res = _validate_sigs(verify_result->signatures, info, sig_filename);

 done:
	gpgme_release(ctx);

	return res;

}

static int check_data_init(gpgme_error_t err)
{
	if (err == GPG_ERR_NO_ERROR)
		return 0;

	// Here we will print useful error messages
	return 1;
}

static int _verify_detached_str(const char *sig, const size_t sig_len,
	const char *dat, const size_t dat_len, wget_gpg_info_t *info,
	const char *sig_filename)
{
	gpgme_data_t sig_d, data_d;

	if (check_data_init(gpgme_data_new_from_mem(&sig_d, sig, sig_len, 0))) {
		gpgme_data_release(sig_d);
		return 0;
	}

	if (check_data_init(gpgme_data_new_from_mem(&data_d, dat, dat_len, 0))) {
		gpgme_data_release(sig_d);
		gpgme_data_release(data_d);
		return 0;
	}

	int ret = _verify_detached_sig(sig_d, data_d, info, sig_filename);

	gpgme_data_release(sig_d);
	gpgme_data_release(data_d);

	return ret;
}

#endif // WITH_GPGME

int wget_verify_pgp_sig_buff(wget_buffer_t *sig, wget_buffer_t *data, wget_gpg_info_t *info)
{
#ifdef WITH_GPGME
	return wget_verify_pgp_sig_str(sig->data, sig->length, data->data, data->length, info);
#else
	return WGET_E_GPG_DISABLED;
#endif
}

int wget_verify_pgp_sig_str(const char *sig, const size_t sig_len, const char *data, const size_t data_len, wget_gpg_info_t *info)
{
#ifdef WITH_GPGME
	return _verify_detached_str(sig, sig_len, data, data_len, info, NULL);
#else
	return WGET_E_GPG_DISABLED;
#endif
}

int wget_verify_job(JOB *job, wget_http_response_t *resp, wget_gpg_info_t *info)
{
#ifdef WITH_GPGME
	if (_proto_for_content_type(resp->content_type) != GPGME_PROTOCOL_OpenPGP) {
		// This is not a super future-proof way to do it.
		error_printf(_("Unsupported protocol type for content: %s\n"), resp->content_type);
		return WGET_E_INVALID;
	}

	// The corrected name of the base file, adjusted for any collision extensions
	char *corrected_base_file = _determine_base_file(job->sig_filename, job->local_filename);

	if (!corrected_base_file) {
		error_printf(_("Couldn't correct signature file!\n"));
		return WGET_E_INVALID;
	}

	size_t num_bytes = -1;
	char *file_contents = NULL;
	debug_printf("Verifying %s against sig %s\n", corrected_base_file, job->sig_filename);

	if (! (file_contents = wget_read_file(corrected_base_file, &num_bytes))) {
		error_printf(_("Failed to read file to verify sig: %s\n"), corrected_base_file);
		xfree(corrected_base_file);
		return WGET_E_INVALID;
	}

	xfree(corrected_base_file);

	int res =
		_verify_detached_str(resp->body->data, resp->body->length, file_contents, num_bytes, info, job->sig_filename);

	xfree(file_contents);

	return res;
#else
	return WGET_E_GPG_DISABLED;
#endif
}

void init_gpgme(void) {
#ifdef WITH_GPGME
	setlocale (LC_ALL, "");
	gpgme_check_version (NULL);
#ifndef LC_MESSAGES
	gpgme_set_locale (NULL, LC_CTYPE, setlocale (LC_CTYPE, NULL));
#else
	gpgme_set_locale (NULL, LC_MESSAGES, setlocale (LC_MESSAGES, NULL));
#endif
#endif
}

char *wget_verify_get_base_file(JOB *job)
{
	if (!job->sig_req) {
		return NULL;
	} else {
		return _determine_base_file(job->sig_filename, job->local_filename);
	}
}
