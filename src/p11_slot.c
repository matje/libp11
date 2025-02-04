/* libp11, a simple layer on to of PKCS#11 API
 * Copyright (C) 2005 Olaf Kirch <okir@lst.de>
 * Copyright (C) 2015-2018 Michał Trojnara <Michal.Trojnara@stunnel.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 */

#include "libp11-int.h"
#include <string.h>
#include <openssl/buffer.h>

static int pkcs11_init_slot(PKCS11_CTX *, PKCS11_SLOT *, CK_SLOT_ID);
static void pkcs11_release_slot(PKCS11_CTX *, PKCS11_SLOT *);
static int pkcs11_check_token(PKCS11_CTX *, PKCS11_SLOT *);
static void pkcs11_destroy_token(PKCS11_TOKEN *);

/*
 * Get slotid from private
 */
unsigned long pkcs11_get_slotid_from_slot(PKCS11_SLOT *slot)
{
	PKCS11_SLOT_private *spriv = PRIVSLOT(slot);

	return spriv->id;
}

/*
 * Enumerate slots
 */
int pkcs11_enumerate_slots(PKCS11_CTX *ctx, PKCS11_SLOT **slotp,
		unsigned int *countp)
{
	PKCS11_CTX_private *cpriv = PRIVCTX(ctx);
	CK_SLOT_ID *slotid;
	CK_ULONG nslots, n;
	PKCS11_SLOT *slots;
	size_t alloc_size;
	int rv;

	rv = cpriv->method->C_GetSlotList(FALSE, NULL_PTR, &nslots);
	CRYPTOKI_checkerr(CKR_F_PKCS11_ENUMERATE_SLOTS, rv);

	alloc_size = nslots * sizeof(CK_SLOT_ID);
	if (alloc_size / sizeof(CK_SLOT_ID) != nslots) /* integer overflow */
		return -1;
	slotid = OPENSSL_malloc(alloc_size);
	if (!slotid)
		return -1;

	rv = cpriv->method->C_GetSlotList(FALSE, slotid, &nslots);
	CRYPTOKI_checkerr(CKR_F_PKCS11_ENUMERATE_SLOTS, rv);

	alloc_size = nslots * sizeof(PKCS11_SLOT);
	if (alloc_size / sizeof(PKCS11_SLOT) != nslots) { /* integer overflow */
		OPENSSL_free(slotid);
		return -1;
	}
	slots = OPENSSL_malloc(alloc_size);
	if (!slots) {
		OPENSSL_free(slotid);
		return -1;
	}

	memset(slots, 0, nslots * sizeof(PKCS11_SLOT));
	for (n = 0; n < nslots; n++) {
		if (pkcs11_init_slot(ctx, &slots[n], slotid[n])) {
			while (n--)
				pkcs11_release_slot(ctx, slots + n);
			OPENSSL_free(slotid);
			OPENSSL_free(slots);
			return -1;
		}
	}

	if (slotp)
		*slotp = slots;
	else
		OPENSSL_free(slots);
	if (countp)
		*countp = nslots;
	OPENSSL_free(slotid);
	return 0;
}

/*
 * Find a slot with a token that looks "valuable"
 */
PKCS11_SLOT *pkcs11_find_token(PKCS11_CTX *ctx, PKCS11_SLOT *slots,
		unsigned int nslots)
{
	PKCS11_SLOT *slot, *best;
	PKCS11_TOKEN *tok;
	unsigned int n;

	(void)ctx;

	if (!slots)
		return NULL;

	best = NULL;
	for (n = 0, slot = slots; n < nslots; n++, slot++) {
		if ((tok = slot->token) != NULL) {
			if (!best ||
					(tok->initialized > best->token->initialized &&
					tok->userPinSet > best->token->userPinSet &&
					tok->loginRequired > best->token->loginRequired))
				best = slot;
		}
	}
	return best;
}

/*
 * Find the next slot with a token that looks "valuable"
 */
PKCS11_SLOT *pkcs11_find_next_token(PKCS11_CTX *ctx, PKCS11_SLOT *slots,
		unsigned int nslots, PKCS11_SLOT *current)
{
	int offset;

	if (!slots)
		return NULL;

	if (current) {
		offset = current + 1 - slots;
		if (offset < 1 || (unsigned int)offset >= nslots)
			return NULL;
	} else {
		offset = 0;
	}

	return pkcs11_find_token(ctx, slots + offset, nslots - offset);
}

/*
 * Open a session with this slot
 */
