#include <tcl.h>
#include <tomcrypt.h>
#include "tcltomcrypt.h"

typedef const struct ltc_cipher_descriptor CipherDesc;
typedef (*CipherFunc)(const unsigned char *, unsigned char *, symmetric_key *);
typedef struct CipherState {
    int uid;
    int refCount;
    Tcl_HashTable *hash;
    CipherDesc *desc;
} CipherState;

void
deleteSymKey(CipherDesc *desc, Tcl_HashEntry *entryPtr)
{
    symmetric_key *symKey;
    symKey = (symmetric_key*)Tcl_GetHashValue(entryPtr);
    Tcl_DeleteHashEntry(entryPtr);
    desc->done(symKey);
    Tcl_Free((char*)symKey);
    return;
}

static void
CipherCleanup(ClientData cdata)
{
    CipherState *state;
    Tcl_HashEntry *entryPtr;
    Tcl_HashSearch search;

    state = (CipherState*)cdata;
    if(--state->refCount > 0){
        return;
    }
    while(entryPtr = Tcl_FirstHashEntry(state->hash, &search)){
        fprintf(stderr, "DBG: deleting symkey for %s\n", state->desc->name);
        deleteSymKey(state->desc, entryPtr);
    }
    Tcl_Free((char*)state);
    return;
}

static int
CipherDone(ClientData cdata, Tcl_Interp *interp,
    int objc, Tcl_Obj *const objv[])
{
    CipherState *state;
    Tcl_HashEntry *entryPtr;
    int err;

    if(objc != 2){
        Tcl_WrongNumArgs(interp, 1, objv, "symkey");
        return TCL_ERROR;
    }
    state = (CipherState*)cdata;
    if((entryPtr = Tcl_FindHashEntry(state->hash, Tcl_GetString(objv[1])))){
        deleteSymKey(state->desc, entryPtr);
    }else{
        Tcl_SetStringObj(Tcl_GetObjResult(interp),
            "invalid symkey provided", -1);
        return TCL_ERROR;
    }

    return TCL_OK;
}

static int
cipheraction(CipherState *state, Tcl_Interp *interp,
    int objc, Tcl_Obj *const objv[],
    CipherFunc func)
{
    Tcl_HashEntry *entryPtr;
    symmetric_key *skey;

    Tcl_Obj *bufObj;
    unsigned char *buf;
    int bufLen;
    unsigned char out[MAXBLOCKSIZE];
    Tcl_Obj *result;
    int err;

    if(objc != 3){
        Tcl_WrongNumArgs(interp, 1, objv, "bytes symkey");
        return TCL_ERROR;
    }
    result = Tcl_GetObjResult(interp);

    buf = Tcl_GetByteArrayFromObj(objv[1], &bufLen);
    if(bufLen < state->desc->block_length){
        Tcl_SetStringObj(result, "bytes are shorter than cipher block length", -1);
        return TCL_ERROR;
    }else if(bufLen > state->desc->block_length){
        Tcl_SetStringObj(result, "bytes are longer than cipher block length", -1);
        return TCL_ERROR;
    }

    entryPtr = Tcl_FindHashEntry(state->hash, Tcl_GetString(objv[2]));
    if(entryPtr == NULL){
        Tcl_SetStringObj(Tcl_GetObjResult(interp),
            "invalid symkey provided", -1);
        return TCL_ERROR;
    }
    skey = (symmetric_key*)Tcl_GetHashValue(entryPtr);
    if((err = func(buf, out, skey)) != CRYPT_OK){
        return tomerr(interp, err);
    }
    
    Tcl_SetByteArrayObj(result, out, bufLen);
    return TCL_OK;
}

static int
CipherECBEncrypt(ClientData cdata, Tcl_Interp *interp,
    int objc, Tcl_Obj *const objv[])
{
    CipherState *state;
    state = (CipherState*)cdata;
    return cipheraction(state, interp, objc, objv, state->desc->ecb_encrypt);
}

