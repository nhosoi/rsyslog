/* mmjsonparse.c
 * This is a message modification module. If give, it extracts JSON data
 * and populates the EE event structure with it.
 *
 * NOTE: read comments in module-template.h for details on the calling interface!
 *
 * File begun on 2012-02-20 by RGerhards
 *
 * Copyright 2012-2018 Adiscon GmbH.
 *
 * This file is part of rsyslog.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *       http://www.apache.org/licenses/LICENSE-2.0
 *       -or-
 *       see COPYING.ASL20 in the source distribution
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"
#include "rsyslog.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <json.h>
#include "conf.h"
#include "syslogd-types.h"
#include "template.h"
#include "module-template.h"
#include "msg.h"
#include "errmsg.h"
#include "cfsysline.h"
#include "parserif.h"
#include "dirty.h"

MODULE_TYPE_OUTPUT
MODULE_TYPE_NOKEEP
MODULE_CNFNAME("mmjsonparse")

static rsRetVal resetConfigVariables(uchar __attribute__((unused)) *pp, void __attribute__((unused)) *pVal);

/* static data */
DEFobjCurrIf(errmsg);

/* internal structures
 */
DEF_OMOD_STATIC_DATA

typedef struct _instanceData {
	sbool bUseRawMsg;     /**< use %rawmsg% instead of %msg% */
	char *cookie;
	uchar *container;
	int lenCookie;
	sbool bCompact;        /* Skip object if the value is null */
	char *messageField;    /* Specify field name which value is escaped json string */
	char *altMessageField; /* Specify alt field name the escaped json string value to be held with */
	msgPropDescr_t *varDescr;/* name of variable to use */
	/* REMOVE dummy when real data items are to be added! */
} instanceData;

typedef struct wrkrInstanceData {
	instanceData *pData;
	struct json_tokener *tokener;
} wrkrInstanceData_t;

struct modConfData_s {
	rsconf_t *pConf;	/* our overall config object */
};
static modConfData_t *loadModConf = NULL;/* modConf ptr to use for the current load process */
static modConfData_t *runModConf = NULL;/* modConf ptr to use for the current exec process */

/* tables for interfacing with the v6 config system */
/* action (instance) parameters */
static struct cnfparamdescr actpdescr[] = {
	{ "cookie", eCmdHdlrString, 0 },
	{ "container", eCmdHdlrString, 0 },
	{ "userawmsg", eCmdHdlrBinary, 0 },
	{ "compact", eCmdHdlrBinary, 0 },
	{ "message_field", eCmdHdlrString, 0 },
	{ "alt_message_field", eCmdHdlrString, 0 },
	{ "variable", eCmdHdlrString, 0 }
};
static struct cnfparamblk actpblk =
	{ CNFPARAMBLK_VERSION,
	  sizeof(actpdescr)/sizeof(struct cnfparamdescr),
	  actpdescr
	};



BEGINbeginCnfLoad
CODESTARTbeginCnfLoad
	loadModConf = pModConf;
	pModConf->pConf = pConf;
ENDbeginCnfLoad

BEGINendCnfLoad
CODESTARTendCnfLoad
ENDendCnfLoad

BEGINcheckCnf
CODESTARTcheckCnf
ENDcheckCnf

BEGINactivateCnf
CODESTARTactivateCnf
	runModConf = pModConf;
ENDactivateCnf

BEGINfreeCnf
CODESTARTfreeCnf
ENDfreeCnf


BEGINcreateInstance
CODESTARTcreateInstance
	CHKmalloc(pData->container = (uchar*)strdup("!"));
	CHKmalloc(pData->cookie = strdup(CONST_CEE_COOKIE));
	pData->lenCookie = CONST_LEN_CEE_COOKIE;
	pData->messageField = NULL;
	pData->altMessageField = NULL;
finalize_it:
ENDcreateInstance

BEGINcreateWrkrInstance
CODESTARTcreateWrkrInstance
	pWrkrData->tokener = json_tokener_new();
	if(pWrkrData->tokener == NULL) {
		errmsg.LogError(0, RS_RET_ERR, "error: could not create json "
				"tokener, cannot activate instance");
		ABORT_FINALIZE(RS_RET_ERR);
	}
finalize_it:
ENDcreateWrkrInstance


BEGINisCompatibleWithFeature
CODESTARTisCompatibleWithFeature
ENDisCompatibleWithFeature


