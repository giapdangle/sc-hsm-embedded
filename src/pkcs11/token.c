/**
 * SmartCard-HSM PKCS#11 Module
 *
 * Copyright (c) 2013, CardContact Systems GmbH, Minden, Germany
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of CardContact Systems GmbH nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL CardContact Systems GmbH BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @file    token.c
 * @author  Frank Thater, Andreas Schwier
 * @brief   Functions for token authentication and token management
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <pkcs11/strbpcpy.h>

#include <pkcs11/token.h>
#include <pkcs11/object.h>
#include <pkcs11/dataobject.h>

#include <pkcs11/token-sc-hsm.h>

#ifdef DEBUG
#include <pkcs11/debug.h>
#endif

extern struct p11Context_t *context;

extern struct p11TokenDriver *getSmartCardHSMTokenDriver();
extern struct p11TokenDriver *getBNotKTokenDriver();
extern struct p11TokenDriver *getDTrustTokenDriver();
extern struct p11TokenDriver *getSigntrust32TokenDriver();
extern struct p11TokenDriver *getSigntrust35TokenDriver();
extern struct p11TokenDriver *getDGNTokenDriver();

typedef struct p11TokenDriver *(*tokenDriver_t)();

static tokenDriver_t tokenDriver[] = {
		getSmartCardHSMTokenDriver,
		getBNotKTokenDriver,
		getDTrustTokenDriver,
		getSigntrust32TokenDriver,
		getSigntrust35TokenDriver,
		getDGNTokenDriver,
		NULL
};



/**
 * Add token object to list of public or private objects
 *
 * @param token     The token for which an object shell be added
 * @param object    The object
 * @param publicObject true to add as public object, false to add as private object
 *
 * @return          0 or -1 if error
 */
int addObject(struct p11Token_t *token, struct p11Object_t *object, int publicObject)
{
	object->token = token;

	if (!object->handle) {
		object->handle = token->freeObjectNumber++;
	}

	if (publicObject) {
		addObjectToList(&token->tokenObjList, object);
		token->numberOfTokenObjects++;
	} else {
		addObjectToList(&token->tokenPrivObjList, object);
		token->numberOfPrivateTokenObjects++;
	}

	object->dirtyFlag = 1;

	return CKR_OK;
}



/**
 * Find public or private object in list of token objects
 *
 * @param token     The token whose object shall be removed
 * @param handle    The objects handle
 */
int findObject(struct p11Token_t *token, CK_OBJECT_HANDLE handle, struct p11Object_t **object, int publicObject)
{
	struct p11Object_t *obj;
	int pos = 0;            /* remember the current position in the list */

	if (!publicObject && (token->user != CKU_USER)) {
		return -1;
	}

	obj = publicObject == TRUE ? token->tokenObjList : token->tokenPrivObjList;
	*object = NULL;

	while (obj != NULL) {
		if (obj->handle == handle) {
			*object = obj;
			return pos;
		}

		obj = obj->next;
		pos++;
	}

	return -1;
}



int findMatchingTokenObject(struct p11Token_t *token, CK_ATTRIBUTE_PTR pTemplate, CK_ULONG ulCount, struct p11Object_t **pObject)
{
	struct p11Object_t *p;

	/* public token objects */
	p = token->tokenObjList;

	while (p != NULL) {
		if (isMatchingObject(p, pTemplate, ulCount)) {
			*pObject = p;
			return CKR_OK;
		}
		p = p->next;
	}

	/* private token objects */
	p = token->tokenPrivObjList;

	while (p != NULL) {
		if (isMatchingObject(p, pTemplate, ulCount)) {
			*pObject = p;
			return CKR_OK;
		}
		p = p->next;
	}

	return CKR_ARGUMENTS_BAD;
}



/**
 * Remove object from list of token objects
 *
 * @param token     The token whose object shall be removed
 * @param handle    The objects handle
 * @param publicObject true to remove public object, false to remove private object
 *
 * @return          0 or -1 if error
 */
