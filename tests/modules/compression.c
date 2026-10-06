/* Test module for compressed string values. It reads a string through a key
 * handle, so tests can check that a module read does not change the stored
 * value. */

#include "valkeymodule.h"

/* Opens the key with the given mode and replies with its bytes, read with
 * ValkeyModule_StringDMA(). Replies null when the key does not exist. */
static int replyWithString(ValkeyModuleCtx *ctx, ValkeyModuleString *keyname,
                           int mode) {
  ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, keyname, mode);
  if (key == NULL || ValkeyModule_KeyType(key) == VALKEYMODULE_KEYTYPE_EMPTY) {
    if (key)
      ValkeyModule_CloseKey(key);
    return ValkeyModule_ReplyWithNull(ctx);
  }
  if (ValkeyModule_KeyType(key) != VALKEYMODULE_KEYTYPE_STRING) {
    ValkeyModule_CloseKey(key);
    return ValkeyModule_ReplyWithError(ctx, VALKEYMODULE_ERRORMSG_WRONGTYPE);
  }
  size_t len;
  char *data = ValkeyModule_StringDMA(key, &len, VALKEYMODULE_READ);
  ValkeyModule_ReplyWithStringBuffer(ctx, data, len);
  ValkeyModule_CloseKey(key);
  return VALKEYMODULE_OK;
}

/* COMPRESSION.READ key: reads the key through a read handle. */
static int compressionRead(ValkeyModuleCtx *ctx, ValkeyModuleString **argv,
                           int argc) {
  if (argc != 2)
    return ValkeyModule_WrongArity(ctx);
  return replyWithString(ctx, argv[1], VALKEYMODULE_READ);
}

/* COMPRESSION.READWRITE key: reads the key through a write handle. */
static int compressionReadWrite(ValkeyModuleCtx *ctx, ValkeyModuleString **argv,
                                int argc) {
  if (argc != 2)
    return ValkeyModule_WrongArity(ctx);
  return replyWithString(ctx, argv[1], VALKEYMODULE_READ | VALKEYMODULE_WRITE);
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv,
                        int argc) {
  VALKEYMODULE_NOT_USED(argv);
  VALKEYMODULE_NOT_USED(argc);
  if (ValkeyModule_Init(ctx, "compression", 1, VALKEYMODULE_APIVER_1) ==
      VALKEYMODULE_ERR)
    return VALKEYMODULE_ERR;
  if (ValkeyModule_CreateCommand(ctx, "compression.read", compressionRead,
                                 "readonly", 1, 1, 1) == VALKEYMODULE_ERR)
    return VALKEYMODULE_ERR;
  if (ValkeyModule_CreateCommand(ctx, "compression.readwrite",
                                 compressionReadWrite, "write", 1, 1,
                                 1) == VALKEYMODULE_ERR)
    return VALKEYMODULE_ERR;
  return VALKEYMODULE_OK;
}