BEGINfreeInstance
CODESTARTfreeInstance
	free(pData->cookie);
	free(pData->container);
	free(pData->messageField);
	free(pData->altMessageField);
	msgPropDescrDestruct(pData->varDescr);
	free(pData->varDescr);
ENDfreeInstance

BEGINfreeWrkrInstance
CODESTARTfreeWrkrInstance
	if(pWrkrData->tokener != NULL)
		json_tokener_free(pWrkrData->tokener);
ENDfreeWrkrInstance


BEGINdbgPrintInstInfo
CODESTARTdbgPrintInstInfo
	DBGPRINTF("mmjsonparse\n");
	DBGPRINTF("\tcookie='%s'\n", pData->cookie);
	DBGPRINTF("\tcontainer='%s'\n", pData->container);
	DBGPRINTF("\tbUseRawMsg='%d'\n", pData->bUseRawMsg);
	DBGPRINTF("\tmessageField='%s'\n", pData->messageField);
	DBGPRINTF("\taltMessageField='%s'\n", pData->altMessageField);
	DBGPRINTF("\tbCompact='%d'\n", pData->bCompact);
	DBGPRINTF("\tvariable='%s'\n", pData->varDescr->name);
ENDdbgPrintInstInfo


BEGINtryResume
CODESTARTtryResume
ENDtryResume