int pkcs11_open_session(PKCS11_SLOT *slot, int rw)
{
	PKCS11_CTX *ctx = SLOT2CTX(slot);
	PKCS11_SLOT_private *spriv = PRIVSLOT(slot);

	pthread_mutex_lock(&spriv->lock);
	/* If different mode requested, flush pool */
	if (rw != spriv->rw_mode) {
		CRYPTOKI_call(ctx, C_CloseAllSessions(spriv->id));
		spriv->rw_mode = rw;
	}
	spriv->num_sessions = 0;
	spriv->session_head = spriv->session_tail = 0;
	pthread_mutex_unlock(&spriv->lock);

	return 0;
}

int pkcs11_get_session(PKCS11_SLOT * slot, int rw, CK_SESSION_HANDLE *sessionp)
{
	PKCS11_CTX *ctx = SLOT2CTX(slot);
	PKCS11_SLOT_private *spriv = PRIVSLOT(slot);
	int rv = CKR_OK;

	if (rw < 0)
		return -1;

	pthread_mutex_lock(&spriv->lock);
	if (spriv->rw_mode < 0)
		spriv->rw_mode = rw;
	rw = spriv->rw_mode;
	do {
		/* Get session from the pool */
		if (spriv->session_head != spriv->session_tail) {
			*sessionp = spriv->session_pool[spriv->session_head];
			spriv->session_head = (spriv->session_head + 1) % spriv->session_poolsize;
			break;
		}

		/* Check if new can be instantiated */
		if (spriv->num_sessions < spriv->max_sessions) {
			rv = CRYPTOKI_call(ctx,
				C_OpenSession(spriv->id,
					CKF_SERIAL_SESSION | (rw ? CKF_RW_SESSION : 0),
					NULL, NULL, sessionp));
			if (rv == CKR_OK) {
				spriv->num_sessions++;
				break;
			}

			/* Remember the maximum session count */
			if (rv == CKR_SESSION_COUNT)
				spriv->max_sessions = spriv->num_sessions;
		}

		/* Wait for a session to become available */
		pthread_cond_wait(&spriv->cond, &spriv->lock);
	} while (1);
	pthread_mutex_unlock(&spriv->lock);

	return 0;
}

void pkcs11_put_session(PKCS11_SLOT * slot, CK_SESSION_HANDLE session)
{
	PKCS11_SLOT_private *spriv = PRIVSLOT(slot);

	pthread_mutex_lock(&spriv->lock);

	spriv->session_pool[spriv->session_tail] = session;
	spriv->session_tail = (spriv->session_tail + 1) % spriv->session_poolsize;
	pthread_cond_signal(&spriv->cond);

	pthread_mutex_unlock(&spriv->lock);
}

/*
 * Determines if user is authenticated with token
 */
int pkcs11_is_logged_in(PKCS11_SLOT *slot, int so, int *res)
{
	PKCS11_SLOT_private *spriv = PRIVSLOT(slot);

	*res = spriv->logged_in == so;
	return 0;
}

/*
 * Authenticate with the card.
 */
int pkcs11_login(PKCS11_SLOT *slot, int so, const char *pin)
{
	PKCS11_CTX *ctx = SLOT2CTX(slot);
	PKCS11_SLOT_private *spriv = PRIVSLOT(slot);
	CK_SESSION_HANDLE session;
	int rv;

	if (spriv->logged_in >= 0)
		return 0; /* Nothing to do */

	/* SO needs a r/w session, user can be checked with a r/o session. */
	if (pkcs11_get_session(slot, so, &session))
		return -1;

	rv = CRYPTOKI_call(ctx,
		C_Login(session, so ? CKU_SO : CKU_USER,
			(CK_UTF8CHAR *) pin, pin ? (unsigned long) strlen(pin) : 0));
	pkcs11_put_session(slot, session);

	if (rv && rv != CKR_USER_ALREADY_LOGGED_IN) { /* logged in -> OK */
		CRYPTOKI_checkerr(CKR_F_PKCS11_LOGIN, rv);
	}
	if (spriv->prev_pin != pin) {
		if (spriv->prev_pin) {
			OPENSSL_cleanse(spriv->prev_pin, strlen(spriv->prev_pin));
			OPENSSL_free(spriv->prev_pin);
		}
		spriv->prev_pin = OPENSSL_strdup(pin);
	}
	spriv->logged_in = so;
	return 0;
}

/*
 * Reopens the slot by creating a session and logging in if needed.
 */