static int
CipherECBDecrypt(ClientData cdata, Tcl_Interp *interp,
    int objc, Tcl_Obj *const objv[])
{
    CipherState *state;
    state = (CipherState*)cdata;
    return cipheraction(state, interp, objc, objv, state->desc->ecb_decrypt);
}

static int
CipherSetup(ClientData cdata, Tcl_Interp *interp,
    int objc, Tcl_Obj *const objv[])
{
    symmetric_key *symkey;
    unsigned char *keyraw;
    int keylen;
    int rounds;
    int err;

    CipherState *state;
    Tcl_HashEntry *entry;
    Tcl_Obj *result;
    char name[64];
    int new;

    if(objc < 2 || objc > 3){
        Tcl_WrongNumArgs(interp, 1, objv, "key ?rounds");
        return TCL_ERROR;
    }

    if(objc < 3){
        rounds = 0;
    }else if(Tcl_GetIntFromObj(interp, objv[2], &rounds) == TCL_ERROR){
        return TCL_ERROR;
    }

    keyraw = Tcl_GetByteArrayFromObj(objv[1], &keylen);
    state = (CipherState*)cdata;
    symkey = (symmetric_key*)Tcl_Alloc(sizeof(symmetric_key));
    if((err = state->desc->setup(keyraw, keylen, rounds, symkey)) != CRYPT_OK){
        Tcl_Free((char*)symkey);
        return tomerr(interp, err);
    }

    /* Store the tomcrypt symmetric_key inside our internal state. */
    snprintf(name, 64, "%skey%d", state->desc->name, ++state->uid);
    result = Tcl_GetObjResult(interp);
    Tcl_SetStringObj(result, name, -1);
    entry = Tcl_CreateHashEntry(state->hash, name, &new);
    if(!new){
        Tcl_Free((char*)symkey);
        Tcl_SetStringObj(result, "internal error: duplicate key name", -1);
        return TCL_ERROR;
    }
    Tcl_SetHashValue(entry, (ClientData)symkey);

    return TCL_OK;
}

static int
CipherKeysize(ClientData cdata, Tcl_Interp *interp,
    int objc, Tcl_Obj *const objv[])
{
    CipherState *state;
    Tcl_Obj *result;
    int keySize;
    int err;

    if(objc != 2){
        Tcl_WrongNumArgs(interp, 1, objv, "keysize");
        return TCL_ERROR;
    }

    if(Tcl_GetIntFromObj(interp, objv[1], &keySize) == TCL_ERROR){
        return TCL_ERROR;
    }
    state = (CipherState*)cdata;
    if((err = state->desc->keysize(&keySize)) != CRYPT_OK){
        return tomerr(interp, err);
    }

    result = Tcl_GetObjResult(interp);
    Tcl_SetIntObj(result, keySize);
    return TCL_OK;
}

static Tcl_Obj*
descarray(CipherDesc *desc)
{
#define LEN 22
    Tcl_Obj *clist[LEN];
    int i;

    i = 0;
#define STR(X) clist[i++] = Tcl_NewStringObj(X, -1)
#define INT clist[i++] = Tcl_NewIntObj
#define FMT clist[i++] = Tcl_ObjPrintf
#define FUNC(X) STR(X); FMT("::tomcrypt::%s_%s", desc->name, X);
    STR("name");
    STR(desc->name);
    STR("ID");
    INT(desc->ID);    
    STR("min_key_length");
    INT(desc->min_key_length);
    STR("man_key_length");
    INT(desc->max_key_length);
    STR("block_length");
    INT(desc->block_length);
    STR("default_rounds");
    INT(desc->default_rounds);
    FUNC("setup");
    FUNC("ecb_encrypt");
    FUNC("ecb_decrypt");
    FUNC("done");
    FUNC("keysize");
#undef STR
#undef INT
#undef FMT
#undef FUNC
    return Tcl_NewListObj(LEN, clist);
#undef LEN
}