static rsRetVal
processJSON(wrkrInstanceData_t *pWrkrData, smsg_t *pMsg, char *buf, size_t lenBuf)
{
	struct json_object *json;
	const char *errMsg;
	char *ostrcopy = NULL;
	DEFiRet;

	assert(pWrkrData->tokener != NULL);
	DBGPRINTF("mmjsonparse: toParse: '%s'\n", buf);
	json_tokener_reset(pWrkrData->tokener);

	json = json_tokener_parse_ex(pWrkrData->tokener, buf, lenBuf);
	if(Debug) {
		errMsg = NULL;
		if(json == NULL) {
			enum json_tokener_error err;

			err = pWrkrData->tokener->err;
			if(err != json_tokener_continue)
				errMsg = json_tokener_error_desc(err);
			else
				errMsg = "Unterminated input";
		} else if((size_t)pWrkrData->tokener->char_offset < lenBuf)
			errMsg = "Extra characters after JSON object";
		else if(!json_object_is_type(json, json_type_object))
			errMsg = "JSON value is not an object";
		if(errMsg != NULL) {
			DBGPRINTF("mmjsonparse: Error parsing JSON '%s': %s\n",
					buf, errMsg);
		}
	}
	if(json == NULL
	   || ((size_t)pWrkrData->tokener->char_offset < lenBuf)
	   || (!json_object_is_type(json, json_type_object))) {
		if(json != NULL) {
			/* Release json object as we are not going to add it to pMsg */
			json_object_put(json);
		}
		ABORT_FINALIZE(RS_RET_NO_CEE_MSG);
	}
	/*
	 * mmjsonparse extension
	 *
	 * The following code allows action(type="mmjsonparse") to have parameters
	 *   message_field, alt_message_field and compact.
	 *
	 * [example config]
	 *  action(type="mmjsonparse" cookie="" message_field="log" container="$!parsed" 
	 *         alt_message_field="original_raw_json" compact="on")
	 *  With this configuration, 
	 *  1) if the json object contains key:value pair where key is message_field value
	 *     and value is a string type:
	 *     "log":"{\"message\":\"Test message\"}", 
	 *     "message":"Test message" is going to be added to the top level json with 
	 *     "original_raw_json":"{\"message\":\"Test message\"}".
	 *  2) if the json object contains key:value pair where key is message_field value
	 *     and value is a json object:
	 *     "log":{"message":"Test message"}, 
	 *     "message":"Test message" is going to be added to the top level json with 
	 *     "original_raw_json":"{\"message\":\"Test message\"}".
	 *
	 *  By setting compact="on", if the json contains string, array or json object
	 *  type object which is empty, they are eliminated.
	 */
	if (NULL != pWrkrData->pData->messageField) { /* message_field is configured? */
		struct json_object *sub_json;
		if (json_object_object_get_ex(json, pWrkrData->pData->messageField, &sub_json)) {
			/* the message_field ("log" in this example) exists in the given json */
			if (json_object_is_type(sub_json, json_type_string)) {
				/* the value is a string type */
				char *ostr = (char *)json_object_to_json_string_ext(sub_json, JSON_C_TO_STRING_PLAIN);
				if (NULL == ostr) {
					DBGPRINTF("mmjsonparse: Error getting the string format value of key '%s' in '%s' failed\n",
						pWrkrData->pData->messageField, buf);
					json_object_put(json);
					ABORT_FINALIZE(RS_RET_JSON_PARSE_ERR);
				}
				char *oscp = NULL;
				int osclen = strlen(ostr);
				if ('"' == ostr[0]) {
					oscp = ostr + 1;
					if ('"' == ostr[osclen-1]) {
						ostr[osclen-1] = '\0';
					}
				} else {
					oscp = ostr;
				}
				ostrcopy = strdup(oscp);
				if (NULL == ostrcopy) {
					DBGPRINTF("mmjsonparse: Error duplicating '%s' of '%s' failed\n", oscp, buf);
					json_object_put(json);
					ABORT_FINALIZE(RS_RET_JSON_PARSE_ERR);
				}
				osclen = strlen(ostrcopy);
				/* 
				 * Turning the string into a string format json object.  E.g.,
				 * {\"message\":\"Test message \",\"log_level\":\"INFO\"}
				 * ==>
				 * {"message":"Test message ","log_level":"INFO"}
				 */
				unescapeStr((uchar *)ostrcopy, osclen + 1);
				struct json_object *const nestedjson = json_tokener_parse_ex(pWrkrData->tokener, ostrcopy, osclen);
				if (NULL != nestedjson) {
					/* Deleting orgnestedjson */
					json_object_object_del(json, pWrkrData->pData->messageField);
					CHKiRet(jsonMerge(json, nestedjson));
				}
				/* 
				 * Else case: (NULL == nestedjson) means the value of messageField is pure string, 
				 * e.g., {"log":"string"}.  Leave it as is.
				 */
			} else if (json_object_is_type(sub_json, json_type_object)) {
				/* the value is a json object */
				json_object_get(sub_json); /* do not free sub_json in the next json_object_object_del */
				if (NULL != pWrkrData->pData->altMessageField) {
					/* convert the object to the string format before consumed in jsonMerge */
					ostrcopy = (char *)json_object_to_json_string_ext(sub_json, JSON_C_TO_STRING_PLAIN);
				}
				json_object_object_del(json, pWrkrData->pData->messageField);
				CHKiRet(jsonMerge(json, sub_json));
			}
		}
	}
	/* Eliminating empty json objects */
	if (pWrkrData->pData->bCompact) {
		int rc = jsonCompact(json);
		if ((rc < 0) /*error*/ || (rc > 0) /*empty*/) {
			json_object_put(json); 
			json = NULL;
		}
	}	
	if (NULL != json) {
		/*
		 * Check if alt_message_field is specified in rsyslog.conf
		 * action(type="mmjsonparse" cookie="" message_field="log" alt_message_field="original_message")
		 * Note: if message_field is not given, alt_message_field is ignored.
		 */
		if ((NULL != pWrkrData->pData->altMessageField) && (NULL != ostrcopy)) {
			json_object_object_add(json, pWrkrData->pData->altMessageField, json_object_new_string(ostrcopy));
		}
 		msgAddJSON(pMsg, pWrkrData->pData->container, json, 0, 0);
	}
finalize_it:
	free(ostrcopy);
	RETiRet;
}

BEGINdoAction_NoStrings
	smsg_t **ppMsg = (smsg_t **) pMsgData;
	smsg_t *pMsg = ppMsg[0];
	uchar *buf;
	rs_size_t len;
	int bSuccess = 0;
	struct json_object *jval;
	struct json_object *json;
	instanceData *pData;
	unsigned short freeBuf = 0;