int removeTokenObject(struct p11Token_t *token, CK_OBJECT_HANDLE handle, int publicObject)
{
	int rc;

	if (publicObject) {
		rc = removeObjectFromList(&token->tokenObjList, handle);
		if (rc != CKR_OK)
			return rc;
		token->numberOfTokenObjects--;
	} else {
		rc = removeObjectFromList(&token->tokenPrivObjList, handle);
		if (rc != CKR_OK)
			return rc;
		token->numberOfPrivateTokenObjects--;
	}

	return CKR_OK;
}



/**
 * Remove all private objects for token from internal list
 *
 * @param token     The token whose objects shall be removed
 *
 * @return          CKR_OK or any other Cryptoki error code
 */
static void removePrivateObjects(struct p11Token_t *token)
{
	removeAllObjectsFromList(&token->tokenPrivObjList);
	token->numberOfPrivateTokenObjects = 0;
}



/**
 * Remove all public objects for token from internal list
 *
 * @param token     The token whose objects shall be removed
 *
 * @return          CKR_OK or any other Cryptoki error code
 */
static void removePublicObjects(struct p11Token_t *token)
{
	removeAllObjectsFromList(&token->tokenObjList);
	token->numberOfTokenObjects = 0;
}



/**
 * Remove object from token but keep attributes as these are transfered into a new object
 */
int removeObjectLeavingAttributes(struct p11Token_t *token, CK_OBJECT_HANDLE handle, int publicObject)
{
	struct p11Object_t *object = NULL;
	struct p11Object_t *prev = NULL;
	int rc;

	rc = findObject(token, handle, &object, publicObject);

	/* no object with this handle found */
	if (rc < 0) {
		return rc;
	}

	if (rc > 0) {      /* there is more than one element in the pool */

		prev = publicObject == TRUE ? token->tokenObjList : token->tokenPrivObjList;

		while (prev->next->handle != handle) {
			prev = prev->next;
		}

		prev->next = object->next;

	}

	free(object);

	token->numberOfTokenObjects--;

	if (rc == 0) {      /* We removed the last element from the list */
		if (publicObject) {
			token->tokenObjList = NULL;
		} else {
			token->tokenPrivObjList = NULL;
		}
	}

	return CKR_OK;
}



/**
 * Remove object from token
 *
 * @param slot      The slot in which the token is inserted
 * @param token     The token to update
 *
 * @return          CKR_OK or any other Cryptoki error code
 */
int destroyObject(struct p11Slot_t *slot, struct p11Token_t *token, struct p11Object_t *object)
{
	return CKR_OK;
}



/**
 * Synchronize a token objects that have been changed (e.g. have the dirty flag set)
 *
 * @param slot      The slot in which the token is inserted
 * @param token     The token to update
 *
 * @return          CKR_OK or any other Cryptoki error code
 */
int synchronizeToken(struct p11Slot_t *slot, struct p11Token_t *token)
{
	return CKR_OK;
}



/**
 * Log into token
 *
 * This token method is called from the C_Login function at the PKCS#11 interface and
 * make all private objects visible at the PKCS#11 interface
 *
 * @param slot      The slot in which the token is inserted
 * @param userType  One of CKU_SO or CKU_USER
 * @param pPin      Pointer to PIN value or NULL is PIN shall be verified using PIN-Pad
 * @param ulPinLen  The length of the PIN supplied in pPin
 *
 * @return          CKR_OK or any other Cryptoki error code
 */
int logIn(struct p11Slot_t *slot, CK_USER_TYPE userType, CK_UTF8CHAR_PTR pPin, CK_ULONG ulPinLen)
{
	return slot->token->drv->login(slot, userType, pPin, ulPinLen);
}