static void
createCipherCmds(Tcl_Interp *interp, CipherDesc *desc, Tcl_HashTable *hash)
{
    CipherState *state;
    char cmd[128];

    state = (CipherState*)Tcl_Alloc(sizeof(CipherState));
    state->uid = 0;
    state->hash = hash;
    state->desc = desc;
    state->refCount = 0;

    snprintf(cmd, 128, "::tomcrypt::%s_setup", desc->name);
    Tcl_CreateObjCommand(interp, cmd, CipherSetup,
        (ClientData)state, CipherCleanup);
    ++state->refCount;

    snprintf(cmd, 128, "::tomcrypt::%s_ecb_encrypt", desc->name);
    Tcl_CreateObjCommand(interp, cmd, CipherECBEncrypt,
        (ClientData)state, CipherCleanup);
    ++state->refCount;

    snprintf(cmd, 128, "::tomcrypt::%s_ecb_decrypt", desc->name);
    Tcl_CreateObjCommand(interp, cmd, CipherECBDecrypt,
        (ClientData)state, CipherCleanup);
    ++state->refCount;

    snprintf(cmd, 128, "::tomcrypt::%s_done", desc->name);
    Tcl_CreateObjCommand(interp, cmd, CipherDone,
        (ClientData)state, CipherCleanup);
    ++state->refCount;

    snprintf(cmd, 128, "::tomcrypt::%s_keysize", desc->name);
    Tcl_CreateObjCommand(interp, cmd, CipherKeysize,
        (ClientData)state, CipherCleanup);
    ++state->refCount;

    return;
}

static int
regCipherTcl(Tcl_Interp *interp, CipherDesc *desc,
    const char *ary, TomcryptState *state)
{
    Tcl_Obj *obj;
    Tcl_HashTable *hashPtr;

    if(register_cipher(desc) == -1){
        Tcl_SetObjResult(interp,
            Tcl_ObjPrintf("failed to register %s cipher", desc->name));
        return TCL_ERROR;
    }

    if(Tcl_SetVar2Ex(interp, ary, desc->name, descarray(desc),
        TCL_LEAVE_ERR_MSG) == NULL){
        return TCL_ERROR;
    }

    hashPtr = TomcryptHashTable(state);
    createCipherCmds(interp, desc, hashPtr);

    return TCL_OK;
}

int
initCiphers(Tcl_Interp *interp, TomcryptState *state)
{
#define RC(C)\
    if(regCipherTcl(interp, & C##_desc, "::tomcrypt::cipher", state) != TCL_OK)\
        return TCL_ERROR;
#ifdef LTC_BLOWFISH
    RC(blowfish);
#endif
#ifdef LTC_XTEA
    RC(xtea);
#endif
#ifdef LTC_RC2
    RC(rc2);
#endif
#ifdef LTC_RC5
    RC(rc5);
#endif
#ifdef LTC_RC6
    RC(rc6);
#endif
#ifdef LTC_SAFERP
    RC(saferp);
#endif
#ifdef LTC_RIJNDAEL
    RC(rijndael);
    RC(aes);
#endif
#ifdef LTC_TWOFISH
    RC(twofish);
#endif
#ifdef LTC_DES
    RC(des);
    RC(des3);
#endif
#ifdef LTC_CAST5
    RC(cast5);
#endif
#ifdef LTC_NOEKEON
    RC(noekeon);
#endif
#ifdef LTC_SKIPJACK
    RC(skipjack);
#endif
#ifdef LTC_ANUBIS
    RC(anubis);
#endif
#ifdef LTC_KHAZAD
    RC(khazad);
#endif
#ifdef LTC_KSEED
    RC(kseed);
#endif
#ifdef LTC_KASUMI
    RC(kasumi);
#endif
#undef RC

    return TCL_OK;
}