int pkcs11_reload_slot(PKCS11_SLOT *slot)
{
	PKCS11_SLOT_private *spriv = PRIVSLOT(slot);
	int logged_in = spriv->logged_in;

	spriv->num_sessions = 0;
	spriv->session_head = spriv->session_tail = 0;
	if (logged_in >= 0) {
		spriv->logged_in = -1;
		if (pkcs11_login(slot, logged_in, spriv->prev_pin))
			return -1;
	}

	return 0;
}

/*
 * Log out
 */
int pkcs11_logout(PKCS11_SLOT *slot)
{
	PKCS11_CTX *ctx = SLOT2CTX(slot);
	PKCS11_SLOT_private *spriv = PRIVSLOT(slot);
	CK_SESSION_HANDLE session;
	int rv = CKR_OK;

	/* Calling PKCS11_logout invalidates all cached
	 * keys we have */
	if (slot->token) {
		pkcs11_destroy_keys(slot->token, CKO_PRIVATE_KEY);
		pkcs11_destroy_keys(slot->token, CKO_PUBLIC_KEY);
		pkcs11_destroy_certs(slot->token);
	}

	if (pkcs11_get_session(slot, spriv->logged_in, &session) == 0) {
		rv = CRYPTOKI_call(ctx, C_Logout(session));
		pkcs11_put_session(slot, session);
	}
	CRYPTOKI_checkerr(CKR_F_PKCS11_LOGOUT, rv);
	spriv->logged_in = -1;
	return 0;
}

/*
 * Initialize the token
 */
int pkcs11_init_token(PKCS11_TOKEN *token, const char *pin, const char *label)
{
	PKCS11_SLOT *slot = TOKEN2SLOT(token);
	PKCS11_CTX *ctx = SLOT2CTX(slot);
	PKCS11_SLOT_private *spriv = PRIVSLOT(slot);
	int rv;

	if (!label)
		label = "PKCS#11 Token";
	rv = CRYPTOKI_call(ctx,
		C_InitToken(spriv->id,
			(CK_UTF8CHAR *) pin, (unsigned long) strlen(pin),
			(CK_UTF8CHAR *) label));
	CRYPTOKI_checkerr(CKR_F_PKCS11_INIT_TOKEN, rv);

	/* FIXME: how to update the token?
	 * PKCS11_CTX_private *cpriv;
	 * int n;
	 * cpriv = PRIVCTX(ctx);
	 * for (n = 0; n < cpriv->nslots; n++) {
	 * 	if (pkcs11_check_token(ctx, cpriv->slots + n) < 0)
	 * 		return -1;
	 * }
	 */

	return 0;
}

/*
 * Set the User PIN
 */
int pkcs11_init_pin(PKCS11_TOKEN *token, const char *pin)
{
	PKCS11_SLOT *slot = TOKEN2SLOT(token);
	PKCS11_CTX *ctx = SLOT2CTX(slot);
	CK_OBJECT_HANDLE session;
	int len, rv;

	if (pkcs11_get_session(slot, 1, &session)) {
		P11err(P11_F_PKCS11_INIT_PIN, P11_R_NO_SESSION);
		return -1;
	}

	len = pin ? (int) strlen(pin) : 0;
	rv = CRYPTOKI_call(ctx, C_InitPIN(session, (CK_UTF8CHAR *) pin, len));
	pkcs11_put_session(slot, session);
	CRYPTOKI_checkerr(CKR_F_PKCS11_INIT_PIN, rv);

	return pkcs11_check_token(ctx, TOKEN2SLOT(token));
}

/*
 * Change the User PIN
 */
int pkcs11_change_pin(PKCS11_SLOT *slot, const char *old_pin,
		const char *new_pin)
{
	PKCS11_CTX *ctx = SLOT2CTX(slot);
	CK_SESSION_HANDLE session;
	int old_len, new_len, rv;

	if (pkcs11_get_session(slot, 1, &session)) {
		P11err(P11_F_PKCS11_CHANGE_PIN, P11_R_NO_SESSION);
		return -1;
	}

	old_len = old_pin ? (int) strlen(old_pin) : 0;
	new_len = new_pin ? (int) strlen(new_pin) : 0;
	rv = CRYPTOKI_call(ctx,
		C_SetPIN(session, (CK_UTF8CHAR *) old_pin, old_len,
			(CK_UTF8CHAR *) new_pin, new_len));
	pkcs11_put_session(slot, session);
	CRYPTOKI_checkerr(CKR_F_PKCS11_CHANGE_PIN, rv);

	return pkcs11_check_token(ctx, slot);
}

/*
 * Seed the random number generator
 */