/**
 * Log out from token, removing private objects from the list of visible token objects
 *
 * This token method is called from the C_Logout function at the PKCS#11 interface
 *
 * @param slot      The slot in which the token is inserted
 *
 * @return          CKR_OK or any other Cryptoki error code
 */
int logOut(struct p11Slot_t *slot)
{
	slot->token->user = 0xFF;

	return slot->token->drv->logout(slot);
}



/**
 * Initialize PIN
 *
 * This token method is called from the C_InitPIN function at the PKCS#11 interface
 *
 * @param slot      The slot in which the token is inserted
 * @param pPin      Pointer to PIN value or NULL is PIN shall be verified using PIN-Pad
 * @param ulPinLen  The length of the PIN supplied in pPin
 *
 * @return          CKR_OK or any other Cryptoki error code
 */
int initPIN(struct p11Slot_t *slot, CK_UTF8CHAR_PTR pPin, CK_ULONG ulPinLen)
{
	if (slot->token->drv->initpin == NULL) {
		return CKR_FUNCTION_NOT_SUPPORTED;
	}
	return slot->token->drv->initpin(slot, pPin, ulPinLen);
}



/**
 * Set PIN
 *
 * This token method is called from the C_SetPIN function at the PKCS#11 interface
 *
 * @param slot         The slot in which the token is inserted
 * @param pOldPin      Pointer to old PIN value or NULL is PIN shall be changed using PIN-Pad
 * @param ulOldPinLen  The length of the PIN supplied in pOldPin
 * @param pNewPin      Pointer to new PIN value or NULL is PIN shall be verified using PIN-Pad
 * @param ulNewPinLen  The length of the PIN supplied in pNewPin
 *
 * @return          CKR_OK or any other Cryptoki error code
 */
int setPIN(struct p11Slot_t *slot, CK_UTF8CHAR_PTR pOldPin, CK_ULONG ulOldPinLen, CK_UTF8CHAR_PTR pNewPin, CK_ULONG ulNewPinLen)
{
	if (slot->token->drv->setpin == NULL) {
		return CKR_FUNCTION_NOT_SUPPORTED;
	}
	return slot->token->drv->setpin(slot, pOldPin, ulOldPinLen, pNewPin, ulNewPinLen);
}



/**
 * Detect a newly inserted token in the designated slot
 *
 * @param slot      The slot in which a token was detected
 * @param token     Pointer to pointer updated with newly created token structure
 * @return          CKR_OK or any other Cryptoki error code
 */
int newToken(struct p11Slot_t *slot, unsigned char *atr, size_t atrlen, struct p11Token_t **token)
{
	int rc;
	tokenDriver_t *t;
	struct p11TokenDriver *drv;

	FUNC_CALLED();

	for (t = tokenDriver; *t != NULL; t++) {
		drv = (*t)();
		if (drv->isCandidate(atr, atrlen)) {
			rc = drv->newToken(slot, token);
			if (rc == CKR_OK)
				FUNC_RETURNS(rc);

			if (rc != CKR_TOKEN_NOT_RECOGNIZED)
				FUNC_FAILS(rc, "Token detection failed for recognized token");
		}
	}

	FUNC_RETURNS(CKR_TOKEN_NOT_RECOGNIZED);
}



/**
 * Release memory allocated for token
 *
 * @param slot      The slot in which the token is inserted
 */
void freeToken(struct p11Token_t *token)
{
	if (token) {
		closeSessionsForSlot(&context->sessionPool, token->slot->id);

		if (token->drv->freeToken)
			token->drv->freeToken(token);

		removePrivateObjects(token);
		removePublicObjects(token);
		free(token);
	}
}



/**
 * Return the base token if this token is in a virtual slot
 *
 * @param token     Pointer to token
 * @return          The same or related base token
 */
struct p11Token_t *getBaseToken(struct p11Token_t *token)
{
	if (!token->slot->primarySlot)
		return token;
	return token->slot->primarySlot->token;
}