CODESTARTdoAction
	pData = pWrkrData->pData;
	/* note that we can performance-optimize the interface, but this also
	 * requires changes to the libraries. For now, we accept message
	 * duplication. -- rgerhards, 2010-12-01
	 */
	if(pWrkrData->pData->bUseRawMsg) {
		getRawMsg(pMsg, &buf, &len);
	} else if (pWrkrData->pData->varDescr) {
		buf = MsgGetProp(pMsg, NULL, pWrkrData->pData->varDescr, &len, &freeBuf, NULL);
	} else {
		buf = getMSG(pMsg);
	}

	while(*buf && isspace(*buf)) {
		++buf;
	}

	if(*buf == '\0' || strncmp((char*)buf, pData->cookie, pData->lenCookie)) {
		DBGPRINTF("mmjsonparse: no JSON cookie: '%s'\n", buf);
		ABORT_FINALIZE(RS_RET_NO_CEE_MSG);
	}
	buf += pData->lenCookie;
	CHKiRet(processJSON(pWrkrData, pMsg, (char*) buf, strlen((char*)buf)));
	if (freeBuf) {
		free(buf);
		buf = NULL;
	}
	bSuccess = 1;
finalize_it:
	if(iRet == RS_RET_NO_CEE_MSG) {
		/* add buf as msg */
		json = json_object_new_object();
		jval = json_object_new_string((char*)buf);
		json_object_object_add(json, "msg", jval);
		msgAddJSON(pMsg, pData->container, json, 0, 0);
		iRet = RS_RET_OK;
	}
	MsgSetParseSuccess(pMsg, bSuccess);
ENDdoAction

static inline void
setInstParamDefaults(instanceData *pData)
{
	pData->bUseRawMsg = 0;
	pData->bCompact = 0;
	pData->messageField = NULL;
	pData->altMessageField = NULL;
	pData->varDescr = NULL;
}

BEGINnewActInst
	struct cnfparamvals *pvals;
	int i;
	char *varName = NULL;
CODESTARTnewActInst
	DBGPRINTF("newActInst (mmjsonparse)\n");
	if((pvals = nvlstGetParams(lst, &actpblk, NULL)) == NULL) {
		ABORT_FINALIZE(RS_RET_MISSING_CNFPARAMS);
	}


	CODE_STD_STRING_REQUESTnewActInst(1)
	CHKiRet(OMSRsetEntry(*ppOMSR, 0, NULL, OMSR_TPL_AS_MSG));
	CHKiRet(createInstance(&pData));
	setInstParamDefaults(pData);

	for(i = 0 ; i < actpblk.nParams ; ++i) {
		if(!pvals[i].bUsed)
			continue;
		if(!strcmp(actpblk.descr[i].name, "cookie")) {
			free(pData->cookie);
			pData->cookie = es_str2cstr(pvals[i].val.d.estr, NULL);
		} else if(!strcmp(actpblk.descr[i].name, "container")) {
			free(pData->container);
			size_t lenvar = es_strlen(pvals[i].val.d.estr);
			pData->container = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL);
			if(pData->container[0] == '$') {
				/* pre 8.35, the container name needed to be specified without
				 * the leading $. This was confusing, so we now require a full
				 * variable name. Nevertheless, we still need to support the
				 * version without $. -- rgerhards, 2018-05-16
				 */
				/* copy lenvar size because of \0 string terminator */
				memmove(pData->container, pData->container+1,  lenvar);
				--lenvar;
			}
			if(   (lenvar == 0)
			   || (  !(   pData->container[0] == '!'
			           || pData->container[0] == '.'
			           || pData->container[0] == '/' ) )
			   ) {
				parser_errmsg("mmjsonparse: invalid container name '%s', name must "
					"start with either '$!', '$.', or '$/'", pData->container);
				ABORT_FINALIZE(RS_RET_INVALID_VAR);
			}
		} else if(!strcmp(actpblk.descr[i].name, "userawmsg")) {
			pData->bUseRawMsg = (int) pvals[i].val.d.n;
		} else if(!strcmp(actpblk.descr[i].name, "compact")) {
			pData->bCompact = (int) pvals[i].val.d.n;
		} else if(!strcmp(actpblk.descr[i].name, "message_field")) {
			free(pData->messageField);
			pData->messageField = es_str2cstr(pvals[i].val.d.estr, NULL);
		} else if(!strcmp(actpblk.descr[i].name, "alt_message_field")) {
			free(pData->altMessageField);
			pData->altMessageField = es_str2cstr(pvals[i].val.d.estr, NULL);
		} else if(!strcmp(actpblk.descr[i].name, "variable")) {
			varName = es_str2cstr(pvals[i].val.d.estr, NULL);
		} else {
			dbgprintf("mmjsonparse: program error, non-handled param '%s'\n", actpblk.descr[i].name);
		}
	}
	if (varName) {
		if(pData->bUseRawMsg) {
			errmsg.LogError(0, RS_RET_CONFIG_ERROR,
							"mmjsonparse: 'variable' param can't be used with 'useRawMsg'. "
							"Ignoring 'variable', will use raw message.");
		} else {
			CHKmalloc(pData->varDescr = MALLOC(sizeof(msgPropDescr_t)));
			CHKiRet(msgPropDescrFill(pData->varDescr, (uchar*) varName, strlen(varName)));
		}
		free(varName);
		varName = NULL;
	}

	if(pData->container == NULL)
		CHKmalloc(pData->container = (uchar*) strdup("!"));
	pData->lenCookie = strlen(pData->cookie);
