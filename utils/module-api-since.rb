# This file maps module API functions to the Redis OSS versions that added them.
# This is needed only for old API functions from the pre-forking time.
$since = {
"Alloc" => "4.0.0",
"TryAlloc" => "7.0.0",
"Calloc" => "4.0.0",
"Realloc" => "4.0.0",
"Free" => "4.0.0",
"Strdup" => "4.0.0",
"PoolAlloc" => "4.0.0",
"IsKeysPositionRequest" => "4.0.0",
"KeyAtPosWithFlags" => "7.0.0",
"KeyAtPos" => "4.0.0",
"IsChannelsPositionRequest" => "7.0.0",
"ChannelAtPosWithFlags" => "7.0.0",
"CreateCommand" => "4.0.0",
"GetCommand" => "7.0.0",
"CreateSubcommand" => "7.0.0",
"SetCommandACLCategories" => "7.2.0",
"SetCommandInfo" => "7.0.0",
"IsModuleNameBusy" => "4.0.3",
"Milliseconds" => "4.0.0",
"MonotonicMicroseconds" => "7.0.0",
"Microseconds" => "7.2.0",
"CachedMicroseconds" => "7.2.0",
"BlockedClientMeasureTimeStart" => "6.2.0",
"BlockedClientMeasureTimeEnd" => "6.2.0",
"Yield" => "7.0.0",
"SetModuleOptions" => "6.0.0",
"SignalModifiedKey" => "6.0.0",
"AutoMemory" => "4.0.0",
"CreateString" => "4.0.0",
"CreateStringPrintf" => "4.0.0",
"CreateStringFromLongLong" => "4.0.0",
"CreateStringFromULongLong" => "7.0.3",
"CreateStringFromDouble" => "6.0.0",
"CreateStringFromLongDouble" => "6.0.0",
"CreateStringFromString" => "4.0.0",
"CreateStringFromStreamID" => "6.2.0",
"FreeString" => "4.0.0",
"RetainString" => "4.0.0",
"HoldString" => "6.0.7",
"StringPtrLen" => "4.0.0",
"StringToLongLong" => "4.0.0",
"StringToULongLong" => "7.0.3",
"StringToDouble" => "4.0.0",
"StringToLongDouble" => "6.0.0",
"StringToStreamID" => "6.2.0",
"StringCompare" => "4.0.0",
"StringAppendBuffer" => "4.0.0",
"TrimStringAllocation" => "7.0.0",
"WrongArity" => "4.0.0",
"ReplyWithLongLong" => "4.0.0",
"ReplyWithError" => "4.0.0",
"ReplyWithErrorFormat" => "7.2.0",
"ReplyWithSimpleString" => "4.0.0",
"ReplyWithArray" => "4.0.0",
"ReplyWithMap" => "7.0.0",
"ReplyWithSet" => "7.0.0",
"ReplyWithAttribute" => "7.0.0",
"ReplyWithNullArray" => "6.0.0",
"ReplyWithEmptyArray" => "6.0.0",
"ReplySetArrayLength" => "4.0.0",
"ReplySetMapLength" => "7.0.0",
"ReplySetSetLength" => "7.0.0",
"ReplySetAttributeLength" => "7.0.0",
"ReplyWithStringBuffer" => "4.0.0",
"ReplyWithCString" => "5.0.6",
"ReplyWithString" => "4.0.0",
"ReplyWithEmptyString" => "6.0.0",
"ReplyWithVerbatimStringType" => "7.0.0",
"ReplyWithVerbatimString" => "6.0.0",
"ReplyWithNull" => "4.0.0",
"ReplyWithBool" => "7.0.0",
"ReplyWithCallReply" => "4.0.0",
"ReplyWithDouble" => "4.0.0",
"ReplyWithBigNumber" => "7.0.0",
"ReplyWithLongDouble" => "6.0.0",
"Replicate" => "4.0.0",
"ReplicateVerbatim" => "4.0.0",
"GetClientId" => "4.0.0",
"GetClientUserNameById" => "6.2.1",
"GetClientInfoById" => "6.0.0",
"GetClientNameById" => "7.0.3",
"SetClientNameById" => "7.0.3",
"PublishMessage" => "6.0.0",
"PublishMessageShard" => "7.0.0",
"GetSelectedDb" => "4.0.0",
"GetContextFlags" => "4.0.3",
"AvoidReplicaTraffic" => "6.0.0",
"SelectDb" => "4.0.0",
"KeyExists" => "7.0.0",
"OpenKey" => "4.0.0",
"GetOpenKeyModesAll" => "7.2.0",
"CloseKey" => "4.0.0",
"KeyType" => "4.0.0",
"ValueLength" => "4.0.0",
"DeleteKey" => "4.0.0",
"UnlinkKey" => "4.0.7",
"GetExpire" => "4.0.0",
"SetExpire" => "4.0.0",
"GetAbsExpire" => "6.2.2",
"SetAbsExpire" => "6.2.2",
"ResetDataset" => "6.0.0",
"DbSize" => "6.0.0",
"RandomKey" => "6.0.0",
"GetKeyNameFromOptCtx" => "7.0.0",
"GetToKeyNameFromOptCtx" => "7.0.0",
"GetDbIdFromOptCtx" => "7.0.0",
"GetToDbIdFromOptCtx" => "7.0.0",
"StringSet" => "4.0.0",
"StringDMA" => "4.0.0",
"StringTruncate" => "4.0.0",
"ListPush" => "4.0.0",
"ListPop" => "4.0.0",
"ListGet" => "7.0.0",
"ListSet" => "7.0.0",
"ListInsert" => "7.0.0",
"ListDelete" => "7.0.0",
"ZsetAdd" => "4.0.0",
"ZsetIncrby" => "4.0.0",
"ZsetRem" => "4.0.0",
"ZsetScore" => "4.0.0",
"ZsetRangeStop" => "4.0.0",
"ZsetRangeEndReached" => "4.0.0",
"ZsetFirstInScoreRange" => "4.0.0",
"ZsetLastInScoreRange" => "4.0.0",
"ZsetFirstInLexRange" => "4.0.0",
"ZsetLastInLexRange" => "4.0.0",
"ZsetRangeCurrentElement" => "4.0.0",
"ZsetRangeNext" => "4.0.0",
"ZsetRangePrev" => "4.0.0",
"HashSet" => "4.0.0",
"HashGet" => "4.0.0",
"StreamAdd" => "6.2.0",
"StreamDelete" => "6.2.0",
"StreamIteratorStart" => "6.2.0",
"StreamIteratorStop" => "6.2.0",
"StreamIteratorNextID" => "6.2.0",
"StreamIteratorNextField" => "6.2.0",
"StreamIteratorDelete" => "6.2.0",
"StreamTrimByLength" => "6.2.0",
"StreamTrimByID" => "6.2.0",
"FreeCallReply" => "4.0.0",
"CallReplyType" => "4.0.0",
"CallReplyLength" => "4.0.0",
"CallReplyArrayElement" => "4.0.0",
"CallReplyInteger" => "4.0.0",
"CallReplyDouble" => "7.0.0",
"CallReplyBigNumber" => "7.0.0",
"CallReplyVerbatim" => "7.0.0",
"CallReplyBool" => "7.0.0",
"CallReplySetElement" => "7.0.0",
"CallReplyMapElement" => "7.0.0",
"CallReplyAttribute" => "7.0.0",
"CallReplyAttributeElement" => "7.0.0",
"CallReplyPromiseSetUnblockHandler" => "7.2.0",
"CallReplyPromiseAbort" => "7.2.0",
"CallReplyStringPtr" => "4.0.0",
"CreateStringFromCallReply" => "4.0.0",
"SetContextUser" => "7.0.6",
"Call" => "4.0.0",
"CallReplyProto" => "4.0.0",
"CreateDataType" => "4.0.0",
"ModuleTypeSetValue" => "4.0.0",
"ModuleTypeGetType" => "4.0.0",
"ModuleTypeGetValue" => "4.0.0",
"IsIOError" => "6.0.0",
"SaveUnsigned" => "4.0.0",
"LoadUnsigned" => "4.0.0",
"SaveSigned" => "4.0.0",
"LoadSigned" => "4.0.0",
"SaveString" => "4.0.0",
"SaveStringBuffer" => "4.0.0",
"LoadString" => "4.0.0",
"LoadStringBuffer" => "4.0.0",
"SaveDouble" => "4.0.0",
"LoadDouble" => "4.0.0",
"SaveFloat" => "4.0.0",
"LoadFloat" => "4.0.0",
"SaveLongDouble" => "6.0.0",
"LoadLongDouble" => "6.0.0",
"DigestAddStringBuffer" => "4.0.0",
"DigestAddLongLong" => "4.0.0",
"DigestEndSequence" => "4.0.0",
"LoadDataTypeFromStringEncver" => "7.0.0",
"LoadDataTypeFromString" => "6.0.0",
"SaveDataTypeToString" => "6.0.0",
"GetKeyNameFromDigest" => "7.0.0",
"GetDbIdFromDigest" => "7.0.0",
"EmitAOF" => "4.0.0",
"GetKeyNameFromIO" => "5.0.5",
"GetKeyNameFromModuleKey" => "6.0.0",
"GetDbIdFromModuleKey" => "7.0.0",
"GetDbIdFromIO" => "7.0.0",
"Log" => "4.0.0",
"LogIOError" => "4.0.0",
"_Assert" => "6.0.0",
"LatencyAddSample" => "6.0.0",
"RegisterAuthCallback" => "7.2.0",
"BlockClient" => "4.0.0",
"BlockClientOnAuth" => "7.2.0",
"BlockClientGetPrivateData" => "7.2.0",
"BlockClientSetPrivateData" => "7.2.0",
"BlockClientOnKeys" => "6.0.0",
"BlockClientOnKeysWithFlags" => "7.2.0",
"SignalKeyAsReady" => "6.0.0",
"UnblockClient" => "4.0.0",
"AbortBlock" => "4.0.0",
"SetDisconnectCallback" => "5.0.0",
"IsBlockedReplyRequest" => "4.0.0",
"IsBlockedTimeoutRequest" => "4.0.0",
"GetBlockedClientPrivateData" => "4.0.0",
"GetBlockedClientReadyKey" => "6.0.0",
"GetBlockedClientHandle" => "5.0.0",
"BlockedClientDisconnected" => "5.0.0",
"GetThreadSafeContext" => "4.0.0",
"GetDetachedThreadSafeContext" => "6.0.9",
"FreeThreadSafeContext" => "4.0.0",
"ThreadSafeContextLock" => "4.0.0",
"ThreadSafeContextTryLock" => "6.0.8",
"ThreadSafeContextUnlock" => "4.0.0",
"SubscribeToKeyspaceEvents" => "4.0.9",
"AddPostNotificationJob" => "7.2.0",
"GetNotifyKeyspaceEvents" => "6.0.0",
"NotifyKeyspaceEvent" => "6.0.0",
"RegisterClusterMessageReceiver" => "5.0.0",
"SendClusterMessage" => "5.0.0",
"GetClusterNodesList" => "5.0.0",
"FreeClusterNodesList" => "5.0.0",
"GetMyClusterID" => "5.0.0",
"GetClusterSize" => "5.0.0",
"GetClusterNodeInfo" => "5.0.0",
"SetClusterFlags" => "5.0.0",
"CreateTimer" => "5.0.0",
"StopTimer" => "5.0.0",
"GetTimerInfo" => "5.0.0",
"EventLoopAdd" => "7.0.0",
"EventLoopDel" => "7.0.0",
"EventLoopAddOneShot" => "7.0.0",
"CreateModuleUser" => "6.0.0",
"FreeModuleUser" => "6.0.0",
"SetModuleUserACL" => "6.0.0",
"SetModuleUserACLString" => "7.0.6",
"GetModuleUserACLString" => "7.0.6",
"GetCurrentUserName" => "7.0.0",
"GetModuleUserFromUserName" => "7.0.0",
"ACLCheckCommandPermissions" => "7.0.0",
"ACLCheckKeyPermissions" => "7.0.0",
"ACLCheckChannelPermissions" => "7.0.0",
"ACLAddLogEntry" => "7.0.0",
"ACLAddLogEntryByUserName" => "7.2.0",
"AuthenticateClientWithUser" => "6.0.0",
"AuthenticateClientWithACLUser" => "6.0.0",
"DeauthenticateAndCloseClient" => "6.0.0",
"RedactClientCommandArgument" => "7.0.0",
"GetClientCertificate" => "6.0.9",
"CreateDict" => "5.0.0",
"FreeDict" => "5.0.0",
"DictSize" => "5.0.0",
"DictSetC" => "5.0.0",
"DictReplaceC" => "5.0.0",
"DictSet" => "5.0.0",
"DictReplace" => "5.0.0",
"DictGetC" => "5.0.0",
"DictGet" => "5.0.0",
"DictDelC" => "5.0.0",
"DictDel" => "5.0.0",
"DictIteratorStartC" => "5.0.0",
"DictIteratorStart" => "5.0.0",
"DictIteratorStop" => "5.0.0",
"DictIteratorReseekC" => "5.0.0",
"DictIteratorReseek" => "5.0.0",
"DictNextC" => "5.0.0",
"DictPrevC" => "5.0.0",
"DictNext" => "5.0.0",
"DictPrev" => "5.0.0",
"DictCompareC" => "5.0.0",
"DictCompare" => "5.0.0",
"InfoAddSection" => "6.0.0",
"InfoBeginDictField" => "6.0.0",
"InfoEndDictField" => "6.0.0",
"InfoAddFieldString" => "6.0.0",
"InfoAddFieldCString" => "6.0.0",
"InfoAddFieldDouble" => "6.0.0",
"InfoAddFieldLongLong" => "6.0.0",
"InfoAddFieldULongLong" => "6.0.0",
"RegisterInfoFunc" => "6.0.0",
"GetServerInfo" => "6.0.0",
"FreeServerInfo" => "6.0.0",
"ServerInfoGetField" => "6.0.0",
"ServerInfoGetFieldC" => "6.0.0",
"ServerInfoGetFieldSigned" => "6.0.0",
"ServerInfoGetFieldUnsigned" => "6.0.0",
"ServerInfoGetFieldDouble" => "6.0.0",
"GetRandomBytes" => "5.0.0",
"GetRandomHexChars" => "5.0.0",
"ExportSharedAPI" => "5.0.4",
"GetSharedAPI" => "5.0.4",
"RegisterCommandFilter" => "5.0.5",
"UnregisterCommandFilter" => "5.0.5",
"CommandFilterArgsCount" => "5.0.5",
"CommandFilterArgGet" => "5.0.5",
"CommandFilterArgInsert" => "5.0.5",
"CommandFilterArgReplace" => "5.0.5",
"CommandFilterArgDelete" => "5.0.5",
"CommandFilterGetClientId" => "7.2.0",
"MallocSize" => "6.0.0",
"MallocUsableSize" => "7.0.1",
"MallocSizeString" => "7.0.0",
"MallocSizeDict" => "7.0.0",
"GetUsedMemoryRatio" => "6.0.0",
"ScanCursorCreate" => "6.0.0",
"ScanCursorRestart" => "6.0.0",
"ScanCursorDestroy" => "6.0.0",
"Scan" => "6.0.0",
"ScanKey" => "6.0.0",
"Fork" => "6.0.0",
"SendChildHeartbeat" => "6.2.0",
"ExitFromChild" => "6.0.0",
"KillForkChild" => "6.0.0",
"SubscribeToServerEvent" => "6.0.0",
"IsSubEventSupported" => "6.0.9",
"RegisterStringConfig" => "7.0.0",
"RegisterBoolConfig" => "7.0.0",
"RegisterEnumConfig" => "7.0.0",
"RegisterNumericConfig" => "7.0.0",
"LoadConfigs" => "7.0.0",
"RdbStreamCreateFromFile" => "7.2.0",
"RdbStreamFree" => "7.2.0",
"RdbLoad" => "7.2.0",
"RdbSave" => "7.2.0",
"SetLRU" => "6.0.0",
"GetLRU" => "6.0.0",
"SetLFU" => "6.0.0",
"GetLFU" => "6.0.0",
"GetModuleOptionsAll" => "7.2.0",
"GetContextFlagsAll" => "6.0.9",
"GetKeyspaceNotificationFlagsAll" => "6.0.9",
"GetServerVersion" => "6.0.9",
"GetTypeMethodVersion" => "6.2.0",
"ModuleTypeReplaceValue" => "6.0.0",
"GetCommandKeysWithFlags" => "7.0.0",
"GetCommandKeys" => "6.0.9",
"GetCurrentCommandName" => "6.2.5",
"RegisterDefragFunc" => "6.2.0",
"DefragShouldStop" => "6.2.0",
"DefragCursorSet" => "6.2.0",
"DefragCursorGet" => "6.2.0",
"DefragAlloc" => "6.2.0",
"DefragRedisModuleString" => "6.2.0",
"GetKeyNameFromDefragCtx" => "7.0.0",
"GetDbIdFromDefragCtx" => "7.0.0",
}