int pkcs11_seed_random(PKCS11_SLOT *slot, const unsigned char *s,
		unsigned int s_len)
{
	PKCS11_CTX *ctx = SLOT2CTX(slot);
	CK_SESSION_HANDLE session;
	int rv;

	if (pkcs11_get_session(slot, 0, &session)) {
		P11err(P11_F_PKCS11_SEED_RANDOM, P11_R_NO_SESSION);
		return -1;
	}

	rv = CRYPTOKI_call(ctx,
		C_SeedRandom(session, (CK_BYTE_PTR) s, s_len));
	pkcs11_put_session(slot, session);
	CRYPTOKI_checkerr(CKR_F_PKCS11_SEED_RANDOM, rv);

	return pkcs11_check_token(ctx, slot);
}

/*
 * Generate random numbers
 */
int pkcs11_generate_random(PKCS11_SLOT *slot, unsigned char *r,
		unsigned int r_len)
{
	PKCS11_CTX *ctx = SLOT2CTX(slot);
	CK_SESSION_HANDLE session;
	int rv;

	if (pkcs11_get_session(slot, 0, &session)) {
		P11err(P11_F_PKCS11_GENERATE_RANDOM, P11_R_NO_SESSION);
		return -1;
	}

	rv = CRYPTOKI_call(ctx,
		C_GenerateRandom(session, (CK_BYTE_PTR) r, r_len));
	pkcs11_put_session(slot, session);

	CRYPTOKI_checkerr(CKR_F_PKCS11_GENERATE_RANDOM, rv);

	return pkcs11_check_token(ctx, slot);
}

/*
 * Helper functions
 */
static int pkcs11_init_slot(PKCS11_CTX *ctx, PKCS11_SLOT *slot, CK_SLOT_ID id)
{
	PKCS11_SLOT_private *spriv;
	CK_SLOT_INFO info;
	int rv;

	rv = CRYPTOKI_call(ctx, C_GetSlotInfo(id, &info));
	CRYPTOKI_checkerr(CKR_F_PKCS11_INIT_SLOT, rv);

	spriv = OPENSSL_malloc(sizeof(PKCS11_SLOT_private));
	if (!spriv)
		return -1;
	memset(spriv, 0, sizeof(PKCS11_SLOT_private));

	spriv->parent = ctx;
	spriv->id = id;
	spriv->forkid = PRIVCTX(ctx)->forkid;
	spriv->prev_pin = NULL;
	spriv->logged_in = -1;
	spriv->rw_mode = -1;
	spriv->max_sessions = 16;
	spriv->session_poolsize = spriv->max_sessions + 1;
	spriv->session_pool = OPENSSL_malloc(spriv->session_poolsize * sizeof(CK_SESSION_HANDLE));
	pthread_mutex_init(&spriv->lock, 0);
	pthread_cond_init(&spriv->cond, 0);

	slot->description = PKCS11_DUP(info.slotDescription);
	slot->manufacturer = PKCS11_DUP(info.manufacturerID);
	slot->removable = (info.flags & CKF_REMOVABLE_DEVICE) ? 1 : 0;
	slot->_private = spriv;

	if ((info.flags & CKF_TOKEN_PRESENT) && pkcs11_check_token(ctx, slot)) {
		if (spriv) {
			if (spriv->prev_pin) {
				OPENSSL_cleanse(spriv->prev_pin, strlen(spriv->prev_pin));
				OPENSSL_free(spriv->prev_pin);
			}
			CRYPTOKI_call(ctx, C_CloseAllSessions(spriv->id));
			OPENSSL_free(spriv->session_pool);
			pthread_mutex_destroy(&spriv->lock);
			pthread_cond_destroy(&spriv->cond);
		}
		return -1;
	}
	return 0;
}

void pkcs11_release_all_slots(PKCS11_CTX *ctx,  PKCS11_SLOT *slots,
		unsigned int nslots)
{
	unsigned int i;

	for (i=0; i < nslots; i++)
		pkcs11_release_slot(ctx, &slots[i]);
	OPENSSL_free(slots);
}

static void pkcs11_release_slot(PKCS11_CTX *ctx, PKCS11_SLOT *slot)
{
	PKCS11_SLOT_private *spriv = PRIVSLOT(slot);

	if (spriv) {
		if (spriv->prev_pin) {
			OPENSSL_cleanse(spriv->prev_pin, strlen(spriv->prev_pin));
			OPENSSL_free(spriv->prev_pin);
		}
		CRYPTOKI_call(ctx, C_CloseAllSessions(spriv->id));
		OPENSSL_free(spriv->session_pool);
		pthread_mutex_destroy(&spriv->lock);
		pthread_cond_destroy(&spriv->cond);
	}
	OPENSSL_free(slot->_private);
	OPENSSL_free(slot->description);
	OPENSSL_free(slot->manufacturer);
	if (slot->token) {
		pkcs11_destroy_token(slot->token);
		OPENSSL_free(slot->token);
	}

	memset(slot, 0, sizeof(*slot));
}