CODE_STD_FINALIZERnewActInst
	cnfparamvalsDestruct(pvals, &actpblk);
ENDnewActInst

BEGINparseSelectorAct
CODESTARTparseSelectorAct
CODE_STD_STRING_REQUESTparseSelectorAct(1)
	/* first check if this config line is actually for us */
	if(strncmp((char*) p, ":mmjsonparse:", sizeof(":mmjsonparse:") - 1)) {
		ABORT_FINALIZE(RS_RET_CONFLINE_UNPROCESSED);
	}

	/* ok, if we reach this point, we have something for us */
	p += sizeof(":mmjsonparse:") - 1; /* eat indicator sequence  (-1 because of '\0'!) */
	CHKiRet(createInstance(&pData));

	/* check if a non-standard template is to be applied */
	if(*(p-1) == ';')
		--p;
	/* we call the function below because we need to call it via our interface definition. However,
	 * the format specified (if any) is always ignored.
	 */
	iRet = cflineParseTemplateName(&p, *ppOMSR, 0, OMSR_TPL_AS_MSG, (uchar*) "RSYSLOG_FileFormat");
CODE_STD_FINALIZERparseSelectorAct
ENDparseSelectorAct


BEGINmodExit
CODESTARTmodExit
	objRelease(errmsg, CORE_COMPONENT);
ENDmodExit


BEGINqueryEtryPt
CODESTARTqueryEtryPt
CODEqueryEtryPt_STD_OMOD_QUERIES
CODEqueryEtryPt_STD_OMOD8_QUERIES
CODEqueryEtryPt_STD_CONF2_OMOD_QUERIES
CODEqueryEtryPt_STD_CONF2_QUERIES
ENDqueryEtryPt



/* Reset config variables for this module to default values.
 */
static rsRetVal resetConfigVariables(uchar __attribute__((unused)) *pp, void __attribute__((unused)) *pVal)
{
	DEFiRet;
	RETiRet;
}


BEGINmodInit()
	rsRetVal localRet;
	rsRetVal (*pomsrGetSupportedTplOpts)(unsigned long *pOpts);
	unsigned long opts;
	int bMsgPassingSupported;
CODESTARTmodInit
	*ipIFVersProvided = CURR_MOD_IF_VERSION;
		/* we only support the current interface specification */
CODEmodInit_QueryRegCFSLineHdlr
	DBGPRINTF("mmjsonparse: module compiled with rsyslog version %s.\n", VERSION);
	/* check if the rsyslog core supports parameter passing code */
	bMsgPassingSupported = 0;
	localRet = pHostQueryEtryPt((uchar*)"OMSRgetSupportedTplOpts",
			&pomsrGetSupportedTplOpts);
	if(localRet == RS_RET_OK) {
		/* found entry point, so let's see if core supports msg passing */
		CHKiRet((*pomsrGetSupportedTplOpts)(&opts));
		if(opts & OMSR_TPL_AS_MSG)
			bMsgPassingSupported = 1;
	} else if(localRet != RS_RET_ENTRY_POINT_NOT_FOUND) {
		ABORT_FINALIZE(localRet); /* Something else went wrong, not acceptable */
	}
	
	if(!bMsgPassingSupported) {
		DBGPRINTF("mmjsonparse: msg-passing is not supported by rsyslog core, "
			  "can not continue.\n");
		ABORT_FINALIZE(RS_RET_NO_MSG_PASSING);
	}

	CHKiRet(objUse(errmsg, CORE_COMPONENT));
	
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"resetconfigvariables", 1, eCmdHdlrCustomHandler,
				    resetConfigVariables, NULL, STD_LOADABLE_MODULE_ID));
ENDmodInit

/* vi:set ai:
 */