static int pkcs11_check_token(PKCS11_CTX *ctx, PKCS11_SLOT *slot)
{
	PKCS11_SLOT_private *spriv = PRIVSLOT(slot);
	PKCS11_TOKEN_private *tpriv;
	CK_TOKEN_INFO info;
	int rv;

	if (slot->token) {
		pkcs11_destroy_token(slot->token);
	} else {
		slot->token = OPENSSL_malloc(sizeof(PKCS11_TOKEN));
		if (!slot->token)
			return -1;
		memset(slot->token, 0, sizeof(PKCS11_TOKEN));
	}

	rv = CRYPTOKI_call(ctx, C_GetTokenInfo(spriv->id, &info));
	if (rv == CKR_TOKEN_NOT_PRESENT || rv == CKR_TOKEN_NOT_RECOGNIZED) {
		OPENSSL_free(slot->token);
		slot->token = NULL;
		return 0;
	}
	CRYPTOKI_checkerr(CKR_F_PKCS11_CHECK_TOKEN, rv);

	/* We have a token */
	tpriv = OPENSSL_malloc(sizeof(PKCS11_TOKEN_private));
	if (!tpriv)
		return -1;
	memset(tpriv, 0, sizeof(PKCS11_TOKEN_private));
	tpriv->parent = slot;
	tpriv->prv.keys = NULL;
	tpriv->prv.num = 0;
	tpriv->pub.keys = NULL;
	tpriv->pub.num = 0;
	tpriv->ncerts = 0;

	slot->token->label = PKCS11_DUP(info.label);
	slot->token->manufacturer = PKCS11_DUP(info.manufacturerID);
	slot->token->model = PKCS11_DUP(info.model);
	slot->token->serialnr = PKCS11_DUP(info.serialNumber);
	slot->token->initialized = (info.flags & CKF_TOKEN_INITIALIZED) ? 1 : 0;
	slot->token->loginRequired = (info.flags & CKF_LOGIN_REQUIRED) ? 1 : 0;
	slot->token->secureLogin = (info.flags & CKF_PROTECTED_AUTHENTICATION_PATH) ? 1 : 0;
	slot->token->userPinSet = (info.flags & CKF_USER_PIN_INITIALIZED) ? 1 : 0;
	slot->token->readOnly = (info.flags & CKF_WRITE_PROTECTED) ? 1 : 0;
	slot->token->hasRng = (info.flags & CKF_RNG) ? 1 : 0;
	slot->token->userPinCountLow = (info.flags & CKF_USER_PIN_COUNT_LOW) ? 1 : 0;
	slot->token->userPinFinalTry = (info.flags & CKF_USER_PIN_FINAL_TRY) ? 1 : 0;
	slot->token->userPinLocked = (info.flags & CKF_USER_PIN_LOCKED) ? 1 : 0;
	slot->token->userPinToBeChanged = (info.flags & CKF_USER_PIN_TO_BE_CHANGED) ? 1 : 0;
	slot->token->soPinCountLow = (info.flags & CKF_SO_PIN_COUNT_LOW) ? 1 : 0;
	slot->token->soPinFinalTry = (info.flags & CKF_SO_PIN_FINAL_TRY) ? 1 : 0;
	slot->token->soPinLocked = (info.flags & CKF_SO_PIN_LOCKED) ? 1 : 0;
	slot->token->soPinToBeChanged = (info.flags & CKF_SO_PIN_TO_BE_CHANGED) ? 1 : 0;
	slot->token->_private = tpriv;

	return 0;
}

static void pkcs11_destroy_token(PKCS11_TOKEN *token)
{
	pkcs11_destroy_keys(token, CKO_PRIVATE_KEY);
	pkcs11_destroy_keys(token, CKO_PUBLIC_KEY);
	pkcs11_destroy_certs(token);

	OPENSSL_free(token->label);
	OPENSSL_free(token->manufacturer);
	OPENSSL_free(token->model);
	OPENSSL_free(token->serialnr);
	OPENSSL_free(token->_private);
	memset(token, 0, sizeof(*token));
}

/* vim: set noexpandtab: */
